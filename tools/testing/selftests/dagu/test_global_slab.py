#!/usr/bin/env python3
"""Bounded production-function global slab policy/control/iterator check.

CSS/iterator/watermarks are substitutes, not real memcg lifecycle evidence.
"""
import os
from pathlib import Path
import subprocess
import tempfile
from c_helpers import function, read_source

source = 'mm/kshrink_slabd.c'
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
typedef uint64_t u64;
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define clamp_t(t,v,l,h) min((t)(h), ((t)(v)<(t)(l)?(t)(l):(t)(v)))
#define div64_u64(a,b) ((a)/(b))
#define PAGE_SHIFT 12
#define GFP_KERNEL 7
#define NR_SLAB_RECLAIMABLE 0
#define MAX_SCHEDULE_TIMEOUT LONG_MAX
#define msecs_to_jiffies(x) ((unsigned long)(x))
#define time_before(a,b) ((long)((a)-(b))<0)
#define time_after(a,b) time_before(b,a)
#define time_after_eq(a,b) (!time_before(a,b))
static unsigned long jiffies;
static unsigned long global_node_page_state(int item) { (void)item; return 51200; }
#define DEFINE_MUTEX(x) int x
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
struct timer_list {bool pending; unsigned long expires;};
static struct timer_list slabd_launch_timer;
static void del_timer_sync(struct timer_list *t) {t->pending=false;}
static void mod_timer(struct timer_list *t, unsigned long deadline) {t->pending=true;t->expires=deadline;}
static int kstrtouint(const char *s,int base,unsigned int *v) {
 char *end; unsigned long value; (void)base;
 if(*s=='-') return -EINVAL;
 value=strtoul(s,&end,10); if(*end || value>UINT_MAX) return -EINVAL;
 *v=value; return 0;
}

