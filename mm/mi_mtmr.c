// SPDX-License-Identifier: GPL-2.0
/* C11 base-file page policies; no donor offsets or private page flag bits. */
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
#include <linux/shrinker.h>
#include "mi_mtmr.h"

#define MTMR_CAPACITY 1024
#define MTMR_BATCH 32
#define MTMR_LOW_FREE_PAGES ((64UL << 20) >> PAGE_SHIFT)
#define MTMR_NOMAP 0
#define MTMR_RA 1
#define MTMR_POLICIES 2
#define MTMR_POLL_MS 100
/* Fixed metadata pool: reserved slots also consume capacity. */
enum mtmr_owner { MTMR_FREE, MTMR_PRODUCING, MTMR_PENDING, MTMR_RUNNING };
struct mtmr_slot {
	struct page *page;
	enum mtmr_owner owner;
	bool cancelled;
	unsigned int kind;
	void *producer;
	bool inserted;
	unsigned long inserted_at;
};
static struct mtmr_slot mtmr_slots[MTMR_CAPACITY];
static DEFINE_SPINLOCK(mtmr_lock);
static DEFINE_MUTEX(mtmr_control);
static DECLARE_WAIT_QUEUE_HEAD(mtmr_wait);
static DECLARE_COMPLETION(mtmr_started);
static struct task_struct *mtmr_task;
static unsigned int mtmr_enabled_mask;
static bool mtmr_stopping;
static unsigned long mtmr_inflight[MTMR_POLICIES];
static unsigned long mtmr_pending[MTMR_POLICIES];
static unsigned long mtmr_running[MTMR_POLICIES];
static unsigned long mtmr_accepted[MTMR_POLICIES];
static unsigned long mtmr_reclaimed[MTMR_POLICIES];
static unsigned long mtmr_returned[MTMR_POLICIES];
static unsigned long mtmr_ra_get, mtmr_ra_put;
static unsigned int mtmr_ra_age_ms = 300;
static unsigned long mtmr_ra_pressure_until;
static bool mtmr_ra_pressured;

static bool mtmr_pressure(void)
{
	/* Atomic producer check, no zone walk or allocation under the PTE lock. */
	return global_zone_page_state(NR_FREE_PAGES) <=
		max(totalreserve_pages, MTMR_LOW_FREE_PAGES);
}

/* Called with mtmr_lock; ownership is exclusive even during isolation. */
static struct mtmr_slot *mtmr_reserve(struct page *page, unsigned int kind)
{
	struct mtmr_slot *free = NULL;
	unsigned int i;

	if (mtmr_stopping || !(mtmr_enabled_mask & BIT(kind)) ||
	    (kind == MTMR_RA && mtmr_ra_pressured &&
	     time_before(jiffies, mtmr_ra_pressure_until)))
		return NULL;
	for (i = 0; i < MTMR_CAPACITY; i++) {
		if (mtmr_slots[i].owner != MTMR_FREE &&
		    mtmr_slots[i].page == page) {
			/* PTE callers must not drop the RA pin here. Cancel and
			 * fall back until its exclusive worker owner releases it.
			 */
			if (kind == MTMR_NOMAP && mtmr_slots[i].kind == MTMR_RA)
				mtmr_slots[i].cancelled = true;
			return NULL;
		}
		if (!free && mtmr_slots[i].owner == MTMR_FREE)
			free = &mtmr_slots[i];
	}
	if (free) {
		free->page = page;
		free->cancelled = false;
		free->kind = kind;
		free->producer = NULL;
		free->inserted = false;
		free->owner = MTMR_PRODUCING;
		mtmr_inflight[kind]++;
	}
	return free;
}

void mi_mtmr_nomap(struct page *page)
{
	struct mtmr_slot *slot;
	unsigned long flags;
	bool isolated;

	if (!(READ_ONCE(mtmr_enabled_mask) & BIT(MTMR_NOMAP)) ||
	    PageCompound(page) ||
	    PageAnon(page) || PageSwapBacked(page) || PageUnevictable(page) ||
	    PageMlocked(page) || !page_mapping(page) || page_mapped(page) ||
	    !PageLRU(page) || mtmr_pressure())
		return;
	spin_lock_irqsave(&mtmr_lock, flags);
	slot = mtmr_reserve(page, MTMR_NOMAP);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	if (!slot) {
		wake_up(&mtmr_wait);
		return;
	}
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
		mtmr_pending[MTMR_NOMAP]++;
		mtmr_accepted[MTMR_NOMAP]++;
	} else {
		slot->page = NULL;
		slot->owner = MTMR_FREE;
	}
	mtmr_inflight[MTMR_NOMAP]--;
	spin_unlock_irqrestore(&mtmr_lock, flags);
	wake_up_all(&mtmr_wait);
}

