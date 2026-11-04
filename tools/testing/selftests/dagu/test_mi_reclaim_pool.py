#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Actual reserve ownership/borrow entry; no real allocator or concurrency."""
import subprocess
import tempfile
from pathlib import Path
from c_helpers import function
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#define WARN_ON(x) (x)
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
static int reserve_owner_lock;
enum allocator_reserve_owner { RESERVE_NONE, RESERVE_C21, RESERVE_C22 };
static enum allocator_reserve_owner reserve_owner;
'''
for name in ('allocator_reserve_acquire', 'allocator_reserve_release'):
    code += function('mm/allocator_reserve.c', name)
code += r'''
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
#define NR_KERNEL_MISC_RECLAIMABLE 0
#define READ_ONCE(x) (x)
#define spin_lock_irqsave(x,f) ((void)(x),(f)=0)
#define spin_unlock_irqrestore(x,f) ((void)(x),(void)(f))
struct zone { int index; };
struct task { int prio; unsigned int flags; } task, worker;
static struct task *current = &task, *refill_task = &worker;
struct list_head { struct list_head *next, *prev; };
struct page { struct list_head lru; struct zone *zone; } item;
static struct list_head reserve_pages, raw_pages;
static int pool_lock, misc;
static unsigned int count, raw_count;
static unsigned long hits, misses, raw_borrowed;
static bool enabled, unsafe;
static void init_list_head(struct list_head *head)
{ head->next = head; head->prev = head; }
static void list_add_tail(struct list_head *node, struct list_head *head)
{
	node->prev = head->prev;
	node->next = head;
	head->prev->next = node;
	head->prev = node;
}
#define list_for_each_entry(p,h,m) \
	for (p=(struct page *)((char *)(h)->next - offsetof(struct page,m)); \
	     &p->m != (h); \
	     p=(struct page *)((char *)p->m.next - offsetof(struct page,m)))
#define list_del_init(node) do { \
	(node)->prev->next=(node)->next; \
	(node)->next->prev=(node)->prev; \
	init_list_head(node); \
} while (0)
#define page_zone(p) ((p)->zone)
#define page_pgdat(p) (p)
#define zone_idx(z) ((z)->index)
#define __mod_node_page_state(p,n,d) ((void)(p),(void)(n),misc+=(d))
#define in_interrupt() unsafe
#define irqs_disabled() unsafe
#define in_atomic() unsafe
static void mi_reclaim_pool_free_event(void) {}
'''
for name in ('take_page', 'take_raw_page', 'mi_reclaim_pool_borrow'):
    code += function('mm/mi_reclaim_pool.c', name)
code += r'''
static void stock(struct zone *z)
{
	init_list_head(&reserve_pages);
	init_list_head(&raw_pages);
	count=1;raw_count=0;misc=1;enabled=true;task.flags=0;unsafe=false;
	item.zone=z; list_add_tail(&item.lru,&reserve_pages);
}
int main(void) {
 struct zone normal={2}, dma={0};
 struct page *page;
 bool needs_prep;
 assert(!allocator_reserve_acquire(RESERVE_C21));
 assert(allocator_reserve_acquire(RESERVE_C22)==-EBUSY);
 allocator_reserve_release(RESERVE_C21);
 assert(!allocator_reserve_acquire(RESERVE_C22));
 assert(allocator_reserve_acquire(RESERVE_C21)==-EBUSY);
 allocator_reserve_release(RESERVE_C22);
 stock(&normal);task.prio=120; assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));
 task.prio=-1;assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));
 task.prio=100;assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));
 task.prio=99;assert(!mi_reclaim_pool_borrow(1,0,&dma,&needs_prep));
 assert(!mi_reclaim_pool_borrow(1,1,&normal,&needs_prep));
 assert(!mi_reclaim_pool_borrow(0,0,&normal,&needs_prep));
 for (unsigned int f=2;f<=128;f*=2) assert(!mi_reclaim_pool_borrow(1|f,0,&normal,&needs_prep));
 unsafe=true;assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));unsafe=false;
 task.flags=PF_MEMALLOC;assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));task.flags=0;
 current=&worker;worker.prio=1;assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));current=&task;
 enabled=false;assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));enabled=true;
 /* Effective prio is sufficient, including a PI-boosted normal-policy task. */
 task.prio=0;page=mi_reclaim_pool_borrow(1,0,&normal,&needs_prep);
 assert(page==&item && !needs_prep);
 assert(count==0 && misc==0 && hits==1 && item.lru.next==&item.lru);
 assert(!mi_reclaim_pool_borrow(1,0,&normal,&needs_prep));assert(hits==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
