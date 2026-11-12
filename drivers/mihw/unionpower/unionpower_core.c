// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Xiaomi Inc.
 *
 * Adapted from MiCode/Xiaomi_Kernel_OpenSource branch bixi-v-oss,
 * commit 56295df675b92553f83bbf333f88b0c6ee8fa7bc,
 * drivers/mihw/unionpower/ subtree.
 */
#ifdef UNIONPOWER_HOST_TEST
#include "unionpower.h"
#else
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "unionpower.h"
#endif

bool frame_jank;

int VSYNC_GAP = UNIONPOWER_VSYNC_GAP_DEFAULT;
module_param(VSYNC_GAP, int, 0644);
MODULE_PARM_DESC(VSYNC_GAP, "unionpower vsync gap in microseconds");

int VSYNC_TIMEOUT = UNIONPOWER_VSYNC_TIMEOUT_DEFAULT;
module_param(VSYNC_TIMEOUT, int, 0644);
MODULE_PARM_DESC(VSYNC_TIMEOUT, "unionpower idle record timeout multiplier");

int JANK_TIMEOUT = UNIONPOWER_JANK_TIMEOUT_DEFAULT;
module_param(JANK_TIMEOUT, int, 0644);
MODULE_PARM_DESC(JANK_TIMEOUT, "unionpower active jank timeout in microseconds");

int JANK_LIMIT = UNIONPOWER_JANK_LIMIT_DEFAULT;
module_param(JANK_LIMIT, int, 0644);
MODULE_PARM_DESC(JANK_LIMIT, "unionpower continuous jank limit");

int NICE_MORE_LIMIT = UNIONPOWER_NICE_MORE_LIMIT_DEFAULT;
module_param(NICE_MORE_LIMIT, int, 0644);
MODULE_PARM_DESC(NICE_MORE_LIMIT, "unionpower continuous nice limit");

static DEFINE_MUTEX(records_lock);
static DEFINE_SPINLOCK(submission_lock);
static LIST_HEAD(buffer_record_list);
static struct workqueue_struct *queue_buffer_wq;
static struct delayed_work cleanup_work;
static bool unionpower_active;
static int pending_work_count;
static struct unionpower_stats unionpower_stats;
static int (*unionpower_writer)(enum WORK_PUSH_TYPE type,
				struct unionpower_identity *caller,
				struct unionpower_identity *target,
				s64 frame_time_us, s64 frame_interval_us,
				u64 frame_id);

static void unionpower_stat_increment(u64 *counter)
{
	unsigned long flags;

	spin_lock_irqsave(&submission_lock, flags);
	(*counter)++;
	spin_unlock_irqrestore(&submission_lock, flags);
}

static bool
unionpower_identity_matches(const struct unionpower_identity *identity)
{
	struct task_struct *task;
	bool matches;

	if (!identity->pid)
		return false;
	task = get_pid_task(identity->pid, PIDTYPE_PID);
	if (!task)
		return false;
	matches = task->start_time == identity->start_time;
	put_task_struct(task);
	return matches;
}

static struct buffer_record *
unionpower_find_record_locked(const struct unionpower_identity *identity)
{
	struct buffer_record *record;

	list_for_each_entry(record, &buffer_record_list, list) {
		if (record->target.pid == identity->pid &&
		    record->target.start_time == identity->start_time)
			return record;
	}
	return NULL;
}

static void unionpower_init_buffer(struct buffer_record *record,
				   const struct unionpower_identity *target,
				   s64 vsync_interval, s64 vsync_time,
				   s64 vsync_id)
{
	struct list_head list = record->list;

	memset(record, 0, sizeof(*record));
	record->list = list;
	record->pid = pid_nr(target->pid);
	record->target.pid = get_pid(target->pid);
	record->target.start_time = target->start_time;
	record->vsync_count = 1;
	record->vsync_record_list[0].vsync_duration = vsync_interval;
	record->vsync_record_list[0].vsync_delay = false;
	record->vsync_record_list[0].vsync_id = vsync_id;
	record->vsync_record_list[0].vsync_interval = vsync_interval;
	record->vsync_record_list[0].vsync_time = vsync_time;
	record->jank_happened = false;
	record->history_buffer_interval = vsync_interval;
}

