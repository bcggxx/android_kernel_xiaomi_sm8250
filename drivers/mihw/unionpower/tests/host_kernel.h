/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UNIONPOWER_HOST_KERNEL_H
#define _UNIONPOWER_HOST_KERNEL_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "../unionpower_uapi.h"

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;
typedef unsigned short umode_t;

#define __user
#define PAGE_SIZE 4096
#define HZ 100
#define GFP_ATOMIC 0
#define GFP_KERNEL 0
#define TASK_COMM_LEN 16
#define PIDTYPE_PID 0
#define WQ_MEM_RECLAIM 0
#define WQ_HIGHPRI 0

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define BUILD_BUG_ON(condition) \
	do { if (condition) abort(); } while (0)
#define READ_ONCE(value) (*(volatile typeof(value) *)&(value))
#define WRITE_ONCE(value, new_value) \
	do { *(volatile typeof(value) *)&(value) = (new_value); } while (0)

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	new->prev = head->prev;
	new->next = head;
	head->prev->next = new;
	head->prev = new;
}

static inline void list_del_init(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	INIT_LIST_HEAD(entry);
}

static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)
#define list_for_each_entry(pos, head, member) \
	for (pos = list_first_entry(head, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = list_first_entry(&pos->member, typeof(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_first_entry(head, typeof(*pos), member), \
	     n = list_first_entry(&pos->member, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = n, n = list_first_entry(&n->member, typeof(*n), member))

struct mutex {
	int unused;
};
struct spinlock {
	int unused;
};

#define DEFINE_MUTEX(name) struct mutex name
#define DEFINE_SPINLOCK(name) struct spinlock name
#define mutex_init(lock) do { (void)(lock); } while (0)
#define spin_lock_init(lock) do { (void)(lock); } while (0)
#define mutex_lock(lock) do { (void)(lock); } while (0)
#define mutex_unlock(lock) do { (void)(lock); } while (0)
#define spin_lock(lock) do { (void)(lock); } while (0)
#define spin_unlock(lock) do { (void)(lock); } while (0)
#define spin_lock_irqsave(lock, flags) \
	do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do { (void)(lock); (void)(flags); } while (0)

struct pid {
	int nr;
	int refs;
	struct task_struct *task;
};

struct task_struct {
	int pid;
	int refs;
	u64 start_time;
	char comm[TASK_COMM_LEN];
	struct pid *thread_pid;
};

extern struct task_struct *current;
extern struct pid *host_current_pid(void);
extern struct task_struct *host_current_task(void);

static inline struct pid *task_pid(struct task_struct *task)
{
	return task->thread_pid;
}

static inline struct pid *get_pid(struct pid *pid)
{
	if (pid)
		pid->refs++;
	return pid;
}

void put_pid(struct pid *pid);
struct pid *get_task_pid(struct task_struct *task, int type);
struct task_struct *get_pid_task(struct pid *pid, int type);
struct pid *find_get_pid(int nr);
void put_task_struct(struct task_struct *task);
void host_pid_register(struct pid *pid);
void host_pid_unregister(struct pid *pid);
void host_pid_reset(void);

static inline char *get_task_comm(char *buf, struct task_struct *task)
{
	memcpy(buf, task->comm, TASK_COMM_LEN);
	return buf;
}

static inline int pid_nr(struct pid *pid)
{
	return pid ? pid->nr : 0;
}

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	work_func_t func;
};

struct delayed_work {
	struct work_struct work;
	bool host_delayed_pending;
};

struct workqueue_struct {
	int host_valid;
};

bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct *wq,
		      struct delayed_work *dwork, unsigned long delay);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
void flush_workqueue(struct workqueue_struct *wq);
void host_run_queued_work(void);
void host_set_queue_work_synchronous(bool enabled);
struct workqueue_struct *host_workqueue_alloc(const char *name, int count);
void host_workqueue_destroy(struct workqueue_struct *wq);
int host_pending_work_count(void);

#define alloc_ordered_workqueue(fmt, flags, args...) \
	host_workqueue_alloc((fmt), 1)
#define destroy_workqueue(wq) host_workqueue_destroy(wq)

#define INIT_WORK(work_item, callback) \
	do { (work_item)->func = (callback); } while (0)
#define INIT_DELAYED_WORK(dwork, callback) \
	do { (dwork)->work.func = (callback); \
	     (dwork)->host_delayed_pending = false; } while (0)

#define kmalloc(size, flags) malloc(size)
#define kzalloc(size, flags) calloc(1, size)
#define kfree(ptr) free(ptr)

static inline s64 timespec64_to_ns(const struct timespec64 *ts)
{
	return ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

static inline s64 ktime_to_us(s64 ns)
{
	return ns / 1000;
}

#define module_param(name, type, permissions)
#define MODULE_PARM_DESC(name, description)
#define MODULE_LICENSE(name)
#define pr_debug(fmt, args...) do { } while (0)
#define pr_err(fmt, args...) do { } while (0)
#define likely(condition) (condition)
#define unlikely(condition) (condition)

struct proc_dir_entry {
	int host_valid;
};

struct seq_file {
	int host_valid;
};

struct inode {
	void *i_private;
};

struct file {
	int host_valid;
};

struct file_operations {
	int (*open)(struct inode *inode, struct file *file);
	ssize_t (*read)(struct file *file, char __user *buf, size_t count,
			loff_t *ppos);
	loff_t (*llseek)(struct file *file, loff_t offset, int whence);
	int (*release)(struct inode *inode, struct file *file);
	long (*unlocked_ioctl)(struct file *file, unsigned int cmd,
			       unsigned long arg);
	long (*compat_ioctl)(struct file *file, unsigned int cmd,
			     unsigned long arg);
};

int single_open(struct file *file, int (*show)(struct seq_file *, void *),
		void *data);
ssize_t seq_read(struct file *file, char __user *buf, size_t count,
		 loff_t *ppos);
loff_t seq_lseek(struct file *file, loff_t offset, int whence);
int single_release(struct inode *inode, struct file *file);
struct proc_dir_entry *proc_mkdir(const char *name,
				  struct proc_dir_entry *parent);
struct proc_dir_entry *proc_create(const char *name, umode_t mode,
				   struct proc_dir_entry *parent,
				   const struct file_operations *ops);
void proc_remove(struct proc_dir_entry *entry);

static inline unsigned long copy_from_user(void *to, const void *from,
					   unsigned long count)
{
	if (from == (const void *)1UL)
		return count;
	memcpy(to, from, count);
	return 0;
}

#endif /* _UNIONPOWER_HOST_KERNEL_H */
