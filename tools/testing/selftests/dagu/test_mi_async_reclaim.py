#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Finite host safety regression of actual admission/ownership functions.

Stubs do not prove kernel rmap locks, LRU, freezer or reclaim behavior.
"""
import os
import subprocess
import tempfile
from pathlib import Path
from c_helpers import function

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define MI_ASYNC_MAX_PAGES 256
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &n, &n }
static void list_add_tail(struct list_head *n, struct list_head *h) {
 n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_del_init(struct list_head *n) {
 n->prev->next=n->next; n->next->prev=n->prev; n->next=n->prev=n;
}
#define list_first_entry(h,t,m) ((t *)((char *)(h)->next - offsetof(t,m)))
struct page { struct list_head lru; bool compound, anon, swapbacked, ksm; };
#define PageCompound(p) ((p)->compound)
#define PageAnon(p) ((p)->anon)
#define PageSwapBacked(p) ((p)->swapbacked)
#define PageKsm(p) ((p)->ksm)
#define ClearPageActive(p) ((void)(p))
#define spin_lock_irqsave(l,f) ((void)(l), (f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l), (void)(f))
#define wake_up(w) ((void)(w))
static int mi_async_lock, mi_async_wait;
static bool mi_async_enabled;
static LIST_HEAD(mi_async_pending);
static unsigned long mi_async_pending_pages, mi_async_running_pages;
static unsigned long mi_async_accepted, mi_async_reclaimed, mi_async_returned;
static int *mi_async_task;
static unsigned cancelled, joined;
#define NR_ISOLATED_FILE 0
#define WRITE_ONCE(x,v) ((x)=(v))
#define list_for_each_entry_safe(p,n,h,m) \
 for (p=list_first_entry(h,struct page,m), n=list_first_entry(&p->m,struct page,m); \
 &p->m!=(h); p=n,n=list_first_entry(&n->m,struct page,m))
static void list_splice_init(struct list_head *h, struct list_head *to) {
 if (!list_empty(h)) { to->next=h->next; to->prev=h->prev;
 to->next->prev=to; to->prev->next=to; h->next=h->prev=h; }
}
static void dec_node_page_state(struct page *p, int i) {(void)p; (void)i; cancelled++;}
static void putback_lru_page(struct page *p) {(void)p;}
static void kthread_stop(int *t) {(void)t; joined++;}

'''
checks = r'''
static void conserved(void) {
 assert(mi_async_accepted == mi_async_pending_pages + mi_async_running_pages +
        mi_async_reclaimed + mi_async_returned);
}
int main(void) {
 struct page p[257] = {0};
 assert(!mi_async_reclaim_queue(&p[0])); conserved();
 mi_async_enabled=true;
 p[0].anon=true; assert(!mi_async_reclaim_queue(&p[0]));
 p[0].anon=false; p[0].compound=true; assert(!mi_async_reclaim_queue(&p[0]));
 p[0].compound=false; p[0].swapbacked=true; assert(!mi_async_reclaim_queue(&p[0]));
 p[0].swapbacked=false; p[0].ksm=true; assert(!mi_async_reclaim_queue(&p[0]));
 p[0].ksm=false;
 for (int i=0; i<256; i++) assert(mi_async_reclaim_queue(&p[i]));
 assert(!mi_async_reclaim_queue(&p[256])); conserved();
 assert(mi_async_take_page()==&p[0]); conserved();
 assert(!mi_async_reclaim_queue(&p[256])); /* running consumes capacity */
 mi_async_complete(1, 1); conserved();
 assert(mi_async_reclaim_queue(&p[256])); conserved();
 mi_async_enabled=false;
 assert(!mi_async_reclaim_queue(&p[0]));
 assert(mi_async_take_page()==NULL); conserved();
 mi_async_enabled=true;
 while (mi_async_take_page()) { mi_async_complete(1, 0); conserved(); }
 mi_async_enabled=false;
 assert(mi_async_pending_pages==0 && mi_async_running_pages==0);
 assert(mi_async_reclaimed==1 && mi_async_returned==256); conserved();
 mi_async_enabled=true;
 assert(mi_async_reclaim_queue(&p[0]));
 assert(mi_async_reclaim_queue(&p[1]));
 mi_async_task=(int *)1;
 mi_async_stop(); conserved();
 assert(!mi_async_enabled && !mi_async_task && joined==1 && cancelled==2);
 assert(mi_async_pending_pages==0 && mi_async_running_pages==0);
 assert(mi_async_returned==258);
 mi_async_stop(); conserved();
 assert(joined==1 && cancelled==2); /* empty stop is idempotent */
}
'''
source = prefix + ''.join(function('mm/mi_async_reclaim.c', name) for name in (
    'mi_async_reclaim_queue', 'mi_async_take_page', 'mi_async_complete', 'mi_async_stop')) + checks
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(source)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('host admission/ownership regression: PASS; real memory seam NOT RUN')
