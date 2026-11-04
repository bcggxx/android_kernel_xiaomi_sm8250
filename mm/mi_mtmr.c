// SPDX-License-Identifier: GPL-2.0
/* C11 normal base-file nomap adaptation; no donor offsets or page flag bits. */
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/reboot.h>
#include "mi_mtmr.h"

#define MTMR_CAPACITY 1024
#define MTMR_BATCH 32
#define MTMR_LOW_FREE_PAGES ((64UL << 20) >> PAGE_SHIFT)
#define MTMR_NOMAP 1U
/* Fixed metadata pool: reserved slots also consume capacity. */
enum mtmr_owner { MTMR_FREE, MTMR_PRODUCING, MTMR_PENDING, MTMR_RUNNING };
struct mtmr_slot {
	struct page *page;
	enum mtmr_owner owner;
	bool cancelled;
};
static struct mtmr_slot mtmr_slots[MTMR_CAPACITY];
static DEFINE_SPINLOCK(mtmr_lock);
static DEFINE_MUTEX(mtmr_control);
static DECLARE_WAIT_QUEUE_HEAD(mtmr_wait);
static DECLARE_COMPLETION(mtmr_started);
static struct task_struct *mtmr_task;
static unsigned int mtmr_enabled_mask;
static bool mtmr_stopping;
static unsigned long mtmr_inflight, mtmr_pending, mtmr_running;
static unsigned long mtmr_accepted, mtmr_reclaimed, mtmr_returned;

static bool mtmr_pressure(void)
{
	/* Atomic producer check, no zone walk or allocation under the PTE lock. */
	return global_zone_page_state(NR_FREE_PAGES) <=
		max(totalreserve_pages, MTMR_LOW_FREE_PAGES);
}

/* Called with mtmr_lock; ownership is exclusive even during isolation. */
static struct mtmr_slot *mtmr_reserve(struct page *page)
{
	struct mtmr_slot *free = NULL;
	unsigned int i;

	if (mtmr_stopping || !(mtmr_enabled_mask & MTMR_NOMAP))
		return NULL;
	for (i = 0; i < MTMR_CAPACITY; i++) {
		if (mtmr_slots[i].owner != MTMR_FREE &&
		    mtmr_slots[i].page == page)
			return NULL;
		if (!free && mtmr_slots[i].owner == MTMR_FREE)
			free = &mtmr_slots[i];
	}
	if (free) {
		free->page = page;
		free->cancelled = false;
		free->owner = MTMR_PRODUCING;
		mtmr_inflight++;
	}
	return free;
}

void mi_mtmr_nomap(struct page *page)
{
	struct mtmr_slot *slot;
	unsigned long flags;
	bool isolated;

	if (!READ_ONCE(mtmr_enabled_mask) || PageCompound(page) ||
	    PageAnon(page) || PageSwapBacked(page) || PageUnevictable(page) ||
	    PageMlocked(page) || !page_mapping(page) || page_mapped(page) ||
	    !PageLRU(page) || mtmr_pressure())
		return;
	spin_lock_irqsave(&mtmr_lock, flags);
	slot = mtmr_reserve(page);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	if (!slot)
		return;
	/* isolate_lru_page takes the only reference transferred to the worker.
	 * Never hold mtmr_lock while acquiring an LRU lock. C05 pages are
	 * already isolated and cannot succeed here.
	 */
	isolated = !isolate_lru_page(page);
	if (isolated)
		inc_node_page_state(page, NR_ISOLATED_FILE);
	spin_lock_irqsave(&mtmr_lock, flags);
	if (isolated) {
		/* Disable waits for this producer before joining/draining. Even a
		 * disabled reservation is published for worker-side putback.
		 */
		slot->owner = MTMR_PENDING;
		mtmr_pending++;
		mtmr_accepted++;
	} else {
		slot->page = NULL;
		slot->owner = MTMR_FREE;
	}
	mtmr_inflight--;
	spin_unlock_irqrestore(&mtmr_lock, flags);
	wake_up_all(&mtmr_wait);
}

void mi_mtmr_remap(struct page *page)
{
	unsigned long flags;
	unsigned int i;

	if (!READ_ONCE(mtmr_enabled_mask))
		return;
	/* No page puts, reclaim, waiting or LRU operations under the PTE lock. */
	spin_lock_irqsave(&mtmr_lock, flags);
	for (i = 0; i < MTMR_CAPACITY; i++)
		if (mtmr_slots[i].owner != MTMR_FREE &&
		    mtmr_slots[i].page == page)
			mtmr_slots[i].cancelled = true;
	spin_unlock_irqrestore(&mtmr_lock, flags);
	wake_up(&mtmr_wait);
}

static struct mtmr_slot *mtmr_take(void)
{
	struct mtmr_slot *slot = NULL;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&mtmr_lock, flags);
	for (i = 0; i < MTMR_CAPACITY; i++) {
		if (mtmr_slots[i].owner != MTMR_PENDING)
			continue;
		slot = &mtmr_slots[i];
		slot->owner = MTMR_RUNNING;
		mtmr_pending--;
		mtmr_running++;
		break;
	}
	spin_unlock_irqrestore(&mtmr_lock, flags);
	return slot;
}

static void mtmr_finish(struct mtmr_slot *slot, unsigned long reclaimed)
{
	unsigned long flags;

	spin_lock_irqsave(&mtmr_lock, flags);
	slot->page = NULL;
	slot->owner = MTMR_FREE;
	mtmr_running--;
	mtmr_reclaimed += reclaimed;
	mtmr_returned += 1 - reclaimed;
	spin_unlock_irqrestore(&mtmr_lock, flags);
	wake_up_all(&mtmr_wait);
}