static void
unionpower_recalculate_frame_jank_locked(s64 cur_time)
{
	struct buffer_record *record;
	s64 cutoff;
	bool total_status = false;
	int last_index;

	cutoff = cur_time - (JANK_TIMEOUT ? JANK_TIMEOUT :
			     UNIONPOWER_JANK_TIMEOUT_DEFAULT);
	list_for_each_entry(record, &buffer_record_list, list) {
		if (!record->jank_happened)
			continue;
		last_index = (record->vsync_count - 1) % UNIONPOWER_RECORD_SIZE;
		if (cutoff <= record->vsync_record_list[last_index].vsync_time -
		    record->history_buffer_interval) {
			total_status = true;
			break;
		}
	}
	if (total_status != READ_ONCE(frame_jank)) {
		WRITE_ONCE(frame_jank, total_status);
		unionpower_sysfs_notify("frame_jank");
	}
}

static void
unionpower_refresh_frame_load_locked(bool new_status, s64 cur_time)
{
	if (new_status == READ_ONCE(frame_jank))
		return;
	if (new_status) {
		WRITE_ONCE(frame_jank, true);
		unionpower_sysfs_notify("frame_jank");
		return;
	}
	unionpower_recalculate_frame_jank_locked(cur_time);
}

static void unionpower_update_vsync_locked(struct buffer_record *record,
					   s64 vsync_interval,
					   s64 vsync_time, s64 vsync_id,
					   bool jank)
{
	struct vsync_record last_vsync;
	s64 duration;
	int cur_index;
	int last_index;

	last_index = (record->vsync_count - 1) % UNIONPOWER_RECORD_SIZE;
	last_vsync = record->vsync_record_list[last_index];
	if (record->vsync_count - record->frame_count >=
	    UNIONPOWER_RECORD_SIZE) {
		cur_index = 1;
		record->vsync_record_list[0] = last_vsync;
		record->vsync_count = 2;
		record->frame_count = 0;
		record->continus_nice = 0;
		record->continus_jank = 0;
	} else {
		cur_index = record->vsync_count % UNIONPOWER_RECORD_SIZE;
		record->vsync_count++;
	}

	duration = vsync_time - last_vsync.vsync_time;
	record->vsync_record_list[cur_index].vsync_duration = duration;
	record->vsync_record_list[cur_index].vsync_delay =
		record->frame_count != record->vsync_count - 1 &&
		duration > last_vsync.vsync_interval + VSYNC_GAP;
	record->vsync_record_list[cur_index].vsync_id = vsync_id;
	record->vsync_record_list[cur_index].vsync_interval = vsync_interval;
	record->vsync_record_list[cur_index].vsync_time = vsync_time;

	if (!jank && record->vsync_record_list[cur_index].vsync_delay) {
		record->history_buffer_interval = duration;
		record->jank_happened = true;
	} else {
		record->jank_happened = jank;
	}
	unionpower_refresh_frame_load_locked(record->jank_happened,
					     vsync_time);
}

