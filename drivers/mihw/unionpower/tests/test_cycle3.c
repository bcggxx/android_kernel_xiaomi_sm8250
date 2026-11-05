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

#define FIXTURE_COUNT (UNIONPOWER_MAX_RECORDS + 1)

static struct pid pids[FIXTURE_COUNT];
static struct task_struct tasks[FIXTURE_COUNT];
static int notify_count;

void unionpower_sysfs_notify(const char *attr)
{
	assert(!strcmp(attr, "frame_jank"));
	notify_count++;
}

static void setup_fixtures(void)
{
	int i;

	memset(pids, 0, sizeof(pids));
	memset(tasks, 0, sizeof(tasks));
	host_pid_reset();
	host_set_ktime(0);
	notify_count = 0;
	for (i = 0; i < FIXTURE_COUNT; i++) {
		tasks[i].pid = 1000 + i;
		tasks[i].refs = 1;
		tasks[i].start_time = 10000 + i;
		strcpy(tasks[i].comm, "RenderThread");
		tasks[i].thread_pid = &pids[i];
		pids[i].nr = tasks[i].pid;
		pids[i].refs = 1;
		pids[i].task = &tasks[i];
		host_pid_register(&pids[i]);
	}
	current = &tasks[0];
}

static void reset_case(void)
{
	unionpower_core_stop();
	host_pid_reset();
	FRAME_MONITOR_ENABLE = 0;
	memset(pids, 0, sizeof(pids));
	memset(tasks, 0, sizeof(tasks));
	current = NULL;
	host_set_queue_work_synchronous(false);
	host_set_ktime(0);
	notify_count = 0;
}

static int dispatch_event(int index, enum WORK_PUSH_TYPE type,
			  s64 frame_time_us, s64 frame_interval_us,
			  u64 frame_id)
{
	struct unionpower_identity caller;
	struct unionpower_identity target;

	caller.pid = get_pid(&pids[index]);
	caller.start_time = tasks[index].start_time;
	target.pid = get_pid(&pids[index]);
	target.start_time = tasks[index].start_time;
	return unionpower_dispatch(type, &caller, &target, frame_time_us,
				   frame_interval_us, frame_id);
}

static int cleanup_recalculates_jank_case(void)
{
	struct unionpower_identity identity;
	struct unionpower_stats stats;

	setup_fixtures();
	CHECK(unionpower_core_start() == 0);
	CHECK(dispatch_event(0, NOTIFIER_VSYNC, 1000000, 16600, 1) == 0);
	host_run_queued_work();
	CHECK(dispatch_event(0, NOTIFIER_VSYNC, 1016600, 16600, 2) == 0);
	host_run_queued_work();
	CHECK(dispatch_event(0, NOTIFIER_QUEUE, 1021000, 0, 1) == 0);
	host_run_queued_work();
	CHECK(unionpower_lookup_record(&(struct unionpower_identity) {
		.pid = &pids[0],
		.start_time = tasks[0].start_time,
	}));
	CHECK(unionpower_get_frame_jank());
	CHECK(notify_count == 1);

	identity.pid = &pids[0];
	identity.start_time = tasks[0].start_time;
	host_set_ktime(1100000000);
	pids[0].task = NULL;
	current = NULL;
	notify_count = 0;
	unionpower_run_cleanup();
	CHECK(!unionpower_lookup_record(&identity));
	CHECK(unionpower_record_count() == 0);
	CHECK(!unionpower_get_frame_jank());
	CHECK(notify_count == 1);
	stats = unionpower_get_stats();
	CHECK(stats.cleanup_removed == 1);
	CHECK(pids[0].refs == 1);

	unionpower_run_cleanup();
	CHECK(!unionpower_get_frame_jank());
	CHECK(notify_count == 1);
	stats = unionpower_get_stats();
	CHECK(stats.cleanup_removed == 1);
	return 0;
}