static void mtmr_consume(struct mtmr_slot *slot)
{
	struct page *page = slot->page;
	struct pglist_data *pgdat = page_pgdat(page);
	unsigned long flags, reclaimed = 0;
	bool cancel;
	LIST_HEAD(pages);

	spin_lock_irqsave(&mtmr_lock, flags);
	cancel = slot->cancelled || !(mtmr_enabled_mask & MTMR_NOMAP);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	if (!cancel && !mtmr_pressure() && trylock_page(page)) {
		cancel = page_mapped(page) || PageDirty(page) ||
			PageWriteback(page) || !page_mapping(page) ||
			PageUnevictable(page) || PageMlocked(page);
		unlock_page(page);
	} else {
		cancel = true;
	}
	if (cancel) {
		dec_node_page_state(page, NR_ISOLATED_FILE);
		putback_lru_page(page);
	} else {
		ClearPageActive(page);
		list_add(&page->lru, &pages);
		/* force=false, normal reference/protection checks, async=NULL.
		 * may_unmap=false also protects a remap racing the check above.
		 */
		reclaimed = mi_reclaim_file_page(&pages, pgdat, true);
	}
	/* The slot remains reserved until all page references are consumed. */
	mtmr_finish(slot, reclaimed);
}

static int mtmr_worker(void *unused)
{
	struct reclaim_state reclaim_state = {};
	unsigned int batch = 0;
	struct mtmr_slot *slot;
	int ret;

	current->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	current->reclaim_state = &reclaim_state;
	set_freezable();
	complete(&mtmr_started);
	for (;;) {
		if (kthread_freezable_should_stop(NULL))
			break;
		slot = mtmr_take();
		if (slot) {
			mtmr_consume(slot);
			if (++batch == MTMR_BATCH) {
				batch = 0;
				cond_resched();
			}
			continue;
		}
		batch = 0;
		ret = wait_event_interruptible(mtmr_wait,
			kthread_should_stop() || freezing(current) ||
			READ_ONCE(mtmr_pending));
		if (ret && !freezing(current) && !kthread_should_stop())
			flush_signals(current);
	}
	current->reclaim_state = NULL;
	return 0;
}

/* Caller holds control mutex. No new generation until all producers finish. */
static void mtmr_stop(void)
{
	struct mtmr_slot *slot;
	unsigned long flags;

	spin_lock_irqsave(&mtmr_lock, flags);
	WRITE_ONCE(mtmr_enabled_mask, 0);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	wake_up_all(&mtmr_wait);
	wait_event(mtmr_wait, !READ_ONCE(mtmr_inflight));
	/* The caller can drain a frozen worker's pending pages. Running
	 * ownership is exclusive; kthread_stop then joins/thaws that worker.
	 */
	while ((slot = mtmr_take()))
		mtmr_consume(slot);
	if (mtmr_task) {
		kthread_stop(mtmr_task);
		mtmr_task = NULL;
	}
}

static ssize_t nomap_enable_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
		!!READ_ONCE(mtmr_enabled_mask));
}

static ssize_t nomap_enable_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned long flags;
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	ret = mutex_lock_interruptible(&mtmr_control);
	if (ret)
		return ret;
	if (mtmr_stopping) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (!enable) {
		mtmr_stop();
	} else if (!mtmr_task) {
		reinit_completion(&mtmr_started);
		mtmr_task = kthread_run(mtmr_worker, NULL, "mi_mtmr_pages");
		if (IS_ERR(mtmr_task)) {
			ret = PTR_ERR(mtmr_task);
			mtmr_task = NULL;
			goto out;
		}
		wait_for_completion(&mtmr_started);
		spin_lock_irqsave(&mtmr_lock, flags);
		WRITE_ONCE(mtmr_enabled_mask, MTMR_NOMAP);
		spin_unlock_irqrestore(&mtmr_lock, flags);
	}
out:
	mutex_unlock(&mtmr_control);
	return ret ? ret : count;
}

static ssize_t state_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned long flags;
	ssize_t n;

	spin_lock_irqsave(&mtmr_lock, flags);
	n = scnprintf(buf, PAGE_SIZE,
		"accepted %lu pending %lu running %lu reclaimed %lu returned %lu inflight %lu capacity %u batch %u\n",
		mtmr_accepted, mtmr_pending, mtmr_running, mtmr_reclaimed,
		mtmr_returned, mtmr_inflight, MTMR_CAPACITY, MTMR_BATCH);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	return n;
}
static struct kobj_attribute nomap_enable_attr = __ATTR_RW(nomap_enable);
static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct attribute *mtmr_attrs[] = {
	&nomap_enable_attr.attr, &state_attr.attr, NULL,
};
static const struct attribute_group mtmr_group = { .attrs = mtmr_attrs };

static int mtmr_shutdown(struct notifier_block *nb, unsigned long action,
		void *data)
{
	unsigned long flags;

	mutex_lock(&mtmr_control);
	spin_lock_irqsave(&mtmr_lock, flags);
	WRITE_ONCE(mtmr_stopping, true);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	mtmr_stop();
	mutex_unlock(&mtmr_control);
	return NOTIFY_DONE;
}
static struct notifier_block mtmr_reboot = { .notifier_call = mtmr_shutdown };

static int __init mtmr_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("mi_mtmr", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = register_reboot_notifier(&mtmr_reboot);
	if (ret)
		goto put;
	ret = sysfs_create_group(kobj, &mtmr_group);
	if (!ret)
		return 0;
	unregister_reboot_notifier(&mtmr_reboot);
put:
	kobject_put(kobj);
	return ret;
}
subsys_initcall(mtmr_init);
