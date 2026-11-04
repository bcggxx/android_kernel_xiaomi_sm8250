// SPDX-License-Identifier: GPL-2.0
/* C06: independent, conservative adaptation of MiCode unfairmem scene 1.
 * Source: Xiaomi_Kernel_OpenSource bixi-v-oss 56295df675b9, author huzhongyang.
 * Control, lifetime, Binder-owner ABI and request restrictions are new target code.
 */
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/binder_mme.h>
#include <linux/cred.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mi_mem_engine.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <trace/events/sched.h>
#include "mi_mem_engine_policy.h"
#define CREATE_TRACE_POINTS
#include <trace/events/mi_mem_engine.h>

struct mme_identity {
	struct pid *pid;
	kuid_t uid;
	u64 start_boottime_ns;
};
struct mme_scene {
	struct rcu_head rcu;
	u64 cookie;
	u64 deadline;
	unsigned int count;
	struct mme_identity tasks[MME_MAX_TASKS];
};
static DEFINE_MUTEX(mme_lock);
static struct mme_scene __rcu *mme_active;
static bool mme_opened;
static bool mme_enabled;
static u64 mme_last_cookie;
static void mme_expire(struct work_struct *work);
static DECLARE_DELAYED_WORK(mme_expiry, mme_expire);

static bool mme_authorized(void)
{
	return ns_capable(&init_user_ns, CAP_SYS_ADMIN);
}
static void mme_free(struct mme_scene *s)
{
	unsigned int i;

	for (i = 0; i < s->count; i++)
		put_pid(s->tasks[i].pid);
	kfree(s);
}
static void mme_free_rcu(struct rcu_head *rcu)
{
	mme_free(container_of(rcu, struct mme_scene, rcu));
}
/* All writers, including expiry, serialize here; allocator never takes lock. */
static void mme_publish(struct mme_scene *s)
{
	struct mme_scene *old = rcu_dereference_protected(mme_active,
						lockdep_is_held(&mme_lock));

	rcu_assign_pointer(mme_active, s);
	if (old)
		call_rcu(&old->rcu, mme_free_rcu);
}
static bool mme_identity_alive(const struct mme_identity *id)
{
	struct task_struct *t = pid_task(id->pid, PIDTYPE_PID);

	return t && pid_alive(t) && !(READ_ONCE(t->flags) & PF_EXITING) &&
		uid_eq(task_uid(t), id->uid) &&
		READ_ONCE(t->real_start_time) == id->start_boottime_ns;
}
static void mme_expire(struct work_struct *work)
{
	struct mme_scene *s;
	bool alive = true;
	unsigned int i;
	u64 now;

	mutex_lock(&mme_lock);
	s = rcu_dereference_protected(mme_active, lockdep_is_held(&mme_lock));
	if (!s)
		goto out;
	now = ktime_get_boottime_ns();
	rcu_read_lock();
	for (i = 0; i < s->count; i++)
		alive &= mme_identity_alive(&s->tasks[i]);
	rcu_read_unlock();
	if (!alive || !mme_session_live(now, s->deadline))
		mme_publish(NULL);
	else
		mod_delayed_work(system_wq, &mme_expiry,
			max_t(unsigned long, 1, nsecs_to_jiffies(s->deadline - now)));
out:
	mutex_unlock(&mme_lock);
}
static void mme_task_exit(void *unused, struct task_struct *task)
{
	struct mme_scene *s;
	unsigned int i;

	rcu_read_lock();
	s = rcu_dereference(mme_active);
	if (s) {
		for (i = 0; i < s->count; i++) {
			if (s->tasks[i].pid == task_pid(task)) {
				mod_delayed_work(system_wq, &mme_expiry, 0);
				break;
			}
		}
	}
	rcu_read_unlock();
}
unsigned long mme_allocation_mark(unsigned long mark, gfp_t gfp,
				 unsigned int order, bool privileged)
{
	struct mme_scene *s;
	unsigned int i;
	bool hit = false;
	u64 cookie = 0;

	if (order || privileged || !(gfp & __GFP_DIRECT_RECLAIM) ||
	    (gfp & (__GFP_MEMALLOC | __GFP_HIGH | __GFP_ATOMIC)) ||
	    (current->flags & PF_MEMALLOC) || in_atomic() || irqs_disabled())
		return mark;
	rcu_read_lock();
	s = rcu_dereference(mme_active);
	if (s && mme_session_live(ktime_get_boottime_ns(), s->deadline)) {
		cookie = s->cookie;
		for (i = 0; i < s->count; i++) {
			const struct mme_identity *id = &s->tasks[i];

			if (id->pid == task_pid(current) &&
			    uid_eq(current_uid(), id->uid) &&
			    current->real_start_time == id->start_boottime_ns &&
			    !(current->flags & PF_EXITING)) {
				hit = true;
				break;
			}
		}
	}
	rcu_read_unlock();
	trace_mme_allocation(cookie, task_pid_nr(current), mark,
			     mme_policy_mark(mark, hit), hit);
	return mme_policy_mark(mark, hit);
}
static int mme_bind(struct mme_scene *s, const struct mme_request *r)
{
	unsigned int i;

	for (i = 0; i < r->nr_tasks; i++) {
		struct task_struct *t;
		kuid_t uid;
		struct fd f = fdget(r->tasks[i].binder_fd);
		struct mme_identity *id = &s->tasks[i];
		int ret = 0;

		if (!f.file)
			return -EBADF;
		t = binder_mme_get_task(f.file, r->tasks[i].binder_handle);
		fdput(f);
		if (IS_ERR(t))
			return PTR_ERR(t);
		rcu_read_lock();
		uid = task_uid(t);
		if (!pid_alive(t) || (READ_ONCE(t->flags) & PF_EXITING)) {
			ret = -ESRCH;
		} else if (task_pid_nr(t) != r->tasks[i].tid ||
			   !uid_eq(uid,
				   make_kuid(&init_user_ns, r->tasks[i].uid))) {
			ret = -EACCES;
		} else {
			/* Stable Binder node owner, never a numeric PID lookup. */
			id->pid = get_task_pid(t, PIDTYPE_PID);
			id->uid = uid;
			id->start_boottime_ns = READ_ONCE(t->real_start_time);
			s->count++;
		}
		rcu_read_unlock();
		put_task_struct(t);
		if (ret)
			return ret;
	}
	return 0;
}
static long mme_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mme_request r;
	struct mme_scene *s = NULL, *active;
	struct mme_request empty = { .version = MME_VERSION, .size = sizeof(r) };
	int ret = 0;

	if (!mme_authorized())
		return -EPERM;
	if (cmd != MME_BEGIN && cmd != MME_END && cmd != MME_ENABLE)
		return -ENOTTY;
	if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
		return -EFAULT;
	if (r.version != MME_VERSION || r.size != sizeof(r))
		return -EINVAL;
	mutex_lock(&mme_lock);
	if (cmd == MME_ENABLE) {
		empty.lease_ms = r.lease_ms;
		if (r.lease_ms > 1 || memcmp(&r, &empty, sizeof(r))) {
			ret = -EINVAL;
			goto out;
		}
		mme_enabled = r.lease_ms;
		if (!mme_enabled)
			mme_publish(NULL);
	} else if (cmd == MME_END) {
		empty.cookie = r.cookie;
		if (!r.cookie || memcmp(&r, &empty, sizeof(r))) {
			ret = -EINVAL;
			goto out;
		}
		active = rcu_dereference_protected(mme_active,
						lockdep_is_held(&mme_lock));
		if (active && mme_end_matches(r.cookie, active->cookie))
			mme_publish(NULL);
	} else {
		if (!mme_enabled) {
			ret = -EACCES;
			goto out;
		}
		ret = mme_validate_begin(&r, mme_last_cookie);
		if (ret)
			goto out;
		s = kzalloc(sizeof(*s), GFP_KERNEL);
		if (!s) {
			ret = -ENOMEM;
			goto out;
		}
		ret = mme_bind(s, &r);
		if (ret) {
			mme_free(s);
			goto out;
		}
		s->cookie = r.cookie;
		s->deadline = ktime_get_boottime_ns() + (u64)r.lease_ms * NSEC_PER_MSEC;
		mme_last_cookie = r.cookie;
		mme_publish(s);
		mod_delayed_work(system_wq, &mme_expiry, msecs_to_jiffies(r.lease_ms));
	}
