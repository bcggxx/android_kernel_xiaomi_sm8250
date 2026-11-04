/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2024 Xiaomi Inc.
 *
 * Adapted from MiCode/Xiaomi_Kernel_OpenSource branch bixi-v-oss,
 * commit 56295df675b92553f83bbf333f88b0c6ee8fa7bc,
 * drivers/mihw/unionpower/ subtree.
 */
#ifndef _UNIONPOWER_UAPI_H
#define _UNIONPOWER_UAPI_H

#ifdef UNIONPOWER_HOST_TEST
#include <stdint.h>
#include <sys/ioctl.h>

typedef uint8_t __u8;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int64_t __s64;

struct timespec64 {
	int64_t tv_sec;
	int64_t tv_nsec;
};
#else
#include <linux/ioctl.h>
#include <linux/time64.h>
#include <linux/types.h>
#endif

/*
 * Native 64-bit ABI: 48 bytes. The two leading u32 fields share one 8-byte
 * unit and timespec64 is two 64-bit fields on the supported ARM64 target.
 */
struct _BUFFER_PACKAGE {
	__u32 pid;
	__u32 start;
	__u64 frame_time;
	__u64 frame_id;
	__s64 frame_interval;
	struct timespec64 current_timespec;
};

#define BUFFER_QUEUE	_IOW('g', 1, struct _BUFFER_PACKAGE)
#define BUFFER_VSYNC	_IOW('g', 5, struct _BUFFER_PACKAGE)

#endif /* _UNIONPOWER_UAPI_H */
