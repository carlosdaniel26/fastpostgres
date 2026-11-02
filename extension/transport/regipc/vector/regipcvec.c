/*
 * regipcvec - the ABI-v3 feasibility variant of regipc: a call that carries the
 * caller's vector state, measured before it is proposed.
 *
 * WHAT IT IS. The ABI 2 call reads x0..x8 out of the caller's syscall frame and
 * runs the callee's work in ring 0 against the callee's pinned page; the callee
 * process never runs. This module keeps that contract (same ioctl numbers, same
 * leaf magic/counter discipline, same single-caller assumption) and adds one
 * command, REGIPC_IOC_CALL_V: the same bookkeeping, and then the caller's live
 * vector state is saved into its own task's fpstate by kernel_fpu_begin() and
 * `width` bytes of that image are copied into the callee's pinned buffer.
 *
 * WHY kernel_fpu_begin() WORKS FOR THIS. On entry from userspace the FPU state
 * is usually live in the registers, not in memory. kernel_fpu_begin_mask()
 * (arch/x86/kernel/fpu/core.c, verified in this kernel's vmlinux) saves the live
 * state with save_fpregs_to_fpstate() before anything can clobber it when
 * TIF_NEED_FPU_LOAD is clear, and sets TIF_NEED_FPU_LOAD so the syscall exit
 * restores it. So immediately after kernel_fpu_begin(),
 * x86_task_fpu(current)->fpstate->regs.xsave is the caller's userspace image.
 *
 * WHERE THE IMAGE LIVES. In this kernel the FPU state is NOT in
 * task_struct.thread: it is allocated directly after the task_struct and
 * accessed through x86_task_fpu(task) (arch/x86/include/asm/processor.h; the
 * disassembly of switch_fpu_return() uses task+0x1840, sizeof task_struct).
 * The image is fps->regs.xsave (XSAVE/XSAVES) or fps->regs.fxsave (FXSAVE-only
 * parts), and fps->size is how many bytes of it are safe to read.
 *
 * THE BUFFER LAYOUT, AND WHY TWO PAGES. The vector payload cannot share a page
 * with the leaf struct: run_leaf() writes the struct on every call, so a copy
 * into the same bytes would destroy magic/calls/values and the next call would
 * fail its magic check. The callee therefore registers TWO contiguous,
 * page-aligned pages (data_len == 2*PAGE_SIZE): page 0 is the leaf struct,
 * byte for byte the ABI 2 struct; page 1 is the vector arena, exactly one page,
 * so the payload is up to 4096 B. The copy never touches the leaf struct and a
 * callee can keep polling leaf.calls throughout.
 *
 * WIDTH. The `width` module parameter (0, 256, 1024 or 4096) is the number of
 * bytes handed to the callee per CALL_V. 0 runs the FP save/restore and copies
 * nothing: that is the point that isolates the FP cost. The module only ever
 * READS fpstate bytes, up to min(width, fpstate->size); when width exceeds the
 * caller's real image (AVX-512 XSAVES images are ~2.5 KB, not 4 KB) the rest of
 * the arena is zero-filled, so the callee always receives exactly width bytes.
 * Without AVX-512 state the payload falls back to 16 XMM = 256 B of image.
 *
 * Two modules, one contract: /dev/regipc (ABI 2, already loaded) and
 * /dev/regipcvec (this one) share ioctls 1..5 unchanged, so the existing
 * userspace wrappers compile and run against either.
 */
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <asm/fpu/api.h>
#include <asm/fpu/types.h>
#include <asm/processor.h>

#include "../../../include/fastpostgres/regipc.h"

#define REGIPC_DRV_NAME		"regipcvec"
#define REGIPC_MAX_CONTEXTS	16

/* The new command: REGIPC_IOC_CALL's cell, plus the vector transfer. */
#define REGIPC_IOC_CALL_V	_IO(REGIPC_IOC_MAGIC, 6)

/*
 * Extension-only introspection: what this module would move, and in which
 * format, so the measurement program never has to guess.
 */
#define REGIPC_IOC_VINFO	_IOR(REGIPC_IOC_MAGIC, 7, struct regipc_vec_info)

/* The callee's registration: page 0 leaf struct, page 1 vector arena. */
#define REGIPC_VEC_PAGES	2
#define REGIPC_VEC_BYTES	(REGIPC_VEC_PAGES * PAGE_SIZE)

