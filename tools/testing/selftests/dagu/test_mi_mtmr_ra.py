#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extract actual pin/producer/age transitions; no FS/LRU concurrency proof."""
import os
import subprocess
import tempfile
from pathlib import Path
from c_helpers import function, read_source

# The actual kernel function names form the agreed controllable seam. Missing
# implementations are deliberately the first red result.
names = ('mtmr_reserve', 'mi_mtmr_ra_prepare', 'mi_mtmr_ra_inserted',
         'mi_mtmr_ra_complete', 'mtmr_take', 'mtmr_finish')
parts = [function('mm/mi_mtmr.c', n) for n in names]
print('extracted', len(parts), 'actual RA transition functions')

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define SHRINK_STOP (~0UL)
#define SHRINK_EMPTY (~0UL-1)
#define MTMR_BATCH 32
#define MTMR_POLL_MS 100
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
struct shrinker {};
struct shrink_control { unsigned long nr_to_scan, nr_scanned; };
#define MTMR_CAPACITY 1024
#define MTMR_NOMAP 0
#define MTMR_RA 1
#define MTMR_POLICIES 2
#define BIT(n) (1U << (n))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define wake_up_all(w) ((void)(w))
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define msecs_to_jiffies(a) (a)
struct page { unsigned refs; bool bad, mapped, referenced, error, locked, lru; };
struct pglist_data {};
struct list_head { struct page *page; };
#define LIST_HEAD(n) struct list_head n={0}
#define page_pgdat(p) ((struct pglist_data *)NULL)
#define page_mapped(p) ((p)->mapped)
#define PageDirty(p) ((p)->bad)
#define PageWriteback(p) ((p)->bad)
#define PageReferenced(p) ((p)->referenced)
#define PageError(p) ((p)->error)
#define PageUptodate(p) (!(p)->bad)
#define ClearPageActive(p) ((void)(p))
#define NR_ISOLATED_FILE 0
static unsigned isolated, reclaim_calls;
static bool isolate_fail;
static bool trylock_page(struct page *p) { if(p->locked)return false; p->locked=true;return true; }
static void unlock_page(struct page *p) {p->locked=false;}
static int isolate_lru_page(struct page *p) {if(isolate_fail||!p->lru)return -1;p->lru=false;p->refs++;return 0;}
#define inc_node_page_state(p,n) ((void)(p),(void)(n),isolated++)
#define dec_node_page_state(p,n) ((void)(p),(void)(n),isolated--)
#define list_add(node,list) ((list)->page=(struct page *)((char *)(node)-offsetof(struct page,lru)))
static void putback_lru_page(struct page *p) {p->lru=true;p->refs--;}
static unsigned long mi_reclaim_file_page(struct list_head *pages,struct pglist_data *pgdat,bool nomap) {
 (void)pgdat;assert(nomap);assert(pages->page->refs==2); /* pagecache+isolation, no RA pin */
 reclaim_calls++; isolated--;putback_lru_page(pages->page);return 0;
}
#define PageCompound(p) ((p)->bad)
#define PageAnon(p) ((p)->bad)
#define PageSwapBacked(p) ((p)->bad)
#define PageUnevictable(p) ((p)->bad)
#define PageMlocked(p) ((p)->bad)
#define page_mapping(p) (!(p)->bad)
static void get_page(struct page *p) { p->refs++; }
static void put_page(struct page *p) { assert(p->refs); p->refs--; }
static bool pressure;
static bool mtmr_pressure(void) { return pressure; }
static int mtmr_lock, mtmr_wait, task, *mtmr_task;
static unsigned joined;
static unsigned long jiffies;
static void kthread_stop(int *t) { (void)t; joined++; }
static void mtmr_consume(struct mtmr_slot *s);
static void wait_progress(void);
#define wait_event(w,c) do { (void)(w); if (!(c)) wait_progress(); assert(c); } while(0)
'''
text = read_source('mm/mi_mtmr.c')
# Keep actual slot layout and policy accounting, with kernel lock declarations
# supplied by this finite harness. No simulated pagecache or LRU operations.
layout = text[text.index('enum mtmr_owner'):text.index('static DEFINE_SPINLOCK')]
state = text[text.index('static unsigned int mtmr_enabled_mask'):text.index('static bool mtmr_pressure')]
checks = r'''
static struct mtmr_slot *deferred;
static void wait_progress(void) {
 assert(deferred);
 mtmr_ra_release(deferred); deferred=NULL;
}

