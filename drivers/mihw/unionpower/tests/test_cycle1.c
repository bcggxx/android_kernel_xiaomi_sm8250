// SPDX-License-Identifier: GPL-2.0
#include "host_kernel.h"
#include "../unionpower.h"

#include <stdio.h>

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #condition); \
			return 1; \
		} \
	} while (0)

static struct pid render_pid;
static struct task_struct render_task;
static struct pid target_pid;
static struct task_struct target_task;

static void setup_tasks(void)
{
	memset(&render_pid, 0, sizeof(render_pid));
	memset(&render_task, 0, sizeof(render_task));
	memset(&target_pid, 0, sizeof(target_pid));
	memset(&target_task, 0, sizeof(target_task));

	render_task.pid = 101;
	render_task.refs = 1;
	render_task.start_time = 1000;
	strcpy(render_task.comm, "RenderThread");
	render_task.thread_pid = &render_pid;
	render_pid.nr = 101;
	render_pid.refs = 1;
	render_pid.task = &render_task;

	target_task.pid = 202;
	target_task.refs = 1;
	target_task.start_time = 2000;
	strcpy(target_task.comm, "surfaceflinger");
	target_task.thread_pid = &target_pid;
	target_pid.nr = 202;
	target_pid.refs = 1;
	target_pid.task = &target_task;

	host_pid_reset();
	host_pid_register(&render_pid);
	host_pid_register(&target_pid);
	current = &render_task;
}

static void reset_case(void)
{
	unionpower_core_stop();
	host_pid_reset();
	FRAME_MONITOR_ENABLE = 0;
	memset(&render_pid, 0, sizeof(render_pid));
	memset(&render_task, 0, sizeof(render_task));
	memset(&target_pid, 0, sizeof(target_pid));
	memset(&target_task, 0, sizeof(target_task));
	current = NULL;
}

static int run_case(void)
{
	const struct file_operations *ops;
	struct _BUFFER_PACKAGE package;
	struct timespec64 timestamp = { .tv_sec = 1, .tv_nsec = 0 };
	struct proc_dir_entry parent = { .host_valid = 1 };
	struct file file = { .host_valid = 1 };
	int ret;

	CHECK(sizeof(struct _BUFFER_PACKAGE) == 48);
	CHECK(BUFFER_QUEUE == _IOW('g', 1, struct _BUFFER_PACKAGE));
	CHECK(BUFFER_VSYNC == _IOW('g', 5, struct _BUFFER_PACKAGE));
	ops = unionpower_ioctl_ops();
	CHECK(ops);
	CHECK(ops->open && ops->read && ops->llseek && ops->release);
	CHECK(ops->unlocked_ioctl);
#ifdef CONFIG_COMPAT
	CHECK(ops->compat_ioctl);
	CHECK(ops->compat_ioctl(&file, BUFFER_QUEUE,
				(unsigned long)&package) == -ENOTTY);
#endif

	setup_tasks();
	CHECK(unionpower_ioctl_init(&parent) == 0);
	CHECK(unionpower_core_start() == 0);
	memset(&package, 0, sizeof(package));
	package.pid = target_task.pid;
	package.start = 0xdeadbeef;
	package.frame_time = 0xfeedface;
	package.frame_id = 1;
	package.frame_interval = 16600 * 1000;
	package.current_timespec = timestamp;

	FRAME_MONITOR_ENABLE = 0;
	ret = ops->unlocked_ioctl(&file, BUFFER_QUEUE,
				  (unsigned long)&package);
	CHECK(ret == 0);
	CHECK(host_pending_work_count() == 0);

	FRAME_MONITOR_ENABLE = 1;
	ret = ops->unlocked_ioctl(&file, 0x7f,
				  (unsigned long)&package);
	CHECK(ret == -EINVAL);
	CHECK(host_pending_work_count() == 0);
	ret = ops->unlocked_ioctl(&file, BUFFER_QUEUE, 1);
	CHECK(ret == -EFAULT);
	CHECK(host_pending_work_count() == 0);

	strcpy(render_task.comm, "OtherThread");
	ret = ops->unlocked_ioctl(&file, BUFFER_QUEUE,
				  (unsigned long)&package);
	CHECK(ret == 0);
	CHECK(host_pending_work_count() == 0);
	strcpy(render_task.comm, "RenderThread");

	ret = ops->unlocked_ioctl(&file, BUFFER_QUEUE,
				  (unsigned long)&package);
	CHECK(ret == 0);
	CHECK(host_pending_work_count() == 1);
	return 0;
}

int main(void)
{
	int ret;

	ret = run_case();
	reset_case();
	unionpower_ioctl_exit();
	if (ret)
		return ret;
	puts("PASS: cycle1 ABI, ioctl mapping, disabled gate, errors");
	return 0;
}
