// SPDX-License-Identifier: GPL-2.0
/* Independent dagu 4.19 adaptation; ordinary prepared, refcount-one pages. */
#include <linux/mi_mempool.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/vmstat.h>
#include <linux/kthread.h>
#include <linux/shrinker.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/interrupt.h>

#define POOL_HIGH 1024U
#define POOL_LOW 512U
#define RETRY_DELAY msecs_to_jiffies(300)
static DEFINE_MUTEX(control_lock);
static DEFINE_SPINLOCK(pool_lock);
static LIST_HEAD(reserve_pages);
static DECLARE_WAIT_QUEUE_HEAD(refill_wait);
static struct task_struct *refill_task;
static bool enabled;
static unsigned int count;
static unsigned long retry_after, next_wake;
static unsigned long generation;
static unsigned long hits, misses, alloc_failures, pressure_returned, stop_drain;

/* Only called with pool_lock held; ownership and vmstat move together. */
static struct page *take_page(struct page *page)
{
	list_del_init(&page->lru);
	count--;
	__mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE, -1);
	return page;
}

struct page *mi_mempool_borrow(gfp_t gfp, unsigned int order, struct zone *zone)
{
	struct page *page, *found = NULL;
	unsigned long flags;

	if (order || !zone || zone_idx(zone) != ZONE_NORMAL ||
	    !(gfp & __GFP_DIRECT_RECLAIM) ||
	    (gfp & (__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM | __GFP_MOVABLE |
		    __GFP_ATOMIC | __GFP_MEMALLOC | __GFP_NOFAIL)) ||
	    in_interrupt() || irqs_disabled() || in_atomic() ||
	    (current->flags & PF_MEMALLOC) || current == READ_ONCE(refill_task))
		return NULL;
	spin_lock_irqsave(&pool_lock, flags);
	if (enabled) {
		list_for_each_entry(page, &reserve_pages, lru) {
			if (page_zone(page) == zone) {
				found = take_page(page);
				hits++;
				break;
			}
		}
		if (!found)
			misses++;
	}
	spin_unlock_irqrestore(&pool_lock, flags);
	mi_mempool_free_event();
	return found;
}

void mi_mempool_free_event(void)
{
	unsigned long flags;

	/* A notification only: never inspect or retain a freed page. */
	if (!READ_ONCE(enabled))
		return;
	spin_lock_irqsave(&pool_lock, flags);
	if (enabled && count < POOL_LOW &&
	    time_after_eq(jiffies, retry_after) &&
	    time_after_eq(jiffies, next_wake)) {
		next_wake = jiffies + RETRY_DELAY;
		wake_up(&refill_wait);
	}
	spin_unlock_irqrestore(&pool_lock, flags);
}

static int refill(void *unused)
{
	gfp_t gfp = (GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN) &
		    ~__GFP_DIRECT_RECLAIM;
	struct zone *zone = &NODE_DATA(numa_node_id())->node_zones[ZONE_NORMAL];

	while (!kthread_should_stop()) {
		struct page *page;
		unsigned long flags, epoch;
		unsigned int remaining;

		wait_event_interruptible_timeout(refill_wait,
			kthread_should_stop() || (READ_ONCE(enabled) &&
			READ_ONCE(count) < POOL_LOW &&
			time_after_eq(jiffies, READ_ONCE(retry_after))), RETRY_DELAY);
		if (kthread_should_stop())
			break;
		if (READ_ONCE(count) >= POOL_LOW)
			continue;
		for (;;) {
			spin_lock_irqsave(&pool_lock, flags);
			remaining = POOL_HIGH - count;
			epoch = generation;
			if (!enabled || !remaining || time_before(jiffies, retry_after)) {
				spin_unlock_irqrestore(&pool_lock, flags);
				break;
			}
			spin_unlock_irqrestore(&pool_lock, flags);
			/* Reserve the entire remaining budget above high watermark. */
			if (!zone_watermark_ok(zone, 0, high_wmark_pages(zone) +
					       remaining, ZONE_NORMAL, 0))
				page = NULL;
			else
				page = alloc_pages_node(zone_to_nid(zone), gfp, 0);
			spin_lock_irqsave(&pool_lock, flags);
			if (!page) {
				alloc_failures++;
				retry_after = jiffies + RETRY_DELAY;
				spin_unlock_irqrestore(&pool_lock, flags);
				break;
			}
			if (enabled && epoch == generation && count < POOL_HIGH &&
			    page_zone(page) == zone && !page_is_pfmemalloc(page)) {
				list_add(&page->lru, &reserve_pages);
				count++;
				__mod_node_page_state(page_pgdat(page),
					NR_KERNEL_MISC_RECLAIMABLE, 1);
				page = NULL;
			}
			spin_unlock_irqrestore(&pool_lock, flags);
			if (page) {
				__free_pages(page, 0);
				break;
			}
			cond_resched();
		}
	}
	return 0;
}