static void unionpower_update_buffer_locked(struct buffer_record *record,
					    s64 frame_time, s64 frame_id)
{
	struct vsync_record cur_vsync;
	s64 queue_buffer_interval;
	bool jank_status;
	int current_index;
	int last_index;

	if (record->frame_count > 0) {
		last_index = (record->frame_count - 1) %
			     UNIONPOWER_RECORD_SIZE;
		cur_vsync = record->vsync_record_list[last_index];
		if (cur_vsync.vsync_id == frame_id) {
			queue_buffer_interval =
				frame_time - cur_vsync.vsync_time;
			record->history_buffer_interval =
				(cur_vsync.vsync_duration +
				 queue_buffer_interval) / 2;
			record->last_queue_buffer_time[last_index] =
				frame_time;
			if (queue_buffer_interval >=
			    cur_vsync.vsync_interval) {
				if (record->continus_nice > 0) {
					record->continus_jank = 1;
					record->continus_nice = 0;
				}
			}
			jank_status = cur_vsync.vsync_delay ||
				(record->continus_jank > JANK_LIMIT &&
				 record->continus_nice < NICE_MORE_LIMIT);
			record->jank_happened = jank_status;
			unionpower_refresh_frame_load_locked(jank_status,
							     cur_vsync.vsync_time);
			return;
		}
	}

	current_index = record->frame_count % UNIONPOWER_RECORD_SIZE;
	cur_vsync = record->vsync_record_list[current_index];
	while (record->frame_count < record->vsync_count &&
	       cur_vsync.vsync_id != frame_id) {
		record->last_queue_buffer_time[current_index] =
			cur_vsync.vsync_time;
		record->history_buffer_interval =
			(record->history_buffer_interval +
			 cur_vsync.vsync_interval) / 2;
		if (!cur_vsync.vsync_delay) {
			record->continus_nice++;
			record->history_buffer_interval =
				(record->history_buffer_interval +
				 cur_vsync.vsync_interval) / 2;
		} else {
			record->history_buffer_interval =
				(record->history_buffer_interval +
				 cur_vsync.vsync_duration) / 2;
			if (record->continus_nice > 0) {
				record->continus_jank = 1;
				record->continus_nice = 0;
			} else {
				record->continus_jank++;
			}
		}
		record->frame_count++;
		current_index =
			record->frame_count % UNIONPOWER_RECORD_SIZE;
		cur_vsync = record->vsync_record_list[current_index];
	}

	if (record->frame_count >= record->vsync_count ||
	    cur_vsync.vsync_id != frame_id)
		return;

	record->last_queue_buffer_time[current_index] = frame_time;
	queue_buffer_interval = frame_time - cur_vsync.vsync_time;
	record->history_buffer_interval =
		(record->history_buffer_interval + queue_buffer_interval) / 2;
	if (queue_buffer_interval < cur_vsync.vsync_interval) {
		record->continus_nice++;
	} else if (record->continus_nice > 0) {
		record->continus_jank = 1;
		record->continus_nice = 0;
	} else {
		record->continus_jank++;
	}
	record->frame_count++;
	jank_status = cur_vsync.vsync_delay ||
		(record->continus_jank > JANK_LIMIT &&
		 record->continus_nice < NICE_MORE_LIMIT);
	record->jank_happened = jank_status;
	unionpower_refresh_frame_load_locked(jank_status,
					     cur_vsync.vsync_time);
}

static bool unionpower_process_queue(struct WORK_MSG *msg)
{
	struct buffer_record *record;
	bool processed = false;

	if (!unionpower_identity_matches(&msg->caller) ||
	    !unionpower_identity_matches(&msg->target))
		return false;

	mutex_lock(&records_lock);
	record = unionpower_find_record_locked(&msg->target);
	if (!record)
		goto out;
	if (!record->render_bound) {
		record->render.pid = get_pid(msg->caller.pid);
		record->render.start_time = msg->caller.start_time;
		record->render_bound = true;
	} else if (record->render.pid != msg->caller.pid ||
		   record->render.start_time != msg->caller.start_time) {
		goto out;
	}
	unionpower_update_buffer_locked(record, msg->frame_time_us,
					msg->frame_id);
	processed = true;
out:
	mutex_unlock(&records_lock);
	return processed;
}

