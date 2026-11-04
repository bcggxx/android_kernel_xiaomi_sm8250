#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Actual FINISH decisions; substitutes do not prove kernel RCU or PTE behavior."""
from c_helpers import function
from pathlib import Path
import subprocess
import tempfile
prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define PF_EXITING 4
#define PIDTYPE_TGID 0
#define rcu_dereference_protected(p,c) (p)
#define lockdep_is_held(p) 1
struct task_struct { unsigned long flags; };
struct dlp_session { void *tgid; uint64_t deadline, cookie; };
static struct dlp_session *dlp_active, *dlp_frozen;
static struct task_struct task;
static bool alive = true;
static uint64_t now;
static int dropped, frozen, detached, freeze_error;
static void rcu_read_lock(void) {}
static void rcu_read_unlock(void) {}
static struct task_struct *pid_task(void *p, int type) { return alive ? &task : 0; }
static uint64_t ktime_get_boottime_ns(void) { return now; }
static struct dlp_session *dlp_detach(void) {
 struct dlp_session *s=dlp_active; dlp_active=0; detached++; return s;
}
static void dlp_put(struct dlp_session *s) { assert(s); dropped++; }
static int dlp_freeze(struct dlp_session *s) { assert(s); frozen++; return freeze_error; }
"""
main = r"""
static void reset(struct dlp_session *s) {
 dlp_active=s; dlp_frozen=0; now=99; alive=true; task.flags=0;
 dropped=frozen=detached=freeze_error=0;
}
int main(void) {
 struct dlp_session s={.deadline=100,.cookie=7};
 reset(&s); assert(dlp_complete(8,true)==-ESRCH && dlp_active==&s && !detached);
 reset(&s); now=100; assert(dlp_complete(7,true)==-ESRCH && !dlp_active && dropped==1 && !frozen);
 reset(&s); alive=false; assert(dlp_complete(7,true)==-ESRCH && dropped==1 && !frozen);
 reset(&s); task.flags=PF_EXITING; assert(dlp_complete(7,true)==-ESRCH && dropped==1 && !frozen);
 reset(&s); assert(dlp_complete(7,true)==0 && dlp_frozen==&s && frozen==1 && !dropped && detached==1);
 reset(&s); now=101; alive=false; assert(dlp_complete(7,false)==0 && dropped==1 && !frozen);
 reset(&s); freeze_error=-ENOMEM; assert(dlp_complete(7,true)==-ENOMEM && dropped==1 && !dlp_frozen);
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="dagu-launch-finish-") as tmp:
 src=Path(tmp)/"check.c"; exe=Path(tmp)/"check"
 src.write_text(prefix+function("mm/dagu_launch_profile.c","dlp_session_valid")+function("mm/dagu_launch_profile.c","dlp_complete")+main)
 subprocess.run(["cc","-std=gnu11","-Wall","-Wextra","-Wno-unused-parameter","-fsanitize=undefined",str(src),"-o",str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=15)
print("PASS: actual FINISH rejects expired/exited sessions without running expiry; valid/abort/failure refs balanced")
