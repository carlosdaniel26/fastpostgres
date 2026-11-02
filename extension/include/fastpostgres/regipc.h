/*
 * regipc.h — the regipc ABI, vendored from the technique's own repository
 * (https://github.com/carlosdaniel26/register-ipc, register-ipc/include/regipc.h,
 * ABI version 2). The ioctls, the carrier register map and the leaf layout are
 * that header's, unchanged; a foreign module would speak the same contract.
 *
 * What differs from the original: this copy implements the userspace helpers as
 * `static inline` below, because fastpostgres has two consumers with two build
 * systems (the extension via PGXS, the client via setuptools) and neither links
 * a shared userspace object. The kernel module itself lives beside this header,
 * in extension/transport/regipc/module/, for reproducibility; the benchmark uses
 * the module the machine already has loaded, which must be ABI version 2.
 *
 * register-ipc - a cross-process call. x86-64 only.
 *
 * The caller puts x0..x8 in general purpose registers and issues one ioctl.
 * The module runs the callee's registered work in ring 0 against the callee's
 * pinned struct and returns a value in RAX. The callee process never runs, so
 * there is no scheduling handoff, no wake and no CR3 switch.
 */
#ifndef FASTPOSTGRES_REGIPC_H
#define FASTPOSTGRES_REGIPC_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define REGIPC_ABI_VERSION 2

/*
 * The nine argument registers, x0..x8, in wire order.
 *
 * RAX carries the syscall number and the return value, RCX and R11 are
 * clobbered by the syscall instruction, RDI and RSI are the ioctl's fd and
 * command, RSP and RBP stay out of the frame. That leaves exactly these nine.
 */
#define REGIPC_CARRIERS 9

struct regipc_carriers {
	__u64 w[REGIPC_CARRIERS];
};

/* Single source of truth for the wire order: index -> pt_regs field. */
#define REGIPC_CARRIER_FIELDS(X) \
	X(0, dx)                 \
	X(1, bx)                 \
	X(2, r8)                 \
	X(3, r9)                 \
	X(4, r10)                \
	X(5, r12)                \
	X(6, r13)                \
	X(7, r14)                \
	X(8, r15)

/*
 * What the module writes into the callee's registered struct, in ring 0.
 * Keep this within one page; the module rejects anything larger.
 */
#define REGIPC_LEAF_MAGIC 0x4c454146u /* "LEAF" */

struct regipc_leaf_result {
	__u32 magic;
	__u32 calls;
	__u64 checksum;                  /* FNV-1a over every value, in order */
	__u64 values[REGIPC_CARRIERS];   /* x0..x8 of the most recent call */
};

struct regipc_context_desc {
	__u64 data;      /* callee: address of its struct in memory */
	__u64 data_len;  /* callee: its size */
	__u32 flags;     /* REGIPC_F_CALLEE */
	__u32 reserved;
};

#define REGIPC_F_CALLEE 1u

struct regipc_stats {
	__u64 calls;          /* caller: calls it has made */
	__u64 leaf_calls;     /* callee's struct: calls the module ran for it */
	__u64 leaf_checksum;  /* callee's struct: checksum over the values handed in */
	__u32 paired;         /* 1 while a peer is bound */
	__u32 abi_version;
};

#define REGIPC_IOC_MAGIC 'c'

#define REGIPC_IOC_REGISTER _IOW(REGIPC_IOC_MAGIC, 1, struct regipc_context_desc)
#define REGIPC_IOC_PAIR     _IOW(REGIPC_IOC_MAGIC, 2, __s32)
#define REGIPC_IOC_CALL     _IO(REGIPC_IOC_MAGIC, 3)
#define REGIPC_IOC_STATS    _IOR(REGIPC_IOC_MAGIC, 4, struct regipc_stats)
#define REGIPC_IOC_QUIT     _IO(REGIPC_IOC_MAGIC, 5)

#ifndef __KERNEL__

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define REGIPC_DEVICE "/dev/regipc"

#if defined(__x86_64__)

/*
 * x0..x8 in the carrier registers, one ioctl, the callee's value back in RAX.
 * This is the original x86_64/switch.S, as inline asm: setuptools does not
 * build .S sources, and the two builds that need it are x86-64 only anyway.
 *
 * RDX carries w[0] and is also the ioctl's argument word, which is why only
 * zero-argument commands (CALL, QUIT) may go through here.
 */
static inline long
regipc_handoff(int fd, unsigned long cmd, const struct regipc_carriers *c)
{
	register long rdx asm("rdx") = (long)c->w[0];
	register long rbx asm("rbx") = (long)c->w[1];
	register long r8 asm("r8") = (long)c->w[2];
	register long r9 asm("r9") = (long)c->w[3];
	register long r10 asm("r10") = (long)c->w[4];
	register long r12 asm("r12") = (long)c->w[5];
	register long r13 asm("r13") = (long)c->w[6];
	register long r14 asm("r14") = (long)c->w[7];
	register long r15 asm("r15") = (long)c->w[8];
	long ret;

	__asm__ volatile("syscall"
	                 : "=a"(ret)
	                 : "a"((long)__NR_ioctl), "D"((long)fd), "S"((long)cmd),
	                   "r"(rdx), "r"(rbx), "r"(r8), "r"(r9), "r"(r10), "r"(r12), "r"(r13), "r"(r14), "r"(r15)
	                 : "rcx", "r11", "memory");
	return ret;
}

#else /* !__x86_64__: the ABI is a register set; there is no portable fallback. */

static inline long
regipc_handoff(int fd, unsigned long cmd, const struct regipc_carriers *c)
{
	(void)fd;
	(void)cmd;
	(void)c;
	errno = ENOSYS;
	return -ENOSYS;
}

#endif

static inline int
regipc_open(void)
{
	int fd = open(REGIPC_DEVICE, O_RDWR | O_CLOEXEC);

	if (fd < 0) return -errno;
	return fd;
}

static inline int
regipc_register(int fd, const struct regipc_context_desc *desc)
{
	if (ioctl(fd, REGIPC_IOC_REGISTER, desc) < 0) return -errno;
	return 0;
}

static inline int
regipc_pair(int fd, __s32 callee_pid)
{
	if (ioctl(fd, REGIPC_IOC_PAIR, &callee_pid) < 0) return -errno;
	return 0;
}

static inline long
regipc_call(int fd, const struct regipc_carriers *args)
{
	return regipc_handoff(fd, REGIPC_IOC_CALL, args);
}

static inline int
regipc_stats(int fd, struct regipc_stats *out)
{
	if (ioctl(fd, REGIPC_IOC_STATS, out) < 0) return -errno;
	return 0;
}

static inline int
regipc_quit(int fd)
{
	if (ioctl(fd, REGIPC_IOC_QUIT) < 0) return -errno;
	return 0;
}

#endif /* !__KERNEL__ */

#endif /* FASTPOSTGRES_REGIPC_H */