static bool
unionpower_reuse_record_locked(const struct unionpower_identity *target,
			       s64 current_time, s64 vsync_interval,
			       s64 vsync_id)
{
	struct buffer_record *record;
	struct vsync_record last_vsync;
	s64 timeout;
	int last_index;

	list_for_each_entry(record, &buffer_record_list, list) {
		if (record->target.pid == target->pid)
			continue;
		if (record->frame_count < record->vsync_count)
			continue;
		last_index = (record->vsync_count - 1) %
			     UNIONPOWER_RECORD_SIZE;
		last_vsync = record->vsync_record_list[last_index];
		timeout = (record->history_buffer_interval >
			   last_vsync.vsync_interval ?
			   record->history_buffer_interval :
			   last_vsync.vsync_interval) * VSYNC_TIMEOUT;
		if (current_time - last_vsync.vsync_time <= timeout)
			continue;
		if (record->jank_happened) {
			record->jank_happened = false;
			unionpower_refresh_frame_load_locked(false,
							     current_time);
		}
		unionpower_identity_put(&record->target);
		unionpower_identity_put(&record->render);
		record->render_bound = false;
		unionpower_init_buffer(record, target, vsync_interval,
				       current_time, vsync_id);
		return true;
	}
	return false;
}

static bool unionpower_process_vsync(struct WORK_MSG *msg)
{
	struct buffer_record *record;
	int count = 0;
	bool processed = true;

	if (!unionpower_identity_matches(&msg->target))
		return false;

	mutex_lock(&records_lock);
	record = unionpower_find_record_locked(&msg->target);
	if (record) {
		unionpower_update_vsync_locked(record,
					       msg->frame_interval_us,
					       msg->frame_time_us,
					       msg->frame_id,
					       record->jank_happened);
		goto out;
	}
	list_for_each_entry(record, &buffer_record_list, list)
		count++;
	if (count >= UNIONPOWER_MAX_RECORDS) {
		if (!unionpower_reuse_record_locked(&msg->target,
						    msg->frame_time_us,
						    msg->frame_interval_us,
						    msg->frame_id)) {
			unionpower_stat_increment(&unionpower_stats.record_overflow);
			processed = false;
			goto out;
		}
		goto out;
	}
	record = kzalloc(sizeof(*record), GFP_KERNEL);
	if (!record) {
		processed = false;
		goto out;
	}
	unionpower_init_buffer(record, &msg->target,
			       msg->frame_interval_us, msg->frame_time_us,
			       msg->frame_id);
	list_add_tail(&record->list, &buffer_record_list);
out:
	mutex_unlock(&records_lock);
	return processed;
}

int unionpower_core_start(void)
{
	unsigned long flags;

	BUILD_BUG_ON(sizeof(struct _BUFFER_PACKAGE) != 48);
	queue_buffer_wq = alloc_ordered_workqueue("union_power_buffer_wq",
						  WQ_MEM_RECLAIM | WQ_HIGHPRI);
	if (!queue_buffer_wq)
		return -ENOMEM;
	INIT_LIST_HEAD(&buffer_record_list);
	INIT_DELAYED_WORK(&cleanup_work, unionpower_cleanup_work_fn);
	WRITE_ONCE(frame_jank, false);
	spin_lock_irqsave(&submission_lock, flags);
	pending_work_count = 0;
	memset(&unionpower_stats, 0, sizeof(unionpower_stats));
	WRITE_ONCE(unionpower_writer, unionpower_dispatch);
	WRITE_ONCE(unionpower_active, true);
	spin_unlock_irqrestore(&submission_lock, flags);
	if (!queue_delayed_work(queue_buffer_wq, &cleanup_work, HZ)) {
		unionpower_stat_increment(&unionpower_stats.queue_failure);
		unionpower_core_quiesce();
		unionpower_core_destroy();
		return -EIO;
	}
	return 0;
}

void unionpower_core_quiesce(void)
{
	struct buffer_record *record;
	struct buffer_record *next;
	unsigned long flags;

	spin_lock_irqsave(&submission_lock, flags);
	WRITE_ONCE(unionpower_writer, NULL);
	WRITE_ONCE(unionpower_active, false);
	spin_unlock_irqrestore(&submission_lock, flags);
	if (queue_buffer_wq) {
		cancel_delayed_work_sync(&cleanup_work);
		flush_workqueue(queue_buffer_wq);
	}
	mutex_lock(&records_lock);
	list_for_each_entry_safe(record, next, &buffer_record_list, list) {
		list_del_init(&record->list);
		unionpower_identity_put(&record->target);
		unionpower_identity_put(&record->render);
		kfree(record);
	}
	INIT_LIST_HEAD(&buffer_record_list);
	mutex_unlock(&records_lock);
	spin_lock_irqsave(&submission_lock, flags);
	pending_work_count = 0;
	spin_unlock_irqrestore(&submission_lock, flags);
	WRITE_ONCE(frame_jank, false);
}

