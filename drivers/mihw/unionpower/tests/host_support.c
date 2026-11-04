// SPDX-License-Identifier: GPL-2.0
#include "host_kernel.h"

#include <stdio.h>

struct task_struct *current;

void unionpower_sysfs_notify(const char *attr)
	__attribute__((weak));

void unionpower_sysfs_notify(const char *attr)
{
	(void)attr;
}

struct host_pid_registry {
	struct pid *pids[128];
	int count;
};

struct host_work_entry {
	struct work_struct *work;
	struct host_work_entry *next;
};

static struct host_pid_registry pid_registry;
static struct host_work_entry *queued_work;
static bool queue_work_synchronous;
static ktime_t host_now;
static bool mod_delayed_work_result;
static int mod_delayed_work_calls;
static unsigned long last_delayed_work_delay;

ktime_t host_ktime_get(void)
{
	return host_now;
}

void host_set_ktime(ktime_t now)
{
	assert(now >= 0);
	host_now = now;
}

struct pid *host_current_pid(void)
{
	return current ? current->thread_pid : NULL;
}

struct task_struct *host_current_task(void)
{
	return current;
}

void put_pid(struct pid *pid)
{
	if (!pid)
		return;
	assert(pid->refs > 0);
	pid->refs--;
}

struct pid *get_task_pid(struct task_struct *task, int type)
{
	(void)type;
	return get_pid(task_pid(task));
}

struct task_struct *get_pid_task(struct pid *pid, int type)
{
	(void)type;
	if (!pid || !pid->task)
		return NULL;
	pid->task->refs++;
	return pid->task;
}

struct pid *find_get_pid(int nr)
{
	int i;

	for (i = 0; i < pid_registry.count; i++) {
		if (pid_registry.pids[i]->nr == nr)
			return get_pid(pid_registry.pids[i]);
	}
	return NULL;
}

void put_task_struct(struct task_struct *task)
{
	assert(task->refs > 0);
	task->refs--;
}

void host_pid_register(struct pid *pid)
{
	assert(pid_registry.count < 128);
	pid_registry.pids[pid_registry.count++] = pid;
}

void host_pid_unregister(struct pid *pid)
{
	int i;

	for (i = 0; i < pid_registry.count; i++) {
		if (pid_registry.pids[i] == pid) {
			pid_registry.pids[i] =
				pid_registry.pids[--pid_registry.count];
			return;
		}
	}
}

void host_pid_reset(void)
{
	memset(&pid_registry, 0, sizeof(pid_registry));
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	struct host_work_entry *entry;

	if (!wq || !wq->host_valid)
		return false;
	if (queue_work_synchronous) {
		work->func(work);
		return true;
	}
	entry = malloc(sizeof(*entry));
	assert(entry);
	entry->work = work;
	entry->next = queued_work;
	queued_work = entry;
	return true;
}

void host_set_queue_work_synchronous(bool enabled)
{
	queue_work_synchronous = enabled;
}

bool queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay)
{
	(void)delay;
	if (!wq || !wq->host_valid)
		return false;
	dwork->host_delayed_pending = true;
	return true;
}

bool mod_delayed_work(struct workqueue_struct *wq,
		      struct delayed_work *dwork, unsigned long delay)
{
	mod_delayed_work_calls++;
	last_delayed_work_delay = delay;
	if (!wq || !wq->host_valid)
		return false;
	dwork->host_delayed_pending = true;
	return mod_delayed_work_result;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	bool was_pending = dwork->host_delayed_pending;

	dwork->host_delayed_pending = false;
	return was_pending;
}

void host_reset_mod_delayed_work_calls(void)
{
	mod_delayed_work_calls = 0;
	last_delayed_work_delay = 0;
}

int host_mod_delayed_work_count(void)
{
	return mod_delayed_work_calls;
}

unsigned long host_last_delayed_work_delay(void)
{
	return last_delayed_work_delay;
}

void host_set_mod_delayed_work_result(bool result)
{
	mod_delayed_work_result = result;
}

int host_pending_work_count(void)
{
	struct host_work_entry *entry;
	int count = 0;

	for (entry = queued_work; entry; entry = entry->next)
		count++;
	return count;
}

void flush_workqueue(struct workqueue_struct *wq)
{
	struct host_work_entry *entry;

	(void)wq;
	while (queued_work) {
		entry = queued_work;
		queued_work = entry->next;
		assert(entry->work->func);
		entry->work->func(entry->work);
		free(entry);
	}
}

void host_run_queued_work(void)
{
	flush_workqueue(NULL);
}

struct workqueue_struct *host_workqueue_alloc(const char *name, int count)
{
	struct workqueue_struct *wq;

	(void)name;
	(void)count;
	wq = malloc(sizeof(*wq));
	assert(wq);
	wq->host_valid = 1;
	return wq;
}

void host_workqueue_destroy(struct workqueue_struct *wq)
{
	assert(wq);
	assert(wq->host_valid);
	wq->host_valid = 0;
	free(wq);
}

int single_open(struct file *file, int (*show)(struct seq_file *, void *),
		void *data)
{
	(void)file;
	(void)show;
	(void)data;
	return 0;
}

ssize_t seq_read(struct file *file, char __user *buf, size_t count,
		 loff_t *ppos)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)ppos;
	return 0;
}

loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return 0;
}

int single_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

struct proc_dir_entry *proc_mkdir(const char *name,
				  struct proc_dir_entry *parent)
{
	struct proc_dir_entry *entry = malloc(sizeof(*entry));

	(void)name;
	(void)parent;
	assert(entry);
	entry->host_valid = 1;
	return entry;
}

struct proc_dir_entry *proc_create(const char *name, umode_t mode,
				   struct proc_dir_entry *parent,
				   const struct file_operations *ops)
{
	struct proc_dir_entry *entry = malloc(sizeof(*entry));

	(void)name;
	(void)mode;
	(void)parent;
	(void)ops;
	assert(entry);
	entry->host_valid = 1;
	return entry;
}

void proc_remove(struct proc_dir_entry *entry)
{
	free(entry);
}