#define DEF_PRIORITY 12
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
typedef unsigned int gfp_t;
struct cgroup_subsys_state { int refs; bool online; };
struct mem_cgroup { struct cgroup_subsys_state css; int id; };
struct task_struct { int unused; };
struct slabd_request { struct mem_cgroup *memcg; gfp_t gfp_mask; int nid, priority; bool policy; };
static struct mem_cgroup groups[3];
static struct mem_cgroup *root_mem_cgroup = &groups[0];
static struct task_struct worker, caller;
static struct task_struct *current = &caller;
static struct task_struct *slabd_task = &worker;
static struct slabd_request slabd_request;
static bool slabd_pending, slabd_busy, slabd_enabled, slabd_global_scan;
static int slabd_lock, slabd_idle, slabd_wait;
static bool caller_kswapd = true, pressure, stopping, memcg_disabled;
static bool mem_cgroup_disabled(void) {return memcg_disabled;}
static int calls, breaks, yields, disable_after, pressure_after, pins;
static unsigned long result;
#define spin_lock_irqsave(l,f) do { (void)(l); (f)=0; } while (0)
#define spin_unlock_irqrestore(l,f) do { (void)(l); (void)(f); } while (0)
#define current_is_kswapd() caller_kswapd
static bool css_tryget_online(struct cgroup_subsys_state *css) {
    if (!css->online) return false;
    css->refs++; pins++; return true;
}
static void mem_cgroup_put(struct mem_cgroup *cg) { if(cg) {cg->css.refs--; pins--;} }
static void wake_up(int *q) {(void)q;}
#define wake_up_all wake_up
static bool kthread_should_stop(void) {return stopping;}
static bool slabd_under_pressure(int nid) {assert(nid == 0); return pressure;}
static void cond_resched(void) {yields++;}
struct kernel_param {void *arg;};
static int param_set_bool(const char *s, const struct kernel_param *kp) {
    if (!strcmp(s,"1")) *(bool *)kp->arg=true;
    else if (!strcmp(s,"0")) *(bool *)kp->arg=false;
    else return -EINVAL;
    return 0;
}
#define wait_event_freezable(q,c) ({(void)(q); assert(c); 0;})
static struct mem_cgroup *mem_cgroup_iter(void *root, struct mem_cgroup *prev, void *cookie) {
    (void)root; (void)cookie;
    int i = prev ? prev->id + 1 : 0;
    if(prev) prev->css.refs--;
    if(i == 3) return NULL;
    groups[i].css.refs++; return &groups[i];
}
static void mem_cgroup_iter_break(void *root, struct mem_cgroup *prev) {
    (void)root; assert(prev); prev->css.refs--; breaks++;
}
static unsigned long shrink_slab(gfp_t gfp, int nid, struct mem_cgroup *cg, int priority) {
    assert(gfp == 7 && nid == 0 && priority == 12);
    assert(cg == (memcg_disabled ? NULL : &groups[calls])); calls++;
    if(disable_after == calls) slabd_enabled=false;
    if(pressure_after == calls) pressure=true;
    return result;
}
'''
state = read_source(source).split('static DEFINE_MUTEX(slabd_control);',1)[1].split('static bool slabd_policy_active',1)[0]
prefix += 'static DEFINE_MUTEX(slabd_control);' + state
main = r'''
static void balanced(void) {for(int i=0;i<3;i++) assert(groups[i].css.refs == 1); assert(!pins);}
int main(void) {
    struct kernel_param kp = {0}; struct slabd_request request;
 unsigned int counter=0;
 assert(slabd_policy_delay(200,100,200,30000,false,10000,&counter)==1000 && counter==0);
 assert(slabd_policy_delay(100,100,200,30000,false,10000,&counter)==10000);
 assert(slabd_policy_delay(150,100,200,30000,false,10000,&counter)==5500);
 assert(slabd_policy_delay(200,100,200,120000,false,0,&counter)==11000 && counter==1);
 counter=0;
 assert(slabd_policy_delay(200,100,200,120000,false,1,&counter)==5000 && counter==1);
 counter=0;
 assert(slabd_policy_delay(200,100,200,120000,true,0,&counter)==11000 && counter==1);
 counter=5;
 assert(slabd_policy_delay(0,100,200,30000,false,0,&counter)==30000 && counter==5);
 assert(slabd_policy_delay(200,100,200,120000,false,10000,&counter)==1000 && counter==0);

    for(int i=0;i<3;i++) {groups[i].id=i; groups[i].css.refs=1; groups[i].css.online=true;}
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true));
    assert(!slabd_set_global_scan("1", &kp));
    slabd_enabled=true;
    assert(slabd_set_global_scan("0", &kp) == -EBUSY);
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,false));
    assert(!wakeup_kshrink_slabd(7,0,&groups[1],12,true));
    caller_kswapd=false; assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true)); caller_kswapd=true;
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,9,true));
    pressure=true; assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true)); pressure=false;
    assert(wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true));
    assert(!pins && !slabd_request.memcg);
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true));
    /* Wake predicate was true, but disable wins before worker takes the slot. */
    assert(!slabd_set_enabled("0", &kp));
    assert(!slabd_take_request(&request)); assert(!slabd_busy && !slabd_pending);
    slabd_enabled=true;
    assert(wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true));
    assert(slabd_take_request(&request));
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true));
    slabd_scan_global(&request); assert(calls == 3 && !breaks && yields == 3); balanced();
    slabd_busy=false;
    for(int reason=0;reason<2;reason++) {
        calls=breaks=yields=0; slabd_enabled=true; pressure=false;
        disable_after= reason ? 0 : 1; pressure_after= reason ? 1 : 0;
        slabd_scan_global(&request);
        assert(calls == 1 && breaks == 1); balanced();
    }
    calls=0; disable_after=pressure_after=0; pressure=false;
    slabd_enabled=true; memcg_disabled=true; slabd_scan_global(&request);
    assert(calls == 1); balanced(); memcg_disabled=false;
    slabd_enabled=false; slabd_busy=true;
    assert(slabd_set_global_scan("0", &kp) == -EBUSY);
    slabd_busy=false; assert(!slabd_set_global_scan("0", &kp));
    slabd_enabled=true; pressure=false;
    assert(wakeup_kshrink_slabd(7,0,&groups[1],12,false)); assert(pins==1);
    assert(slabd_take_request(&request)); mem_cgroup_put(request.memcg); slabd_busy=false; balanced();
    /* Actual policy/control functions share the transfer slot. */
    slabd_enabled=false; slabd_global_scan=true;
    assert(!slabd_set_policy("1", &kp));
    assert(!slabd_set_enabled("1", &kp));
    assert(slabd_deadline_valid && slabd_sleep_ms==30000);
    assert(slabd_wait_timeout()==30000);
    assert(slabd_worker_ready(slabd_control_epoch-1)); /* enable breaks indefinite wait */
    jiffies=slabd_deadline; slabd_policy_schedule();
    assert(slabd_pending && slabd_request.policy && slabd_periodic==1);
    assert(slabd_take_request(&request));
    calls=0; assert(slabd_scan_global(&request)==0 && calls==3); balanced();
    slabd_finish_request(0,false);
    assert(!slabd_busy && slabd_backoff==1 && slabd_scanned);
    jiffies=slabd_deadline; slabd_policy_schedule();
    assert(!slabd_pending && slabd_recalculations==1 && slabd_backoff==2);
    /* Low priority input stays synchronous but creates one force hint. */
    jiffies+=2000;
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,2,true));
    assert(slabd_force && slabd_pending_priority==2 && slabd_force_wakes==1);
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,2,true));
    assert(slabd_force_wakes==1);
    assert(!slabd_set_launch("1", &kp));
    unsigned long expires=slabd_launch_timer.expires;
    assert(!slabd_set_launch("1", &kp) && slabd_launch_timer.expires==expires);
    assert(slabd_set_launch("2", &kp)==-EINVAL);
    slabd_policy_schedule(); assert(slabd_take_request(&request));
    assert(request.priority==2 && !slabd_skip_policy(&request));
    slabd_finish_request(10000,false);
    assert(slabd_backoff==0);
    assert(!wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true)); /* launch fallback */
    jiffies=slabd_deadline; slabd_policy_schedule();
    assert(slabd_take_request(&request) && slabd_skip_policy(&request));
    slabd_finish_request(0,true);
    assert(slabd_skips==1 && slabd_root_hint && !slabd_busy);
    slabd_launch_timer.pending=false; slabd_launch_timeout(&slabd_launch_timer);
    assert(!slabd_launch && slabd_launch_ended==1);
    slabd_launch_timeout(&slabd_launch_timer); assert(slabd_launch_ended==1);
    /* Transfer admitted before launch cannot be skipped. */
    assert(wakeup_kshrink_slabd(7,0,root_mem_cgroup,12,true));
    assert(slabd_take_request(&request)); assert(!slabd_set_launch("1", &kp));
    assert(!slabd_skip_policy(&request)); slabd_finish_request(1,false);
    struct kernel_param setting={.arg=&slabd_high_mb};
    assert(slabd_set_uint("50",&setting)==-EINVAL);
    setting.arg=&slabd_low_mb; assert(slabd_set_uint("2048",&setting)==-EINVAL);
    setting.arg=&slabd_max_sleep_ms; assert(slabd_set_uint("999",&setting)==-EINVAL);
    setting.arg=&slabd_priority; assert(slabd_set_uint("13",&setting)==-EINVAL);
    slabd_pending_priority=2; assert(!slabd_set_uint("12",&setting));
    assert(slabd_pending_priority==2);
    assert(!slabd_set_enabled("0", &kp));
    assert(!slabd_launch && !slabd_launch_timer.pending && !slabd_deadline_valid);
    assert(!slabd_pending && !slabd_busy && !slabd_iterator_live);
    unsigned long rounds=slabd_periodic;
    jiffies+=100000; slabd_policy_schedule(); assert(slabd_periodic==rounds);
    puts("PASS: production global scan/policy, force hints, launch skip/expiry/transfer, control validation, quiescent disable");
}
'''
functions = ['slabd_policy_active', 'slabd_policy_delay', 'slabd_set_deadline',
 'slabd_end_launch', 'slabd_launch_timeout', 'slabd_set_policy', 'slabd_set_launch', 'slabd_set_uint',
 'slabd_is_idle', 'slabd_set_enabled', 'slabd_set_global_scan',
             'wakeup_kshrink_slabd', 'slabd_take_request', 'slabd_scan_global', 'slabd_policy_schedule', 'slabd_wait_timeout',
 'slabd_worker_ready', 'slabd_skip_policy', 'slabd_finish_request']
with tempfile.TemporaryDirectory(prefix='dagu-global-slab-') as tmp:
    src=Path(tmp)/'check.c'; binary=Path(tmp)/'check'
    src.write_text(prefix + '\n'.join(function(source, name) for name in functions) + main)
    subprocess.run([os.environ.get('HOSTCC','cc'), '-std=gnu11','-Wall','-Wextra',
                    '-Wno-unused-parameter','-fsanitize=undefined','-o',str(binary),str(src)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)
