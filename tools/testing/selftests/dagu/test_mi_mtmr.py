#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Finite extracted admission/owner/disable checks, not a kernel/LRU model."""
import os
import subprocess
import tempfile
from pathlib import Path
from c_helpers import function

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define MTMR_CAPACITY 1024
#define MTMR_NOMAP 0
#define MTMR_RA 1
#define BIT(n) (1U << (n))
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define msecs_to_jiffies(a) (a)
static unsigned long jiffies, mtmr_ra_pressure_until;
static bool mtmr_ra_pressured;
static unsigned int mtmr_ra_age_ms=300;
#define MTMR_LOW_FREE_PAGES 16
#define max(a,b) ((a) > (b) ? (a) : (b))
#define NR_ISOLATED_FILE 0
#define NR_FREE_PAGES 0
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define spin_lock_irqsave(l,f) ((void)(l), (f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l), (void)(f))
#define wake_up_all(w) ((void)(w))
#define wake_up(w) ((void)(w))
struct page { bool bad, mapped, lru; unsigned refs; };
#define PageCompound(p) ((p)->bad)
#define PageAnon(p) ((p)->bad)
#define PageSwapBacked(p) ((p)->bad)
#define PageUnevictable(p) ((p)->bad)
#define PageMlocked(p) ((p)->bad)
#define PageLRU(p) ((p)->lru)
#define page_mapping(p) (!(p)->bad)
#define page_mapped(p) ((p)->mapped)
enum mtmr_owner { MTMR_FREE, MTMR_PRODUCING, MTMR_PENDING, MTMR_RUNNING };
struct mtmr_slot { struct page *page; enum mtmr_owner owner; bool cancelled; unsigned int kind; void *producer; bool inserted; unsigned long inserted_at; };
static struct mtmr_slot mtmr_slots[MTMR_CAPACITY];
static unsigned mtmr_enabled_mask;
static bool mtmr_stopping;
static unsigned long mtmr_inflight[2], mtmr_pending[2], mtmr_running[2];
static unsigned long mtmr_accepted[2], mtmr_reclaimed[2], mtmr_returned[2];
static unsigned long totalreserve_pages=10, free_pages=100;
static int mtmr_lock, mtmr_wait, task, *mtmr_task;
static unsigned isolated, returned, joined;
static bool isolation_fail;
static unsigned long global_zone_page_state(int n) { (void)n; return free_pages; }
static int isolate_lru_page(struct page *p) {
 if (isolation_fail || !p->lru) return -1;
 p->lru=false; p->refs++; return 0;
}
static void inc_node_page_state(struct page *p, int n) {(void)p;(void)n;isolated++;}
static void kthread_stop(int *t) {(void)t;joined++;}
static void drain(void);
static void mtmr_consume(struct mtmr_slot *s);
#define wait_event(w,condition) do { (void)(w); if (!(condition)) drain(); assert(condition); } while (0)
'''
checks = r'''
static void conserved(void) {
 assert(mtmr_accepted[MTMR_NOMAP] == mtmr_pending[MTMR_NOMAP] + mtmr_running[MTMR_NOMAP] + mtmr_reclaimed[MTMR_NOMAP] + mtmr_returned[MTMR_NOMAP]);
}
static void mtmr_consume(struct mtmr_slot *s) {
 assert(!mtmr_enabled_mask); s->page->lru=true; s->page->refs--;
 isolated--; returned++; mtmr_finish(s,0);
}
static void drain(void) {
 struct mtmr_slot *s;
 while ((s=mtmr_take(~0U,false))) mtmr_consume(s);
}
int main(void) {
 struct page p[1025] = {0};
 for (int i=0;i<1025;i++) {p[i].lru=true; p[i].refs=1;}
 mi_mtmr_nomap(&p[0]); assert(!mtmr_accepted[MTMR_NOMAP]);
 mtmr_enabled_mask=BIT(MTMR_NOMAP); mtmr_task=&task;
 isolation_fail=true; mi_mtmr_nomap(&p[0]); isolation_fail=false;
 assert(!mtmr_inflight[MTMR_NOMAP] && !mtmr_pending[MTMR_NOMAP] && !isolated);
 free_pages=1; mi_mtmr_nomap(&p[0]); assert(!mtmr_accepted[MTMR_NOMAP]); free_pages=100;
 for(int i=0;i<1024;i++) mi_mtmr_nomap(&p[i]);
 mi_mtmr_nomap(&p[1024]); assert(p[1024].refs==1 && p[1024].lru);
 assert(mtmr_accepted[MTMR_NOMAP]==1024 && isolated==1024); conserved();
 mi_mtmr_nomap(&p[0]); assert(mtmr_accepted[MTMR_NOMAP]==1024);
 mi_mtmr_remap(&p[0]); assert(mtmr_slots[0].cancelled);
 struct mtmr_slot *s=mtmr_take(~0U,false); assert(s && s->owner==MTMR_RUNNING);
 mi_mtmr_nomap(&p[1024]); assert(mtmr_accepted[MTMR_NOMAP]==1024); conserved();
 /* A completed normal reclaim consumes its sole isolation reference. */
 s->page->refs--; isolated--; mtmr_finish(s,1); conserved();
 mi_mtmr_nomap(&p[1024]); assert(mtmr_accepted[MTMR_NOMAP]==1025); conserved();
 mtmr_stop(); conserved();
 assert(!mtmr_enabled_mask && !mtmr_inflight[MTMR_NOMAP] && !mtmr_pending[MTMR_NOMAP] && !mtmr_running[MTMR_NOMAP]);
 assert(!isolated && joined==1 && returned==1024 && !mtmr_task);
 for(int i=0;i<1025;i++) assert(p[i].refs==1);
 mtmr_stop(); assert(joined==1); conserved();
 /* Reserved in-flight owner prevents duplicate reservation, including
  * disable; a later generation cannot start before stop's wait completes. */
 mtmr_enabled_mask=BIT(MTMR_NOMAP);
 mtmr_stopping=true; assert(!mtmr_reserve(&p[0],MTMR_NOMAP));
 mtmr_stopping=false;
 s=mtmr_reserve(&p[0],MTMR_NOMAP); assert(s && mtmr_inflight[MTMR_NOMAP]==1);
 assert(!mtmr_reserve(&p[0],MTMR_NOMAP));
 mtmr_enabled_mask=0; assert(!mtmr_reserve(&p[1],MTMR_NOMAP));
}
'''
names = ('mtmr_pressure', 'mtmr_reserve', 'mi_mtmr_nomap', 'mi_mtmr_remap',
         'mtmr_take', 'mtmr_finish', 'mtmr_disable', 'mtmr_stop')
source = prefix + ''.join(function('mm/mi_mtmr.c', n) for n in names) + checks
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p/'test.c').write_text(source)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-Wall', '-Wextra', '-Werror',
                    str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
print('host nomap ownership: PASS; real PTE/LRU/RCU memory seam NOT RUN')
