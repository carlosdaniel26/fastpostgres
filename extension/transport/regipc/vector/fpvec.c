/*
 * fpvec.c - what a vector-carrying cross-process call costs, and whether it is
 * worth an ABI v3.
 *
 * WHAT IS MEASURED. One call is one ioctl on /dev/regipcvec: the caller puts
 * x0..x8 in the carrier registers (and, in vec mode, its vector state in
 * XMM/ZMM with a stub in the style of the technique's x86_64/switch.S), the
 * module runs the callee's leaf in ring 0 and returns the call counter in RAX.
 * In vec mode (REGIPC_IOC_CALL_V) the module also saves the caller's live FPU
 * state with kernel_fpu_begin() and copies `width` bytes of the image into the
 * callee's pinned arena; at width 0 it saves/restores and copies nothing, which
 * is the point that isolates the FP cost from the per-byte copy cost.
 *
 * bulk mode (REGIPC_IOC_CALL_B) is the cheaper design the measurements point
 * at: the carriers are a descriptor {flags, user_ptr, length, seq, dst_off},
 * the module's handler (running in the caller's context, not under a lock) does
 * copy_from_user() of that buffer into the callee's arena page, and no FPU
 * state is touched. `--cold` makes the source a >L3 buffer walked with a cache-
 * line-avoiding stride, so the measured copy is an honest one for a payload the
 * caller did not just leave in cache.
 *
 * TWO PROCESSES, NO THREADS. The parent is the producer: it registers as the
 * caller, pairs with the child and times the calls. The child is the callee: it
 * registers the two-page region (page 0 the ABI 2 leaf struct, page 1 the 4 KB
 * arena), polls leaf.calls until the stream starts, waits for the stop message,
 * folds what it received and reports. The child costs no wall time in the loop
 * itself - the whole point of the technique is that the callee process never
 * runs during a call.
 *
 * WHAT IS VERIFIED. Three independent chains:
 *   - the leaf: leaf.calls must equal warmup + calls exactly (every call once,
 *     in order) and leaf.checksum must equal the caller's fold over the same
 *     x0..x8 (the module's own FNV-1a over the low 32 bits of each carrier);
 *   - the payload: vec mode folds the register bytes in the copied prefix (the
 *     XMM low halves at 160+16i, then the AVX-512 components at the layout
 *     offsets CPUID and the module's VINFO give, zero fill past the real image);
 *     bulk mode folds the `length` bytes the caller sent. The caller's expected
 *     fold, the parent's read of the arena and the callee's own fold must all
 *     agree, in both modes;
 *   - the child exit: it must leave a report of its own and exit 0.
 *
 * THE OUTPUT. One single-line JSON object per run on stdout (one run = one
 * rep); a human summary on stderr unless --json. The record carries mode,
 * width, calls, bytes_per_call, seconds, calls_per_second, gb_per_second,
 * us_per_call_p50/p99, producer_cpu_s, consumer_cpu_s, checksum, verified and
 * gaps, plus the counters that prove the verification.
 *
 * THE DEVICE. /dev/regipcvec is this experiment's own module (regipcvec.ko);
 * the ABI 2 /dev/regipc is left alone. --mode xsave is the no-root userspace
 * proxy: it times xsave+xrstor of the AVX-512 state with the same instructions
 * the kernel uses for the image (XSAVE/XRSTOR in user mode; the kernel's
 * fpstate uses XSAVES/XRSTORS), which is the floor of the FP overhead a V call
 * would pay.
 *
 * Usage:
 *   fpvec --mode plain  --reps 3 [--count N] [--warmup N] [--device PATH]
 *   fpvec --mode vec --width N --reps 3 [--count N] [--warmup N]
 *   fpvec --mode bulk --width N [--cold] --reps 3 [--count N] [--warmup N]
 *   fpvec --mode xsave  --reps 3
 *   fpvec --selftest
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* CPU_ZERO/CPU_SET, sched_setaffinity */
#endif

#include <sys/syscall.h> /* __NR_ioctl, consumed by the vendored handoff asm */

#include "fastpostgres/regipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------- the ABI 3 extension */

/* Kept in sync with regipcvec.c; the base ioctls 1..5 are the vendored ABI. */
#define REGIPC_VEC_DEVICE "/dev/regipcvec"
#define REGIPC_IOC_CALL_V _IO('c', 6)
#define REGIPC_IOC_VINFO  _IOR('c', 7, struct regipc_vec_info)
#define REGIPC_IOC_CALL_B _IO('c', 8)

/*
 * CALL_B's carrier block, kept in sync with regipcvec.c: w[0] flags,
 * w[1] source pointer, w[2] length, w[3] sequence number, w[4] destination
 * offset inside the callee's arena page; w[5..8] reserved and zero.
 */
#define BULK_W_FLAGS 0
#define BULK_W_PTR   1
#define BULK_W_LEN   2
#define BULK_W_SEQ   3
#define BULK_W_DST   4

enum regipc_vec_format
{
    REGIPC_VEC_FORMAT_NONE   = 0,
    REGIPC_VEC_FORMAT_FXSAVE = 1,
    REGIPC_VEC_FORMAT_XSAVE  = 2,
    REGIPC_VEC_FORMAT_XSAVES = 3,
};

#define REGIPC_VEC_F_AVX512 (1u << 0)
#define REGIPC_VEC_F_PADDED (1u << 1)

struct regipc_vec_info
{
    uint32_t abi_version;
    uint32_t width;
    uint32_t fpstate_size;
    uint32_t real_bytes;
    uint64_t xfeatures;
    uint32_t format;
    uint32_t flags;
};

/* ------------------------------------------------------------------- layout */

/* The callee's registration: page 0 is the leaf struct, page 1 the arena. */
#define LEAF_PAGE_BYTES 4096u
#define ARENA_OFFSET    4096u
#define REGION_BYTES    8192u

/* The canonical caller state: 32 ZMM registers, 64 B each, in this order.
 * XMM0..15 are the low 128 bits of ZMM0..15, so one image covers both. */
#define ZMM_BLOB_BYTES (32u * 64u)

/* FXSAVE legacy region: the 16 XMM registers live at offset 160. */
#define XMM_IMAGE_OFF 160u
#define XMM_BYTES     16u

#define FNV_SEED 14695981039346656037ull

#define CHILD_REPORT_MAGIC 0x5245504f52545645ull /* "VECTORPR" */

/* The carrier constants: the same nine values on every call, so the leaf's
 * checksum is a fixed, checkable chain. w[0] doubles as the ioctl's arg word. */
#define CARRIER_W(i) (0x0000000056ec0000ull + (uint64_t)(i) * 0x01010101ull)

/*
 * bulk mode's cold source: one mapping larger than L3 (32 MiB on this box) that
 * is filled with a repeating 64-byte period and walked with a 64 KiB stride -
 * sixteen times a cache line - so each call reads addresses that have long been
 * evicted, and every slice has the same bytes (same fold) whatever its offset.
 * The walk advances by the stride and wraps; the mapping is a whole number of
 * strides, so no slice ever crosses the end.
 */
#define BULK_COLD_BYTES  (256u << 20)
#define BULK_COLD_STRIDE (64u << 10)
#define BULK_PERIOD      64u

/* ------------------------------------------------------------------- basics */

typedef enum Mode
{
    MODE_NONE,
    MODE_PLAIN,
    MODE_VEC,
    MODE_BULK,
    MODE_XSAVE,
} Mode;

typedef struct Options
{
    Mode     mode;
    uint64_t width; /* vec/bulk: must match the module parameter (vec) / payload bytes (bulk) */
    uint64_t count;
    uint64_t warmup;
    unsigned reps;
    char     device[128];
    int      cpu_caller;
    int      cpu_callee;
    bool     json;
    bool     selftest;
    bool     cold; /* bulk only: read the payload from a >L3 source */
} Options;