static int cleanup_requeues_after_idle_case(void)
{
	struct unionpower_stats stats;

	setup_fixtures();
	CHECK(unionpower_core_start() == 0);
	host_reset_mod_delayed_work_calls();
	host_set_mod_delayed_work_result(false);
	unionpower_cleanup_work_fn(NULL);
	stats = unionpower_get_stats();
	CHECK(stats.queue_failure == 0);
	CHECK(host_mod_delayed_work_count() == 1);
	CHECK(host_last_delayed_work_delay() == HZ);
	return 0;
}

static int synchronous_queue_case(void)
{
	struct unionpower_identity caller;
	struct unionpower_identity target;
	struct unionpower_stats stats;
	int i;

	setup_fixtures();
	CHECK(unionpower_core_start() == 0);
	host_set_queue_work_synchronous(true);
	for (i = 0; i < UNIONPOWER_MAX_WORK + 1; i++) {
		caller.pid = get_pid(&pids[i]);
		caller.start_time = tasks[i].start_time;
		target.pid = get_pid(&pids[i]);
		target.start_time = tasks[i].start_time;
		CHECK(unionpower_dispatch(NOTIFIER_QUEUE, &caller, &target,
					  2000000, 0, i + 1) == 0);
		CHECK(unionpower_pending_work_count() == 0);
	}
	host_set_queue_work_synchronous(false);
	stats = unionpower_get_stats();
	CHECK(stats.pending_overflow == 0);
	CHECK(stats.stale_drop == UNIONPOWER_MAX_WORK + 1);
	unionpower_core_stop();
	CHECK(unionpower_pending_work_count() == 0);
	return 0;
}

static int pending_limit_case(void)
{
	struct unionpower_identity caller;
	struct unionpower_identity target;
	struct unionpower_stats stats;
	int i;
	int ret;

	setup_fixtures();
	CHECK(unionpower_core_start() == 0);
	for (i = 0; i < UNIONPOWER_MAX_WORK; i++) {
		caller.pid = get_pid(&pids[i]);
		caller.start_time = tasks[i].start_time;
		target.pid = get_pid(&pids[i]);
		target.start_time = tasks[i].start_time;
		ret = unionpower_dispatch(NOTIFIER_QUEUE, &caller, &target,
					  2000000, 0, i + 1);
		CHECK(ret == 0);
	}
	CHECK(unionpower_pending_work_count() == UNIONPOWER_MAX_WORK);
	caller.pid = get_pid(&pids[UNIONPOWER_MAX_WORK]);
	caller.start_time = tasks[UNIONPOWER_MAX_WORK].start_time;
	target.pid = get_pid(&pids[UNIONPOWER_MAX_WORK]);
	target.start_time = tasks[UNIONPOWER_MAX_WORK].start_time;
	ret = unionpower_dispatch(NOTIFIER_QUEUE, &caller, &target,
				  2000000, 0, 65);
	CHECK(ret == -EAGAIN);
	stats = unionpower_get_stats();
	CHECK(stats.pending_overflow == 1);
	CHECK(unionpower_pending_work_count() == UNIONPOWER_MAX_WORK);
	unionpower_core_stop();
	CHECK(unionpower_pending_work_count() == 0);
	CHECK(unionpower_record_count() == 0);
	return 0;
}

static int record_limit_case(void)
{
	struct unionpower_identity caller;
	struct unionpower_identity target;
	struct unionpower_stats stats;
	int i;

	setup_fixtures();
	CHECK(unionpower_core_start() == 0);
	for (i = 0; i < FIXTURE_COUNT; i++) {
		caller.pid = get_pid(&pids[i]);
		caller.start_time = tasks[i].start_time;
		target.pid = get_pid(&pids[i]);
		target.start_time = tasks[i].start_time;
		CHECK(unionpower_dispatch(NOTIFIER_VSYNC, &caller, &target,
					  1000000 + i * 16600,
					  16600, i + 1) == 0);
		host_run_queued_work();
	}
	CHECK(unionpower_record_count() == UNIONPOWER_MAX_RECORDS);
	stats = unionpower_get_stats();
	CHECK(stats.record_overflow == 1);
	unionpower_core_stop();
	CHECK(unionpower_record_count() == 0);
	return 0;
}

