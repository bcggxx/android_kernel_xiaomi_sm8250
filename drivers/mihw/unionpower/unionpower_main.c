// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Xiaomi Inc.
 *
 * Adapted from MiCode/Xiaomi_Kernel_OpenSource branch bixi-v-oss,
 * commit 56295df675b92553f83bbf333f88b0c6ee8fa7bc,
 * drivers/mihw/unionpower/ subtree.
 */
#include <asm/page.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "unionpower.h"

static struct kobject *unionpower_kobj;
static struct proc_dir_entry *unionpower_proc_root;

static ssize_t frame_jank_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	(void)kobj;
	(void)attr;
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 unionpower_get_frame_jank() ? 1 : 0);
}

static ssize_t frame_jank_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	(void)kobj;
	(void)attr;
	(void)buf;
	return count;
}

static struct kobj_attribute frame_jank_attr =
	__ATTR(frame_jank, 0664, frame_jank_show, frame_jank_store);

void unionpower_sysfs_notify(const char *attr)
{
	struct kobject *kobj = READ_ONCE(unionpower_kobj);

	if (kobj)
		sysfs_notify(kobj, NULL, attr);
}

static int unionpower_sysfs_init(void)
{
	int ret;

	unionpower_kobj = kobject_create_and_add("unionpower", NULL);
	if (!unionpower_kobj)
		return -ENOMEM;
	ret = sysfs_create_file(unionpower_kobj, &frame_jank_attr.attr);
	if (ret) {
		kobject_del(unionpower_kobj);
		kobject_put(unionpower_kobj);
		unionpower_kobj = NULL;
		return ret;
	}
	return 0;
}

static void unionpower_sysfs_exit(void)
{
	if (!unionpower_kobj)
		return;
	sysfs_remove_file(unionpower_kobj, &frame_jank_attr.attr);
	kobject_del(unionpower_kobj);
	kobject_put(unionpower_kobj);
	unionpower_kobj = NULL;
}

static int unionpower_proc_init(void)
{
	int ret;

	unionpower_proc_root = proc_mkdir("unionpower", NULL);
	if (!unionpower_proc_root)
		return -ENOMEM;
	ret = unionpower_ioctl_init(unionpower_proc_root);
	if (ret) {
		proc_remove(unionpower_proc_root);
		unionpower_proc_root = NULL;
		return ret;
	}
	return 0;
}

static void unionpower_proc_exit(void)
{
	unionpower_ioctl_exit();
	if (unionpower_proc_root) {
		proc_remove(unionpower_proc_root);
		unionpower_proc_root = NULL;
	}
}

static int unionpower_setup(void)
{
	int ret;

	ret = unionpower_sysfs_init();
	if (ret)
		return ret;
	ret = unionpower_proc_init();
	if (ret)
		goto err_sysfs;
	ret = unionpower_core_start();
	if (ret)
		goto err_proc;
	return 0;

err_proc:
	unionpower_proc_exit();
err_sysfs:
	unionpower_sysfs_exit();
	return ret;
}

static void unionpower_teardown(void)
{
	unionpower_core_quiesce();
	unionpower_proc_exit();
	unionpower_sysfs_exit();
	unionpower_core_destroy();
}

static int __init unionpower_init(void)
{
	return unionpower_setup();
}

static void __exit unionpower_exit(void)
{
	unionpower_teardown();
}

module_init(unionpower_init);
module_exit(unionpower_exit);
MODULE_LICENSE("GPL");