enum regipc_vec_format {
	REGIPC_VEC_FORMAT_NONE		= 0,
	REGIPC_VEC_FORMAT_FXSAVE	= 1,
	REGIPC_VEC_FORMAT_XSAVE		= 2,
	REGIPC_VEC_FORMAT_XSAVES	= 3,
};

#define REGIPC_VEC_F_AVX512	(1u << 0)	/* opmask/ZMM state is present */
#define REGIPC_VEC_F_PADDED	(1u << 1)	/* width > the real image; zero fill */

struct regipc_vec_info {
	__u32 abi_version;	/* 2: ioctls 1..5 are the unchanged contract */
	__u32 width;		/* the module parameter */
	__u32 fpstate_size;	/* bytes of the saved image that may be read */
	__u32 real_bytes;	/* min(width, available); rest is zero filled */
	__u64 xfeatures;	/* fps->xfeatures of the caller */
	__u32 format;		/* enum regipc_vec_format */
	__u32 flags;		/* REGIPC_VEC_F_* */
};

struct regipc_ctx {
	struct task_struct *task;
	struct regipc_ctx *peer;
	pid_t pid_nr;

	/* callee: its leaf page and its vector arena, pinned and kernel-mapped */
	struct page *pages[REGIPC_VEC_PAGES];
	unsigned long offset;
	size_t data_len;

	bool callee;
	bool used;

	u64 calls;
};

static struct regipc_ctx contexts[REGIPC_MAX_CONTEXTS];
static DEFINE_SPINLOCK(registry_lock);

/* 0, 256, 1024 or 4096: validated in regipcvec_init(). */
static uint vec_width = 1024;
module_param_named(width, vec_width, uint, 0444);
MODULE_PARM_DESC(width, "bytes of vector state transferred per CALL_V: 0, 256, 1024 or 4096");

static void regs_to_carriers(const struct pt_regs *regs, struct regipc_carriers *c)
{
#define X(i, field) c->w[i] = regs->field;
	REGIPC_CARRIER_FIELDS(X)
#undef X
}

static struct regipc_ctx *current_ctx(void)
{
	struct regipc_ctx *ctx = NULL;
	int i;

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		if (contexts[i].used && contexts[i].task == current) {
			ctx = &contexts[i];
			break;
		}
	}
	spin_unlock(&registry_lock);
	return ctx;
}

/*
 * A process that dies without QUIT must not pin a slot or pages forever, so
 * dead contexts are reclaimed by the next registration.
 */
static void reap_dead(void)
{
	struct task_struct *tasks[REGIPC_MAX_CONTEXTS];
	struct page *pages[REGIPC_MAX_CONTEXTS][REGIPC_VEC_PAGES];
	int i, n = 0;

	memset(pages, 0, sizeof(pages));

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		struct regipc_ctx *ctx = &contexts[i];

		if (!ctx->used || !ctx->task || pid_alive(ctx->task))
			continue;

		if (ctx->peer)
			ctx->peer->peer = NULL;
		tasks[n] = ctx->task;
		memcpy(pages[n], ctx->pages, sizeof(ctx->pages));
		n++;
		ctx->task = NULL;
		memset(ctx->pages, 0, sizeof(ctx->pages));
		ctx->peer = NULL;
		ctx->used = false;
	}
	spin_unlock(&registry_lock);

	/* both may sleep */
	for (i = 0; i < n; i++) {
		put_task_struct(tasks[i]);
		if (pages[i][0])
			unpin_user_pages(pages[i], REGIPC_VEC_PAGES);
	}
}

/* ---------------------------------------------------------------- the callee */

static u64 leaf_fnv(u64 hash, u32 value)
{
	for (int i = 0; i < 4; i++) {
		hash ^= (u8)(value >> (i * 8));
		hash *= 1099511628211ull;
	}
	return hash;
}

/*
 * The callee's work, executed in ring 0 against its own leaf page. It touches
 * only the pinned page, never sleeps and never calls out, so it cannot fault.
 * x0..x8 land in the struct; the call counter comes back as the return value.
 * This is the ABI 2 leaf, unchanged.
 */
static long run_leaf(void *buf, const struct regipc_carriers *args)
{
	struct regipc_leaf_result *r = buf;

	if (READ_ONCE(r->magic) != REGIPC_LEAF_MAGIC)
		return -EINVAL;

	r->calls++;
	for (int i = 0; i < REGIPC_CARRIERS; i++) {
		r->values[i] = args->w[i];
		r->checksum = leaf_fnv(r->checksum, (u32)args->w[i]);
	}
	return (long)r->calls;
}

