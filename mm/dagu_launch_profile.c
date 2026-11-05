// SPDX-License-Identifier: GPL-2.0
/* Independent DLP1 collector, inspired by iorap2 binary control-flow evidence.
 * No donor structure layouts or ABI are reproduced. Default off.
 */
#include <linux/anon_inodes.h>
#include <linux/binder_mme.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dagu_launch_profile.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <uapi/linux/dagu_launch_profile.h>
#define DLP_FILES 128
#define DLP_RANGES 4096
#define DLP_PAGES ((32 * 1024 * 1024) / PAGE_SIZE)
struct dlp_extent {
	u64 first, last;
	u32 file;
};
struct dlp_session {
	refcount_t refs;
	spinlock_t lock;
	struct pid *tgid;
	u64 deadline, epoch, cookie;
	unsigned int nr_files, nr_ranges, pages, dropped;
	bool truncated, revoked;
	struct file *files[DLP_FILES];
	struct dlp_extent ranges[DLP_RANGES];
	void *wire;
	size_t wire_size;
};
static DEFINE_MUTEX(dlp_lock);
static struct dlp_session __rcu *dlp_active;
static struct dlp_session *dlp_frozen;
static bool dlp_opened, dlp_enabled;
static u64 dlp_epoch, dlp_cookie;
static atomic_t dlp_snapshots = ATOMIC_INIT(0);
static void dlp_expire(struct work_struct *work);
static DECLARE_DELAYED_WORK(dlp_expiry, dlp_expire);
static void dlp_put(struct dlp_session *s)
{
	unsigned int i;
	if (!s || !refcount_dec_and_test(&s->refs))
		return;
	for (i = 0; i < s->nr_files; i++)
		fput(s->files[i]);
	put_pid(s->tgid);
	vfree(s->wire);
	vfree(s);
}
/* Process context only. No hook takes this mutex or drops a file reference. */
static struct dlp_session *dlp_detach(void)
{
	struct dlp_session *s = rcu_dereference_protected(dlp_active,
			lockdep_is_held(&dlp_lock));
	RCU_INIT_POINTER(dlp_active, NULL);
	synchronize_rcu();
	return s;
}
void dlp_collect(struct file *file, u64 offset, u64 length)
{
	struct dlp_session *s;
	unsigned long flags;
	u64 first, last;
	unsigned int i, f, work = 0;
	if (!length || offset > U64_MAX - (length - 1) ||
			!S_ISREG(file_inode(file)->i_mode))
		return;
	first = offset >> PAGE_SHIFT;
	last = (offset + length - 1) >> PAGE_SHIFT;
	rcu_read_lock();
	s = rcu_dereference(dlp_active);
	if (!s || s->tgid != task_tgid(current) ||
			ktime_get_boottime_ns() >= s->deadline)
		goto out;
	spin_lock_irqsave(&s->lock, flags);
	/* Hook indexes only the referenced file object; freeze merges identities. */
	for (f = 0; f < s->nr_files; f++)
		if (s->files[f] == file)
			break;
	if (f == s->nr_files) {
		if (f == DLP_FILES)
			goto full;
		s->files[f] = get_file(file);
		s->nr_files++;
	}
	/* Page granularity deduplicates read/PTE hits for this open description. */
	while (first <= last) {
		/* Current hooks contribute <=1 page; bound future callers too. */
		if (work++ == 2)
			goto full;
		for (i = 0; i < s->nr_ranges; i++)
			if (s->ranges[i].file == f && s->ranges[i].first <= first &&
			    s->ranges[i].last >= first)
				break;
		if (i == s->nr_ranges) {
			if (i == DLP_RANGES || s->pages == DLP_PAGES)
				goto full;
			s->ranges[i] = (struct dlp_extent){ first, first, f };
			s->nr_ranges++;
			s->pages++;
		}
		first++;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	goto out;
full:
	s->truncated = true;
	if (s->dropped != U32_MAX)
		s->dropped++;
	spin_unlock_irqrestore(&s->lock, flags);
out:
	rcu_read_unlock();
}
static bool dlp_session_valid(struct dlp_session *s)
{
	struct task_struct *task;
	bool alive;

	rcu_read_lock();
	task = pid_task(s->tgid, PIDTYPE_TGID);
	alive = task && !(READ_ONCE(task->flags) & PF_EXITING);
	rcu_read_unlock();
	return alive && ktime_get_boottime_ns() < s->deadline;
}

static void dlp_expire(struct work_struct *work)
{
	struct dlp_session *s;

	mutex_lock(&dlp_lock);
	s = rcu_dereference_protected(dlp_active, lockdep_is_held(&dlp_lock));
	if (s) {
		if (!dlp_session_valid(s))
			dlp_put(dlp_detach());
		else
			mod_delayed_work(system_wq, &dlp_expiry, msecs_to_jiffies(50));
	}
	mutex_unlock(&dlp_lock);
}
static int dlp_compare(const void *a, const void *b)
{
	const struct dlp_extent *x = a, *y = b;
	if (x->file != y->file)
		return x->file < y->file ? -1 : 1;
	return x->first < y->first ? -1 : x->first != y->first;
}
static int dlp_freeze(struct dlp_session *s)
{
	struct dlp_snapshot_header *h;
	struct dlp_snapshot_range *r;
	unsigned int i, j, n = 0, old_files = s->nr_files;
	u32 canonical[DLP_FILES];

	/* Process context, after detach + RCU grace period; references pin identity.
	 * Do not merge custom mappings just because their host inode matches.
	 */
	s->nr_files = 0;
	for (i = 0; i < old_files; i++) {
		struct file *file = s->files[i];

		for (j = 0; j < s->nr_files; j++)
			if (file_inode(s->files[j]) == file_inode(file) &&
			    s->files[j]->f_mapping == file->f_mapping &&
			    s->files[j]->f_path.mnt == file->f_path.mnt)
				break;
		canonical[i] = j;
		if (j == s->nr_files)
			s->files[s->nr_files++] = file;
		else
			fput(file);
	}
	for (i = 0; i < s->nr_ranges; i++)
		s->ranges[i].file = canonical[s->ranges[i].file];
	sort(s->ranges, s->nr_ranges, sizeof(s->ranges[0]), dlp_compare, NULL);
	for (i = 0; i < s->nr_ranges; i++) {
		struct dlp_extent *e = &s->ranges[i];
		if (n && s->ranges[n - 1].file == e->file &&
		    s->ranges[n - 1].last + 1 >= e->first)
			s->ranges[n - 1].last = max(s->ranges[n - 1].last, e->last);
		else
			s->ranges[n++] = *e;
	}
	s->wire_size = sizeof(*h) + n * sizeof(*r);
	s->wire = vzalloc(s->wire_size);
	if (!s->wire)
		return -ENOMEM;
	h = s->wire;
	*h = (struct dlp_snapshot_header){cpu_to_le32(0x31504c44),
		cpu_to_le32(1), cpu_to_le32(s->wire_size), cpu_to_le32(s->nr_files),
		cpu_to_le32(n), cpu_to_le32(s->truncated ? DLP_TRUNCATED : 0),
		cpu_to_le32(PAGE_SHIFT), cpu_to_le32(s->dropped)};
	r = (void *)(h + 1);
	for (i = 0; i < n; i++) {
		r[i].file_index = cpu_to_le32(s->ranges[i].file);
		r[i].offset = cpu_to_le64(s->ranges[i].first << PAGE_SHIFT);
		r[i].length = cpu_to_le64((s->ranges[i].last - s->ranges[i].first + 1) << PAGE_SHIFT);
	}
	return 0;
}
/* Caller holds dlp_lock. Detach before checking validity so the grace period
 * cannot turn an expired session into a successful frozen profile.
 */
static int dlp_complete(u64 cookie, bool success)
{
	struct dlp_session *s;
	int ret;

	s = rcu_dereference_protected(dlp_active, lockdep_is_held(&dlp_lock));
	if (!s || s->cookie != cookie)
		return -ESRCH;
	s = dlp_detach();
	if (!success) {
		dlp_put(s);
		return 0;
	}
	if (!dlp_session_valid(s)) {
		dlp_put(s);
		return -ESRCH;
	}
	ret = dlp_freeze(s);
	if (ret)
		dlp_put(s);
	else
		dlp_frozen = s;
	return ret;
}

static ssize_t dlp_read(struct file *file, char __user *buf, size_t n, loff_t *pos)
{
	struct dlp_session *s = file->private_data;
	if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	if (READ_ONCE(s->revoked))
		return -ESHUTDOWN;
	return simple_read_from_buffer(buf, n, pos, s->wire, s->wire_size);
}
static int dlp_snapshot_close(struct inode *inode, struct file *file)
{
	dlp_put(file->private_data);
	atomic_dec(&dlp_snapshots);
	return 0;
}
static const struct file_operations dlp_snapshot_ops = {
	.read = dlp_read, .release = dlp_snapshot_close, .llseek = default_llseek,
};
static void dlp_clear(void)
{
	dlp_put(dlp_detach());
	if (dlp_frozen)
		WRITE_ONCE(dlp_frozen->revoked, true);
	dlp_put(dlp_frozen);
	dlp_frozen = NULL;
}
static long dlp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dlp_request r;
	struct dlp_session *s;
	struct task_struct *task;
	struct fd f;
	int ret = 0;
	BUILD_BUG_ON(sizeof(r) != 64);
	BUILD_BUG_ON(sizeof(struct dlp_snapshot_header) != 32);
	BUILD_BUG_ON(sizeof(struct dlp_snapshot_range) != 24);
	if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd != DLP_IOCTL)
		return -ENOTTY;
	if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
		return -EFAULT;
	if (r.op < DLP_ENABLE || r.op > DLP_EXPORT_FILE)
		return -ENOTTY;
	if (r.version != 1 || r.size != sizeof(r) || r.reserved ||
			(r.op != DLP_ENABLE && r.flags) ||
			(r.op == DLP_ENABLE && r.flags > 1))
		return -EINVAL;
	if ((r.op != DLP_BEGIN && (r.binder_fd || r.binder_handle ||
			r.tid || r.uid || r.lease_ms)) ||
			(r.op != DLP_EXPORT_FILE && r.index) ||
			(r.op == DLP_ENABLE && r.cookie))
		return -EINVAL;
	mutex_lock(&dlp_lock);
	if (r.op == DLP_ENABLE) {
		dlp_clear();
		dlp_enabled = r.flags;
		dlp_epoch = r.epoch;
		dlp_cookie = 0;
		goto out;
	}
	if (!dlp_enabled) {
		ret = -EACCES;
		goto out;
	}
	if (r.epoch != dlp_epoch) {
		ret = -ESRCH;
		goto out;
	}
	s = rcu_dereference_protected(dlp_active, lockdep_is_held(&dlp_lock));
	if (r.op == DLP_BEGIN) {
		if (!r.cookie || r.cookie <= dlp_cookie) {
			ret = -ESRCH;
			goto out;
		}
		if (s || atomic_read(&dlp_snapshots)) {
			ret = -ENOSPC;
			goto out;
		}
		if (!r.lease_ms || r.lease_ms > 2000) {
			ret = -EINVAL;
			goto out;
		}
		s = vzalloc(sizeof(*s));
		if (!s) {
			ret = -ENOMEM;
			goto out;
		}
		refcount_set(&s->refs, 1);
		spin_lock_init(&s->lock);
		f = fdget(r.binder_fd);
		if (!f.file) {
			dlp_put(s);
			ret = -EBADF;
			goto out;
		}
		task = binder_mme_get_task(f.file, r.binder_handle);
		fdput(f);
		if (IS_ERR(task)) {
			dlp_put(s);
			ret = PTR_ERR(task);
			goto out;
		}
		rcu_read_lock();
		if (!pid_alive(task) || task_pid_nr(task) != r.tid ||
			!uid_eq(task_uid(task), make_kuid(&init_user_ns, r.uid)) ||
			(READ_ONCE(task->flags) & PF_EXITING))
			ret = -ESRCH;
		else
			s->tgid = get_task_pid(task, PIDTYPE_TGID);
		rcu_read_unlock();
		put_task_struct(task);
		if (ret) {
			dlp_put(s);
			goto out;
		}
		dlp_put(dlp_frozen);
		dlp_frozen = NULL;
		s->cookie = r.cookie;
		s->epoch = r.epoch;
		s->deadline = ktime_get_boottime_ns() + r.lease_ms * NSEC_PER_MSEC;
		dlp_cookie = r.cookie;
		rcu_assign_pointer(dlp_active, s);
		mod_delayed_work(system_wq, &dlp_expiry, msecs_to_jiffies(50));
	} else if (r.op == DLP_FINISH || r.op == DLP_ABORT) {
		ret = dlp_complete(r.cookie, r.op == DLP_FINISH);
	} else if (r.op == DLP_SNAPSHOT || r.op == DLP_EXPORT_FILE) {
		s = dlp_frozen;
		if (!s || s->cookie != r.cookie) {
			ret = -ESRCH;
			goto out;
		}
		if (r.op == DLP_SNAPSHOT) {
			if (atomic_read(&dlp_snapshots)) {
				ret = -ENOSPC;
				goto out;
			}
			refcount_inc(&s->refs);
			atomic_inc(&dlp_snapshots);
			ret = anon_inode_getfd("dagu-launch-snapshot", &dlp_snapshot_ops, s,
			O_RDONLY | O_CLOEXEC);
			if (ret < 0) {
				atomic_dec(&dlp_snapshots);
				dlp_put(s);
			}
		} else {
			struct file *original, *export;

			if (r.index >= s->nr_files) {
				ret = -EINVAL;
				goto out;
			}
			original = s->files[r.index];
			if ((original->f_flags & O_ACCMODE) != O_RDONLY ||
			    !(original->f_mode & FMODE_READ) ||
			    (original->f_mode & FMODE_WRITE)) {
				ret = -EACCES;
				goto out;
			}
			ret = inode_permission(file_inode(original), MAY_READ);
			if (ret)
				goto out;
			/* Independent description: never share the app's f_pos/f_flags. */
			export = dentry_open(&original->f_path, O_RDONLY | O_LARGEFILE,
			     current_cred());
			if (IS_ERR(export)) {
				ret = PTR_ERR(export);
				goto out;
			}
			if (!S_ISREG(file_inode(export)->i_mode) ||
			    file_inode(export) != file_inode(original) ||
			    export->f_path.mnt != original->f_path.mnt ||
			    !(export->f_mode & FMODE_READ) ||
			    (export->f_mode & FMODE_WRITE)) {
				fput(export);
				ret = -ESTALE;
				goto out;
			}
			ret = get_unused_fd_flags(O_CLOEXEC);
			if (ret >= 0)
				fd_install(ret, export);
			else
				fput(export);
		}
	} else if (r.op == DLP_STATUS) {
		ret = s ? 1 : 0;
	}
out:
	mutex_unlock(&dlp_lock);
	return ret;
}
static int dlp_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	mutex_lock(&dlp_lock);
	if (dlp_opened)
		ret = -EBUSY;
	else
		dlp_opened = true;
	mutex_unlock(&dlp_lock);
	return ret;
}
static int dlp_close(struct inode *inode, struct file *file)
{
	mutex_lock(&dlp_lock);
	dlp_enabled = false;
	dlp_clear();
	dlp_opened = false;
	mutex_unlock(&dlp_lock);
	return 0;
}
static const struct file_operations dlp_ops = {
	.open = dlp_open, .release = dlp_close, .unlocked_ioctl = dlp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = dlp_ioctl,
#endif
	.llseek = no_llseek,
};
static struct miscdevice dlp_device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "dagu_launch_profile",
	.fops = &dlp_ops, .mode = 0600,
};
static int __init dlp_init(void)
{
	return misc_register(&dlp_device);
}
device_initcall(dlp_init);