void unionpower_core_destroy(void)
{
	if (queue_buffer_wq) {
		destroy_workqueue(queue_buffer_wq);
		queue_buffer_wq = NULL;
	}
}

void unionpower_core_stop(void)
{
	unionpower_core_quiesce();
	unionpower_core_destroy();
}

bool unionpower_core_active(void)
{
	return READ_ONCE(unionpower_active);
}

int unionpower_capture_current(struct unionpower_identity *identity)
{
	identity->pid = get_task_pid(current, PIDTYPE_PID);
	identity->start_time = current->start_time;
	return identity->pid ? 0 : -ESRCH;
}

bool unionpower_capture_pid(u32 pid, struct unionpower_identity *identity)
{
	struct task_struct *task;
	struct pid *pid_ref;

	pid_ref = find_get_pid(pid);
	if (!pid_ref)
		return false;
	task = get_pid_task(pid_ref, PIDTYPE_PID);
	if (!task) {
		put_pid(pid_ref);
		return false;
	}
	identity->pid = pid_ref;
	identity->start_time = task->start_time;
	put_task_struct(task);
	return true;
}

bool unionpower_current_is_render_thread(void)
{
	char comm[TASK_COMM_LEN];

	get_task_comm(comm, current);
	return !strcmp(comm, "RenderThread");
}

void unionpower_identity_put(struct unionpower_identity *identity)
{
	put_pid(identity->pid);
	identity->pid = NULL;
	identity->start_time = 0;
}

#ifdef UNIONPOWER_HOST_TEST
struct buffer_record *
unionpower_lookup_record(const struct unionpower_identity *identity)
{
	struct buffer_record *record;

	mutex_lock(&records_lock);
	record = unionpower_find_record_locked(identity);
	mutex_unlock(&records_lock);
	return record;
}
#endif

int unionpower_dispatch(enum WORK_PUSH_TYPE type,
			struct unionpower_identity *caller,
			struct unionpower_identity *target,
			s64 frame_time_us, s64 frame_interval_us, u64 frame_id)
{
	struct WORK_MSG *msg;
	unsigned long flags;

	spin_lock_irqsave(&submission_lock, flags);
	if (!READ_ONCE(unionpower_active) || !queue_buffer_wq) {
		spin_unlock_irqrestore(&submission_lock, flags);
		unionpower_identity_put(caller);
		unionpower_identity_put(target);
		return -ENODEV;
	}
	if (pending_work_count >= UNIONPOWER_MAX_WORK) {
		unionpower_stats.pending_overflow++;
		spin_unlock_irqrestore(&submission_lock, flags);
		unionpower_identity_put(caller);
		unionpower_identity_put(target);
		return -EAGAIN;
	}
	msg = kzalloc(sizeof(*msg), GFP_ATOMIC);
	if (!msg) {
		unionpower_stats.queue_failure++;
		spin_unlock_irqrestore(&submission_lock, flags);
		unionpower_identity_put(caller);
		unionpower_identity_put(target);
		return -ENOMEM;
	}
	msg->push_type = type;
	msg->caller = *caller;
	msg->target = *target;
	caller->pid = NULL;
	target->pid = NULL;
	msg->frame_time_us = frame_time_us;
	msg->frame_interval_us = frame_interval_us;
	msg->frame_id = frame_id;
	INIT_WORK(&msg->work, unionpower_work_fn);
	pending_work_count++;
	if (!queue_work(queue_buffer_wq, &msg->work)) {
		pending_work_count--;
		unionpower_stats.queue_failure++;
		spin_unlock_irqrestore(&submission_lock, flags);
		unionpower_identity_put(&msg->caller);
		unionpower_identity_put(&msg->target);
		kfree(msg);
		return -EAGAIN;
	}
	spin_unlock_irqrestore(&submission_lock, flags);
	return 0;
}