/*
 * The vector image of the calling task. Returns the source pointer and how many
 * bytes are safe to read, plus which format the image is in. XSAVES writes the
 * compacted format (xcomp_bv bit 63 set), XSAVE the standard one; on CPUs with
 * only FXSAVE the image is the 512-byte fxsave area. Callers must hold the FPU
 * (kernel_fpu_begin()) so the image is the live userspace state.
 */
static void vec_image(struct fpstate *fps, void **src, size_t *avail, u32 *format)
{
	if (cpu_feature_enabled(X86_FEATURE_XSAVE)) {
		struct xregs_state *xs = &fps->regs.xsave;

		*src = xs;
		*avail = fps->size;
		if (format) {
			if (xs->header.xcomp_bv & XCOMP_BV_COMPACTED_FORMAT)
				*format = REGIPC_VEC_FORMAT_XSAVES;
			else
				*format = REGIPC_VEC_FORMAT_XSAVE;
		}
	} else if (cpu_feature_enabled(X86_FEATURE_FXSR)) {
		*src = &fps->regs.fxsave;
		*avail = sizeof(struct fxregs_state);
		if (format)
			*format = REGIPC_VEC_FORMAT_FXSAVE;
	} else {
		*src = NULL;
		*avail = 0;
		if (format)
			*format = REGIPC_VEC_FORMAT_NONE;
	}
}

#define REGIPC_AVX512_STATE (XFEATURE_MASK_OPMASK | XFEATURE_MASK_ZMM_Hi256 | \
			     XFEATURE_MASK_Hi16_ZMM)

/*
 * How many real bytes a given width can carry on this CPU. Without AVX-512 the
 * contract falls back to the 16 XMM registers (256 B); the rest is zero fill.
 */
static size_t vec_real_bytes(struct fpstate *fps, size_t avail)
{
	size_t cap = 4096;

	if (!(fps->xfeatures & REGIPC_AVX512_STATE))
		cap = 256;
	if (avail > cap)
		avail = cap;
	return avail;
}

/*
 * Hand the caller's vector state to the callee: the FP save/restore runs even
 * at width 0 (that is the point), and the arena always receives exactly
 * `vec_width` bytes: min(width, image) copied, the rest zeroed.
 */
static void vec_transfer(struct regipc_ctx *callee)
{
	struct fpstate *fps = x86_task_fpu(current)->fpstate;
	u8 *dst = page_address(callee->pages[1]);
	void *src = NULL;
	size_t avail = 0, real;

	if (!fps || !callee->pages[1])
		return;

	kernel_fpu_begin();

	vec_image(fps, &src, &avail, NULL);
	real = vec_real_bytes(fps, avail);
	if (real > vec_width)
		real = vec_width;

	if (real && src)
		memcpy(dst, src, real);
	if (vec_width > real)
		memset(dst + real, 0, vec_width - real);

	kernel_fpu_end();
}

/* ------------------------------------------------------------------- the API */

static long do_register(unsigned long arg)
{
	struct regipc_context_desc desc;
	struct regipc_ctx *ctx = NULL;
	struct page *pages[REGIPC_VEC_PAGES] = { NULL, };
	bool callee;
	int i;

	if (copy_from_user(&desc, (void __user *)arg, sizeof(desc)))
		return -EFAULT;

	callee = desc.flags & REGIPC_F_CALLEE;

	/*
	 * A caller registers with nothing but its identity (some ABI 2 builds
	 * insist on a leaf even from the caller, so a caller page is accepted
	 * and ignored). A callee hands over two contiguous pages: the leaf at
	 * offset 0 of the first, the vector arena in the second.
	 */
	if (callee) {
		long pinned;

		if (!desc.data || (desc.data & ~PAGE_MASK) || desc.data_len != REGIPC_VEC_BYTES)
			return -EINVAL;

		/* pin_user_pages() may sleep: outside registry_lock, in our own mm */
		pinned = pin_user_pages(desc.data & PAGE_MASK, REGIPC_VEC_PAGES, FOLL_WRITE, pages);
		if (pinned != REGIPC_VEC_PAGES) {
			if (pinned > 0)
				unpin_user_pages(pages, (unsigned int)pinned);
			return -EFAULT;
		}
	}

	reap_dead();

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		if (contexts[i].used && contexts[i].task == current) {
			spin_unlock(&registry_lock);
			if (pages[0])
				unpin_user_pages(pages, REGIPC_VEC_PAGES);
			return -EALREADY;
		}
	}
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		if (!contexts[i].used) {
			ctx = &contexts[i];
			break;
		}
	}
	if (!ctx) {
		spin_unlock(&registry_lock);
		if (pages[0])
			unpin_user_pages(pages, REGIPC_VEC_PAGES);
		return -ENOSPC;
	}

	get_task_struct(current);
	memset(ctx, 0, sizeof(*ctx));
	ctx->task = current;
	ctx->pid_nr = task_pid_nr(current);
	memcpy(ctx->pages, pages, sizeof(pages));
	ctx->offset = 0;
	ctx->data_len = desc.data_len;
	ctx->callee = callee;
	ctx->used = true;

	spin_unlock(&registry_lock);
	return 0;
}