/* Only newly allocated speculative pages with a known demand range enter.
 * The allocation reference exists until read_pages completes. The extra pin
 * protects failed insertions too, until producer completion releases it.
 */
void mi_mtmr_ra_prepare(struct page *page, void *producer)
{
	struct mtmr_slot *slot;
	unsigned long flags;

	if (!(READ_ONCE(mtmr_enabled_mask) & BIT(MTMR_RA)) || mtmr_pressure())
		return;
	spin_lock_irqsave(&mtmr_lock, flags);
	slot = mtmr_reserve(page, MTMR_RA);
	if (slot) {
		get_page(page);
		mtmr_ra_get++;
		slot->producer = producer;
	}
	spin_unlock_irqrestore(&mtmr_lock, flags);
}

/* Common successful insertion boundary used by ext4, f2fs, mpage and the
 * generic readpage fallback. This records insertion, never IO completion.
 */
void mi_mtmr_ra_inserted(struct page *page)
{
	unsigned long flags;
	unsigned int i;

	if (!READ_ONCE(mtmr_inflight[MTMR_RA]))
		return;
	spin_lock_irqsave(&mtmr_lock, flags);
	for (i = 0; i < MTMR_CAPACITY; i++) {
		struct mtmr_slot *slot = &mtmr_slots[i];

		if (slot->owner == MTMR_PRODUCING && slot->kind == MTMR_RA &&
		    slot->page == page) {
			slot->inserted = true;
			slot->inserted_at = jiffies;
			break;
		}
	}
	spin_unlock_irqrestore(&mtmr_lock, flags);
}

