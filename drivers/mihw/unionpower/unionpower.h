/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Xiaomi Inc.
 *
 * Adapted from MiCode/Xiaomi_Kernel_OpenSource branch bixi-v-oss,
 * commit 56295df675b92553f83bbf333f88b0c6ee8fa7bc,
 * drivers/mihw/unionpower/ subtree.
 */
#ifndef _UNIONPOWER_H
#define _UNIONPOWER_H

#include "unionpower_uapi.h"

#ifdef UNIONPOWER_HOST_TEST
#include "tests/host_kernel.h"
#else
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#endif

#define UNIONPOWER_RECORD_SIZE		5
#define UNIONPOWER_MAX_WORK		64
#define UNIONPOWER_MAX_RECORDS		64

#define UNIONPOWER_VSYNC_GAP_DEFAULT	10000
#define UNIONPOWER_JANK_TIMEOUT_DEFAULT	(16600 * 5)
#define UNIONPOWER_VSYNC_TIMEOUT_DEFAULT	10
#define UNIONPOWER_JANK_LIMIT_DEFAULT	0
#define UNIONPOWER_NICE_MORE_LIMIT_DEFAULT	3

struct proc_dir_entry;

enum WORK_PUSH_TYPE {
	NOTIFIER_QUEUE = 0x01,
	NOTIFIER_VSYNC = 0x05,
};

struct unionpower_identity {
	struct pid *pid;
	u64 start_time;
};

struct WORK_MSG {
	enum WORK_PUSH_TYPE push_type;
	struct unionpower_identity caller;
	struct unionpower_identity target;
	s64 frame_time_us;
	s64 frame_interval_us;
	u64 frame_id;
	struct work_struct work;
};

struct vsync_record {
	bool vsync_delay;
	s64 vsync_id;
	s64 vsync_interval;
	s64 vsync_time;
	s64 vsync_duration;
};

struct buffer_record {
	u32 pid;
	struct unionpower_identity target;
	struct unionpower_identity render;
	bool render_bound;
	bool jank_happened;
	int vsync_count;
	struct vsync_record vsync_record_list[UNIONPOWER_RECORD_SIZE];
	s64 last_queue_buffer_time[UNIONPOWER_RECORD_SIZE];
	s64 history_buffer_interval;
	int frame_count;
	int continus_nice;
	int continus_jank;
	struct list_head list;
};

struct unionpower_stats {
	u64 pending_overflow;
	u64 record_overflow;
	u64 stale_drop;
	u64 queue_failure;
	u64 cleanup_removed;
};

extern bool frame_jank;
extern int FRAME_MONITOR_ENABLE;
extern int VSYNC_GAP;
extern int VSYNC_TIMEOUT;
extern int JANK_TIMEOUT;
extern int JANK_LIMIT;
extern int NICE_MORE_LIMIT;

int unionpower_core_start(void);
void unionpower_core_quiesce(void);
void unionpower_core_destroy(void);
void unionpower_core_stop(void);
bool unionpower_core_active(void);

int unionpower_capture_current(struct unionpower_identity *identity);
bool unionpower_capture_pid(u32 pid, struct unionpower_identity *identity);
bool unionpower_current_is_render_thread(void);
void unionpower_identity_put(struct unionpower_identity *identity);
#ifdef UNIONPOWER_HOST_TEST
struct buffer_record *
unionpower_lookup_record(const struct unionpower_identity *identity);
#endif

int unionpower_dispatch(enum WORK_PUSH_TYPE type,
			struct unionpower_identity *caller,
			struct unionpower_identity *target,
			s64 frame_time_us, s64 frame_interval_us, u64 frame_id);
int unionpower_submit_event(enum WORK_PUSH_TYPE type,
			    struct unionpower_identity *caller,
			    struct unionpower_identity *target,
			    s64 frame_time_us, s64 frame_interval_us,
			    u64 frame_id);
void unionpower_work_fn(struct work_struct *work);
void unionpower_cleanup_work_fn(struct work_struct *work);

bool unionpower_command_valid(unsigned int cmd);
int unionpower_ioctl_init(struct proc_dir_entry *parent);
void unionpower_ioctl_exit(void);
const struct file_operations *unionpower_ioctl_ops(void);

bool unionpower_get_frame_jank(void);
int unionpower_record_count(void);
int unionpower_pending_work_count(void);
struct unionpower_stats unionpower_get_stats(void);
void unionpower_run_cleanup(void);
void unionpower_sysfs_notify(const char *attr);

#endif /* _UNIONPOWER_H */
