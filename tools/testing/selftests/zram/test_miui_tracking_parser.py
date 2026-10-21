#!/usr/bin/env python3
"""Exercise the actual time_list parser/allocation code with userspace stubs.

This tests parsing, allocation rollback and sysfs return values with sanitizers;
it does not replace an ARM64 kernel build or RCU/concurrency tests on-device.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'drivers/block/zram/zram_drv.c').read_text()

def function(name):
    start = source.index(name)
    start = source.rfind('\nstatic ', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

preamble = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define GFP_KERNEL 0
#define MAX_LIFE_TIME_BINS 64
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct rcu_head { int unused; };
struct zram_pages_life {
    unsigned int time_nr;
    int *time_list;
    long *lifes;
    struct rcu_head rcu;
};
struct zram { struct zram_pages_life *pages_life; int init_lock; int ready; };
struct device { struct zram *zram; };
struct device_attribute { int unused; };
static int default_time_list[] = {60, 120, 180, 300, 600};
static int fail_after = -1;
static int live_allocs;
static void *allocation(size_t size, int zero) {
    void *p;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    p = zero ? calloc(1, size) : malloc(size);
    if (p) live_allocs++;
    return p;
}
static void kfree(void *p) { if (p) live_allocs--; free(p); }
#define kzalloc(n, g) allocation(n, 1)
#define kmalloc_array(n, s, g) allocation((n) * (s), 0)
static char *kstrndup(const char *s, size_t n, int g) {
    char *p = allocation(n + 1, 0); (void)g;
    if (p) { memcpy(p, s, n); p[n] = 0; } return p;
}
static int kstrtouint(const char *s, int base, unsigned int *out) {
    char *end; unsigned long value;
    if (*s == '-' || !*s) return -EINVAL;
    errno = 0; value = strtoul(s, &end, base);
    if (errno || *end || value > UINT_MAX) return -EINVAL;
    *out = value; return 0;
}
static int kstrtoint(const char *s, int base, int *out) {
    char *end; long value;
    if (!*s) return -EINVAL;
    errno = 0; value = strtol(s, &end, base);
    if (errno || *end || value < INT_MIN || value > INT_MAX) return -EINVAL;
    *out = value; return 0;
}
#define dev_to_zram(d) ((d)->zram)
#define init_done(z) ((z)->ready)
#define lockdep_is_held(lock) (*(lock) == 1)
#define rcu_dereference_protected(p, held) (assert(held), (p))
#define rcu_assign_pointer(p, value) ((p) = (value))
static void down_write(int *lock) { assert(!*lock); *lock = 1; }
static void up_write(int *lock) { assert(*lock == 1); *lock = 0; }
static void call_rcu(struct rcu_head *rcu, void (*fn)(struct rcu_head *)) { fn(rcu); }
'''
body = '\n'.join(function(n) for n in [
    'free_pages_life(', 'init_pages_life(', 'clean_pages_life(',
    'renew_pages_life(', 'next_time_arg(', 'time_list_store('])
tests = r'''
static ssize_t store(struct device *d, const char *s) {
    return time_list_store(d, NULL, s, strlen(s));
}
int main(void) {
    struct zram z = {.ready = 1}; struct device d = {.zram = &z};
    const char *bad[] = {"", "0", "65", "4294967295", "-1 1", "1 -1",
        "1 2147483648", "2 1", "2 2 1", "2 1 1", "1 1 2", "x", "1 x"};
    unsigned int i; char input[1024]; size_t off;
    for (int failure = 0; failure < 3; failure++) {
        fail_after = failure;
        assert(!init_pages_life()); assert(live_allocs == 0);
    }
    fail_after = -1;
    z.pages_life = init_pages_life(); assert(z.pages_life);
    for (i = 0; i < ARRAY_SIZE(bad); i++) {
        struct zram_pages_life *old = z.pages_life;
        assert(store(&d, bad[i]) == -EINVAL); assert(z.pages_life == old);
    }
    assert(store(&d, " 3\t0 60 120\n") == (ssize_t)strlen(" 3\t0 60 120\n"));
    assert(z.pages_life->time_nr == 3 && z.pages_life->time_list[2] == 120);
    off = snprintf(input, sizeof(input), "64");
    for (i = 0; i < 64; i++) off += snprintf(input + off, sizeof(input) - off, " %u", i);
    assert(store(&d, input) == (ssize_t)off);
    assert(z.pages_life->time_nr == 64);
    for (int failure = 0; failure < 5; failure++) {
        struct zram_pages_life *old = z.pages_life;
        fail_after = failure;
        assert(store(&d, "1 60") == -ENOMEM); assert(z.pages_life == old);
        assert(live_allocs == 3);
    }
    fail_after = -1; z.ready = 0;
    assert(store(&d, "1 60") == -EINVAL); assert(live_allocs == 3);
    free_pages_life(z.pages_life); assert(live_allocs == 0);
    puts("PASS: invalid input, whitespace, 64 bins, allocation failures, return values");
}
'''
with tempfile.TemporaryDirectory(prefix='zram-parser-') as temp:
    test = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    test.write_text(preamble + body + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined'),
                    '-o', str(binary), str(test)], check=True)
    subprocess.run([str(binary)], check=True)