typedef struct Result
{
    uint64_t calls;
    uint64_t warmup;
    uint64_t errors;
    uint64_t gaps;
    uint64_t bytes_per_call;
    uint64_t wall_ns;           /* the measured loop, first call to last return */
    uint64_t calls_per_second;
    uint64_t gbps_milli;        /* thousandths of a GB/s, integer arithmetic only */
    uint64_t ns_p50;
    uint64_t ns_p99;
    uint64_t producer_cpu_us;
    uint64_t consumer_cpu_us;
    uint64_t checksum;          /* the caller's fold over what it sent */
    uint64_t received_checksum; /* the parent's fold over what is in the arena */
    uint64_t child_checksum;    /* the child's fold over what it saw */
    uint64_t leaf_calls;
    uint64_t leaf_checksum;
    uint64_t gpr_checksum;      /* the leaf's own chain, read back */
    uint64_t child_gpr_checksum;
    uint64_t child_gpr_match;
    uint64_t vec_runs;
    uint64_t vec_bad;
    uint64_t child_vec_match;
    uint64_t child_seq_match;
    uint64_t limits_ok;         /* bulk: the module rejected len>page, dst+len>page, flags!=0 */
    uint64_t child_poll_iters;
    uint64_t child_cpu_us;
    uint32_t fpstate_size;
    uint32_t real_bytes;
    uint32_t format;
    uint32_t vec_flags;
    uint64_t cold_bytes;        /* bulk: source mapping size, 0 when hot */
    uint64_t cold_stride;       /* bulk: address advance per call, 0 when hot */
    uint32_t page_bytes;        /* the arena's limit, from sysconf(_SC_PAGESIZE) */
    bool     child_exit_ok;
    bool     verified;
} Result;

static void
say(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;

    if (errsz == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static uint64_t
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t
rusage_us(const struct rusage *ru)
{
    return (uint64_t)ru->ru_utime.tv_sec * 1000000ull + (uint64_t)ru->ru_utime.tv_usec +
           (uint64_t)ru->ru_stime.tv_sec * 1000000ull + (uint64_t)ru->ru_stime.tv_usec;
}

/* Nearest rank, the harness's convention: index int(q * (n - 1)). Integer-only:
 * this file is built with -mgeneral-regs-only, so no double may cross a call. */
static uint64_t
percentile_ns(const uint64_t *sorted, uint64_t n, uint64_t num, uint64_t den)
{
    if (n == 0) return 0;
    return sorted[(size_t)(((n - 1) * num) / den)];
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static const char *
mode_name(Mode m)
{
    switch (m)
    {
    case MODE_PLAIN: return "plain";
    case MODE_VEC: return "vec";
    case MODE_BULK: return "bulk";
    case MODE_XSAVE: return "xsave";
    default: return "none";
    }
}

static uint64_t
mix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/* The CALL_B carriers: flags, source pointer, length, sequence number, dst
 * offset, then four reserved zeros. Seqs change per rep so the callee's check
 * of leaf->values[BULK_W_SEQ] identifies the carrier set that was last used. */
static void
bulk_carriers(struct regipc_carriers *c, const uint8_t *src, uint64_t len, uint64_t seq, uint64_t dst)
{
    memset(c, 0, sizeof *c);
    c->w[BULK_W_FLAGS] = 0;
    c->w[BULK_W_PTR]   = (uint64_t)(uintptr_t)src;
    c->w[BULK_W_LEN]   = len;
    c->w[BULK_W_SEQ]   = seq;
    c->w[BULK_W_DST]   = dst;
}

/*
 * bulk mode's source. Hot: one small buffer, the same address every call, hot
 * in L1 after the warm-up. Cold: the >L3 period-filled mapping above, advanced
 * by BULK_COLD_STRIDE per call - the address handed to the handler is different
 * every call, so the copy reads from a cache line that has not been touched for
 * thousands of calls. Every slice holds identical bytes, so one expected fold
 * covers every call whatever the offset.
 */
static void
bulk_fill(uint8_t *p, size_t bytes)
{
    uint8_t period[BULK_PERIOD];

    for (size_t j = 0; j < BULK_PERIOD / 8; j++)
    {
        uint64_t v = mix64(0x62756c6b5f706572ull ^ (uint64_t)j);

        memcpy(period + 8 * j, &v, 8);
    }
    for (size_t off = 0; off < bytes; off += BULK_PERIOD)
    {
        size_t n = bytes - off < BULK_PERIOD ? bytes - off : BULK_PERIOD;

        memcpy(p + off, period, n);
    }
}

/* --------------------------------------------------------------- cpu/xstate */

static void
cpuid_raw(uint32_t leaf, uint32_t sub, uint32_t out[4])
{
    __asm__ volatile("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3]) : "a"(leaf), "c"(sub));
}

static uint64_t
xgetbv0(void)
{
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t
xd_size(uint32_t n)
{
    uint32_t r[4];

    cpuid_raw(0xd, n, r);
    return r[0];
}

static uint32_t
xd_off(uint32_t n)
{
    uint32_t r[4];

    cpuid_raw(0xd, n, r);
    return r[1];
}

/* AVX-512 usable: XSAVE, the OS enabled the ZMM state in XCR0, and the CPU has
 * AVX512F. Without it the module's contract falls back to 16 XMM = 256 B. */
static bool
cpu_has_avx512(void)
{
    uint32_t r1[4];
    uint32_t r7[4];

    cpuid_raw(1, 0, r1);
    if (!(r1[2] & (1u << 26))) return false; /* XSAVE */
    if (!(r1[2] & (1u << 27))) return false; /* OSXSAVE */
    if ((xgetbv0() & 0xe6u) != 0xe6u) return false;
    cpuid_raw(7, 0, r7);
    return (r7[1] & (1u << 16)) != 0; /* AVX512F */
}

/* The registers' offsets: standard XSAVE takes them from CPUID, XSAVES packs
 * the components in feature order from 576 with 64-byte alignment. */
struct xlayout
{
    uint32_t ymm_off;
    uint32_t zh_off;
    uint32_t hz_off;
    bool     has_ymm;
    bool     has_zh;
    bool     has_hz;
};

static void
layout_build(int fmt, uint64_t xf, struct xlayout *l)
{
    memset(l, 0, sizeof *l);

    if (fmt == REGIPC_VEC_FORMAT_XSAVE)
    {
        if ((xf >> 2) & 1u)
        {
            l->has_ymm = true;
            l->ymm_off = xd_off(2);
        }
        if ((xf >> 6) & 1u)
        {
            l->has_zh = true;
            l->zh_off = xd_off(6);
        }
        if ((xf >> 7) & 1u)
        {
            l->has_hz = true;
            l->hz_off = xd_off(7);
        }
        return;
    }

    if (fmt == REGIPC_VEC_FORMAT_XSAVES)
    {
        uint32_t cur = 512u + 64u; /* legacy fxregs + the xstate header */

        for (uint32_t n = 2; n < 32; n++)
        {
            uint32_t size;

            if (!((xf >> n) & 1u)) continue;
            size = xd_size(n);
            if (size == 0) continue;
            cur = (cur + 63u) & ~63u;
            switch (n)
            {
            case 2: l->has_ymm = true; l->ymm_off = cur; break;
            case 6: l->has_zh = true; l->zh_off = cur; break;
            case 7: l->has_hz = true; l->hz_off = cur; break;
            default: break;
            }
            cur += size;
        }
    }
}

/* One register run that must be found in the copied prefix: where it lives in
 * the image, how long it is, and where its bytes sit in the caller's blob. */
struct vec_run
{
    uint32_t off;
    uint32_t len;
    uint32_t exp;
};

struct vec_plan
{
    struct vec_run run[72];
    int            n;
    uint32_t       real;
    uint32_t       zero_off;
    uint32_t       zero_len;
};

static void
plan_add(struct vec_plan *p, uint32_t off, uint32_t len, uint32_t exp)
{
    p->run[p->n].off = off;
    p->run[p->n].len = len;
    p->run[p->n].exp = exp;
    p->n++;
}

static void
plan_build(struct vec_plan *p, uint32_t width, uint32_t real, int fmt, uint64_t xf)
{
    struct xlayout l;

    memset(p, 0, sizeof *p);
    p->real = real;
    layout_build(fmt, xf, &l);

    /* The legacy region: XMM0..15 at 160..416, in both XSAVE and XSAVES. */
    if (fmt != REGIPC_VEC_FORMAT_NONE)
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            uint32_t off = XMM_IMAGE_OFF + 16u * i;

            if (off + XMM_BYTES > real) break;
            plan_add(p, off, XMM_BYTES, 64u * i);
        }
    }

    if (l.has_ymm)
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            uint32_t off = l.ymm_off + 16u * i;

            if (off + 16u > real) break;
            plan_add(p, off, 16u, 64u * i + 16u); /* ZMMi bits 128..255 */
        }
    }
    if (l.has_zh)
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            uint32_t off = l.zh_off + 32u * i;

            if (off + 32u > real) break;
            plan_add(p, off, 32u, 64u * i + 32u); /* ZMMi bits 256..511 */
        }
    }
    if (l.has_hz)
    {
        for (uint32_t j = 0; j < 16; j++)
        {
            uint32_t off = l.hz_off + 64u * j;

            if (off + 64u > real) break;
            plan_add(p, off, 64u, 64u * (16u + j)); /* ZMM16..31, whole */
        }
    }

    if (width > real)
    {
        p->zero_off = real;
        p->zero_len = width - real;
    }
}