/* The caller binds itself to the callee's pid. */
static long do_pair(unsigned long arg)
{
	struct regipc_ctx *me = current_ctx();
	struct regipc_ctx *callee = NULL;
	struct task_struct *task;
	struct pid *pidp;
	__s32 pid;
	int i;

	if (!me)
		return -ENODEV;
	if (me->callee)
		return -EINVAL; /* only the caller pairs */
	if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
		return -EFAULT;

	rcu_read_lock();
	pidp = find_vpid(pid);
	task = pidp ? pid_task(pidp, PIDTYPE_PID) : NULL;
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		if (contexts[i].used && contexts[i].task == task && contexts[i].callee) {
			callee = &contexts[i];
			break;
		}
	}
	if (callee && (callee == me || me->peer || callee->peer))
		callee = NULL;
	if (callee) {
		me->peer = callee;
		callee->peer = me;
	}
	spin_unlock(&registry_lock);

	put_task_struct(task);
	return callee ? 0 : -EBUSY;
}

/*
 * The call: x0..x8 are read from the caller's own syscall frame, the callee's
 * work runs in ring 0 against the pinned leaf page, and its value comes back in
 * RAX. CALL_V adds the vector transfer after the bookkeeping; the leaf struct
 * is never touched by it, so the counter keeps its ABI 2 meaning.
 */
static long do_call(bool vector)
{
	struct regipc_carriers args;
	struct regipc_ctx *me = current_ctx();
	struct regipc_ctx *callee;
	long ret;

	if (!me)
		return -ENODEV;

	callee = me->peer;
	if (!callee || !callee->pages[0])
		return -ENOLINK;

	regs_to_carriers(task_pt_regs(current), &args);

	ret = run_leaf(page_address(callee->pages[0]) + callee->offset, &args);
	me->calls++;
	if (ret < 0)
		return ret;

	if (vector)
		vec_transfer(callee);

	return ret;
}

