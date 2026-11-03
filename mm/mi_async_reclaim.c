// SPDX-License-Identifier: GPL-2.0
/*
 * File-rmap contention handoff adapted from MiCode commit
 * 56295df675b92553f83bbf333f88b0c6ee8fa7bc, mi_async_reclaim.c.
 * Original author: maminghui5 <maminghui5@xiaomi.com>, MODULE_LICENSE("GPL").
 * Rebuilt for 4.19 isolated-page ownership; no donor page flag bits used.
 */
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/vmstat.h>
#include "mi_async_reclaim.h"

/* One MiB including the running page; independent of device RAM size. */
#define MI_ASYNC_MAX_PAGES 256
static DEFINE_SPINLOCK(mi_async_lock);
static DEFINE_MUTEX(mi_async_control);
static DECLARE_COMPLETION(mi_async_started);
static DECLARE_WAIT_QUEUE_HEAD(mi_async_wait);
static LIST_HEAD(mi_async_pending);
static struct task_struct *mi_async_task;
static bool mi_async_enabled;
static unsigned long mi_async_pending_pages, mi_async_running_pages;
static unsigned long mi_async_accepted, mi_async_reclaimed, mi_async_returned;

bool mi_async_reclaim_ready(void)
{
	return READ_ONCE(mi_async_enabled);
}

/* Page is off every list, unlocked, and owns the original isolation ref. */
bool mi_async_reclaim_queue(struct page *page)
{
	unsigned long flags;

	/* Do not defer compound heads: an unlocked split changes their weight. */
	if (PageCompound(page) || PageAnon(page) || PageSwapBacked(page) ||
	    PageKsm(page))
		return false;
	spin_lock_irqsave(&mi_async_lock, flags);
	if (!mi_async_enabled || mi_async_pending_pages + mi_async_running_pages >=
	    MI_ASYNC_MAX_PAGES) {
		spin_unlock_irqrestore(&mi_async_lock, flags);
		return false;
	}
	ClearPageActive(page);
	list_add_tail(&page->lru, &mi_async_pending);
	mi_async_pending_pages++;
	mi_async_accepted++;
	spin_unlock_irqrestore(&mi_async_lock, flags);
	wake_up(&mi_async_wait);
	return true;
}

static struct page *mi_async_take_page(void)
{
	struct page *page = NULL;
	unsigned long flags;

	spin_lock_irqsave(&mi_async_lock, flags);
	if (mi_async_enabled && !list_empty(&mi_async_pending)) {
		page = list_first_entry(&mi_async_pending, struct page, lru);
		list_del_init(&page->lru);
		mi_async_pending_pages--;
		mi_async_running_pages++;
	}
	spin_unlock_irqrestore(&mi_async_lock, flags);
	return page;
}

/* Called only after reclaim or putback consumed all isolation references. */
static void mi_async_complete(unsigned long nr, unsigned long reclaimed)
{
	unsigned long flags;

	spin_lock_irqsave(&mi_async_lock, flags);
	mi_async_running_pages -= nr;
	mi_async_reclaimed += reclaimed;
	mi_async_returned += nr - reclaimed;
	spin_unlock_irqrestore(&mi_async_lock, flags);
}

static int mi_async_worker(void *unused)
{
	struct reclaim_state reclaim_state = {};
	struct page *page;
	int ret;

	current->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	current->reclaim_state = &reclaim_state;
	/* set_freezable initially uses try_to_freeze in 4.19. The startup
	 * handshake prevents stop from racing that one-time initialization.
	 */
	set_freezable();
	complete(&mi_async_started);
	for (;;) {
		/* Unlike try_to_freeze(), this can leave the freezer on stop. */
		if (kthread_freezable_should_stop(NULL))
			break;
		page = mi_async_take_page();
		if (page) {
			LIST_HEAD(pages);
			struct pglist_data *pgdat = page_pgdat(page);
			unsigned long reclaimed;

			list_add(&page->lru, &pages);
			reclaimed = mi_async_reclaim_page(&pages, pgdat);
			mi_async_complete(1, reclaimed);
			cond_resched();
			continue;
		}
		/* Freezer wakeups must reach kthread_freezable_should_stop(). */
		ret = wait_event_interruptible(mi_async_wait,
			kthread_should_stop() || freezing(current) ||
			READ_ONCE(mi_async_pending_pages));
		if (ret && !freezing(current) && !kthread_should_stop())
			flush_signals(current);
	}
	current->reclaim_state = NULL;
	return 0;
}

/* mi_async_control serializes sysfs writers and worker start/stop. */
static void mi_async_stop(void)
{
	LIST_HEAD(cancelled);
	struct page *page, *next;
	unsigned long flags, nr;

	spin_lock_irqsave(&mi_async_lock, flags);
	WRITE_ONCE(mi_async_enabled, false);
	list_splice_init(&mi_async_pending, &cancelled);
	nr = mi_async_pending_pages;
	mi_async_pending_pages = 0;
	mi_async_running_pages += nr;
	spin_unlock_irqrestore(&mi_async_lock, flags);

	list_for_each_entry_safe(page, next, &cancelled, lru) {
		list_del_init(&page->lru);
		dec_node_page_state(page, NR_ISOLATED_FILE);
		putback_lru_page(page);
	}
	mi_async_complete(nr, 0);
	if (mi_async_task) {
		kthread_stop(mi_async_task);
		mi_async_task = NULL;
	}
}

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", mi_async_reclaim_ready());
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool enable;
	unsigned long flags;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	ret = mutex_lock_interruptible(&mi_async_control);
	if (ret)
		return ret;
	if (!enable) {
		mi_async_stop();
	} else if (!mi_async_task) {
		reinit_completion(&mi_async_started);
		mi_async_task = kthread_run(mi_async_worker, NULL, "mi_async_reclaim");
		if (IS_ERR(mi_async_task)) {
			ret = PTR_ERR(mi_async_task);
			mi_async_task = NULL;
			goto out;
		}
		/* Only the control mutex is held; admission is still closed.
		 * The sysfs caller is a user process: kernel-thread freezing
		 * follows user-process freezing, so startup can finish first.
		 */
		wait_for_completion(&mi_async_started);
		spin_lock_irqsave(&mi_async_lock, flags);
		WRITE_ONCE(mi_async_enabled, true);
		spin_unlock_irqrestore(&mi_async_lock, flags);
	}
out:
	mutex_unlock(&mi_async_control);
	return ret ? ret : count;
}

static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	unsigned long flags;
	ssize_t n;

	spin_lock_irqsave(&mi_async_lock, flags);
	n = scnprintf(buf, PAGE_SIZE,
		"accepted %lu pending %lu running %lu reclaimed %lu returned %lu capacity %u\n",
		mi_async_accepted, mi_async_pending_pages, mi_async_running_pages,
		mi_async_reclaimed, mi_async_returned, MI_ASYNC_MAX_PAGES);
	spin_unlock_irqrestore(&mi_async_lock, flags);
	return n;
}

static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);
static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct attribute *mi_async_attrs[] = {
	&enabled_attr.attr, &state_attr.attr, NULL,
};
static const struct attribute_group mi_async_group = {
	.attrs = mi_async_attrs,
};

static int __init mi_async_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("mi_async_reclaim", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &mi_async_group);
	if (ret)
		kobject_put(kobj);
	return ret;
}
subsys_initcall(mi_async_init);