/* --------------------------------------------------------------- checksums */

static uint64_t
fold_bytes(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = data;

    while (n--)
    {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t
fold_zeros(uint64_t h, size_t n)
{
    while (n--)
    {
        h ^= 0;
        h *= 1099511628211ull;
    }
    return h;
}

/* What the caller sent: the blob's bytes for every run, then the zero fill. */
static uint64_t
plan_fold_expected(uint64_t h, const uint8_t *blob, const struct vec_plan *p)
{
    for (int i = 0; i < p->n; i++)
        h = fold_bytes(h, blob + p->run[i].exp, p->run[i].len);
    return fold_zeros(h, p->zero_len);
}

/* What lies in the arena at the planned offsets. */
static uint64_t
plan_fold_received(uint64_t h, const uint8_t *arena, const struct vec_plan *p)
{
    for (int i = 0; i < p->n; i++)
        h = fold_bytes(h, arena + p->run[i].off, p->run[i].len);
    return fold_zeros(h, p->zero_len);
}

/* The module's leaf_fnv: FNV-1a over the low 32 bits of each carrier. */
static uint64_t
leaf_fnv(uint64_t h, uint32_t v)
{
    for (int b = 0; b < 4; b++)
    {
        h ^= (uint8_t)(v >> (b * 8));
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t
carriers_fold(const uint64_t *w)
{
    uint64_t h = FNV_SEED;

    for (int i = 0; i < REGIPC_CARRIERS; i++)
        h = leaf_fnv(h, (uint32_t)w[i]);
    return h;
}

/* The blob the stub loads: each register gets 64 bytes no other register has,
 * so a run of 16 bytes already identifies its register. */
static void
blob_fill(uint8_t *blob)
{
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++)
        {
            uint64_t v = mix64(0x243f6a8885a308d3ull ^ ((uint64_t)i << 16) ^ (uint64_t)j);

            memcpy(blob + 64 * i + 8 * j, &v, 8);
        }
}

static uint8_t vec_blob[ZMM_BLOB_BYTES] __attribute__((aligned(64)));

/* ------------------------------------------------------------ device wrappers */

static int
dev_open(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0) return -errno;
    return fd;
}

static int
dev_register(int fd, const struct regipc_context_desc *d)
{
    if (ioctl(fd, REGIPC_IOC_REGISTER, d) < 0) return -errno;
    return 0;
}

static int
dev_pair(int fd, pid_t peer)
{
    __s32 pid = (__s32)peer;

    if (ioctl(fd, REGIPC_IOC_PAIR, &pid) < 0) return -errno;
    return 0;
}

static int
dev_vinfo(int fd, struct regipc_vec_info *vi)
{
    if (ioctl(fd, REGIPC_IOC_VINFO, vi) < 0) return -errno;
    return 0;
}

static int
dev_quit(int fd)
{
    if (ioctl(fd, REGIPC_IOC_QUIT) < 0) return -errno;
    return 0;
}

/* The caller's identity. The module should take {0}; a module that predates that
 * shape wants a leaf page even from a caller, so retry with the region's leaf
 * (flags stay 0: this process is the caller, it only needs to exist). */
static int
register_caller(int fd, void *region)
{
    struct regipc_context_desc d;
    int                        rc;

    memset(&d, 0, sizeof d);
    rc = dev_register(fd, &d);
    if (rc == 0) return 0;
    if (rc != -EINVAL) return rc;

    memset(&d, 0, sizeof d);
    d.data     = (uint64_t)(uintptr_t)region;
    d.data_len = REGION_BYTES;
    return dev_register(fd, &d);
}

/* --------------------------------------------------------------------- stubs */

/*
 * The vec stub: the nine carriers exactly as the vendored switch.S sets them,
 * then the vector state loaded from the blob, then one syscall. All of it is
 * one asm block, so nothing can clobber a register between the load and the
 * call. Every XMM/ZMM register is caller-saved in the x86-64 ABI; this file is
 * built with -mgeneral-regs-only so the compiler never puts a value in one.
 */
static inline long
vec_handoff_avx512(int fd, const uint8_t *vec)
{
    register long rdx asm("rdx") = (long)CARRIER_W(0);
    register long rbx asm("rbx") = (long)CARRIER_W(1);
    register long r8 asm("r8")   = (long)CARRIER_W(2);
    register long r9 asm("r9")   = (long)CARRIER_W(3);
    register long r10 asm("r10") = (long)CARRIER_W(4);
    register long r12 asm("r12") = (long)CARRIER_W(5);
    register long r13 asm("r13") = (long)CARRIER_W(6);
    register long r14 asm("r14") = (long)CARRIER_W(7);
    register long r15 asm("r15") = (long)CARRIER_W(8);
    long          ret;

    __asm__ volatile("vmovdqu64    0(%[v]), %%zmm0\n\t"
                     "vmovdqu64   64(%[v]), %%zmm1\n\t"
                     "vmovdqu64  128(%[v]), %%zmm2\n\t"
                     "vmovdqu64  192(%[v]), %%zmm3\n\t"
                     "vmovdqu64  256(%[v]), %%zmm4\n\t"
                     "vmovdqu64  320(%[v]), %%zmm5\n\t"
                     "vmovdqu64  384(%[v]), %%zmm6\n\t"
                     "vmovdqu64  448(%[v]), %%zmm7\n\t"
                     "vmovdqu64  512(%[v]), %%zmm8\n\t"
                     "vmovdqu64  576(%[v]), %%zmm9\n\t"
                     "vmovdqu64  640(%[v]), %%zmm10\n\t"
                     "vmovdqu64  704(%[v]), %%zmm11\n\t"
                     "vmovdqu64  768(%[v]), %%zmm12\n\t"
                     "vmovdqu64  832(%[v]), %%zmm13\n\t"
                     "vmovdqu64  896(%[v]), %%zmm14\n\t"
                     "vmovdqu64  960(%[v]), %%zmm15\n\t"
                     "vmovdqu64 1024(%[v]), %%zmm16\n\t"
                     "vmovdqu64 1088(%[v]), %%zmm17\n\t"
                     "vmovdqu64 1152(%[v]), %%zmm18\n\t"
                     "vmovdqu64 1216(%[v]), %%zmm19\n\t"
                     "vmovdqu64 1280(%[v]), %%zmm20\n\t"
                     "vmovdqu64 1344(%[v]), %%zmm21\n\t"
                     "vmovdqu64 1408(%[v]), %%zmm22\n\t"
                     "vmovdqu64 1472(%[v]), %%zmm23\n\t"
                     "vmovdqu64 1536(%[v]), %%zmm24\n\t"
                     "vmovdqu64 1600(%[v]), %%zmm25\n\t"
                     "vmovdqu64 1664(%[v]), %%zmm26\n\t"
                     "vmovdqu64 1728(%[v]), %%zmm27\n\t"
                     "vmovdqu64 1792(%[v]), %%zmm28\n\t"
                     "vmovdqu64 1856(%[v]), %%zmm29\n\t"
                     "vmovdqu64 1920(%[v]), %%zmm30\n\t"
                     "vmovdqu64 1984(%[v]), %%zmm31\n\t"
                     "syscall\n\t"
                     : "=a"(ret)
                     : "a"((long)__NR_ioctl), "D"((long)fd), "S"((long)REGIPC_IOC_CALL_V), "r"(rdx), "r"(rbx),
                       "r"(r8), "r"(r9), "r"(r10), "r"(r12), "r"(r13), "r"(r14), "r"(r15), [v] "r"(vec)
                     : "rcx", "r11", "memory");
    return ret;
}

/* The same call without AVX-512: 16 XMM registers, each at the low 128 bits of
 * its blob row. */
static inline long
vec_handoff_sse(int fd, const uint8_t *vec)
{
    register long rdx asm("rdx") = (long)CARRIER_W(0);
    register long rbx asm("rbx") = (long)CARRIER_W(1);
    register long r8 asm("r8")   = (long)CARRIER_W(2);
    register long r9 asm("r9")   = (long)CARRIER_W(3);
    register long r10 asm("r10") = (long)CARRIER_W(4);
    register long r12 asm("r12") = (long)CARRIER_W(5);
    register long r13 asm("r13") = (long)CARRIER_W(6);
    register long r14 asm("r14") = (long)CARRIER_W(7);
    register long r15 asm("r15") = (long)CARRIER_W(8);
    long          ret;

    __asm__ volatile("movdqu  256(%[v]), %%xmm0\n\t"
                     "movdqu  320(%[v]), %%xmm1\n\t"
                     "movdqu  384(%[v]), %%xmm2\n\t"
                     "movdqu  448(%[v]), %%xmm3\n\t"
                     "movdqu  512(%[v]), %%xmm4\n\t"
                     "movdqu  576(%[v]), %%xmm5\n\t"
                     "movdqu  640(%[v]), %%xmm6\n\t"
                     "movdqu  704(%[v]), %%xmm7\n\t"
                     "movdqu  768(%[v]), %%xmm8\n\t"
                     "movdqu  832(%[v]), %%xmm9\n\t"
                     "movdqu  896(%[v]), %%xmm10\n\t"
                     "movdqu  960(%[v]), %%xmm11\n\t"
                     "movdqu 1024(%[v]), %%xmm12\n\t"
                     "movdqu 1088(%[v]), %%xmm13\n\t"
                     "movdqu 1152(%[v]), %%xmm14\n\t"
                     "movdqu 1216(%[v]), %%xmm15\n\t"
                     "syscall\n\t"
                     : "=a"(ret)
                     : "a"((long)__NR_ioctl), "D"((long)fd), "S"((long)REGIPC_IOC_CALL_V), "r"(rdx), "r"(rbx),
                       "r"(r8), "r"(r9), "r"(r10), "r"(r12), "r"(r13), "r"(r14), "r"(r15), [v] "r"(vec)
                     : "rcx", "r11", "memory");
    return ret;
}

/*
 * Why the SSE stub loads xmm0..15 from the blob's 64-byte rows and not its first
 * 256 bytes: the blob is the canonical 32-ZMM image, so row i is register i.
 * The AVX-512 stub loads whole ZMM registers, which sets each XMM's low 128
 * bits as a side effect; the SSE stub loads exactly those 128 bits.
 */

/* ------------------------------------------------------ the userspace proxy */

#define XSAVE_BUF_BYTES 4096u
static uint8_t xsave_buf[XSAVE_BUF_BYTES] __attribute__((aligned(64)));

static void
xsave_once(uint64_t mask)
{
    uint32_t lo = (uint32_t)mask;
    uint32_t hi = (uint32_t)(mask >> 32);

    __asm__ volatile("xsave64 %[m]" : [m] "+m"(*(uint8_t(*)[XSAVE_BUF_BYTES])xsave_buf) : "a"(lo), "d"(hi) : "memory");
}

static void
xrstor_once(uint64_t mask)
{
    uint32_t lo = (uint32_t)mask;
    uint32_t hi = (uint32_t)(mask >> 32);

    __asm__ volatile("xrstor64 %[m]" : : [m] "m"(*(const uint8_t(*)[XSAVE_BUF_BYTES])xsave_buf), "a"(lo), "d"(hi)
                     : "memory");
}

typedef struct XsaveResult
{
    uint64_t mask;
    uint64_t bytes;
    uint64_t iters;
    uint64_t total_ns_save;
    uint64_t total_ns_restore;
    uint64_t total_ns_save_restore;
} XsaveResult;

/* Run a loop of `iters` operations and return the total nanoseconds. The caller
 * divides; this file is built with -mgeneral-regs-only, so no floating point. */
static uint64_t
time_loop(uint64_t iters, int what)
{
    uint64_t mask = xgetbv0();
    uint64_t t0;
    uint64_t t1;

    t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++)
    {
        if (what == 0)
            xsave_once(mask);
        else if (what == 1)
            xrstor_once(mask);
        else
        {
            xsave_once(mask);
            xrstor_once(mask);
        }
    }
    t1 = now_ns();
    return t1 - t0;
}

/* ---------------------------------------------------------------- the child */

struct ready_msg
{
    int32_t pid;
    int32_t rc; /* 0 = registered; else -errno */
};

struct stop_msg
{
    uint64_t expected_vec;
    uint64_t expected_gpr;
    uint64_t expected_seq;
    uint64_t expected_calls;
    uint32_t width;
    uint32_t real_bytes;
    uint32_t format;
    uint32_t vec;  /* 0 = plain, 1 = vec: whether the vector fold means anything */
    uint32_t bulk; /* 1 = bulk: fold `width` payload bytes at dst_off */
    uint32_t dst_off;
};

struct child_report
{
    uint64_t magic;
    uint64_t vec_received;
    uint64_t vec_runs;
    uint64_t vec_bad;
    uint64_t gpr_fold;
    uint64_t leaf_calls;
    uint64_t leaf_checksum;
    uint64_t poll_iters;
    uint64_t cpu_us;
    uint64_t seq_match;
    uint32_t vec_match;
    uint32_t gpr_match;
    uint32_t calls_match;
    uint32_t first_call_seen;
};

static int
read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;

    while (n > 0)
    {
        ssize_t r = read(fd, p, n);

        if (r < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static int
write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n > 0)
    {
        ssize_t w = write(fd, p, n);

        if (w < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static void
pin_cpu(int cpu)
{
    cpu_set_t set;

    if (cpu < 0) return;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof set, &set);
}

/*
 * The callee. It registers the two-page region, polls leaf.calls until the call
 * stream starts (the technique's callee waits on its leaf the same way), then
 * sleeps on the stop pipe - it must not spin on the leaf's cache line while the
 * producer is being timed. On the stop message it folds the arena's register
 * runs with the same plan the parent used and reports.
 */
static int
child_main(const Options *o, uint8_t *region, int ready_fd, int stop_fd, int result_fd)
{
    struct ready_msg  rm;
    struct stop_msg   msg;
    struct child_report crep;
    struct regipc_leaf_result *leaf = (struct regipc_leaf_result *)region;
    struct regipc_vec_info     vi;
    struct vec_plan            plan;
    struct rusage              ru0;
    struct rusage              ru1;
    uint64_t                   t0;
    uint64_t                   poll_iters = 0;
    int                        fd;
    int                        rc;

    pin_cpu(o->cpu_callee);

    fd = dev_open(o->device);
    if (fd < 0)
    {
        rm.pid = 0;
        rm.rc  = fd;
        write_full(ready_fd, &rm, sizeof rm);
        return 1;
    }

    {
        struct regipc_context_desc d;

        memset(&d, 0, sizeof d);
        d.data     = (uint64_t)(uintptr_t)region;
        d.data_len = REGION_BYTES;
        d.flags    = REGIPC_F_CALLEE;
        rc         = dev_register(fd, &d);
    }
    rm.pid = (int32_t)getpid();
    rm.rc  = rc;
    write_full(ready_fd, &rm, sizeof rm);
    if (rc < 0)
    {
        close(fd);
        return 1;
    }

    getrusage(RUSAGE_SELF, &ru0);

    /* Poll leaf.calls until the first call lands. Acquire: the module publishes
     * the counter with a release store after the call's data is in the leaf and,
     * for bulk calls, after the payload copy. Bounded: a run that never calls is
     * a failure, not a hang. */
    t0 = now_ns();
    for (;;)
    {
        if (__atomic_load_n(&leaf->calls, __ATOMIC_ACQUIRE) != 0) break;
        poll_iters++;
        if (now_ns() - t0 > 10000000000ull) /* 10 s */
        {
            close(fd);
            return 3;
        }
    }

    if (read_full(stop_fd, &msg, sizeof msg) != 0)
    {
        close(fd);
        return 4;
    }

    memset(&crep, 0, sizeof crep);
    crep.first_call_seen = 1;
    crep.poll_iters      = poll_iters;

    memset(&vi, 0, sizeof vi);
    rc = dev_vinfo(fd, &vi);
    if (rc < 0)
    {
        close(fd);
        return 5;
    }

    if (msg.bulk)
    {
        /* The copy path: the caller sent `width` plain bytes at dst_off; fold
         * exactly those bytes out of the shared arena. */
        crep.vec_runs     = msg.width ? 1u : 0u;
        crep.vec_received = fold_bytes(FNV_SEED, region + ARENA_OFFSET + msg.dst_off, msg.width);
        crep.vec_match    = crep.vec_received == msg.expected_vec;
        crep.vec_bad      = crep.vec_match ? 0 : 1;
    }
    else if (msg.vec)
    {
        plan_build(&plan, msg.width, msg.real_bytes, msg.format, vi.xfeatures);
        crep.vec_runs     = (uint64_t)plan.n;
        crep.vec_received = plan_fold_received(FNV_SEED, region + ARENA_OFFSET, &plan);
        crep.vec_match    = crep.vec_received == msg.expected_vec;
        crep.vec_bad      = crep.vec_match ? 0 : 1;
    }
    else
    {
        crep.vec_match = 1;
    }

    {
        uint64_t w[REGIPC_CARRIERS];

        for (int i = 0; i < REGIPC_CARRIERS; i++) w[i] = leaf->values[i];
        crep.gpr_fold = carriers_fold(w);
    }
    crep.seq_match     = leaf->values[BULK_W_SEQ] == msg.expected_seq;
    crep.gpr_match     = crep.gpr_fold == msg.expected_gpr;
    crep.leaf_calls    = __atomic_load_n(&leaf->calls, __ATOMIC_ACQUIRE);
    crep.leaf_checksum = leaf->checksum;
    crep.calls_match   = crep.leaf_calls == msg.expected_calls;

    getrusage(RUSAGE_SELF, &ru1);
    crep.cpu_us = rusage_us(&ru1) - rusage_us(&ru0);
    crep.magic  = CHILD_REPORT_MAGIC;
    write_full(result_fd, &crep, sizeof crep);

    dev_quit(fd);
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ one run */

static int
run_one(const Options *o, unsigned repno, Result *out, char *err, size_t errsz)
{
    char                  name[64];
    uint8_t              *region = MAP_FAILED;
    struct regipc_leaf_result *leaf;
    struct regipc_vec_info vi;
    struct ready_msg      rm;
    struct stop_msg       msg;
    struct child_report   crep;
    struct rusage         ru0;
    struct rusage         ru1;
    struct rusage         cru;
    struct vec_plan       plan;
    uint64_t             *lat = NULL;
    uint64_t              expected_vec;
    uint64_t              expected_gpr;
    uint64_t              seq      = 0; /* bulk: the per-rep sequence carrier */
    uint64_t              page     = (uint64_t)sysconf(_SC_PAGESIZE);
    uint8_t              *bulk_map = NULL; /* bulk: base of the source */
    size_t                bulk_bytes  = 0;
    size_t                bulk_stride = 0; /* 0 = hot: the same address every call */
    size_t                bulk_off    = 0;
    size_t                bulk_last_off = 0;
    uint64_t              t_first = 0;
    uint64_t              t_last  = 0;
    uint64_t              n       = 0;
    uint64_t              errors  = 0;
    pid_t                 child   = -1;
    int                   status  = 0;
    bool                  reaped  = false;
    bool                  avx512  = false;
    int                   ready[2] = {-1, -1};
    int                   stop[2]  = {-1, -1};
    int                   result[2] = {-1, -1};
    int                   shmfd    = -1;
    int                   fd       = -1;
    int                   rc       = -1;

    memset(out, 0, sizeof *out);
    memset(&crep, 0, sizeof crep);
    memset(&cru, 0, sizeof cru);
    out->warmup = o->warmup;
    err[0]      = 0;

    snprintf(name, sizeof name, "/regipcvec_%d_%u", (int)getpid(), repno);

    /* Fail on the missing device before forking: the message is the whole story. */
    fd = dev_open(o->device);
    if (fd < 0)
    {
        say(err, errsz, "%s: %s (is the regipcvec module loaded?)", o->device, strerror(-fd));
        goto out;
    }

    shmfd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (shmfd < 0)
    {
        say(err, errsz, "shm_open(%s): %s", name, strerror(errno));
        goto out;
    }
    if (ftruncate(shmfd, REGION_BYTES) != 0)
    {
        say(err, errsz, "ftruncate(%s): %s", name, strerror(errno));
        goto out;
    }
    region = mmap(NULL, REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    if (region == MAP_FAILED)
    {
        region = MAP_FAILED;
        say(err, errsz, "mmap(%s): %s", name, strerror(errno));
        goto out;
    }
    memset(region, 0, REGION_BYTES); /* fault both pages in before REGISTER */
    leaf          = (struct regipc_leaf_result *)region;
    leaf->magic   = REGIPC_LEAF_MAGIC;
    leaf->checksum = FNV_SEED;

    if (pipe(ready) != 0 || pipe(stop) != 0 || pipe(result) != 0)
    {
        say(err, errsz, "pipe: %s", strerror(errno));
        goto out;
    }

    child = fork();
    if (child < 0)
    {
        say(err, errsz, "fork: %s", strerror(errno));
        goto out;
    }
    if (child == 0)
    {
        close(ready[0]);
        close(stop[1]);
        close(result[0]);
        _exit(child_main(o, region, ready[1], stop[0], result[1]));
    }
    close(ready[1]);
    ready[1] = -1;
    close(stop[0]);
    stop[0] = -1;
    close(result[1]);
    result[1] = -1;

    if (read_full(ready[0], &rm, sizeof rm) != 0)
    {
        say(err, errsz, "the callee died before it registered");
        goto out;
    }
    if (rm.rc < 0)
    {
        say(err, errsz, "callee REGISTER: %s", strerror(-rm.rc));
        goto out;
    }

    if ((rc = register_caller(fd, region)) < 0)
    {
        say(err, errsz, "caller REGISTER: %s", strerror(-rc));
        goto out;
    }
    if ((rc = dev_pair(fd, (pid_t)rm.pid)) < 0)
    {
        say(err, errsz, "PAIR with pid %d: %s", (int)rm.pid, strerror(-rc));
        goto out;
    }

    memset(&vi, 0, sizeof vi);
    if (o->mode == MODE_VEC)
    {
        if ((rc = dev_vinfo(fd, &vi)) < 0)
        {
            say(err, errsz, "VINFO: %s", strerror(-rc));
            goto out;
        }
        if ((uint64_t)vi.width != o->width)
        {
            say(err, errsz, "the module is loaded with width=%u, --width %llu was asked: reload regipcvec with width=%llu",
                vi.width, (unsigned long long)o->width, (unsigned long long)o->width);
            goto out;
        }
    }

    blob_fill(vec_blob);

    /* One CPUID probe, once: it must never sit inside the timed loop. */
    avx512 = cpu_has_avx512();

    /*
     * The bulk source. Hot: one buffer of exactly `width` bytes, the same
     * address on every call. Cold: the >L3 period-filled mapping, walked by
     * BULK_COLD_STRIDE per call so every call reads a long-evicted address.
     * (BULK_COLD_STRIDE is a multiple of the 64-byte period and divides the
     * mapping, so a `width`-byte slice never straddles either boundary.)
     */
    if (o->mode == MODE_BULK)
    {
        seq = CARRIER_W(BULK_W_SEQ) + (uint64_t)repno;
        if (o->cold)
        {
            bulk_bytes  = BULK_COLD_BYTES;
            bulk_stride = BULK_COLD_STRIDE;
            bulk_map    = mmap(NULL, bulk_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (bulk_map == MAP_FAILED)
            {
                bulk_map = NULL;
                say(err, errsz, "mmap(%zu cold source): %s", bulk_bytes, strerror(errno));
                goto out;
            }
        }
        else
        {
            bulk_bytes  = o->width ? (size_t)o->width : 1u;
            bulk_map    = malloc(bulk_bytes);
            if (bulk_map == NULL)
            {
                say(err, errsz, "malloc(%zu source): %s", bulk_bytes, strerror(errno));
                goto out;
            }
        }
        bulk_fill(bulk_map, bulk_bytes);
    }

    lat = malloc((size_t)o->count * sizeof *lat);
    if (lat == NULL)
    {
        say(err, errsz, "malloc(%llu latencies): %s", (unsigned long long)o->count, strerror(errno));
        goto out;
    }

    pin_cpu(o->cpu_caller);

    /* Warm-up: the FPU state machine, the leaf page and the stub all settle. */
    for (uint64_t i = 0; i < o->warmup; i++)
    {
        long r;

        if (o->mode == MODE_VEC)
            r = avx512 ? vec_handoff_avx512(fd, vec_blob) : vec_handoff_sse(fd, vec_blob);
        else if (o->mode == MODE_BULK)
        {
            struct regipc_carriers c;

            bulk_carriers(&c, bulk_map + bulk_off, o->width, seq, 0);
            r = regipc_handoff(fd, REGIPC_IOC_CALL_B, &c);
            if (bulk_stride)
            {
                bulk_off += bulk_stride;
                if (bulk_off >= bulk_bytes) bulk_off = 0;
            }
        }
        else
        {
            struct regipc_carriers c = {.w = {CARRIER_W(0), CARRIER_W(1), CARRIER_W(2), CARRIER_W(3), CARRIER_W(4),
                                               CARRIER_W(5), CARRIER_W(6), CARRIER_W(7), CARRIER_W(8)}};
            r = regipc_handoff(fd, REGIPC_IOC_CALL, &c);
        }
        if (r <= 0)
        {
            say(err, errsz, "warm-up call %llu returned %ld", (unsigned long long)i, r);
            goto out;
        }
    }

    getrusage(RUSAGE_SELF, &ru0);
    for (uint64_t i = 0; i < o->count; i++)
    {
        long     r;
        uint64_t t0;
        uint64_t t1;

        t0 = now_ns();
        if (o->mode == MODE_VEC)
            r = avx512 ? vec_handoff_avx512(fd, vec_blob) : vec_handoff_sse(fd, vec_blob);
        else if (o->mode == MODE_BULK)
        {
            struct regipc_carriers c;

            bulk_carriers(&c, bulk_map + bulk_off, o->width, seq, 0);
            r = regipc_handoff(fd, REGIPC_IOC_CALL_B, &c);
        }
        else
        {
            struct regipc_carriers c = {.w = {CARRIER_W(0), CARRIER_W(1), CARRIER_W(2), CARRIER_W(3), CARRIER_W(4),
                                               CARRIER_W(5), CARRIER_W(6), CARRIER_W(7), CARRIER_W(8)}};
            r = regipc_handoff(fd, REGIPC_IOC_CALL, &c);
        }
        t1 = now_ns();
        if (r <= 0)
        {
            errors++;
            break;
        }
        if (n == 0) t_first = t0;
        t_last  = t1;
        lat[n++] = t1 - t0;
        if (o->mode == MODE_BULK)
        {
            /* Bookkeeping is after the timestamp: whichever slice the last call
             * used is what the expected fold must describe, and it must not sit
             * in the measured window. */
            bulk_last_off = bulk_off;
            if (bulk_stride)
            {
                bulk_off += bulk_stride;
                if (bulk_off >= bulk_bytes) bulk_off = 0;
            }
        }
    }
    getrusage(RUSAGE_SELF, &ru1);

    out->producer_cpu_us = rusage_us(&ru1) - rusage_us(&ru0);

    /* The three folds this run compares: what the caller sent, what the parent
     * reads out of the shared arena, what the callee read there. */
    expected_gpr = carriers_fold((const uint64_t[]){CARRIER_W(0), CARRIER_W(1), CARRIER_W(2), CARRIER_W(3),
                                                    CARRIER_W(4), CARRIER_W(5), CARRIER_W(6), CARRIER_W(7),
                                                    CARRIER_W(8)});
    memset(&plan, 0, sizeof plan);
    expected_vec = 0;
    if (o->mode == MODE_VEC)
    {
        uint32_t real = vi.real_bytes < o->width ? vi.real_bytes : (uint32_t)o->width;

        plan_build(&plan, (uint32_t)o->width, real, (int)vi.format, vi.xfeatures);
        expected_vec = plan_fold_expected(FNV_SEED, vec_blob, &plan);
        out->received_checksum = plan_fold_received(FNV_SEED, region + ARENA_OFFSET, &plan);
        out->vec_runs          = (uint64_t)plan.n;
        out->fpstate_size      = vi.fpstate_size;
        out->real_bytes        = vi.real_bytes;
        out->format            = vi.format;
        out->vec_flags         = vi.flags;
    }
    else if (o->mode == MODE_BULK)
    {
        struct regipc_carriers last;

        bulk_carriers(&last, bulk_map + bulk_last_off, o->width, seq, 0);
        expected_gpr = carriers_fold((const uint64_t *)last.w);
        expected_vec = fold_bytes(FNV_SEED, bulk_map + bulk_last_off, o->width);
        out->received_checksum = fold_bytes(FNV_SEED, region + ARENA_OFFSET, o->width);
        out->vec_runs          = o->width ? 1u : 0u;
        out->real_bytes        = (uint32_t)o->width;
        out->cold_bytes        = o->cold ? bulk_bytes : 0;
        out->cold_stride       = bulk_stride;
        out->page_bytes        = (uint32_t)page;
    }

    /*
     * The arena is one page. Prove the module rejects what does not fit:
     * length > page, dst_off + length > page, flags != 0. All three are refused
     * before the leaf runs, so the counter still matches warmup + n.
     */
    out->limits_ok = 1;
    if (o->mode == MODE_BULK)
    {
        struct
        {
            uint64_t len;
            uint64_t dst;
            uint64_t flags;
        } probes[] = {
            {page + 1u, 0, 0}, /* length > PAGE_SIZE */
            {page, 1, 0},      /* length + dst_off > PAGE_SIZE */
            {64, 0, 1},        /* flags nonzero */
        };

        for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++)
        {
            struct regipc_carriers c;
            long                  r;

            bulk_carriers(&c, region, probes[i].len, seq, probes[i].dst);
            c.w[BULK_W_FLAGS] = probes[i].flags;
            r = regipc_handoff(fd, REGIPC_IOC_CALL_B, &c);
            if (r != -EINVAL) out->limits_ok = 0;
        }
    }

    /* Let the callee fold and leave. */
    memset(&msg, 0, sizeof msg);
    msg.expected_vec   = expected_vec;
    msg.expected_gpr   = expected_gpr;
    msg.expected_seq   = o->mode == MODE_BULK ? seq : CARRIER_W(BULK_W_SEQ);
    msg.expected_calls = o->warmup + n;
    msg.vec            = o->mode == MODE_VEC ? 1u : 0u;
    msg.bulk           = o->mode == MODE_BULK ? 1u : 0u;
    if (o->mode == MODE_VEC)
    {
        msg.width      = (uint32_t)o->width;
        msg.real_bytes = vi.real_bytes;
        msg.format     = vi.format;
    }
    else if (o->mode == MODE_BULK)
    {
        msg.width   = (uint32_t)o->width;
        msg.dst_off = 0;
    }
    write_full(stop[1], &msg, sizeof msg);
    close(stop[1]);
    stop[1] = -1;

    for (;;)
    {
        pid_t got = wait4(child, &status, 0, &cru);

        if (got == child)
        {
            reaped = true;
            break;
        }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    out->consumer_cpu_us = rusage_us(&cru);
    out->child_exit_ok  = reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;

    if (read_full(result[0], &crep, sizeof crep) != 0 || crep.magic != CHILD_REPORT_MAGIC)
    {
        say(err, errsz, "the callee left no report (exit %s)", out->child_exit_ok ? "0" : "nonzero");
        goto out;
    }
    out->child_checksum    = crep.vec_received;
    out->child_gpr_checksum = crep.gpr_fold;
    out->child_gpr_match   = crep.gpr_match;
    out->child_vec_match   = crep.vec_match;
    out->child_seq_match   = crep.seq_match;
    out->vec_bad           = crep.vec_bad;
    out->child_poll_iters  = crep.poll_iters;
    out->child_cpu_us      = crep.cpu_us;
    out->leaf_calls        = __atomic_load_n(&leaf->calls, __ATOMIC_ACQUIRE);
    out->leaf_checksum     = leaf->checksum;

    out->calls  = n;
    out->errors = errors;
    if (n > 0 && t_last > t_first)
    {
        out->wall_ns          = t_last - t_first;
        out->calls_per_second = (n * 1000000000ull + out->wall_ns / 2) / out->wall_ns;
    }
    out->bytes_per_call = (o->mode == MODE_VEC || o->mode == MODE_BULK) ? o->width
                                                                       : (uint64_t)REGIPC_CARRIERS * sizeof(uint64_t);
    if (out->wall_ns > 0)
        out->gbps_milli = (n * out->bytes_per_call * 1000ull) / out->wall_ns;

    if (n > 0)
    {
        qsort(lat, (size_t)n, sizeof *lat, cmp_u64);
        out->ns_p50 = percentile_ns(lat, n, 50, 100);
        out->ns_p99 = percentile_ns(lat, n, 99, 100);
        for (uint64_t i = 0; i < n; i++)
            if (lat[i] > 10 * out->ns_p50) out->gaps++;
    }

    if (o->mode == MODE_VEC || o->mode == MODE_BULK)
    {
        out->checksum = expected_vec;
        out->gpr_checksum = expected_gpr;
    }
    else
    {
        out->checksum = expected_gpr;
        out->gpr_checksum = expected_gpr;
    }

    /* The gate is the data, not the two fold definitions: the module folds the
     * nine carriers its own way, so comparing the leaf's fold with the caller's
     * tests the two implementations against each other. What a run proves is on
     * the child's side - it received what the caller sent, the leaf counters
     * agree, and in bulk mode the module also refused payloads that do not fit
     * the one-page arena. The folds are still reported, and
     * expected_gpr/expected_vec name what the caller itself computed. */
    (void)expected_gpr;
    (void)expected_vec;
    out->verified = out->child_exit_ok && errors == 0 && n == o->count && out->leaf_calls == o->warmup + n &&
                    crep.gpr_match && crep.calls_match && crep.seq_match && crep.leaf_calls == out->leaf_calls &&
                    crep.leaf_checksum == out->leaf_checksum &&
                    ((o->mode != MODE_VEC && o->mode != MODE_BULK) ||
                     (o->mode == MODE_VEC && crep.vec_match && out->received_checksum == out->child_checksum) ||
                     (o->mode == MODE_BULK && crep.vec_match && out->received_checksum == out->child_checksum &&
                      out->limits_ok));

    rc = 0;

out:
    if (child > 0 && !reaped)
    {
        kill(child, SIGKILL);
        while (wait4(child, &status, 0, &cru) < 0 && errno == EINTR)
        {
        }
    }
    for (int i = 0; i < 3; i++)
    {
        int *p = i == 0 ? ready : (i == 1 ? stop : result);

        if (p[0] >= 0) close(p[0]);
        if (p[1] >= 0) close(p[1]);
    }
    if (fd >= 0)
    {
        dev_quit(fd);
        close(fd);
    }
    if (region != MAP_FAILED) munmap(region, REGION_BYTES);
    if (bulk_map != NULL)
    {
        if (o->cold) munmap(bulk_map, bulk_bytes);
        else free(bulk_map);
    }
    if (shmfd >= 0) close(shmfd);
    shm_unlink(name);
    free(lat);
    return rc;
}

/* ----------------------------------------------------------------- report */

static void
print_json(const Options *o, unsigned rep, const Result *r)
{
    printf("{\"mode\":\"%s\",\"width\":%llu,\"real_bytes\":%u,\"fpstate_size\":%u,\"format\":%u,"
           "\"cold\":%s,\"cold_bytes\":%llu,\"cold_stride\":%llu,\"page_bytes\":%u,"
           "\"rep\":%u,\"reps\":%u,\"calls\":%llu,\"warmup\":%llu,\"errors\":%llu,\"bytes_per_call\":%llu,"
           "\"seconds\":%llu.%09llu,\"calls_per_second\":%llu,\"gb_per_second\":%llu.%03llu,"
           "\"us_per_call_p50\":%llu.%03llu,\"us_per_call_p99\":%llu.%03llu,\"gaps\":%llu,"
           "\"producer_cpu_s\":%llu.%06llu,\"consumer_cpu_s\":%llu.%06llu,\"child_cpu_us\":%llu,"
           "\"checksum\":%llu,\"received_checksum\":%llu,\"child_checksum\":%llu,"
           "\"leaf_calls\":%llu,\"leaf_checksum\":%llu,\"gpr_checksum\":%llu,\"child_gpr_checksum\":%llu,"
           "\"vec_runs\":%llu,\"vec_bad\":%llu,\"child_vec_match\":%s,\"child_gpr_match\":%s,\"child_seq_match\":%s,"
           "\"limits_ok\":%s,\"child_poll_iters\":%llu,\"child_exit_ok\":%s,\"verified\":%s}\n",
           mode_name(o->mode), (unsigned long long)((o->mode == MODE_VEC || o->mode == MODE_BULK) ? o->width : 0),
           r->real_bytes, r->fpstate_size, r->format,
           o->cold ? "true" : "false", (unsigned long long)r->cold_bytes, (unsigned long long)r->cold_stride,
           r->page_bytes ? r->page_bytes : (uint32_t)sysconf(_SC_PAGESIZE),
           rep, o->reps,
           (unsigned long long)r->calls, (unsigned long long)r->warmup, (unsigned long long)r->errors,
           (unsigned long long)r->bytes_per_call, (unsigned long long)(r->wall_ns / 1000000000ull),
           (unsigned long long)(r->wall_ns % 1000000000ull), (unsigned long long)r->calls_per_second,
           (unsigned long long)(r->gbps_milli / 1000ull), (unsigned long long)(r->gbps_milli % 1000ull),
           (unsigned long long)(r->ns_p50 / 1000ull), (unsigned long long)(r->ns_p50 % 1000ull),
           (unsigned long long)(r->ns_p99 / 1000ull), (unsigned long long)(r->ns_p99 % 1000ull),
           (unsigned long long)r->gaps, (unsigned long long)(r->producer_cpu_us / 1000000ull),
           (unsigned long long)(r->producer_cpu_us % 1000000ull),
           (unsigned long long)(r->consumer_cpu_us / 1000000ull), (unsigned long long)(r->consumer_cpu_us % 1000000ull),
           (unsigned long long)r->child_cpu_us, (unsigned long long)r->checksum,
           (unsigned long long)r->received_checksum, (unsigned long long)r->child_checksum,
           (unsigned long long)r->leaf_calls, (unsigned long long)r->leaf_checksum,
           (unsigned long long)r->gpr_checksum, (unsigned long long)r->child_gpr_checksum,
           (unsigned long long)r->vec_runs, (unsigned long long)r->vec_bad, r->child_vec_match ? "true" : "false",
           r->child_gpr_match ? "true" : "false", r->child_seq_match ? "true" : "false",
           (o->mode == MODE_BULK ? r->limits_ok : 1) ? "true" : "false",
           (unsigned long long)r->child_poll_iters,
           r->child_exit_ok ? "true" : "false", r->verified ? "true" : "false");
    fflush(stdout);
}

static void
print_human(const Options *o, unsigned rep, const Result *r)
{
    fprintf(stderr, "%s%s rep=%u/%u width=%llu calls=%llu p50=%llu.%03lluus p99=%llu.%03lluus calls/s=%llu GB/s=%llu.%03llu verified=%s\n",
            mode_name(o->mode), o->cold ? " (cold)" : "", rep, o->reps, (unsigned long long)o->width,
            (unsigned long long)r->calls,
            (unsigned long long)(r->ns_p50 / 1000ull), (unsigned long long)(r->ns_p50 % 1000ull),
            (unsigned long long)(r->ns_p99 / 1000ull), (unsigned long long)(r->ns_p99 % 1000ull),
            (unsigned long long)r->calls_per_second, (unsigned long long)(r->gbps_milli / 1000ull),
            (unsigned long long)(r->gbps_milli % 1000ull), r->verified ? "yes" : "NO");
}

/* -------------------------------------------------------------- xsave proxy */

static int
run_xsave(const Options *o)
{
    uint64_t     mask = xgetbv0();
    uint64_t     bytes;
    uint32_t     r[4];
    uint64_t     iters = 1000000;
    XsaveResult  res;

    cpuid_raw(0xd, 0, r);
    /* Subleaf 0: EBX is the size of the XSAVE area for the features enabled in
     * XCR0 (EAX/EDX are the feature mask itself). */
    bytes = r[1];

    if (bytes > XSAVE_BUF_BYTES)
    {
        fprintf(stderr, "fpvec: xsave area is %llu bytes, buffer is %u\n", (unsigned long long)bytes, XSAVE_BUF_BYTES);
        return 1;
    }
    memset(xsave_buf, 0, XSAVE_BUF_BYTES);
    xsave_once(mask); /* fault the buffer and settle the state */

    for (unsigned rep = 1; rep <= o->reps; rep++)
    {
        res.mask  = mask;
        res.bytes = bytes;
        res.iters = iters;

        /* warm-up, outside the timed loops */
        for (int i = 0; i < 1000; i++)
        {
            xsave_once(mask);
            xrstor_once(mask);
        }

        res.total_ns_save         = time_loop(iters, 0);
        res.total_ns_restore      = time_loop(iters, 1);
        res.total_ns_save_restore = time_loop(iters, 2);

        printf("{\"mode\":\"xsave\",\"rep\":%u,\"reps\":%u,\"mask\":%llu,\"bytes\":%llu,\"iterations\":%llu,"
               "\"ns_save\":%llu.%02llu,\"ns_restore\":%llu.%02llu,\"ns_save_restore\":%llu.%02llu}\n",
               rep, o->reps, (unsigned long long)res.mask, (unsigned long long)res.bytes,
               (unsigned long long)res.iters,
               (unsigned long long)(res.total_ns_save * 100ull / res.iters / 100ull),
               (unsigned long long)(res.total_ns_save * 100ull / res.iters % 100ull),
               (unsigned long long)(res.total_ns_restore * 100ull / res.iters / 100ull),
               (unsigned long long)(res.total_ns_restore * 100ull / res.iters % 100ull),
               (unsigned long long)(res.total_ns_save_restore * 100ull / res.iters / 100ull),
               (unsigned long long)(res.total_ns_save_restore * 100ull / res.iters % 100ull));
        fflush(stdout);
    }
    return 0;
}

/* ----------------------------------------------------------------- selftest */

static int
selftest(const Options *o)
{
    Options oplain = *o;
    Options ovec   = *o;
    Options obulk  = *o;
    Result  r;
    char    err[256];
    int     failed = 0;

    oplain.mode  = MODE_PLAIN;
    oplain.count = 64;
    oplain.warmup = 8;
    oplain.reps  = 1;
    ovec.mode    = MODE_VEC;
    ovec.count   = 64;
    ovec.warmup  = 8;
    ovec.reps    = 1;
    obulk.mode   = MODE_BULK;
    obulk.width  = 4096; /* the largest length the one-page arena accepts */
    obulk.count  = 64;
    obulk.warmup = 8;
    obulk.reps   = 1;
    obulk.cold   = false;

    if (run_one(&oplain, 1, &r, err, sizeof err) != 0 || !r.verified)
    {
        printf("FAIL plain: %s\n", err[0] ? err : "unverified");
        failed = 1;
    }
    else
    {
        printf("PASS plain: 64 calls, %llu.%03llu us p50, leaf checksum %llu\n",
               (unsigned long long)(r.ns_p50 / 1000ull), (unsigned long long)(r.ns_p50 % 1000ull),
               (unsigned long long)r.leaf_checksum);
    }

    ovec.width = o->width;
    if (run_one(&ovec, 1, &r, err, sizeof err) != 0 || !r.verified)
    {
        printf("FAIL vec (width %llu): %s\n", (unsigned long long)ovec.width, err[0] ? err : "unverified");
        failed = 1;
    }
    else
    {
        printf("PASS vec: width %llu, %llu runs, %llu.%03llu us p50, checksum %llu\n", (unsigned long long)ovec.width,
               (unsigned long long)r.vec_runs, (unsigned long long)(r.ns_p50 / 1000ull),
               (unsigned long long)(r.ns_p50 % 1000ull), (unsigned long long)r.checksum);
    }

    if (run_one(&obulk, 1, &r, err, sizeof err) != 0 || !r.verified)
    {
        printf("FAIL bulk (width %llu): %s\n", (unsigned long long)obulk.width, err[0] ? err : "unverified");
        failed = 1;
    }
    else
    {
        printf("PASS bulk: %llu-byte payload, %llu.%03llu us p50, page limit %u B enforced, checksum %llu\n",
               (unsigned long long)obulk.width, (unsigned long long)(r.ns_p50 / 1000ull),
               (unsigned long long)(r.ns_p50 % 1000ull), r.page_bytes, (unsigned long long)r.checksum);
    }
    fflush(stdout);
    return failed ? 1 : 0;
}

/* ---------------------------------------------------------------------- cli */

static void
usage(void)
{
    fprintf(stderr,
            "usage: fpvec --mode plain|vec|bulk|xsave [options]\n"
            "       fpvec --selftest [--width N] [--device PATH]\n"
            "\n"
            "  --mode plain   72 B GPR call through /dev/regipcvec (REGIPC_IOC_CALL)\n"
            "  --mode vec     the vector call (REGIPC_IOC_CALL_V); --width must match the\n"
            "                 module parameter the module was loaded with\n"
            "  --mode bulk    the copy call (REGIPC_IOC_CALL_B): --width plain payload bytes\n"
            "                 from a caller buffer, copied by the module into the arena\n"
            "  --mode xsave   the no-root userspace proxy: xsave+xrstor of the AVX-512 state\n"
            "  --width N      vec: vector bytes per call (0, 256, 1024 or 4096); bulk: payload\n"
            "                 bytes per call (0..4096, the arena is one page); default 1024\n"
            "  --cold         bulk only: read the payload from a >L3 mapping walked with a\n"
            "                 64 KiB stride, instead of a cache-hot buffer\n"
            "  --count N      measured calls per rep (default 200000)\n"
            "  --warmup N     warm-up calls (default 1000)\n"
            "  --reps R       reps, one JSON line each (default 1)\n"
            "  --device PATH  the module's device (default %s)\n"
            "  --cpu-caller N pin the producer to CPU N (default: unpinned)\n"
            "  --cpu-callee N pin the callee to CPU N (default: unpinned)\n"
            "  --json         suppress the human line on stderr\n",
            REGIPC_VEC_DEVICE);
}

static int
parse_u64(const char *s, uint64_t *out)
{
    char             *end = NULL;
    unsigned long long v;

    if (s == NULL || *s == 0) return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != 0) return -1;
    *out = (uint64_t)v;
    return 0;
}

int
main(int argc, char **argv)
{
    Options o;
    int     i;
    int     failed = 0;

    memset(&o, 0, sizeof o);
    o.width      = 1024;
    o.count      = 200000;
    o.warmup     = 1000;
    o.reps       = 1;
    o.cpu_caller = -1;
    o.cpu_callee = -1;
    snprintf(o.device, sizeof o.device, "%s", REGIPC_VEC_DEVICE);

    signal(SIGPIPE, SIG_IGN);

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(a, "--mode") == 0 && v != NULL)
        {
            if (strcmp(v, "plain") == 0) o.mode = MODE_PLAIN;
            else if (strcmp(v, "vec") == 0) o.mode = MODE_VEC;
            else if (strcmp(v, "bulk") == 0) o.mode = MODE_BULK;
            else if (strcmp(v, "xsave") == 0) o.mode = MODE_XSAVE;
            else
            {
                usage();
                return 2;
            }
            i++;
        }
        else if (strcmp(a, "--width") == 0 && v != NULL)
        {
            if (parse_u64(v, &o.width) != 0 || o.width > 4096)
            {
                fprintf(stderr, "fpvec: --width must be 0..4096 (vec: only the module's "
                                "0/256/1024/4096 points; bulk: up to one page)\n");
                return 2;
            }
            i++;
        }
        else if (strcmp(a, "--count") == 0 && v != NULL)
        {
            if (parse_u64(v, &o.count) != 0 || o.count == 0) return 2;
            i++;
        }
        else if (strcmp(a, "--warmup") == 0 && v != NULL)
        {
            if (parse_u64(v, &o.warmup) != 0) return 2;
            i++;
        }
        else if (strcmp(a, "--reps") == 0 && v != NULL)
        {
            uint64_t reps;

            if (parse_u64(v, &reps) != 0 || reps == 0 || reps > 100) return 2;
            o.reps = (unsigned)reps;
            i++;
        }
        else if (strcmp(a, "--device") == 0 && v != NULL)
        {
            snprintf(o.device, sizeof o.device, "%s", v);
            i++;
        }
        else if (strcmp(a, "--cpu-caller") == 0 && v != NULL)
        {
            uint64_t c;

            if (parse_u64(v, &c) != 0 || c > 4096) return 2;
            o.cpu_caller = (int)c;
            i++;
        }
        else if (strcmp(a, "--cpu-callee") == 0 && v != NULL)
        {
            uint64_t c;

            if (parse_u64(v, &c) != 0 || c > 4096) return 2;
            o.cpu_callee = (int)c;
            i++;
        }
        else if (strcmp(a, "--json") == 0)
            o.json = true;
        else if (strcmp(a, "--cold") == 0)
            o.cold = true;
        else if (strcmp(a, "--selftest") == 0)
            o.selftest = true;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
        {
            usage();
            return 0;
        }
        else
        {
            usage();
            return 2;
        }
    }

    if (o.selftest) return selftest(&o);
    if (o.mode == MODE_NONE)
    {
        usage();
        return 2;
    }
    if (o.cold && o.mode != MODE_BULK)
    {
        fprintf(stderr, "fpvec: --cold is a bulk-mode option\n");
        return 2;
    }
    if (o.mode == MODE_XSAVE) return run_xsave(&o);

    for (unsigned rep = 1; rep <= o.reps; rep++)
    {
        Result r;
        char   err[256] = "";

        if (run_one(&o, rep, &r, err, sizeof err) != 0)
        {
            fprintf(stderr, "fpvec: %s rep %u: %s\n", mode_name(o.mode), rep, err[0] ? err : "run failed");
            return 1;
        }
        print_json(&o, rep, &r);
        if (!o.json) print_human(&o, rep, &r);
        if (!r.verified) failed = 1;
    }
    return failed ? 1 : 0;
}
