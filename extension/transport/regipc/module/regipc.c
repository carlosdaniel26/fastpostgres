/*
 * regipc - the module. x86-64 only.
 *
 * Two things, and nothing else:
 *
 *   REGISTER  the callee hands over its struct; the page is pinned once and kept
 *             reachable through a kernel alias, so a call never switches CR3
 *   CALL      the caller's x0..x8 are read out of its own syscall frame and the
 *             callee's work runs in ring 0 against that alias
 *
 * No task states, no park, no wake, no scheduler involvement: the callee process
 * is not running during a call, and never needs to be.
 */
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include "fastpostgres/regipc.h"

#define REGIPC_DRV_NAME     "regipc"
#define REGIPC_MAX_CONTEXTS 16

struct regipc_ctx {
	struct task_struct *task;
	struct regipc_ctx *peer;
	pid_t pid_nr;

	/* callee: its struct in memory, pinned and reachable from kernel space */
	struct page *page;
	unsigned long offset;
	size_t data_len;

	bool callee;
	bool used;

	u64 calls;
};

static struct regipc_ctx contexts[REGIPC_MAX_CONTEXTS];
static DEFINE_SPINLOCK(registry_lock);

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
 * A process that dies without QUIT must not pin a slot or a page forever, so
 * dead contexts are reclaimed by the next registration.
 */
