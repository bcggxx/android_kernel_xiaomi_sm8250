// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi software sync device
 *
 * Copyright (C) 2012 Google, Inc.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

#include <uapi/linux/mi_sw_sync.h>

#include "sync_debug.h"

static const struct dma_fence_ops mi_sw_sync_fence_ops;

static inline struct sync_pt *dma_fence_to_sync_pt(struct dma_fence *fence)
{
	if (fence->ops != &mi_sw_sync_fence_ops)
		return NULL;
	return container_of(fence, struct sync_pt, base);
}

static struct sync_timeline *mi_sw_sync_timeline_create(const char *name)
{
	struct sync_timeline *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return NULL;

	kref_init(&obj->kref);
	obj->context = dma_fence_context_alloc(1);
	strscpy(obj->name, name, sizeof(obj->name));

	obj->pt_tree = RB_ROOT;
	INIT_LIST_HEAD(&obj->pt_list);
	spin_lock_init(&obj->lock);

	return obj;
}

static void mi_sw_sync_timeline_free(struct kref *kref)
{
	struct sync_timeline *obj =
		container_of(kref, struct sync_timeline, kref);

	kfree(obj);
}

static void mi_sw_sync_timeline_get(struct sync_timeline *obj)
{
	kref_get(&obj->kref);
}

static void mi_sw_sync_timeline_put(struct sync_timeline *obj)
{
	kref_put(&obj->kref, mi_sw_sync_timeline_free);
}

static const char *mi_sw_sync_fence_get_driver_name(struct dma_fence *fence)
{
	return "sw_sync";
}

static const char *mi_sw_sync_fence_get_timeline_name(struct dma_fence *fence)
{
	struct sync_timeline *parent = dma_fence_parent(fence);

	return parent->name;
}

static void mi_sw_sync_fence_release(struct dma_fence *fence)
{
	struct sync_pt *pt = dma_fence_to_sync_pt(fence);
	struct sync_timeline *parent = dma_fence_parent(fence);
	unsigned long flags;

	spin_lock_irqsave(fence->lock, flags);
	if (!list_empty(&pt->link)) {
		list_del(&pt->link);
		rb_erase(&pt->node, &parent->pt_tree);
	}
	spin_unlock_irqrestore(fence->lock, flags);

	mi_sw_sync_timeline_put(parent);
	dma_fence_free(fence);
}

static bool mi_sw_sync_fence_signaled(struct dma_fence *fence)
{
	struct sync_timeline *parent = dma_fence_parent(fence);

	return !__dma_fence_is_later(fence->seqno, parent->value, fence->ops);
}

static bool mi_sw_sync_fence_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static void mi_sw_sync_fence_value_str(struct dma_fence *fence,
				       char *str, int size)
{
	snprintf(str, size, "%lld", fence->seqno);
}

static void mi_sw_sync_timeline_value_str(struct dma_fence *fence,
					  char *str, int size)
{
	struct sync_timeline *parent = dma_fence_parent(fence);

	snprintf(str, size, "%d", parent->value);
}

static const struct dma_fence_ops mi_sw_sync_fence_ops = {
	.get_driver_name = mi_sw_sync_fence_get_driver_name,
	.get_timeline_name = mi_sw_sync_fence_get_timeline_name,
	.enable_signaling = mi_sw_sync_fence_enable_signaling,
	.signaled = mi_sw_sync_fence_signaled,
	.release = mi_sw_sync_fence_release,
	.fence_value_str = mi_sw_sync_fence_value_str,
	.timeline_value_str = mi_sw_sync_timeline_value_str,
};

static void mi_sw_sync_timeline_signal(struct sync_timeline *obj,
				       unsigned int inc)
{
	LIST_HEAD(signalled);
	struct sync_pt *pt, *next;

	spin_lock_irq(&obj->lock);

	obj->value += inc;

	list_for_each_entry_safe(pt, next, &obj->pt_list, link) {
		if (!mi_sw_sync_fence_signaled(&pt->base))
			break;

		dma_fence_get(&pt->base);

		list_move_tail(&pt->link, &signalled);
		rb_erase(&pt->node, &obj->pt_tree);

		dma_fence_signal_locked(&pt->base);
	}

	spin_unlock_irq(&obj->lock);

	list_for_each_entry_safe(pt, next, &signalled, link) {
		list_del_init(&pt->link);
		dma_fence_put(&pt->base);
	}
}

