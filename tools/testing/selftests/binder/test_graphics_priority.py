#!/usr/bin/env python3
"""Exercise actual binder_prio classification and bounded boost functions.

Task/RCU/scheduler substitutes verify decisions and ref balance; this is not
an IPC, priority-restoration or kernel lockdep stress test.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'drivers/staging/binder_prio/binder_prio.c').read_text()

def function(name):
    pos = source.index(name + '(')
    start = source.rfind('\n', 0, pos) + 1
    brace = source.index('{', pos)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#define TASK_COMM_LEN 16
#define SCHED_NORMAL 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_RESET_ON_FORK 0x40000000
#define MAX_RT_PRIO 100
#define TF_ONE_WAY 1U
#define CAP_SYS_NICE 1
#define RLIMIT_RTPRIO 1
#define READ_ONCE(x) (x)
struct task_struct {
    unsigned int policy;
    int normal_prio, pid, tgid, refs;
    bool cap_nice;
    unsigned long rt_limit;
    char comm[TASK_COMM_LEN];
    struct task_struct *group_leader;
};
struct binder_thread { struct task_struct *task; };
struct binder_transaction {
    unsigned int flags;
    struct binder_thread *from;
    pthread_mutex_t lock;
    int saved_priority;
};
static void get_task_comm(char *buf, struct task_struct *task) {
    memcpy(buf, task->comm, TASK_COMM_LEN);
}
static void get_task_struct(struct task_struct *task) { task->refs++; }
static void put_task_struct(struct task_struct *task) { task->refs--; }
static void rcu_read_lock(void) {}
static void rcu_read_unlock(void) {}
static void spin_lock(pthread_mutex_t *lock) { assert(!pthread_mutex_lock(lock)); }
static void spin_unlock(pthread_mutex_t *lock) { assert(!pthread_mutex_unlock(lock)); }
static bool has_capability_noaudit(struct task_struct *task, int cap) {
    (void)cap; return task->cap_nice;
}
#define task_rlimit(task, limit) ((task)->rt_limit)
static int calls, last_policy, last_rt;
static int sched_setscheduler_nocheck(struct task_struct *task, int policy,
                                     const struct sched_param *params) {
    calls++; last_policy = policy; last_rt = params->sched_priority;
    task->policy = policy & ~SCHED_RESET_ON_FORK;
    task->normal_prio = MAX_RT_PRIO - 1 - params->sched_priority;
    return 0;
}
static void name_task(struct task_struct *task, const char *name, int pid, int tgid,
                      struct task_struct *leader) {
    memset(task, 0, sizeof(*task));
    snprintf(task->comm, TASK_COMM_LEN, "%s", name);
    task->pid = pid; task->tgid = tgid; task->refs = 1;
    task->group_leader = leader ? leader : task;
    task->normal_prio = 120;
}
'''
body = '\n'.join(function(name) for name in [
    'binder_prio_is_rt', 'binder_prio_is_graphics_sender',
    'binder_prio_is_graphics_transaction', 'binder_prio_receiver_allows_rt',
    'binder_prio_set_priority'])
tests = r'''
int main(void) {
    struct task_struct home, render, sf, ui, other, camera, blur;
    struct binder_thread from;
    struct binder_transaction t = {.lock = PTHREAD_MUTEX_INITIALIZER, .saved_priority = 120};
    name_task(&home, "com.miui.home", 10, 10, NULL);
    name_task(&render, "RenderThread", 11, 10, &home);
    name_task(&sf, "surfaceflinger", 20, 20, NULL);
    name_task(&ui, "ndroid.systemui", 30, 30, NULL);
    name_task(&other, "other", 40, 40, NULL);
    name_task(&camera, "cameraserver", 50, 50, NULL);
    name_task(&blur, "passBlur", 21, 20, &sf);
    home.policy = render.policy = sf.policy = ui.policy = camera.policy = blur.policy = SCHED_FIFO;
    assert(binder_prio_is_graphics_sender(&render, &sf));
    assert(!binder_prio_is_graphics_sender(&render, &other));
    assert(binder_prio_is_graphics_sender(&home, &sf));
    assert(binder_prio_is_graphics_sender(&ui, &sf));
    assert(binder_prio_is_graphics_sender(&blur, &other));
    assert(binder_prio_is_graphics_sender(&camera, &other));
    other.policy = SCHED_FIFO;
    assert(!binder_prio_is_graphics_sender(&other, &sf));
    snprintf(other.comm, TASK_COMM_LEN, "com.miui.homeX");
    assert(!binder_prio_is_graphics_sender(&other, &sf));
    render.policy = SCHED_NORMAL;
    assert(!binder_prio_is_graphics_sender(&render, &sf));
    render.policy = SCHED_FIFO;
    from.task = &render; t.from = &from;
    assert(binder_prio_is_graphics_transaction(&t, &sf) && render.refs == 1);
    t.flags = TF_ONE_WAY;
    assert(!binder_prio_is_graphics_transaction(&t, &sf) && render.refs == 1);
    t.flags = 0; t.from = NULL;
    assert(!binder_prio_is_graphics_transaction(&t, &sf));
    t.from = &from;
    assert(!binder_prio_is_graphics_transaction(NULL, &sf));
    assert(!binder_prio_is_graphics_transaction(&t, NULL));
    sf.policy = SCHED_NORMAL; sf.normal_prio = 120;
    binder_prio_set_priority(NULL, &t, &sf);
    assert(calls == 0 && sf.policy == SCHED_NORMAL);
    sf.rt_limit = 1;
    binder_prio_set_priority(NULL, &t, &sf);
    assert(calls == 1 && last_rt == 1 && (last_policy & SCHED_RESET_ON_FORK));
    assert(sf.normal_prio == 98 && t.saved_priority == 120);
    binder_prio_set_priority(NULL, &t, &sf);
    assert(calls == 1);
    sf.policy = SCHED_RR; sf.normal_prio = 80;
    binder_prio_set_priority(NULL, &t, &sf);
    assert(calls == 1 && sf.policy == SCHED_RR && sf.normal_prio == 80);
    sf.policy = SCHED_NORMAL; sf.normal_prio = 120;
    sf.rt_limit = 0; sf.cap_nice = true;
    binder_prio_set_priority(NULL, &t, &sf);
    assert(calls == 2);
    sf.policy = SCHED_NORMAL; sf.normal_prio = 120; t.flags = TF_ONE_WAY;
    binder_prio_set_priority(NULL, &t, &sf);
    assert(calls == 2 && sf.policy == SCHED_NORMAL && render.refs == 1);
    pthread_mutex_destroy(&t.lock);
    puts("PASS: graphics classification, one-way/reply bypass, RT limits, stronger priority, refs");
}
'''
with tempfile.TemporaryDirectory(prefix='binder-graphics-') as temp:
    test, binary = Path(temp) / 'test.c', Path(temp) / 'test'
    test.write_text(preamble + body + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-pthread', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined'),
                    '-o', str(binary), str(test)], check=True)
    subprocess.run([str(binary)], check=True)
