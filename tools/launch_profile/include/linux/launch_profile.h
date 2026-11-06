/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LAUNCH_PROFILE_H
#define _UAPI_LAUNCH_PROFILE_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define DLP_VERSION 1
#define DLP_ENABLE 1
#define DLP_BEGIN 2
#define DLP_FINISH 3
#define DLP_ABORT 4
#define DLP_STATUS 5
#define DLP_SNAPSHOT 6
#define DLP_EXPORT_FILE 7
#define DLP_TRUNCATED 1
struct dlp_request {
	__u32 version, size, op, flags;
	__aligned_u64 epoch, cookie;
	__s32 binder_fd;
	__u32 binder_handle, tid, uid, lease_ms, index;
	__aligned_u64 reserved;
};
#define DLP_IOCTL _IOWR('D', 0x70, struct dlp_request)
/* Snapshot bytes are little endian; offsets and lengths are bytes. */
struct dlp_snapshot_header {
	__le32 magic, version, size, nr_files, nr_ranges, flags, page_shift, dropped;
};
struct dlp_snapshot_range {
	__le32 file_index, reserved;
	__le64 offset, length;
};
#endif
