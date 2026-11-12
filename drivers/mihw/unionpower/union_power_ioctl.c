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
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "unionpower.h"
#endif

int FRAME_MONITOR_ENABLE;
module_param(FRAME_MONITOR_ENABLE, int, 0644);
MODULE_PARM_DESC(FRAME_MONITOR_ENABLE,
		 "enable unionpower ioctl processing (0 or 1)");

static struct proc_dir_entry *ioctl_entry;

static int unionpower_ioctl_show(struct seq_file *m, void *v)
{
	return 0;
}

static int unionpower_ioctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, unionpower_ioctl_show, inode->i_private);
}

static long unionpower_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct _BUFFER_PACKAGE package;
	struct unionpower_identity caller;
	struct unionpower_identity target;
	s64 frame_time_us;
	s64 frame_interval_us;
	int ret;

	(void)file;
	if (!unionpower_command_valid(cmd))
		return -EINVAL;
	if (READ_ONCE(FRAME_MONITOR_ENABLE) != 1)
		return 0;
	if (copy_from_user(&package, (const void __user *)arg,
			   sizeof(package)))
		return -EFAULT;

	if (cmd == BUFFER_QUEUE && !unionpower_current_is_render_thread())
		return 0;
	if (cmd == BUFFER_VSYNC && current->pid != package.pid)
		return 0;

	ret = unionpower_capture_current(&caller);
	if (ret)
		return 0;
	if (cmd == BUFFER_VSYNC) {
		target = caller;
		get_pid(target.pid);
	} else if (!unionpower_capture_pid(package.pid, &target)) {
		unionpower_identity_put(&caller);
		return 0;
	}

	frame_time_us =
		ktime_to_us(timespec64_to_ns(&package.current_timespec));
	frame_interval_us = cmd == BUFFER_VSYNC ?
		ktime_to_us(package.frame_interval) : 0;
	ret = unionpower_submit_event(cmd == BUFFER_QUEUE ? NOTIFIER_QUEUE :
				      NOTIFIER_VSYNC, &caller, &target,
				      frame_time_us, frame_interval_us,
				      package.frame_id);
	return ret == -ENODEV ? 0 : ret;
}

#ifdef CONFIG_COMPAT
static long unionpower_compat_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	return -ENOTTY;
}
#endif

static const struct file_operations unionpower_ioctl_operations = {
	.open = unionpower_ioctl_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.unlocked_ioctl = unionpower_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = unionpower_compat_ioctl,
#endif
};

bool unionpower_command_valid(unsigned int cmd)
{
	return cmd == BUFFER_QUEUE || cmd == BUFFER_VSYNC;
}

const struct file_operations *unionpower_ioctl_ops(void)
{
	return &unionpower_ioctl_operations;
}

int unionpower_ioctl_init(struct proc_dir_entry *parent)
{
	ioctl_entry = proc_create("ioctl", 0664, parent,
				  &unionpower_ioctl_operations);
	return ioctl_entry ? 0 : -ENOMEM;
}

void unionpower_ioctl_exit(void)
{
	if (ioctl_entry) {
		proc_remove(ioctl_entry);
		ioctl_entry = NULL;
	}
}
