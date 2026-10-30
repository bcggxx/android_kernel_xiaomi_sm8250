#!/usr/bin/env python3
"""Test actual optional reclaim policy/submit functions with userspace stubs.

This covers policy decisions and concurrent request ownership, not kernel
reclaim progress, kthread execution, CSS teardown, freezer or boot behavior.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]

def function(path, name):
    source = (root / path).read_text()
    pos = source.index(name + '(')
    start = source.rfind('\n', 0, pos) + 1
    brace = source.index('{', pos)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#define DEF_PRIORITY 12
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define max(a, b) ((a) > (b) ? (a) : (b))
typedef unsigned int gfp_t;
'''
policy_prefix = r'''
#define PAGE_SHIFT 12
#define SZ_16M (16UL << 20)
#define SZ_512M (512UL << 20)
#define MI_RMAP_MAX_PROTECTED_PAGES (SZ_16M >> PAGE_SHIFT)
#define __GFP_DMA 1U
#define __GFP_DMA32 2U
struct page { int mapcount; bool compound, swapbacked, anon, mapped; };
#define PageCompound(p) ((p)->compound)
#define PageSwapBacked(p) ((p)->swapbacked)
#define PageAnon(p) ((p)->anon)
#define page_mapped(p) ((p)->mapped)
#define page_mapcount(p) ((p)->mapcount)
enum mi_rmap_control { MI_RMAP_RECLAIM, MI_RMAP_KEEP, MI_RMAP_ACTIVATE };
static unsigned int mi_mapcount_thres = 20;
static bool mi_rmap_enabled;
struct scan_control { bool global; int order; gfp_t gfp_mask; };
static long available;
static long si_mem_available(void) { return available; }
static bool global_reclaim(struct scan_control *sc) { return sc->global; }
'''
policy_tests = r'''
int main(void) {
    struct page p = {.mapcount = 20, .mapped = true};
    struct scan_control sc = {.global = true};
    unsigned long count = 0;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM);
    mi_rmap_enabled = true;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_ACTIVATE && count == 1);
    p.mapcount = 19;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM);
    p.mapcount = 10;
    assert(mi_rmap_check(&p, 64, 11, &count) == MI_RMAP_KEEP && count == 2);
    p.mapcount = 9;
    assert(mi_rmap_check(&p, 64, 11, &count) == MI_RMAP_RECLAIM);
    p.mapcount = 100;
    assert(mi_rmap_check(&p, 64, 6, &count) == MI_RMAP_RECLAIM);
    count = MI_RMAP_MAX_PROTECTED_PAGES;
    assert(mi_rmap_check(&p, ULONG_MAX, 12, &count) == MI_RMAP_RECLAIM);
    count = 1;
    assert(mi_rmap_check(&p, 16, 9, &count) == MI_RMAP_RECLAIM);
    assert(mi_rmap_check(&p, 32, 9, &count) == MI_RMAP_ACTIVATE && count == 2);
    assert(mi_rmap_check(&p, 32, 9, &count) == MI_RMAP_RECLAIM);
    p.compound = true;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM);
    p.compound = false; p.anon = true;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM);
    p.anon = false; p.swapbacked = true;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM);
    p.swapbacked = false; p.mapped = false;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM);
    assert(mi_rmap_check(NULL, 64, 12, &count) == MI_RMAP_RECLAIM);
    assert(mi_rmap_check(&p, 64, 12, NULL) == MI_RMAP_RECLAIM);
    available = (SZ_512M >> PAGE_SHIFT) + 1;
    assert(mi_rmap_may_protect(&sc, false));
    available--;
    assert(!mi_rmap_may_protect(&sc, false));
    available = -1;
    assert(!mi_rmap_may_protect(&sc, false));
    available = (SZ_512M >> PAGE_SHIFT) + 1;
    assert(!mi_rmap_may_protect(&sc, true));
    sc.global = false;
    assert(!mi_rmap_may_protect(&sc, false));
    sc.global = true; sc.order = 1;
    assert(!mi_rmap_may_protect(&sc, false));
    sc.order = 0; sc.gfp_mask = __GFP_DMA;
    assert(!mi_rmap_may_protect(&sc, false));
    sc.gfp_mask = __GFP_DMA32;
    assert(!mi_rmap_may_protect(&sc, false));
    puts("PASS: mapcount thresholds, shared budgets, pressure and allocator fallbacks");
}
'''
submit_prefix = r'''
struct cgroup_subsys_state { atomic_int refs; atomic_bool online; };
struct mem_cgroup { struct cgroup_subsys_state css; };
struct task_struct { void *reclaim_state; };
struct slabd_request { struct mem_cgroup *memcg; gfp_t gfp_mask; int nid, priority; };
static pthread_mutex_t slabd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct task_struct worker;
static struct task_struct *slabd_task;
static struct slabd_request slabd_request;
static bool slabd_pending, slabd_busy;
static bool slabd_enabled = true;
static int slabd_wait;
static atomic_int wake_count, winners;
static struct mem_cgroup memcg;
static pthread_barrier_t barrier;
static _Thread_local int caller_kswapd;
static _Thread_local struct task_struct caller;
#define current (&caller)
#define current_is_kswapd() (caller_kswapd)
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; assert(!pthread_mutex_lock(lock)); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); assert(!pthread_mutex_unlock(lock)); } while (0)
static bool css_tryget_online(struct cgroup_subsys_state *css) {
    if (!atomic_load(&css->online)) return false;
    atomic_fetch_add(&css->refs, 1); return true;
}
static void wake_up(int *queue) { (void)queue; atomic_fetch_add(&wake_count, 1); }
'''
submit_tests = r'''
static void complete_request(void) {
    assert(slabd_pending && atomic_load(&memcg.css.refs) == 2);
    assert(slabd_request.memcg == &memcg && slabd_request.nid == 0);
    assert(slabd_request.priority == 12 && slabd_request.gfp_mask == 7);
    slabd_pending = false;
    atomic_fetch_sub(&memcg.css.refs, 1);
}
static void *submit(void *unused) {
    (void)unused; caller_kswapd = 1; caller.reclaim_state = &caller;
    pthread_barrier_wait(&barrier);
    if (wakeup_kshrink_slabd(7, 0, &memcg, 12)) atomic_fetch_add(&winners, 1);
    assert(caller.reclaim_state == &caller);
    return NULL;
}
int main(void) {
    enum { THREADS = 8, ROUNDS = 64 };
    pthread_t threads[THREADS];
    atomic_init(&memcg.css.refs, 1); atomic_init(&memcg.css.online, true);
    caller_kswapd = 1;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12));
    slabd_task = &worker; slabd_enabled = false;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12));
    slabd_enabled = true; caller_kswapd = 0;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12));
    caller_kswapd = 1;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 9));
    atomic_store(&memcg.css.online, false);
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12));
    atomic_store(&memcg.css.online, true);
    caller.reclaim_state = &caller;
    assert(wakeup_kshrink_slabd(7, 0, &memcg, 12));
    assert(caller.reclaim_state == &caller);
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12));
    complete_request();
    slabd_busy = true;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12));
    slabd_busy = false;
    assert(!pthread_barrier_init(&barrier, NULL, THREADS));
    for (int round = 0; round < ROUNDS; round++) {
        atomic_store(&winners, 0);
        for (int i = 0; i < THREADS; i++) assert(!pthread_create(&threads[i], NULL, submit, NULL));
        for (int i = 0; i < THREADS; i++) assert(!pthread_join(threads[i], NULL));
        assert(atomic_load(&winners) == 1);
        complete_request();
        assert(atomic_load(&memcg.css.refs) == 1);
    }
    assert(atomic_load(&wake_count) == ROUNDS + 1);
    pthread_barrier_destroy(&barrier);
    puts("PASS: concurrent slab submissions, memcg ownership, busy/pressure/direct fallbacks");
}
'''
programs = {
    'policy': common + policy_prefix +
        function('mm/mi_rmap_efficiency.c', 'mi_keep_threshold') +
        function('mm/mi_rmap_efficiency.c', 'mi_rmap_check') +
        function('mm/vmscan.c', 'mi_rmap_may_protect') + policy_tests,
    'submit': common + submit_prefix +
        function('mm/kshrink_slabd.c', 'wakeup_kshrink_slabd') + submit_tests,
}
with tempfile.TemporaryDirectory(prefix='dagu-reclaim-') as temp:
    for name, code in programs.items():
        test, binary = Path(temp) / (name + '.c'), Path(temp) / name
        test.write_text(code)
        subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-pthread',
                        '-Wall', '-Wextra', '-Wno-unused-parameter', '-g',
                        '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined'),
                        '-o', str(binary), str(test)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
