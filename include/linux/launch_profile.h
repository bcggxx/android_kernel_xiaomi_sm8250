/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_LAUNCH_PROFILE_H
#define _LINUX_LAUNCH_PROFILE_H
#include <linux/types.h>
struct file;
#ifdef CONFIG_LAUNCH_PROFILE
void dlp_collect(struct file *file, u64 offset, u64 length);
#else
static inline void dlp_collect(struct file *file, u64 offset, u64 length) {}
#endif
#endif
