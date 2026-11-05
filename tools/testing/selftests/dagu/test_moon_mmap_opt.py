#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host check for the Moon RT direct-reclaim bypass policy.

This does not exercise reclaim throttling, scheduler classes or a kernel wait
queue. Those require the target kernel and device under memory pressure.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from c_helpers import function

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#define SCHED_NORMAL 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6
#define READ_ONCE(value) (value)

struct task_struct {
    unsigned int policy;
};
'''

checks = r'''
int main(void)
{
    struct task_struct task = { .policy = SCHED_NORMAL };

    assert(!moon_mmap_opt_rt_bypass(NULL));
    assert(!moon_mmap_opt_rt_bypass(&task));
    task.policy = SCHED_BATCH;
    assert(!moon_mmap_opt_rt_bypass(&task));
    task.policy = SCHED_IDLE;
    assert(!moon_mmap_opt_rt_bypass(&task));
    task.policy = SCHED_FIFO;
    assert(moon_mmap_opt_rt_bypass(&task));
    task.policy = SCHED_RR;
    assert(moon_mmap_opt_rt_bypass(&task));
    task.policy = SCHED_DEADLINE;
    assert(moon_mmap_opt_rt_bypass(&task));
    puts("PASS: Moon RT direct-reclaim bypass classification");
}
'''

source = prefix + function("mm/moon_mmap_opt.c",
                           "moon_mmap_opt_rt_bypass") + checks
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(source)
    subprocess.run(
        [os.environ.get("HOSTCC", "cc"), "-Wall", "-Wextra", "-Werror",
         str(path / "test.c"), "-o", str(path / "test")],
        check=True,
    )
    subprocess.run([str(path / "test")], check=True)
