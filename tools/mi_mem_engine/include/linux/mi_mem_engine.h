/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MI_MEM_ENGINE_H
#define _UAPI_LINUX_MI_MEM_ENGINE_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define MME_VERSION 1
#define MME_MAX_TASKS 4
#define MME_MAX_LEASE_MS 1000
struct mme_task {
	__s32 binder_fd;
	__u32 binder_handle;
	__u32 tid;
	__u32 uid;
};
struct mme_request {
	__u32 version;
	__u32 size;
	__u64 cookie;
	__u32 lease_ms;
	__u32 nr_tasks;
	struct mme_task tasks[MME_MAX_TASKS];
};
#define MME_BEGIN _IOW('M', 1, struct mme_request)
#define MME_END _IOW('M', 2, struct mme_request)
/* ENABLE uses lease_ms=0/1; all other payload fields zero. */
#define MME_ENABLE _IOW('M', 3, struct mme_request)
#endif
