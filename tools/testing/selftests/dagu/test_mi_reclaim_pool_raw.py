#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Finite host ownership test for actual C22 raw capture/borrow functions."""
import subprocess
import tempfile
from pathlib import Path

from c_helpers import function

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef unsigned int gfp_t;
#define __GFP_DIRECT_RECLAIM 1U
#define __GFP_DMA 2U
#define __GFP_DMA32 4U
#define __GFP_HIGHMEM 8U
#define __GFP_MOVABLE 16U
#define __GFP_ATOMIC 32U
#define __GFP_MEMALLOC 64U
#define __GFP_NOFAIL 128U
#define PF_MEMALLOC 1U
#define MAX_RT_PRIO 100
#define ZONE_NORMAL 2
#define MIGRATE_RECLAIMABLE 1
#define NR_KERNEL_MISC_RECLAIMABLE 0
#define PGFREE 0
#define POOL_HIGH 1024U
#define RAW_HIGH (POOL_HIGH / 2)
#define READ_ONCE(x) (x)

struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define list_entry(ptr, type, member) container_of(ptr, type, member)
static inline void init_list_head(struct list_head *head)
{ head->next = head; head->prev = head; }
static inline bool list_empty(const struct list_head *head)
{ return head->next == head; }
static inline void list_add_tail(struct list_head *node, struct list_head *head)
{
	node->prev = head->prev;
	node->next = head;
	head->prev->next = node;
	head->prev = node;
}
static inline void list_del_init(struct list_head *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
	init_list_head(node);
}
#define list_for_each_entry(pos, head, member) \
	for (pos = list_entry((head)->next, __typeof__(*pos), member); \
	     &pos->member != (head); \
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_entry((head)->next, __typeof__(*pos), member), \
	     n = list_entry(pos->member.next, __typeof__(*pos), member); \
	     &pos->member != (head); \
	     pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

struct zone { int index; };
struct task { int prio; unsigned int flags; } task, worker;
static struct task *current = &task;
static struct task *refill_task = &worker;
struct page {
	struct list_head lru;
	struct zone *zone;
	unsigned long index;
	unsigned long private;
	int refcount;
};

static int pool_lock;
static int misc_pages;
static int pgfree_events;
static int raw_returns;
static unsigned long hits, misses, raw_borrowed, raw_stop_drain;
static unsigned long raw_captured, raw_rejected;
static unsigned int count, raw_count;
static bool enabled, raw_enabled = true, watermark_ok = true;
static LIST_HEAD(reserve_pages);
static LIST_HEAD(raw_pages);

#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define page_zone(page) ((page)->zone)
#define page_pgdat(page) ((void *)(page)->zone)
#define page_to_pfn(page) ((unsigned long)(page))
#define zone_idx(zone) ((zone)->index)
#define high_wmark_pages(zone) (100UL)
#define __mod_node_page_state(pgdat, item, delta) \
	((void)(pgdat), (void)(item), misc_pages += (delta))
#define __count_vm_event(event) do { (void)(event); pgfree_events++; } while (0)
#define in_interrupt() false
#define irqs_disabled() false
#define in_atomic() false
static bool zone_watermark_ok_safe(struct zone *zone, unsigned int order,
		unsigned long mark, int classzone_idx)
{
	(void)zone; (void)order; (void)mark; (void)classzone_idx;
	return watermark_ok;
}
static void mi_reclaim_pool_free_event(void) { }
static void mi_reclaim_pool_return_raw_page(struct page *page)
{
	assert(page->refcount == 0);
	assert(list_empty(&page->lru));
	raw_returns++;
}
'''

checks = r'''
static void reset_pool(void)
{
	init_list_head(&reserve_pages);
	init_list_head(&raw_pages);
	count = raw_count = 0;
	misc_pages = 0;
	pgfree_events = 0;
	raw_returns = 0;
	hits = misses = raw_borrowed = raw_stop_drain = 0;
	raw_captured = raw_rejected = 0;
	enabled = true;
	raw_enabled = true;
	watermark_ok = true;
	current = &task;
	task.prio = 0;
	task.flags = 0;
}

int main(void)
{
	struct zone normal = { ZONE_NORMAL };
	struct zone other = { 0 };
	struct page raw = { .zone = &normal, .refcount = 0 };
	struct page prepared = { .zone = &normal, .refcount = 1 };
	struct page *page;
	bool needs_prep;

	init_list_head(&raw.lru);
	init_list_head(&prepared.lru);

	reset_pool();
	raw_enabled = false;
	assert(!mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));
	assert(raw_count == 0 && pgfree_events == 0);

	reset_pool();
	current = &worker;
	assert(!mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));
	current = &task;
	raw.zone = &other;
	assert(!mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));
	raw.zone = &normal;

	reset_pool();
	watermark_ok = false;
	assert(!mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));
	watermark_ok = true;
	assert(!mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE + 1, false));

	reset_pool();
	raw_count = RAW_HIGH;
	assert(!mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));
	reset_pool();
	count = POOL_HIGH;
	assert(!mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));

	reset_pool();
	assert(mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));
	assert(raw_count == 1 && misc_pages == 1 && pgfree_events == 1);
	assert(raw.lru.next == &raw_pages && raw_pages.next == &raw.lru);

	reset_pool();
	count = 1;
	raw_count = 1;
	list_add_tail(&prepared.lru, &reserve_pages);
	list_add_tail(&raw.lru, &raw_pages);
	misc_pages = 2;
	needs_prep = false;
	page = mi_reclaim_pool_borrow(__GFP_DIRECT_RECLAIM, 0, &normal,
				     &needs_prep);
	assert(page == &raw && needs_prep);
	assert(raw_count == 0 && count == 1 && misc_pages == 1);
	needs_prep = true;
	page = mi_reclaim_pool_borrow(__GFP_DIRECT_RECLAIM, 0, &normal,
				     &needs_prep);
	assert(page == &prepared && !needs_prep);
	assert(raw_count == 0 && count == 0 && misc_pages == 0);
	assert(hits == 2);

	reset_pool();
	assert(mi_reclaim_pool_capture_free_page(&raw,
		MIGRATE_RECLAIMABLE, false));
	assert(raw_drain() == 1);
	assert(raw_count == 0 && misc_pages == 0 && raw_stop_drain == 1);
	assert(raw_returns == 1);
	assert(list_empty(&raw_pages));
	return 0;
}
'''

source = prefix + ''.join(function('mm/mi_reclaim_pool.c', name) for name in (
	'take_page',
	'take_raw_page',
	'mi_reclaim_pool_raw_capture_enabled',
	'mi_reclaim_pool_capture_free_page',
	'mi_reclaim_pool_borrow',
	'raw_drain',
)) + checks

with tempfile.TemporaryDirectory() as tmp:
	path = Path(tmp)
	(path / 'test.c').write_text(source)
	subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
			str(path / 'test.c'), '-o', str(path / 'test')], check=True)
	subprocess.run([str(path / 'test')], check=True)
print('PASS: C22 raw ownership, budget, borrow ordering and drain')
