// SPDX-License-Identifier: GPL-2.0-only
/*
 * Conservative page-based adaptation of Xiaomi's mi_rmap_efficiency policy.
 * Only ordinary file pages are eligible; pressure/allocator checks live in
 * vmscan. Both KEEP and ACTIVATE consume the same bounded per-scan budget.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/sizes.h>
#include <linux/sysfs.h>

#include "mi_rmap_efficiency.h"

#define MI_RMAP_MAX_PROTECTED_PAGES (SZ_16M >> PAGE_SHIFT)

static unsigned int mi_mapcount_thres __read_mostly = 20;
static bool mi_rmap_enabled __read_mostly;

static unsigned int mi_keep_threshold(unsigned int threshold, int priority)
{
	if (priority <= DEF_PRIORITY / 2)
		return max(threshold >> 2, 2U);
	return max(threshold >> 1, 2U);
}

enum mi_rmap_control mi_rmap_check(struct page *page, unsigned long nr_scanned,
				 int priority, unsigned long *nr_protected)
{
	unsigned int threshold = READ_ONCE(mi_mapcount_thres);
	unsigned int mapcount;
	enum mi_rmap_control control;

	if (!READ_ONCE(mi_rmap_enabled) || !page || !nr_protected ||
	    priority <= DEF_PRIORITY / 2 || PageCompound(page) ||
	    PageAnon(page) || PageSwapBacked(page) || !page_mapped(page))
		return MI_RMAP_RECLAIM;
	if (*nr_protected >= MI_RMAP_MAX_PROTECTED_PAGES)
		return MI_RMAP_RECLAIM;
	if (priority < DEF_PRIORITY - 2 &&
	    *nr_protected >= max(1UL, nr_scanned >> 4))
		return MI_RMAP_RECLAIM;

	mapcount = page_mapcount(page);
	if (mapcount >= threshold)
		control = MI_RMAP_ACTIVATE;
	else if (priority != DEF_PRIORITY &&
		 mapcount >= mi_keep_threshold(threshold, priority))
		control = MI_RMAP_KEEP;
	else
		return MI_RMAP_RECLAIM;

	(*nr_protected)++;
	return control;
}

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(mi_rmap_enabled));
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool enabled;
	int ret = kstrtobool(buf, &enabled);

	if (ret)
		return ret;
	WRITE_ONCE(mi_rmap_enabled, enabled);
	return count;
}

static ssize_t mapcount_thres_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(mi_mapcount_thres));
}

static ssize_t mapcount_thres_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int threshold;
	int ret = kstrtouint(buf, 0, &threshold);

	if (ret)
		return ret;
	if (threshold < 2 || threshold > INT_MAX)
		return -EINVAL;
	WRITE_ONCE(mi_mapcount_thres, threshold);
	return count;
}

static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);
static struct kobj_attribute mapcount_thres_attr = __ATTR_RW(mapcount_thres);
static struct attribute *mi_rmap_attrs[] = {
	&enabled_attr.attr,
	&mapcount_thres_attr.attr,
	NULL,
};
static const struct attribute_group mi_rmap_attr_group = {
	.attrs = mi_rmap_attrs,
};

static int __init mi_rmap_efficiency_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("mi_rmap_efficiency", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &mi_rmap_attr_group);
	if (ret) {
		kobject_put(kobj);
		return ret;
	}
	WRITE_ONCE(mi_rmap_enabled, true);
	return 0;
}
late_initcall(mi_rmap_efficiency_init);