int main(void) {
 struct page p[1025]={0}; int producer, other;
 for(unsigned i=0;i<1025;i++) { p[i].refs=1; p[i].bad=false; p[i].lru=true; }
 mi_mtmr_ra_prepare(&p[0], &producer); assert(p[0].refs==1);
 mtmr_enabled_mask=BIT(MTMR_RA)|BIT(MTMR_NOMAP); mtmr_task=&task;
 mi_mtmr_ra_prepare(&p[0], &producer);
 assert(p[0].refs==2 && mtmr_inflight[MTMR_RA]==1);
 assert(!mtmr_take(~0U,false));
 jiffies=10; mi_mtmr_ra_inserted(&p[0]);
 assert(!mtmr_take(~0U,false)); /* FS still owns producer */
 mi_mtmr_ra_complete(&other); assert(mtmr_inflight[MTMR_RA]==1);
 mi_mtmr_ra_complete(&producer);
 assert(!mtmr_inflight[MTMR_RA] && mtmr_pending[MTMR_RA]==1);
 assert(!mtmr_take(~0U,false));
 jiffies=309; assert(!mtmr_take(~0U,false));
 jiffies=310; struct mtmr_slot *s=mtmr_take(~0U,false); assert(s);
 /* RUNNING duplicate cannot own the page; nomap only marks cancellation. */
 assert(!mtmr_reserve(&p[0],MTMR_NOMAP) && s->cancelled);
 mtmr_consume(s); assert(p[0].refs==1);
 /* No insertion success -> producer terminal releases the pin. */
 mi_mtmr_ra_prepare(&p[1],&producer); mi_mtmr_ra_complete(&producer);
 assert(p[1].refs==1 && !mtmr_pending[MTMR_RA]);
 /* Capacity counts PRODUCING too, and pressure overrides long ages. */
 for(unsigned i=0;i<1025;i++) mi_mtmr_ra_prepare(&p[i],&producer);
 assert(mtmr_inflight[MTMR_RA]==1024 && p[1024].refs==1);
 for(unsigned i=0;i<1024;i++) mi_mtmr_ra_inserted(&p[i]);
 mi_mtmr_ra_complete(&producer); mtmr_ra_age_ms=60000;
 pressure=true;
 struct shrink_control sc={.nr_to_scan=1000};
 assert(mtmr_ra_count(NULL,&sc)==1024);
 assert(mtmr_ra_scan(NULL,&sc)==32 && sc.nr_scanned==32);
 while((s=mtmr_take(BIT(MTMR_RA),false))) mtmr_consume(s);
 pressure=false;
 assert(mtmr_ra_scan(NULL,&sc)==SHRINK_STOP && !sc.nr_scanned);
 assert(mtmr_ra_count(NULL,&sc)==SHRINK_EMPTY);
 jiffies+=100;
 for(unsigned i=0;i<1025;i++) assert(p[i].refs==1);
 /* Closing nomap leaves RA pending and the shared worker alive. */
 mi_mtmr_ra_prepare(&p[0],&producer); mi_mtmr_ra_inserted(&p[0]);
 mi_mtmr_ra_complete(&producer);
 mtmr_disable(MTMR_NOMAP);
 assert(mtmr_enabled_mask==BIT(MTMR_RA) && mtmr_task && !joined);
 assert(mtmr_pending[MTMR_RA]==1 && p[0].refs==2);
 /* Shrinker RUNNING is independent of worker join; final disable must wait
  * for its pin terminal operation, not merely for kthread_stop. */
 deferred=mtmr_take(BIT(MTMR_RA),true); assert(deferred);
 mtmr_disable(MTMR_RA);
 assert(!deferred);
 assert(!mtmr_task && joined==1 && p[0].refs==1);
 assert(mtmr_ra_get==mtmr_ra_put && !mtmr_inflight[MTMR_RA]);
 assert(!mtmr_pending[MTMR_RA] && !mtmr_running[MTMR_RA]);
 assert(mtmr_accepted[MTMR_RA]==mtmr_returned[MTMR_RA]);
 /* A disable gate rejects producer completion even after insertion. */
 mtmr_enabled_mask=BIT(MTMR_RA);
 mi_mtmr_ra_prepare(&p[0],&producer); mi_mtmr_ra_inserted(&p[0]);
 mtmr_enabled_mask=0; mi_mtmr_ra_complete(&producer);
 assert(p[0].refs==1 && mtmr_ra_get==mtmr_ra_put);
 /* Wrap-safe age crossing. */
 mtmr_enabled_mask=BIT(MTMR_RA); mtmr_ra_age_ms=300;
 mtmr_ra_pressured=false; jiffies=~0UL-100; mi_mtmr_ra_prepare(&p[0],&producer);
 mi_mtmr_ra_inserted(&p[0]); mi_mtmr_ra_complete(&producer);
 jiffies=198; assert(!mtmr_take(~0U,false));
 jiffies=199; s=mtmr_take(~0U,false); assert(s);
 s->cancelled=true; mtmr_consume(s);
 assert(mtmr_ra_get==mtmr_ra_put);
 /* Actual consumer must drop the pin before the normal helper. */
 jiffies=1000; mi_mtmr_ra_prepare(&p[0],&producer);
 mi_mtmr_ra_inserted(&p[0]); mi_mtmr_ra_complete(&producer);
 jiffies=1300; s=mtmr_take(~0U,false); assert(s); mtmr_consume(s);
 assert(reclaim_calls==1 && !isolated && p[0].refs==1);
 isolate_fail=true; mi_mtmr_ra_prepare(&p[0],&producer);
 mi_mtmr_ra_inserted(&p[0]); mi_mtmr_ra_complete(&producer);
 jiffies=1600; s=mtmr_take(~0U,false); assert(s); mtmr_consume(s);
 assert(reclaim_calls==1 && !isolated && p[0].refs==1);
 isolate_fail=false; mi_mtmr_ra_prepare(&p[0],&producer);
 mi_mtmr_ra_inserted(&p[0]); mi_mtmr_ra_complete(&producer);
 p[0].error=true; jiffies=1900; s=mtmr_take(~0U,false); assert(s);mtmr_consume(s);
 assert(reclaim_calls==1 && !isolated && p[0].refs==1);
 assert(mtmr_ra_get==mtmr_ra_put);
}
'''
# Forward declarations must follow the actual struct declaration.
prefix = prefix.replace('static void mtmr_consume(struct mtmr_slot *s);', '')
source = prefix + layout + state + '\nstatic void mtmr_consume(struct mtmr_slot *s);\n'
source += ''.join(parts) + ''.join(function('mm/mi_mtmr.c', n) for n in ('mtmr_ra_release','mtmr_ra_count','mtmr_ra_scan','mtmr_consume','mtmr_disable')) + checks
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)
    (p/'test.c').write_text(source)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('host RA pin/producer/age/policy-close: PASS; real FS/PTE/LRU seam NOT RUN')