int unionpower_submit_event(enum WORK_PUSH_TYPE type,
			    struct unionpower_identity *caller,
			    struct unionpower_identity *target,
			    s64 frame_time_us, s64 frame_interval_us,
			    u64 frame_id)
{
	int (*writer)(enum WORK_PUSH_TYPE type,
		      struct unionpower_identity *caller,
		      struct unionpower_identity *target,
		      s64 frame_time_us, s64 frame_interval_us,
		      u64 frame_id);

	writer = READ_ONCE(unionpower_writer);
	if (!writer) {
		unionpower_identity_put(caller);
		unionpower_identity_put(target);
		return -ENODEV;
	}
	return writer(type, caller, target, frame_time_us, frame_interval_us,
		      frame_id);
}

void unionpower_work_fn(struct work_struct *work)
{
	struct WORK_MSG *msg = container_of(work, struct WORK_MSG, work);
	bool processed = false;
	unsigned long flags;

	switch (msg->push_type) {
	case NOTIFIER_QUEUE:
		processed = unionpower_process_queue(msg);
		break;
	case NOTIFIER_VSYNC:
		processed = unionpower_process_vsync(msg);
		break;
	default:
		break;
	}

	spin_lock_irqsave(&submission_lock, flags);
	if (pending_work_count > 0)
		pending_work_count--;
	if (!processed)
		unionpower_stats.stale_drop++;
	spin_unlock_irqrestore(&submission_lock, flags);
	unionpower_identity_put(&msg->caller);
	unionpower_identity_put(&msg->target);
	kfree(msg);
}

static void unionpower_cleanup_records_locked(void)
{
	struct buffer_record *record;
	struct buffer_record *next;
	bool removed = false;

	list_for_each_entry_safe(record, next, &buffer_record_list, list) {
		if (unionpower_identity_matches(&record->target) &&
		    (!record->render_bound ||
		     unionpower_identity_matches(&record->render)))
			continue;
		list_del_init(&record->list);
		unionpower_identity_put(&record->target);
		unionpower_identity_put(&record->render);
		kfree(record);
		unionpower_stat_increment(&unionpower_stats.cleanup_removed);
		removed = true;
	}
	if (removed)
		unionpower_recalculate_frame_jank_locked(ktime_to_us(ktime_get()));
}

void unionpower_cleanup_work_fn(struct work_struct *work)
{
	(void)work;
	mutex_lock(&records_lock);
	unionpower_cleanup_records_locked();
	mutex_unlock(&records_lock);
	if (READ_ONCE(unionpower_active) && queue_buffer_wq)
		mod_delayed_work(queue_buffer_wq, &cleanup_work, HZ);
}

bool unionpower_get_frame_jank(void)
{
	return READ_ONCE(frame_jank);
}

int unionpower_record_count(void)
{
	struct buffer_record *record;
	int count = 0;

	mutex_lock(&records_lock);
	list_for_each_entry(record, &buffer_record_list, list)
		count++;
	mutex_unlock(&records_lock);
	return count;
}

int unionpower_pending_work_count(void)
{
	int count;
	unsigned long flags;

	spin_lock_irqsave(&submission_lock, flags);
	count = pending_work_count;
	spin_unlock_irqrestore(&submission_lock, flags);
	return count;
}

struct unionpower_stats unionpower_get_stats(void)
{
	struct unionpower_stats stats;
	unsigned long flags;

	spin_lock_irqsave(&submission_lock, flags);
	stats = unionpower_stats;
	spin_unlock_irqrestore(&submission_lock, flags);
	return stats;
}

void unionpower_run_cleanup(void)
{
	mutex_lock(&records_lock);
	unionpower_cleanup_records_locked();
	mutex_unlock(&records_lock);
}