static void reap_dead(void)
{
	struct task_struct *tasks[REGIPC_MAX_CONTEXTS];
	struct page *pages[REGIPC_MAX_CONTEXTS];
	int i, n = 0;

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		struct regipc_ctx *ctx = &contexts[i];

		if (!ctx->used || !ctx->task || pid_alive(ctx->task))
			continue;

		if (ctx->peer)
			ctx->peer->peer = NULL;
		tasks[n] = ctx->task;
		pages[n] = ctx->page;
		n++;
		ctx->task = NULL;
		ctx->page = NULL;
		ctx->peer = NULL;
		ctx->used = false;
	}
	spin_unlock(&registry_lock);

	/* both may sleep */
	for (i = 0; i < n; i++) {
		put_task_struct(tasks[i]);
		if (pages[i])
			unpin_user_pages(&pages[i], 1);
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
 * The callee's work, executed in ring 0 against its own struct. It touches only
 * the pinned page, never sleeps and never calls out, so it cannot fault. x0..x8
 * land in the struct; the call counter comes back as the return value.
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

static void unpin(struct page *page)
{
	if (page)
		unpin_user_pages(&page, 1);
}

/* ------------------------------------------------------------------- the API */

static long do_register(unsigned long arg)
{
	struct regipc_context_desc desc;
	struct regipc_ctx *ctx = NULL;
	unsigned long offset = 0;
	bool callee;
	struct page *page = NULL;
	int i;

	if (copy_from_user(&desc, (void __user *)arg, sizeof(desc)))
		return -EFAULT;

	callee = desc.flags & REGIPC_F_CALLEE;

	/*
	 * A caller registers with nothing but its identity: it has no struct to
	 * pin, the module only needs to know it exists so PAIR and CALL can find
	 * it. A callee hands over the struct it wants the work done against.
	 */
	if (callee) {
		offset = desc.data & ~PAGE_MASK;
		if (!desc.data || desc.data_len < sizeof(struct regipc_leaf_result) ||
		    offset + desc.data_len > PAGE_SIZE)
			return -EINVAL;

		/* pin_user_pages() may sleep: outside registry_lock, in our own mm */
		if (pin_user_pages(desc.data & PAGE_MASK, 1, FOLL_WRITE, &page) != 1)
			return -EFAULT;
	}

	reap_dead();

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		if (contexts[i].used && contexts[i].task == current) {
			spin_unlock(&registry_lock);
			unpin(page);
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
		unpin(page);
		return -ENOSPC;
	}

	get_task_struct(current);
	memset(ctx, 0, sizeof(*ctx));
	ctx->task = current;
	ctx->pid_nr = task_pid_nr(current);
	ctx->page = page;
	ctx->offset = offset;
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
 * work runs in ring 0 against the pinned page, and its value comes back in RAX.
 */
static long do_call(void)
{
	struct regipc_carriers args;
	struct regipc_ctx *me = current_ctx();
	struct regipc_ctx *callee;

	if (!me)
		return -ENODEV;

	callee = me->peer;
	if (!callee || !callee->page)
		return -ENOLINK;

	regs_to_carriers(task_pt_regs(current), &args);

	me->calls++;
	return run_leaf(page_address(callee->page) + callee->offset, &args);
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
	if (callee && callee->page) {
		struct regipc_leaf_result *r = page_address(callee->page) + callee->offset;

		st.leaf_calls = READ_ONCE(r->calls);
		st.leaf_checksum = READ_ONCE(r->checksum);
	}

	if (copy_to_user((void __user *)arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

static long do_quit(void)
{
	struct regipc_ctx *me = current_ctx();
	struct task_struct *task;
	struct page *page;

	if (!me)
		return -ENODEV;

	spin_lock(&registry_lock);
	if (me->peer)
		me->peer->peer = NULL;
	me->peer = NULL;
	task = me->task;
	page = me->page;
	me->task = NULL;
	me->page = NULL;
	me->used = false;
	spin_unlock(&registry_lock);

	put_task_struct(task);
	unpin(page);
	return 0;
}

static long regipc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case REGIPC_IOC_REGISTER:
		return do_register(arg);
	case REGIPC_IOC_PAIR:
		return do_pair(arg);
	case REGIPC_IOC_CALL:
		return do_call();
	case REGIPC_IOC_STATS:
		return do_stats(arg);
	case REGIPC_IOC_QUIT:
		return do_quit();
	default:
		return -ENOTTY;
	}
}

static const struct file_operations regipc_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = regipc_ioctl,
	.compat_ioctl   = regipc_ioctl,
	.llseek         = noop_llseek,
};

static uint regipc_mode = 0600;
module_param_named(mode, regipc_mode, uint, 0444);
MODULE_PARM_DESC(mode, "permissions of /dev/regipc, default 0600 (use 0666 for unprivileged callers)");

static struct miscdevice regipc_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = REGIPC_DRV_NAME,
	.fops  = &regipc_fops,
	.mode  = 0600,
};

static int __init regipc_init(void)
{
	int ret;

	regipc_misc.mode = regipc_mode;
	ret = misc_register(&regipc_misc);
	if (ret)
		return ret;

	pr_info(REGIPC_DRV_NAME ": loaded, abi %u, %u carrier registers, mode %04o\n",
		REGIPC_ABI_VERSION, REGIPC_CARRIERS, regipc_mode);
	return 0;
}

static void __exit regipc_exit(void)
{
	struct task_struct *tasks[REGIPC_MAX_CONTEXTS];
	struct page *pages[REGIPC_MAX_CONTEXTS];
	int i, n = 0;

	spin_lock(&registry_lock);
	for (i = 0; i < REGIPC_MAX_CONTEXTS; i++) {
		struct regipc_ctx *ctx = &contexts[i];

		if (!ctx->used)
			continue;
		ctx->used = false;
		tasks[n] = ctx->task;
		pages[n] = ctx->page;
		n++;
		ctx->task = NULL;
		ctx->page = NULL;
		ctx->peer = NULL;
	}
	spin_unlock(&registry_lock);

	for (i = 0; i < n; i++) {
		if (tasks[i])
			put_task_struct(tasks[i]);
		if (pages[i])
			unpin_user_pages(&pages[i], 1);
	}

	misc_deregister(&regipc_misc);
	pr_info(REGIPC_DRV_NAME ": unloaded\n");
}

module_init(regipc_init);
module_exit(regipc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("register-ipc experiment");
MODULE_DESCRIPTION("Cross-process call: x0..x8 in registers, the module runs the callee's work");
