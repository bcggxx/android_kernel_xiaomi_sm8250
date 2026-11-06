#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extracted control/decision checks, not a real mmap or concurrency test."""
import subprocess
import tempfile
from pathlib import Path
from c_helpers import function

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
struct kernel_param { int unused; };
#define MI_FAULT_UID_BASE 10000U
#define MI_FAULT_BITS 512U
#define PAGE_SIZE 4096
#define READ_ONCE(x) (x)
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define scnprintf snprintf
static int fault_around_mutex;
static bool enabled;
static unsigned long fault_around[8];
static u32 subject_uid;
#define current_uid() subject_uid
#define __kuid_val(x) (x)
static int test_bit(unsigned int i, const unsigned long *p) { return !!(p[i/64] & (1UL<<(i%64))); }
static void set_bit(unsigned int i, unsigned long *p) { p[i/64] |= 1UL<<(i%64); }
static void clear_bit(unsigned int i, unsigned long *p) { p[i/64] &= ~(1UL<<(i%64)); }
/* Adapter for the kernel's standard decimal u32 parser. */
static int kstrtouint(const char *s, unsigned int base, unsigned int *v) {
 char *end; unsigned long long n;
 if (*s == '-') return -EINVAL;
 errno=0; n=strtoull(s,&end,base);
 if (end==s || (*end && strcmp(end,"\n"))) return -EINVAL;
 if (errno || n>UINT32_MAX) return -ERANGE;
 *v=n; return 0;
}
'''
for name in ('fault_around_set', 'fault_around_get', 'mi_fault_around_allowed'):
    code += function('mm/mi_fault_around.c', name)
code += r'''
int main(void) {
 char buf[4096];
 subject_uid=9999; assert(mi_fault_around_allowed());
 enabled=true; assert(!mi_fault_around_allowed());
 subject_uid=10000; assert(mi_fault_around_allowed());
 subject_uid=10511; assert(mi_fault_around_allowed());
 subject_uid=10512; assert(mi_fault_around_allowed());
 subject_uid=20000; assert(mi_fault_around_allowed());
 assert(!fault_around_set("10000",NULL));
 assert(!mi_fault_around_allowed());
 subject_uid=10000; assert(!mi_fault_around_allowed());
 assert(!fault_around_set("10511\n",NULL));
 subject_uid=10511; assert(!mi_fault_around_allowed());
 subject_uid=10512; assert(mi_fault_around_allowed());
 assert(!fault_around_set("20000",NULL));
 fault_around_get(buf,NULL); assert(!strcmp(buf,"10000 10511\n"));
 assert(fault_around_set("9999",NULL)==-EINVAL);
 assert(fault_around_set("10512",NULL)==-ENOSPC);
 assert(fault_around_set("-1",NULL)==-EINVAL);
 assert(fault_around_set("bad",NULL)==-EINVAL);
 assert(fault_around_set("4294967296",NULL)==-ERANGE);
 fault_around_get(buf,NULL); assert(!strcmp(buf,"10000 10511\n"));
 subject_uid=UINT32_MAX; assert(mi_fault_around_allowed());
 enabled=false; subject_uid=10511; assert(mi_fault_around_allowed());
 assert(!fault_around_set("0",NULL));
 fault_around_get(buf,NULL); assert(!strcmp(buf,"\n"));
 enabled=true; assert(mi_fault_around_allowed());
 subject_uid=9999; assert(!mi_fault_around_allowed());
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-Wall', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
