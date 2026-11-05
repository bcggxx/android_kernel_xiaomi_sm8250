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

static int notify_count;
static struct pid render_pid;
static struct task_struct render_task;

void unionpower_sysfs_notify(const char *attr)
{
	assert(!strcmp(attr, "frame_jank"));
	notify_count++;
}

static void setup_task(void)
{
	memset(&render_pid, 0, sizeof(render_pid));
	memset(&render_task, 0, sizeof(render_task));
	render_task.pid = 301;
	render_task.refs = 1;
	render_task.start_time = 3000;
	strcpy(render_task.comm, "RenderThread");
	render_task.thread_pid = &render_pid;
	render_pid.nr = 301;
	render_pid.refs = 1;
	render_pid.task = &render_task;
	host_pid_reset();
	host_pid_register(&render_pid);
	current = &render_task;
	notify_count = 0;
}

static struct timespec64 to_timespec(s64 usec)
{
	struct timespec64 ts;

	ts.tv_sec = usec / 1000000;
	ts.tv_nsec = (usec % 1000000) * 1000;
	return ts;
}

static int issue_event(const struct file_operations *ops,
		       unsigned int cmd, s64 time_us, s64 interval_us,
		       u64 frame_id)
{
	struct _BUFFER_PACKAGE package;

	memset(&package, 0, sizeof(package));
	package.pid = render_task.pid;
	package.start = 0xffffffff;
	package.frame_time = 0x123456789ULL;
	package.frame_id = frame_id;
	package.frame_interval = interval_us * 1000;
	package.current_timespec = to_timespec(time_us);
	return ops->unlocked_ioctl(NULL, cmd, (unsigned long)&package);
}

static void reset_case(void)
{
	unionpower_core_stop();
	host_pid_reset();
	FRAME_MONITOR_ENABLE = 0;
	memset(&render_pid, 0, sizeof(render_pid));
	memset(&render_task, 0, sizeof(render_task));
	current = NULL;
}

static int run_case(void)
{
	const struct file_operations *ops = unionpower_ioctl_ops();
	struct unionpower_identity identity;
	struct buffer_record *record;

	CHECK(sizeof(record->vsync_record_list) /
	      sizeof(record->vsync_record_list[0]) == UNIONPOWER_RECORD_SIZE);
	setup_task();
	FRAME_MONITOR_ENABLE = 1;
	CHECK(unionpower_core_start() == 0);

	CHECK(issue_event(ops, BUFFER_VSYNC, 1000000, 16600, 1) == 0);
	host_run_queued_work();
	CHECK(unionpower_record_count() == 1);
	CHECK(issue_event(ops, BUFFER_VSYNC, 1016600, 16600, 2) == 0);
	host_run_queued_work();

	identity.pid = &render_pid;
	identity.start_time = render_task.start_time;
	record = unionpower_lookup_record(&identity);
	CHECK(record);
	CHECK(record->vsync_count == 2);
	CHECK(issue_event(ops, BUFFER_QUEUE, 1021000, 0, 1) == 0);
	host_run_queued_work();
	CHECK(record->frame_count == 1);
	CHECK(record->continus_jank == 1);
	CHECK(record->jank_happened);
	CHECK(unionpower_get_frame_jank());
	CHECK(notify_count == 1);
	CHECK(record->history_buffer_interval == 18800);
	CHECK(record->last_queue_buffer_time[0] == 1021000);

	CHECK(issue_event(ops, BUFFER_QUEUE, 1022500, 0, 2) == 0);
	host_run_queued_work();
	CHECK(record->frame_count == 2);
	CHECK(record->continus_nice == 1);
	CHECK(record->jank_happened);
	CHECK(unionpower_get_frame_jank());
	CHECK(notify_count == 1);
	CHECK(record->last_queue_buffer_time[1] == 1022500);

	CHECK(issue_event(ops, BUFFER_VSYNC, 1033200, 16600, 3) == 0);
	host_run_queued_work();
	CHECK(issue_event(ops, BUFFER_QUEUE, 1037800, 0, 3) == 0);
	host_run_queued_work();
	CHECK(record->continus_nice == 2);
	CHECK(record->jank_happened);
	CHECK(issue_event(ops, BUFFER_VSYNC, 1049800, 16600, 4) == 0);
	host_run_queued_work();
	CHECK(issue_event(ops, BUFFER_QUEUE, 1054400, 0, 4) == 0);
	host_run_queued_work();
	CHECK(record->continus_nice == 3);
	CHECK(!record->jank_happened);
	CHECK(!unionpower_get_frame_jank());
	CHECK(notify_count == 2);

	CHECK(issue_event(ops, BUFFER_VSYNC, 1066400, 16600, 5) == 0);
	host_run_queued_work();
	CHECK(issue_event(ops, BUFFER_VSYNC, 1083000, 16600, 6) == 0);
	host_run_queued_work();
	CHECK(issue_event(ops, BUFFER_VSYNC, 1099600, 16600, 7) == 0);
	host_run_queued_work();
	CHECK(issue_event(ops, BUFFER_VSYNC, 1116200, 16600, 8) == 0);
	host_run_queued_work();
	CHECK(issue_event(ops, BUFFER_VSYNC, 1132800, 16600, 9) == 0);
	host_run_queued_work();
	CHECK(issue_event(ops, BUFFER_VSYNC, 1149400, 16600, 10) == 0);
	host_run_queued_work();
	CHECK(record->vsync_count == 2);
	CHECK(record->frame_count == 0);
	CHECK(record->vsync_record_list[0].vsync_id == 9);
	CHECK(record->vsync_record_list[1].vsync_id == 10);
	return 0;
}

int main(void)
{
	int ret;

	ret = run_case();
	reset_case();
	if (ret)
		return ret;
	puts("PASS: cycle2 queue/vsync five-slot algorithm and jank transitions");
	return 0;
}