static long do_stats(unsigned long arg)
{
	struct regipc_ctx *me = current_ctx();
	struct regipc_ctx *callee;
	struct regipc_stats st;

	if (!me)
		return -ENODEV;

	memset(&st, 0, sizeof(st));
	st.calls = me->calls;
	st.paired = me->peer ? 1 : 0;
	st.abi_version = REGIPC_ABI_VERSION;

	callee = me->peer;
	if (callee && callee->pages[0]) {
		struct regipc_leaf_result *r =
			page_address(callee->pages[0]) + callee->offset;

		st.leaf_calls = READ_ONCE(r->calls);
		st.leaf_checksum = READ_ONCE(r->checksum);
	}

	if (copy_to_user((void __user *)arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

/*
 * What a CALL_V would do, answered from a real save so the format is the
 * kernel's, not a guess: save the caller's state, read the image header, leave.
 */
static long do_vinfo(unsigned long arg)
{
	struct regipc_vec_info vi;
	struct fpstate *fps = x86_task_fpu(current)->fpstate;
	void *src = NULL;
	size_t avail = 0;

	if (!fps)
		return -ENODEV;

	memset(&vi, 0, sizeof(vi));
	vi.abi_version = REGIPC_ABI_VERSION;
	vi.width = vec_width;

	kernel_fpu_begin();
	vec_image(fps, &src, &avail, &vi.format);
	vi.fpstate_size = avail;
	vi.xfeatures = fps->xfeatures;
	kernel_fpu_end();

	if (fps->xfeatures & REGIPC_AVX512_STATE)
		vi.flags |= REGIPC_VEC_F_AVX512;

	vi.real_bytes = vec_real_bytes(fps, avail);
	if (vi.real_bytes > vec_width)
		vi.real_bytes = vec_width;
	if (vec_width > vi.real_bytes)
		vi.flags |= REGIPC_VEC_F_PADDED;

	if (copy_to_user((void __user *)arg, &vi, sizeof(vi)))
		return -EFAULT;
	return 0;
}

static long do_quit(void)
{
	struct regipc_ctx *me = current_ctx();
	struct task_struct *task;
	struct page *pages[REGIPC_VEC_PAGES];

	if (!me)
		return -ENODEV;

	spin_lock(&registry_lock);
	if (me->peer)
		me->peer->peer = NULL;
	me->peer = NULL;
	task = me->task;
	memcpy(pages, me->pages, sizeof(pages));
	me->task = NULL;
	memset(me->pages, 0, sizeof(me->pages));
	me->used = false;
	spin_unlock(&registry_lock);

	put_task_struct(task);
	if (pages[0])
		unpin_user_pages(pages, REGIPC_VEC_PAGES);
	return 0;
}

static long regipcvec_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case REGIPC_IOC_REGISTER:
		return do_register(arg);
	case REGIPC_IOC_PAIR:
		return do_pair(arg);
	case REGIPC_IOC_CALL:
		return do_call(false);
	case REGIPC_IOC_STATS:
		return do_stats(arg);
	case REGIPC_IOC_QUIT:
		return do_quit();
	case REGIPC_IOC_CALL_V:
		return do_call(true);
	case REGIPC_IOC_VINFO:
		return do_vinfo(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations regipcvec_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = regipcvec_ioctl,
	.compat_ioctl   = regipcvec_ioctl,
	.llseek         = noop_llseek,
};

static uint regipcvec_mode = 0600;
module_param_named(mode, regipcvec_mode, uint, 0444);
MODULE_PARM_DESC(mode, "permissions of /dev/regipcvec, default 0600 (use 0666 for unprivileged callers)");

static struct miscdevice regipcvec_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = REGIPC_DRV_NAME,
	.fops  = &regipcvec_fops,
	.mode  = 0600,
};

static int __init regipcvec_init(void)
{
	int ret;

	if (vec_width != 0 && vec_width != 256 && vec_width != 1024 && vec_width != 4096) {
		pr_err(REGIPC_DRV_NAME ": width=%u is not one of 0, 256, 1024, 4096\n", vec_width);
		return -EINVAL;
	}

	regipcvec_misc.mode = regipcvec_mode;
	ret = misc_register(&regipcvec_misc);
	if (ret)
		return ret;

	pr_info(REGIPC_DRV_NAME ": loaded, abi %u + CALL_V, width %u, leaf page + %lu-byte arena, mode %04o\n",
		REGIPC_ABI_VERSION, vec_width, (unsigned long)PAGE_SIZE * (REGIPC_VEC_PAGES - 1), regipcvec_mode);
	return 0;
}

static void __exit regipcvec_exit(void)
{
	struct task_struct *tasks[REGIPC_MAX_CONTEXTS];
	struct page *pages[REGIPC_MAX_CONTEXTS][REGIPC_VEC_PAGES];
	int i, n = 0;

	memset(pages, 0, sizeof(pages));

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		struct regipc_ctx *ctx = &contexts[i];

		if (!ctx->used)
			continue;
		ctx->used = false;
		tasks[n] = ctx->task;
		memcpy(pages[n], ctx->pages, sizeof(ctx->pages));
		n++;
		ctx->task = NULL;
		memset(ctx->pages, 0, sizeof(ctx->pages));
		ctx->peer = NULL;
	}
	spin_unlock(&registry_lock);

	for (i = 0; i < n; i++) {
		if (tasks[i])
			put_task_struct(tasks[i]);
		if (pages[i][0])
			unpin_user_pages(pages[i], REGIPC_VEC_PAGES);
	}

	misc_deregister(&regipcvec_misc);
	pr_info(REGIPC_DRV_NAME ": unloaded\n");
}

module_init(regipcvec_init);
module_exit(regipcvec_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("register-ipc experiment");
MODULE_DESCRIPTION("regipc ABI v3 probe: a call that carries the caller's vector state (CALL_V)");