static int starttime_reuse_case(void)
{
	struct pid reused_pid;
	struct task_struct old_task;
	struct task_struct new_task;
	struct unionpower_identity caller;
	struct unionpower_identity target;
	struct unionpower_identity old_identity;
	struct unionpower_stats stats;

	memset(&reused_pid, 0, sizeof(reused_pid));
	memset(&old_task, 0, sizeof(old_task));
	memset(&new_task, 0, sizeof(new_task));
	reused_pid.nr = 5000;
	reused_pid.refs = 1;
	reused_pid.task = &old_task;
	old_task.pid = 5000;
	old_task.refs = 1;
	old_task.start_time = 5100;
	strcpy(old_task.comm, "RenderThread");
	old_task.thread_pid = &reused_pid;
	new_task.pid = 5000;
	new_task.refs = 1;
	new_task.start_time = 6100;
	strcpy(new_task.comm, "RenderThread");
	new_task.thread_pid = &reused_pid;
	host_pid_register(&reused_pid);
	current = &old_task;
	FRAME_MONITOR_ENABLE = 1;
	CHECK(unionpower_core_start() == 0);

	CHECK(unionpower_capture_current(&caller) == 0);
	CHECK(unionpower_capture_pid(reused_pid.nr, &target));
	CHECK(unionpower_dispatch(NOTIFIER_VSYNC, &caller, &target,
				  7000000, 16600, 1) == 0);
	host_run_queued_work();
	CHECK(unionpower_record_count() == 1);

	CHECK(unionpower_capture_current(&caller) == 0);
	CHECK(unionpower_capture_pid(reused_pid.nr, &target));
	CHECK(unionpower_dispatch(NOTIFIER_QUEUE, &caller, &target,
				  7021000, 0, 1) == 0);
	reused_pid.task = &new_task;
	current = &new_task;
	host_run_queued_work();
	stats = unionpower_get_stats();
	CHECK(stats.stale_drop == 1);

	old_identity.pid = &reused_pid;
	old_identity.start_time = old_task.start_time;
	CHECK(unionpower_lookup_record(&old_identity));
	unionpower_run_cleanup();
	CHECK(!unionpower_lookup_record(&old_identity));
	CHECK(unionpower_record_count() == 0);
	stats = unionpower_get_stats();
	CHECK(stats.cleanup_removed == 1);

	CHECK(unionpower_capture_current(&caller) == 0);
	CHECK(unionpower_capture_pid(reused_pid.nr, &target));
	CHECK(unionpower_dispatch(NOTIFIER_VSYNC, &caller, &target,
				  8000000, 16600, 2) == 0);
	host_run_queued_work();
	CHECK(unionpower_record_count() == 1);
	unionpower_core_stop();
	CHECK(unionpower_record_count() == 0);
	CHECK(reused_pid.refs == 1);
	CHECK(old_task.refs == 1);
	CHECK(new_task.refs == 1);
	return 0;
}

static int run_case(void)
{
	CHECK(cleanup_recalculates_jank_case() == 0);
	reset_case();
	CHECK(cleanup_requeues_after_idle_case() == 0);
	reset_case();
	CHECK(synchronous_queue_case() == 0);
	reset_case();
	CHECK(pending_limit_case() == 0);
	reset_case();
	CHECK(record_limit_case() == 0);
	reset_case();
	CHECK(starttime_reuse_case() == 0);
	reset_case();
	return 0;
}

int main(void)
{
	int ret = run_case();

	if (ret)
		return ret;
	puts("PASS: cycle3 work/record limits, stable identity cleanup, stop");
	return 0;
}
