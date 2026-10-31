/* kernels_bench.c — prove each SIMD kernel correct and faster before it is
 * wired into the hot path (ffmpeg's checkasm idea, without the harness).
 *
 * Run with `just kernels`, which needs no container: the kernels are
 * freestanding. It has already earned its keep once — the first draft of the
 * null test used `vptest` against a zero register, which is always zero, and
 * this caught it before it reached a backend process.
 *
 * Correctness first, over random shapes and every interesting size, comparing
 * against the scalar reference; only then timings. Failures are counted and
 * only the first few printed, so a broken kernel does not bury the report.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernels.h"

#define BYTES (64u << 20) /* 64 MiB: long enough for the scan to be memory-bound */
#define MAX_PRINT 5

static int g_fails;
static int g_printed;

static void
fail(const char *what, size_t a, size_t b, int want, int got)
{
    g_fails++;
    if (g_printed++ < MAX_PRINT)
        printf("  FAIL %s (size=%zu pos=%zu) want=%d got=%d\n", what, a, b, want, got);
}

static double
now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Compare one core, or a table entry, against the scalar reference, one set
 * byte at every position of every size. */
static void
check_one(const char *name, fp_nullmap_any_fn fn, const size_t *sizes, size_t nsizes)
{
    uint8_t buf[4096];
    size_t  i, pos;

    for (i = 0; i < nsizes; i++)
    {
        size_t s = sizes[i];

        if (s > sizeof(buf)) continue;
        /* pos == s sets a byte just past the range: a kernel that reads a whole
         * block for a short range must still report "no bits set". */
        for (pos = 0; pos <= s; pos++)
        {
            int want, got;

            memset(buf, 0, sizeof(buf));
            if (pos < sizeof(buf)) buf[pos] = 1;
            want = fp_kernels_scalar.nullmap_any(buf, s);
            got  = fn(buf, s);
            if (want != got) fail(name, s, pos, want, got);
        }
    }
}

/* Every shape that crosses a block boundary, for the tail logic in kernels.c. */
static const size_t SIZES[] = {0, 1, 7, 31, 32, 33, 63, 64, 65, 95, 96, 127, 128, 129, 1000, 4096};
/* Exact multiples, so the assembly cores are driven directly (a host with
 * AVX-512 never calls the AVX2 core through the table). 0 is a multiple of
 * both, and is the case a size ladder falls through. */
static const size_t SIZES_32[] = {0, 32, 64, 96, 128, 4096};
static const size_t SIZES_64[] = {0, 64, 128, 192, 4096};

static void
check_copy_small(void)
{
    uint8_t src[64], dst[64];
    size_t  len;

    /* len == 0 is included on purpose: the ladder must not fall through and
     * write a byte, which is a fault against a page boundary. */
    for (len = 0; len <= 32; len++)
    {
        size_t i;

        for (i = 0; i < sizeof(src); i++)
        {
            src[i] = (uint8_t)i;
            dst[i] = 0xA5;
        }
        fp_kernels->copy_small(dst, src, len);

        for (i = 0; i < len; i++)
            if (dst[i] != src[i]) fail("copy_small byte", len, i, src[i], dst[i]);
        for (i = len; i < 40; i++)
            if (dst[i] != 0xA5) fail("copy_small overrun", len, i, 0xA5, dst[i]);
    }
}

static void
bench_nullmap_any(void)
{
    uint8_t *z = calloc(BYTES, 1);
    double   t0, t_scalar, t_pick;
    int      repeats = 16, i;
    volatile int sink;

    if (!z)
    {
        printf("  out of memory\n");
        return;
    }
    t0 = now_s();
    for (i = 0; i < repeats; i++) sink = fp_kernels_scalar.nullmap_any(z, BYTES);
    t_scalar = now_s() - t0;

    t0 = now_s();
    for (i = 0; i < repeats; i++) sink = fp_kernels->nullmap_any(z, BYTES);
    t_pick = now_s() - t0;

    printf("  nullmap_any (all clear, %u MiB x%d, worst case for the scan)\n", BYTES >> 20, repeats);
    printf("    scalar %.1f GB/s   %-6s %.1f GB/s   %.2fx\n", (double)BYTES * repeats / t_scalar / 1e9,
           fp_kernels->name, (double)BYTES * repeats / t_pick / 1e9, t_scalar / t_pick);
    (void)sink;
    free(z);
}

static void
bench_copy_small(void)
{
    size_t  lens[] = {4, 8, 12, 16, 24, 32};
    uint8_t src[64], dst[64];
    size_t  li, i;
    long    iters = 40 * 1000 * 1000;

    memset(src, 0x5A, sizeof(src));
    for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++)
    {
        size_t len = lens[li];
        double t0, t_scalar, t_pick;
        volatile size_t sink = 0;

        t0 = now_s();
        for (i = 0; i < (size_t)iters; i++) fp_kernels_scalar.copy_small(dst, src, len);
        t_scalar = now_s() - t0;

        t0 = now_s();
        for (i = 0; i < (size_t)iters; i++) fp_kernels->copy_small(dst, src, len);
        t_pick = now_s() - t0;

        printf("  copy_small len=%2zu: scalar %6.0f M/s   %-6s %6.0f M/s   %.2fx\n", len,
               (double)iters / t_scalar / 1e6, fp_kernels->name, (double)iters / t_pick / 1e6,
               t_scalar / t_pick);
        sink = dst[0];
        (void)sink;
    }
}

int
main(void)
{
    fp_kernels_init();
    printf("kernels: %s\n", fp_kernels->name);

    printf("correctness\n");
    check_one("table", fp_kernels->nullmap_any, SIZES, sizeof(SIZES) / sizeof(SIZES[0]));
    check_one("avx2-core", fp_nullmap_any_avx2_core, SIZES_32, sizeof(SIZES_32) / sizeof(SIZES_32[0]));
    check_one("avx512-core", fp_nullmap_any_avx512_core, SIZES_64, sizeof(SIZES_64) / sizeof(SIZES_64[0]));
    check_copy_small();

    if (g_fails)
    {
        printf("  %d mismatches — the kernels disagree with the scalar reference\n", g_fails);
        return 1;
    }
    printf("  ok (every kernel matches the scalar reference)\n");

    printf("timings\n");
    bench_nullmap_any();
    bench_copy_small();
    return 0;
}