void mi_mtmr_ra_complete(void *producer)
{
	unsigned long flags;
	unsigned int i;

	if (!READ_ONCE(mtmr_inflight[MTMR_RA]))
		return;

	for (i = 0; i < MTMR_CAPACITY; i++) {
		struct mtmr_slot *slot = &mtmr_slots[i];
		struct page *drop = NULL;

		spin_lock_irqsave(&mtmr_lock, flags);
		if (slot->owner != MTMR_PRODUCING || slot->kind != MTMR_RA ||
		    slot->producer != producer) {
			spin_unlock_irqrestore(&mtmr_lock, flags);
			continue;
		}
		slot->producer = NULL;
		if (slot->inserted && !slot->cancelled && !mtmr_pressure() &&
		    !PageCompound(slot->page) && !PageAnon(slot->page) &&
		    !PageSwapBacked(slot->page) && !PageUnevictable(slot->page) &&
		    !PageMlocked(slot->page) && page_mapping(slot->page) &&
		    (!mtmr_ra_pressured ||
		     !time_before(jiffies, mtmr_ra_pressure_until)) &&
		    (mtmr_enabled_mask & BIT(MTMR_RA))) {
			slot->owner = MTMR_PENDING;
			mtmr_pending[MTMR_RA]++;
			mtmr_accepted[MTMR_RA]++;
			mtmr_inflight[MTMR_RA]--;
		} else {
			/* Keep PRODUCING exclusive through the last put, so close
			 * cannot finish and a new generation cannot reuse this slot.
			 */
			drop = slot->page;
		}
		spin_unlock_irqrestore(&mtmr_lock, flags);
		if (drop) {
			put_page(drop);
			spin_lock_irqsave(&mtmr_lock, flags);
			mtmr_ra_put++;
			slot->page = NULL;
			slot->owner = MTMR_FREE;
			mtmr_inflight[MTMR_RA]--;
			spin_unlock_irqrestore(&mtmr_lock, flags);
		}
	}
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

static struct mtmr_slot *mtmr_take(unsigned int mask, bool drain)
{
	struct mtmr_slot *slot = NULL;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&mtmr_lock, flags);
	for (i = 0; i < MTMR_CAPACITY; i++) {
		if (mtmr_slots[i].owner != MTMR_PENDING ||
		    !(mask & BIT(mtmr_slots[i].kind)))
			continue;
		if (!drain && mtmr_slots[i].kind == MTMR_RA &&
		    !mtmr_slots[i].cancelled &&
		    (mtmr_enabled_mask & BIT(MTMR_RA)) && !mtmr_pressure() &&
		    time_before(jiffies, mtmr_slots[i].inserted_at +
			msecs_to_jiffies(mtmr_ra_age_ms)))
			continue;
		slot = &mtmr_slots[i];
		slot->owner = MTMR_RUNNING;
		mtmr_pending[slot->kind]--;
		mtmr_running[slot->kind]++;
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
	mtmr_running[slot->kind]--;
	mtmr_reclaimed[slot->kind] += reclaimed;
	mtmr_returned[slot->kind] += 1 - reclaimed;
	spin_unlock_irqrestore(&mtmr_lock, flags);
	wake_up_all(&mtmr_wait);
}

/* Releasing a metadata object does not promise the page itself is freed.
 * No LRU/page lock/reclaim recursion here; puts occur outside owner lock.
 */
static void mtmr_ra_release(struct mtmr_slot *slot)
{
	unsigned long flags;

	put_page(slot->page);
	spin_lock_irqsave(&mtmr_lock, flags);
	mtmr_ra_put++;
	spin_unlock_irqrestore(&mtmr_lock, flags);
	mtmr_finish(slot, 0);
}

static unsigned long mtmr_ra_count(struct shrinker *shrinker,
		struct shrink_control *sc)
{
	unsigned long nr = READ_ONCE(mtmr_pending[MTMR_RA]);

	return nr ? nr : SHRINK_EMPTY;
}

static unsigned long mtmr_ra_scan(struct shrinker *shrinker,
		struct shrink_control *sc)
{
	struct mtmr_slot *slot;
	unsigned long flags, nr = 0;

	spin_lock_irqsave(&mtmr_lock, flags);
	mtmr_ra_pressured = true;
	mtmr_ra_pressure_until = jiffies + msecs_to_jiffies(MTMR_POLL_MS);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	while (nr < min_t(unsigned long, sc->nr_to_scan, MTMR_BATCH) &&
	       (slot = mtmr_take(BIT(MTMR_RA), true))) {
		mtmr_ra_release(slot);
		nr++;
	}
	sc->nr_scanned = nr;
	return nr ? nr : SHRINK_STOP;
}

static struct shrinker mtmr_ra_shrinker = {
	.count_objects = mtmr_ra_count,
	.scan_objects = mtmr_ra_scan,
	.seeks = DEFAULT_SEEKS,
	.batch = MTMR_BATCH,
};

static void mtmr_consume(struct mtmr_slot *slot)
{
	struct page *page = slot->page;
	struct pglist_data *pgdat = page_pgdat(page);
	unsigned long flags, reclaimed = 0;
	bool cancel;
	LIST_HEAD(pages);

	spin_lock_irqsave(&mtmr_lock, flags);
	cancel = slot->cancelled ||
		!(mtmr_enabled_mask & BIT(slot->kind));
	spin_unlock_irqrestore(&mtmr_lock, flags);
	if (!cancel && !mtmr_pressure() && trylock_page(page)) {
		cancel = page_mapped(page) || PageDirty(page) ||
			PageWriteback(page) || !page_mapping(page) ||
			PageUnevictable(page) || PageMlocked(page) ||
			(slot->kind == MTMR_RA && (PageReferenced(page) ||
			 PageError(page) || !PageUptodate(page)));
		unlock_page(page);
	} else {
		cancel = true;
	}
	if (slot->kind == MTMR_RA) {
		/* Metadata pin and isolation reference are different owners. */
		if (!cancel && !isolate_lru_page(page)) {
			inc_node_page_state(page, NR_ISOLATED_FILE);
		} else {
			mtmr_ra_release(slot);
			return;
		}
		put_page(page);
		spin_lock_irqsave(&mtmr_lock, flags);
		mtmr_ra_put++;
		spin_unlock_irqrestore(&mtmr_lock, flags);
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
		slot = mtmr_take(~0U, false);
		if (slot) {
			mtmr_consume(slot);
			if (++batch == MTMR_BATCH) {
				batch = 0;
				cond_resched();
			}
			continue;
		}
		batch = 0;
		/* Poll at most 100ms even for a 60s age: pressure must release
		 * pending RA pins promptly. New nomap work wakes immediately.
		 */
		ret = wait_event_interruptible_timeout(mtmr_wait,
			kthread_should_stop() || freezing(current) ||
			READ_ONCE(mtmr_pending[MTMR_NOMAP]),
			msecs_to_jiffies(MTMR_POLL_MS));
		if (ret < 0 && !freezing(current) && !kthread_should_stop())
			flush_signals(current);
	}
	current->reclaim_state = NULL;
	return 0;
}

/* Caller holds control. Closing one policy cannot drain the other. */
static void mtmr_disable(unsigned int kind)
{
	struct mtmr_slot *slot;
	unsigned long flags;

	spin_lock_irqsave(&mtmr_lock, flags);
	WRITE_ONCE(mtmr_enabled_mask, mtmr_enabled_mask & ~BIT(kind));
	spin_unlock_irqrestore(&mtmr_lock, flags);
	wake_up_all(&mtmr_wait);
	wait_event(mtmr_wait, !READ_ONCE(mtmr_inflight[kind]));
	while ((slot = mtmr_take(BIT(kind), true)))
		mtmr_consume(slot);
	if (!READ_ONCE(mtmr_enabled_mask) && mtmr_task) {
		/* Stop thaws/joins a frozen running owner as in the nomap path. */
		kthread_stop(mtmr_task);
		mtmr_task = NULL;
	}
	/* A pressure shrinker may also hold RUNNING independently of worker. */
	wait_event(mtmr_wait, !READ_ONCE(mtmr_running[kind]));
}

static void mtmr_stop(void)
{
	/* Reject both producers before waiting for either. */
	unsigned long flags;

	spin_lock_irqsave(&mtmr_lock, flags);
	WRITE_ONCE(mtmr_enabled_mask, 0);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	mtmr_disable(MTMR_NOMAP);
	mtmr_disable(MTMR_RA);
}

static ssize_t mtmr_enable_store(unsigned int kind, const char *buf, size_t count)
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
		mtmr_disable(kind);
	} else {
		if (!mtmr_task) {
			reinit_completion(&mtmr_started);
			mtmr_task = kthread_run(mtmr_worker, NULL, "mi_mtmr_pages");
			if (IS_ERR(mtmr_task)) {
				ret = PTR_ERR(mtmr_task);
				mtmr_task = NULL;
				goto out;
			}
			wait_for_completion(&mtmr_started);
		}
		spin_lock_irqsave(&mtmr_lock, flags);
		WRITE_ONCE(mtmr_enabled_mask, mtmr_enabled_mask | BIT(kind));
		spin_unlock_irqrestore(&mtmr_lock, flags);
	}
