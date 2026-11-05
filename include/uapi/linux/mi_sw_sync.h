/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2012 Google, Inc.
 */

#ifndef _UAPI_LINUX_MI_SW_SYNC_H
#define _UAPI_LINUX_MI_SW_SYNC_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * struct sw_sync_create_fence_data - create a fence on a software timeline
 * @value: sequence number of the new fence
 * @name: optional debug name for the timeline
 * @fence: returns the sync_file fd for the new fence
 */
struct sw_sync_create_fence_data {
	__u32 value;
	char name[32];
	__s32 fence;
};

#define SW_SYNC_IOC_MAGIC 'W'

#define SW_SYNC_IOC_CREATE_FENCE	_IOWR(SW_SYNC_IOC_MAGIC, 0, \
		struct sw_sync_create_fence_data)

#define SW_SYNC_IOC_INC			_IOW(SW_SYNC_IOC_MAGIC, 1, __u32)

#endif /* _UAPI_LINUX_MI_SW_SYNC_H */