static struct sync_pt *mi_sw_sync_pt_create(struct sync_timeline *obj,
					    unsigned int value)
{
	struct sync_pt *pt;

	pt = kzalloc(sizeof(*pt), GFP_KERNEL);
	if (!pt)
		return NULL;

	mi_sw_sync_timeline_get(obj);
	dma_fence_init(&pt->base, &mi_sw_sync_fence_ops, &obj->lock,
		       obj->context, value);
	INIT_LIST_HEAD(&pt->link);

	spin_lock_irq(&obj->lock);
	if (!dma_fence_is_signaled_locked(&pt->base)) {
		struct rb_node **p = &obj->pt_tree.rb_node;
		struct rb_node *parent = NULL;

		while (*p) {
			struct sync_pt *other;
			int cmp;

			parent = *p;
			other = rb_entry(parent, typeof(*pt), node);
			cmp = value - other->base.seqno;
			if (cmp > 0) {
				p = &parent->rb_right;
			} else if (cmp < 0) {
				p = &parent->rb_left;
			} else {
				if (dma_fence_get_rcu(&other->base)) {
					mi_sw_sync_timeline_put(obj);
					kfree(pt);
					pt = other;
					goto unlock;
				}
				p = &parent->rb_left;
			}
		}
		rb_link_node(&pt->node, parent, p);
		rb_insert_color(&pt->node, &obj->pt_tree);

		parent = rb_next(&pt->node);
		list_add_tail(&pt->link,
			      parent ? &rb_entry(parent, typeof(*pt), node)->link :
				       &obj->pt_list);
	}
unlock:
	spin_unlock_irq(&obj->lock);

	return pt;
}

static int mi_sw_sync_open(struct inode *inode, struct file *file)
{
	struct sync_timeline *obj;
	char task_comm[TASK_COMM_LEN];

	get_task_comm(task_comm, current);

	obj = mi_sw_sync_timeline_create(task_comm);
	if (!obj)
		return -ENOMEM;

	file->private_data = obj;

	return 0;
}

static int mi_sw_sync_release(struct inode *inode, struct file *file)
{
	struct sync_timeline *obj = file->private_data;
	struct sync_pt *pt, *next;

	spin_lock_irq(&obj->lock);

	list_for_each_entry_safe(pt, next, &obj->pt_list, link) {
		dma_fence_set_error(&pt->base, -ENOENT);
		dma_fence_signal_locked(&pt->base);
	}

	spin_unlock_irq(&obj->lock);

	mi_sw_sync_timeline_put(obj);
	return 0;
}

static long mi_sw_sync_ioctl_create_fence(struct sync_timeline *obj,
					  unsigned long arg)
{
	struct sw_sync_create_fence_data data;
	struct sync_file *sync_file;
	struct sync_pt *pt;
	int fd;
	int err;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	pt = mi_sw_sync_pt_create(obj, data.value);
	if (!pt) {
		err = -ENOMEM;
		goto err_put_fd;
	}

	sync_file = sync_file_create(&pt->base);
	dma_fence_put(&pt->base);
	if (!sync_file) {
		err = -ENOMEM;
		goto err_put_fd;
	}

	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		fput(sync_file->file);
		err = -EFAULT;
		goto err_put_fd;
	}

	fd_install(fd, sync_file->file);
	return 0;

err_put_fd:
	put_unused_fd(fd);
	return err;
}

static long mi_sw_sync_ioctl_inc(struct sync_timeline *obj, unsigned long arg)
{
	u32 value;

	if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
		return -EFAULT;

	while (value > INT_MAX) {
		mi_sw_sync_timeline_signal(obj, INT_MAX);
		value -= INT_MAX;
	}

	mi_sw_sync_timeline_signal(obj, value);
	return 0;
}

static long mi_sw_sync_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct sync_timeline *obj = file->private_data;

	switch (cmd) {
	case SW_SYNC_IOC_CREATE_FENCE:
		return mi_sw_sync_ioctl_create_fence(obj, arg);
	case SW_SYNC_IOC_INC:
		return mi_sw_sync_ioctl_inc(obj, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations mi_sw_sync_fops = {
	.owner = THIS_MODULE,
	.open = mi_sw_sync_open,
	.release = mi_sw_sync_release,
	.unlocked_ioctl = mi_sw_sync_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice mi_sw_sync_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mi_sw_sync",
	.fops = &mi_sw_sync_fops,
	.mode = 0666,
};

static int __init mi_sw_sync_init(void)
{
	return misc_register(&mi_sw_sync_device);
}

static void __exit mi_sw_sync_exit(void)
{
	misc_deregister(&mi_sw_sync_device);
}

module_init(mi_sw_sync_init);
module_exit(mi_sw_sync_exit);

MODULE_DESCRIPTION("Xiaomi software sync device");
MODULE_LICENSE("GPL v2");