out:
	mutex_unlock(&mme_lock);
	return ret;
}
static int mme_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	if (!mme_authorized())
		return -EPERM;
	mutex_lock(&mme_lock);
	if (mme_opened)
		ret = -EBUSY;
	else {
		mme_opened = true;
		mme_enabled = false;
		mme_last_cookie = 0;
	}
	mutex_unlock(&mme_lock);
	return ret;
}
static int mme_release(struct inode *inode, struct file *file)
{
	mutex_lock(&mme_lock);
	mme_enabled = false;
	mme_publish(NULL);
	mme_opened = false;
	mutex_unlock(&mme_lock);
	return 0;
}
static const struct file_operations mme_fops = {
	.owner = THIS_MODULE,
	.open = mme_open,
	.release = mme_release,
	.unlocked_ioctl = mme_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mme_ioctl,
#endif
	.llseek = no_llseek,
};
static struct miscdevice mme_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mi_mem_engine",
	.fops = &mme_fops,
	.mode = 0600,
};
static int __init mme_init(void)
{
	int ret = register_trace_sched_process_exit(mme_task_exit, NULL);

	if (ret)
		return ret;
	ret = misc_register(&mme_device);
	if (ret)
		unregister_trace_sched_process_exit(mme_task_exit, NULL);
	return ret;
}
late_initcall(mme_init);