out:
	mutex_unlock(&mtmr_control);
	return ret ? ret : count;
}

#define MTMR_ENABLE_ATTR(name, kind) \
static ssize_t name##_enable_show(struct kobject *kobj, \
		struct kobj_attribute *attr, char *buf) \
{ \
	return scnprintf(buf, PAGE_SIZE, "%u\n", \
		!!(READ_ONCE(mtmr_enabled_mask) & BIT(kind))); \
} \
static ssize_t name##_enable_store(struct kobject *kobj, \
		struct kobj_attribute *attr, const char *buf, size_t count) \
{ \
	return mtmr_enable_store(kind, buf, count); \
}
MTMR_ENABLE_ATTR(nomap, MTMR_NOMAP)
MTMR_ENABLE_ATTR(readahead, MTMR_RA)

static ssize_t readahead_age_ms_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(mtmr_ra_age_ms));
}

static ssize_t readahead_age_ms_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int age;
	unsigned long flags;
	int ret = kstrtouint(buf, 10, &age);

	if (ret)
		return ret;
	if (age < 100 || age > 60000)
		return -ERANGE;
	ret = mutex_lock_interruptible(&mtmr_control);
	if (ret)
		return ret;
	spin_lock_irqsave(&mtmr_lock, flags);
	if (mtmr_stopping)
		ret = -ESHUTDOWN;
	else if ((mtmr_enabled_mask & BIT(MTMR_RA)) || mtmr_inflight[MTMR_RA] ||
		 mtmr_pending[MTMR_RA] || mtmr_running[MTMR_RA])
		ret = -EBUSY;
	else
		WRITE_ONCE(mtmr_ra_age_ms, age);
	spin_unlock_irqrestore(&mtmr_lock, flags);
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
		"nomap accepted %lu pending %lu running %lu reclaimed %lu returned %lu inflight %lu\n"
		"readahead accepted %lu pending %lu running %lu reclaimed %lu returned %lu inflight %lu get %lu put %lu age_ms %u capacity %u batch %u\n",
		mtmr_accepted[0], mtmr_pending[0], mtmr_running[0],
		mtmr_reclaimed[0], mtmr_returned[0], mtmr_inflight[0],
		mtmr_accepted[1], mtmr_pending[1], mtmr_running[1],
		mtmr_reclaimed[1], mtmr_returned[1], mtmr_inflight[1],
		mtmr_ra_get, mtmr_ra_put, mtmr_ra_age_ms, MTMR_CAPACITY, MTMR_BATCH);
	spin_unlock_irqrestore(&mtmr_lock, flags);
	return n;
}
static struct kobj_attribute nomap_enable_attr = __ATTR_RW(nomap_enable);
static struct kobj_attribute readahead_enable_attr = __ATTR_RW(readahead_enable);
static struct kobj_attribute readahead_age_ms_attr = __ATTR_RW(readahead_age_ms);
static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct attribute *mtmr_attrs[] = {
	&nomap_enable_attr.attr, &readahead_enable_attr.attr,
	&readahead_age_ms_attr.attr, &state_attr.attr, NULL,
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
	ret = register_shrinker(&mtmr_ra_shrinker);
	if (ret)
		goto put;
	ret = register_reboot_notifier(&mtmr_reboot);
	if (ret)
		goto unshrink;
	ret = sysfs_create_group(kobj, &mtmr_group);
	if (!ret)
		return 0;
	unregister_reboot_notifier(&mtmr_reboot);
unshrink:
	unregister_shrinker(&mtmr_ra_shrinker);
put:
	kobject_put(kobj);
	return ret;
}
subsys_initcall(mtmr_init);
