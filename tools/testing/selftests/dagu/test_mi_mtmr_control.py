#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Finite actual sysfs control check; no worker/lifetime/concurrency model."""
import os
import subprocess
import tempfile
from pathlib import Path
from c_helpers import function

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>
#define MTMR_NOMAP 0
#define MTMR_RA 1
#define BIT(n) (1U << (n))
#define IS_ENABLED(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define spin_lock_irqsave(l,f) ((void)(l), (f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l), (void)(f))
#define IS_ERR(t) (false)
#define PTR_ERR(t) (-ENOMEM)
static unsigned mtmr_enabled_mask, starts, disables[2];
static bool mtmr_stopping;
static int mtmr_lock, mtmr_control, mtmr_started, task, *mtmr_task;
static int kstrtobool(const char *s, bool *b) {
 if (*s != '0' && *s != '1') return -EINVAL;
 *b = *s == '1'; return 0;
}
static int mutex_lock_interruptible(int *l) { (void)l; return 0; }
static void mutex_unlock(int *l) { (void)l; }
static void reinit_completion(int *c) { (void)c; }
static void wait_for_completion(int *c) { (void)c; }
static int mtmr_worker(void *p) { (void)p; return 0; }
static int *kthread_run(int (*fn)(void *), void *p, const char *n) {
 (void)fn; (void)p; (void)n; starts++; return &task;
}
static void mtmr_disable(unsigned kind) {
 disables[kind]++; mtmr_enabled_mask &= ~BIT(kind);
}
'''
checks = r'''
int main(void) {
 assert(mtmr_enable_store(MTMR_RA, "bad", 3) == -EINVAL);
 assert(!starts && !mtmr_enabled_mask);
 if (CONFIG_READAHEAD_LIMIT) {
  assert(mtmr_enable_store(MTMR_RA, "1", 1) == 1);
  assert(starts == 1 && mtmr_enabled_mask == BIT(MTMR_RA));
 } else {
  assert(mtmr_enable_store(MTMR_RA, "1", 1) == -EOPNOTSUPP);
  assert(!starts && !mtmr_task && !mtmr_enabled_mask);
 }
 assert(mtmr_enable_store(MTMR_RA, "0", 1) == 1);
 assert(disables[MTMR_RA] == 1 && !mtmr_enabled_mask);
 assert(mtmr_enable_store(MTMR_NOMAP, "1", 1) == 1);
 assert(mtmr_enabled_mask == BIT(MTMR_NOMAP));
 assert(starts == 1);
 if (!CONFIG_READAHEAD_LIMIT) {
  assert(mtmr_enable_store(MTMR_RA, "1", 1) == -EOPNOTSUPP);
  assert(mtmr_enabled_mask == BIT(MTMR_NOMAP) && starts == 1);
 }
 unsigned before = starts;
 assert(mtmr_enable_store(MTMR_NOMAP, "0", 1) == 1);
 assert(disables[MTMR_NOMAP] == 1 && !mtmr_enabled_mask && starts == before);
 return 0;
}
'''
source = prefix + function('mm/mi_mtmr.c', 'mtmr_enable_store') + checks
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'control.c'
    src.write_text(source)
    for available in (0, 1):
        binary = Path(directory) / ('control-' + str(available))
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                        '-Wextra', '-Wno-sign-compare', '-Werror', '-DCONFIG_READAHEAD_LIMIT=' + str(available),
                        str(src), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print('MTMR control READAHEAD_LIMIT=' + str(available) + ': PASS', flush=True)