static unsigned long pool_count(struct shrinker *s, struct shrink_control *sc)
{
	struct page *page;
	unsigned long flags, nr = 0;

	spin_lock_irqsave(&pool_lock, flags);
	if (enabled)
		list_for_each_entry(page, &reserve_pages, lru)
			if (sc->nid == NUMA_NO_NODE || page_to_nid(page) == sc->nid)
				nr++;
	spin_unlock_irqrestore(&pool_lock, flags);
	return nr ? nr : SHRINK_EMPTY;
}

static unsigned long pool_scan(struct shrinker *s, struct shrink_control *sc)
{
	LIST_HEAD(dispose);
	struct page *page, *next;
	unsigned long flags, nr = 0;

	spin_lock_irqsave(&pool_lock, flags);
	generation++;
	retry_after = jiffies + RETRY_DELAY;
	if (enabled)
		list_for_each_entry_safe(page, next, &reserve_pages, lru) {
			if (nr >= sc->nr_to_scan)
				break;
			if (sc->nid != NUMA_NO_NODE && page_to_nid(page) != sc->nid)
				continue;
			list_add_tail(&take_page(page)->lru, &dispose);
			nr++;
		}
	pressure_returned += nr;
	spin_unlock_irqrestore(&pool_lock, flags);
	list_for_each_entry_safe(page, next, &dispose, lru) {
		list_del_init(&page->lru);
		__free_pages(page, 0);
	}
	sc->nr_scanned = nr;
	/* A stale count snapshot must not leave reclaim with zero progress. */
	return nr ? nr : SHRINK_STOP;
}

static struct shrinker pool_shrinker = {
	.count_objects = pool_count,
	.scan_objects = pool_scan,
	.seeks = DEFAULT_SEEKS,
	.flags = SHRINKER_NUMA_AWARE,
};

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(enabled));
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	struct page *page;
	unsigned long flags;
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	mutex_lock(&control_lock);
	if (on == enabled)
		goto out;
	if (on) {
		if (POOL_HIGH > totalram_pages / 512) {
			ret = -ENOMEM;
			goto out;
		}
		ret = allocator_reserve_acquire(RESERVE_C21);
		if (ret)
			goto out;
		ret = register_shrinker(&pool_shrinker);
		if (ret)
			goto release_owner;
		/* Start sleeping; publish ON only after all resources exist. */
		refill_task = kthread_run(refill, NULL, "mi_mempool");
		if (IS_ERR(refill_task)) {
			ret = PTR_ERR(refill_task);
			refill_task = NULL;
			unregister_shrinker(&pool_shrinker);
			goto release_owner;
		}
		spin_lock_irqsave(&pool_lock, flags);
		generation++;
		retry_after = jiffies;
		enabled = true;
		spin_unlock_irqrestore(&pool_lock, flags);
		wake_up(&refill_wait);
	} else {
		spin_lock_irqsave(&pool_lock, flags);
		enabled = false;
		generation++;
		spin_unlock_irqrestore(&pool_lock, flags);
		unregister_shrinker(&pool_shrinker);
		kthread_stop(refill_task);
		refill_task = NULL;
		spin_lock_irqsave(&pool_lock, flags);
		while (!list_empty(&reserve_pages)) {
			page = take_page(list_first_entry(&reserve_pages, struct page, lru));
			stop_drain++;
			spin_unlock_irqrestore(&pool_lock, flags);
			__free_pages(page, 0);
			spin_lock_irqsave(&pool_lock, flags);
		}
		spin_unlock_irqrestore(&pool_lock, flags);
		allocator_reserve_release(RESERVE_C21);
	}
	goto out;
release_owner:
	allocator_reserve_release(RESERVE_C21);
out:
	mutex_unlock(&control_lock);
	return ret ? ret : len;
}

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	unsigned long flags;
	ssize_t ret;

	spin_lock_irqsave(&pool_lock, flags);
	ret = scnprintf(buf, PAGE_SIZE,
		"order=0 blocks=%u pages=%u high=%u low=%u hits=%lu misses=%lu refill_failures=%lu pressure_returned=%lu stop_drain=%lu\n",
		count, count, POOL_HIGH, POOL_LOW, hits, misses, alloc_failures,
		pressure_returned, stop_drain);
	spin_unlock_irqrestore(&pool_lock, flags);
	return ret;
}
static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);
static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct attribute *pool_attrs[] = {
	&enabled_attr.attr, &stats_attr.attr, NULL,
};
static const struct attribute_group pool_group = { .attrs = pool_attrs };

static int __init mi_mempool_init(void)
{
	struct kobject *kobj = kobject_create_and_add("mi_mempool", kernel_kobj);
	int ret;

	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &pool_group);
	if (ret)
		kobject_put(kobj);
	return ret;
}
late_initcall(mi_mempool_init);
