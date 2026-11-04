#!/usr/bin/env python3
"""Test actual optional reclaim policy/submit functions with userspace stubs.

This covers policy decisions and concurrent request ownership, not kernel
reclaim progress, kthread execution, CSS teardown, freezer or boot behavior.
"""
import os
import re
from pathlib import Path
import subprocess
import tempfile

from c_helpers import function, read_source

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
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
MI_RMAP_ENABLED_DECL
struct scan_control { bool global; int order; gfp_t gfp_mask; };
static long available;
static long si_mem_available(void) { return available; }
static bool global_reclaim(struct scan_control *sc) { return sc->global; }
'''
policy_prefix = policy_prefix.replace('MI_RMAP_ENABLED_DECL', re.search(
    r'^static bool mi_rmap_enabled[^;]*;', read_source('mm/mi_rmap_efficiency.c'),
    re.MULTILINE)[0].replace('__read_mostly', ''))
policy_prefix += r'''
#define __init
#define ENOMEM 12
struct kobject { int unused; };
static struct kobject object;
static struct kobject *kernel_kobj = &object;
static int mi_rmap_attr_group;
static struct kobject *kobject_create_and_add(const char *name, struct kobject *parent) { return &object; }
static int sysfs_create_group(struct kobject *obj, int *group) { return 0; }
static void kobject_put(struct kobject *obj) { }
'''
policy_tests = r'''
int main(void) {
    struct page p = {.mapcount = 20, .mapped = true};
    struct scan_control sc = {.global = true};
    unsigned long count = 0;
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM);
    assert(!mi_rmap_efficiency_init());
    assert(mi_rmap_check(&p, 64, 12, &count) == MI_RMAP_RECLAIM && count == 0);
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
    /* Protection is exhausted; bypass permits deactivation, never retention. */
    assert(mi_rmap_should_bypass(&p, 9));
    assert(count == 2);
    mi_rmap_enabled = false;
    assert(!mi_rmap_should_bypass(&p, 9));
    mi_rmap_enabled = true;
    assert(!mi_rmap_should_bypass(&p, 6));
    assert(!mi_rmap_should_bypass(&p, 10));
    p.mapcount = 9;
    assert(!mi_rmap_should_bypass(&p, 9));
    p.mapcount = 10;
    assert(mi_rmap_should_bypass(&p, 9));
    p.compound = true;
    assert(!mi_rmap_should_bypass(&p, 9));
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
static bool slabd_pending, slabd_busy, slabd_global_scan;
static struct mem_cgroup *root_mem_cgroup;
static bool slabd_under_pressure(int nid) { return false; }
SLABD_ENABLED_DECL
static int slabd_wait;
static atomic_int wake_count, winners;
static struct mem_cgroup memcg;
static pthread_barrier_t barrier;
static _Thread_local int caller_kswapd;
static _Thread_local struct task_struct caller;
static _Thread_local bool gate_submit;
static atomic_bool submit_at_lock, release_submit;
static void test_lock(pthread_mutex_t *lock) {
    if (gate_submit) {
        atomic_store(&submit_at_lock, true);
        while (!atomic_load(&release_submit)) sched_yield();
    }
    assert(!pthread_mutex_lock(lock));
}
#define current (&caller)
#define current_is_kswapd() (caller_kswapd)
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; test_lock(lock); } while (0)
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
    if (wakeup_kshrink_slabd(7, 0, &memcg, 12, true)) atomic_fetch_add(&winners, 1);
    assert(caller.reclaim_state == &caller);
    return NULL;
}
int main(void) {
    enum { THREADS = 8, ROUNDS = 64 };
    pthread_t threads[THREADS];
    atomic_init(&memcg.css.refs, 1); atomic_init(&memcg.css.online, true);
    caller_kswapd = 1;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    slabd_task = &worker; slabd_enabled = false;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    slabd_enabled = true; caller_kswapd = 0;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    caller_kswapd = 1;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 9, true));
    atomic_store(&memcg.css.online, false);
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    atomic_store(&memcg.css.online, true);
    caller.reclaim_state = &caller;
    assert(wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    assert(caller.reclaim_state == &caller);
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    complete_request();
    slabd_busy = true;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
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
slabd_source = read_source('mm/kshrink_slabd.c')
# Use the actual initial state and registered setter, including the legacy
# module_param_named path, so the pre-fix source fails on behavior.
enabled_decl = re.search(r'^static bool slabd_enabled[^;]*;', slabd_source,
                         re.MULTILINE)[0].replace('__read_mostly', '')
submit_prefix = submit_prefix.replace('SLABD_ENABLED_DECL', enabled_decl)
control_functions = ''
if 'module_param_cb(enabled,' in slabd_source:
    control_functions = function('mm/kshrink_slabd.c', 'slabd_is_idle') + \
        function('mm/kshrink_slabd.c', 'slabd_set_enabled')
    control_functions += re.search(
        r'static const struct kernel_param_ops slabd_enabled_ops = \{.*?\};',
        slabd_source, re.DOTALL)[0] + '\n'
    registered_setter = 'slabd_enabled_ops.set'
else:
    assert 'module_param_named(enabled, slabd_enabled, bool, 0644)' in slabd_source
    registered_setter = 'param_set_bool'

control_prefix = r'''
#include <errno.h>
#include <string.h>
#include <sched.h>
struct kernel_param { void *arg; };
#define KERNEL_PARAM_OPS_FL_NOARG 1
struct kernel_param_ops {
    unsigned int flags;
    int (*set)(const char *, const struct kernel_param *);
    int (*get)(char *, const struct kernel_param *);
};
static int param_get_bool(char *buffer, const struct kernel_param *kp) {
    (void)buffer; (void)kp; return 0;
}
static pthread_mutex_t parameter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static atomic_bool drain_waiting, disable_done;
static bool interrupt_drain;
static int slabd_idle;
static int param_set_bool(const char *val, const struct kernel_param *kp) {
    bool parsed;
    if (!val || !strcmp(val, "1")) parsed = true;
    else if (!strcmp(val, "0")) parsed = false;
    else return -EINVAL;
    *(bool *)kp->arg = parsed;
    return 0;
}
#undef wake_up
#define wake_up(queue) do { \
    (void)(queue); \
    assert(!pthread_mutex_lock(&wait_lock)); \
    assert(!pthread_cond_broadcast(&wait_cond)); \
    assert(!pthread_mutex_unlock(&wait_lock)); \
} while (0)
#define wait_event(queue, condition) do { \
    (void)(queue); \
    assert(!pthread_mutex_lock(&wait_lock)); \
    atomic_store(&drain_waiting, true); \
    while (!(condition)) assert(!pthread_cond_wait(&wait_cond, &wait_lock)); \
    assert(!pthread_mutex_unlock(&wait_lock)); \
} while (0)
#define wait_event_freezable(queue, condition) ({ \
    int ret = -EINTR; \
    if (!interrupt_drain) { wait_event(queue, condition); ret = 0; } \
    ret; \
})
'''
control_tests = r'''
static int set_enabled(const char *value) {
    struct kernel_param kp = { .arg = &slabd_enabled };
    assert(!pthread_mutex_lock(&parameter_lock));
    int ret = REGISTERED_SETTER(value, &kp);
    assert(!pthread_mutex_unlock(&parameter_lock));
    return ret;
}
static void *disable(void *unused) {
    assert(!set_enabled("0"));
    atomic_store(&disable_done, true);
    return NULL;
}
static void *delayed_submit(void *unused) {
    caller_kswapd = 1;
    gate_submit = true;
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    return NULL;
}
static void finish_request(void) {
    unsigned long flags;
    spin_lock_irqsave(&slabd_lock, flags);
    assert(slabd_pending || slabd_busy);
    /* Model the worker's reclaim completion and CSS put before going idle. */
    atomic_fetch_sub(&memcg.css.refs, 1);
    WRITE_ONCE(slabd_pending, false);
    WRITE_ONCE(slabd_busy, false);
    spin_unlock_irqrestore(&slabd_lock, flags);
    wake_up(&slabd_idle);
}
int main(void) {
    pthread_t closer;
    caller_kswapd = 1;
    slabd_task = &worker;
    atomic_init(&memcg.css.refs, 1);
    atomic_init(&memcg.css.online, true);
    /* Default admission, exercised through the real submission entry. */
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    assert(!set_enabled("0"));
    assert(!set_enabled(NULL));
    assert(wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    finish_request();
    assert(!set_enabled("0"));
    for (int running = 0; running < 2; running++) {
        assert(!set_enabled("1"));
        assert(set_enabled("invalid") == -EINVAL);
        assert(wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
        assert(atomic_load(&memcg.css.refs) == 2);
        if (running) { slabd_pending = false; slabd_busy = true; }
        atomic_store(&drain_waiting, false);
        atomic_store(&disable_done, false);
        assert(!pthread_create(&closer, NULL, disable, NULL));
        /* The drain wait stub is a scheduling handshake, not a sleep race. */
        while (!atomic_load(&drain_waiting) && !atomic_load(&disable_done))
            sched_yield();
        assert(!atomic_load(&disable_done));
        assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
        finish_request();
        assert(!pthread_join(closer, NULL));
        assert(atomic_load(&disable_done));
        assert(atomic_load(&memcg.css.refs) == 1);
        assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
        assert(set_enabled("invalid") == -EINVAL);
        assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
        assert(!set_enabled("0"));
    }
    assert(!set_enabled("1"));
    assert(wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    interrupt_drain = true;
    assert(set_enabled("0") == -EINTR);
    assert(!wakeup_kshrink_slabd(7, 0, &memcg, 12, true));
    assert(atomic_load(&memcg.css.refs) == 2);
    interrupt_drain = false;
    finish_request();
    assert(!set_enabled("0"));
    assert(atomic_load(&memcg.css.refs) == 1);
    /* Pause a producer before the shared lock, then finish disable first. */
    assert(!set_enabled("1"));
    assert(!pthread_create(&closer, NULL, delayed_submit, NULL));
    while (!atomic_load(&submit_at_lock)) sched_yield();
    assert(!set_enabled("0"));
    atomic_store(&release_submit, true);
    assert(!pthread_join(closer, NULL));
    assert(atomic_load(&memcg.css.refs) == 1);
    puts("PASS: default sync, invalid control, pending/running drain, close/submit race");
}
'''
control_tests = control_tests.replace('REGISTERED_SETTER', registered_setter)

programs = {
    'policy': common + policy_prefix +
        function('mm/mi_rmap_efficiency.c', 'mi_keep_threshold') +
        function('mm/mi_rmap_efficiency.c', 'mi_rmap_check') +
        function('mm/mi_rmap_efficiency.c', 'mi_rmap_should_bypass') +
        function('mm/mi_rmap_efficiency.c', 'mi_rmap_efficiency_init') +
        function('mm/vmscan.c', 'mi_rmap_may_protect') + policy_tests,
    'control': common + submit_prefix + control_prefix + control_functions +
        function('mm/kshrink_slabd.c', 'wakeup_kshrink_slabd') + control_tests,
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
