#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check pool creation after actual zsmalloc initialization with injected errors.

Kernel allocation/mount/hotplug dependencies are substitutes. This tests the
pool API's failure boundary, not kernel boot or allocator concurrency.
"""
import os
from pathlib import Path
import subprocess
import tempfile

from c_helpers import extract_function, read_source

source = read_source('mm/zsmalloc.c')
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __init
#define GFP_KERNEL 0
#define ZS_HANDLE_SIZE 8
#define ZS_SIZE_CLASSES 2
#define ZS_MIN_ALLOC_SIZE 32
#define ZS_SIZE_CLASS_DELTA 32
#define ZS_MAX_ALLOC_SIZE 4096
#define PAGE_SIZE 4096
#define ZS_EMPTY 0
#define NR_ZS_FULLNESS 4
#define CPUHP_MM_ZS_PREPARE 1
#define CONFIG_COMPACTION 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
struct kmem_cache { int unused; };
struct zspage { int unused; };
struct size_class {
    int size, index, pages_per_zspage, objs_per_zspage, lock;
    int fullness_list[NR_ZS_FULLNESS];
};
struct zs_pool {
    char *name;
    int migration_wait;
    struct size_class *size_class[ZS_SIZE_CLASSES];
};
static struct kmem_cache *handle_cachep, *zspage_cachep;
static bool zs_ready;
static unsigned int huge_class_size;
static int fail_at, cache_calls;
static struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                           int align, int flags, void *ctor) {
    (void)name; (void)size; (void)align; (void)flags; (void)ctor;
    return ++cache_calls == fail_at ? NULL : calloc(1, sizeof(struct kmem_cache));
}
static void kmem_cache_destroy(struct kmem_cache *cache) { free(cache); }
static void *kzalloc(size_t size, int flags) { (void)flags; return calloc(1, size); }
static char *kstrdup(const char *name, int flags) { (void)flags; return strdup(name); }
static void init_deferred_free(struct zs_pool *pool) { (void)pool; }
static void init_waitqueue_head(int *head) { *head = 0; }
static int get_pages_per_zspage(int size) { (void)size; return 1; }
static bool can_merge(struct size_class *class, int pages, int objs) {
    (void)class; (void)pages; (void)objs; return false;
}
static void spin_lock_init(int *lock) { *lock = 0; }
#define INIT_LIST_HEAD(head) (*(head) = 0)
static void zs_pool_stat_create(struct zs_pool *pool, const char *name) {
    (void)pool; (void)name;
}
static int zs_register_migration(struct zs_pool *pool) { (void)pool; return 0; }
static void zs_register_shrinker(struct zs_pool *pool) { (void)pool; }
static void zs_destroy_pool(struct zs_pool *pool) {
    if (!pool) return;
    for (int i = 0; i < ZS_SIZE_CLASSES; i++) free(pool->size_class[i]);
    free(pool->name); free(pool);
}
static int zsmalloc_mount(void) { return fail_at == 3 ? -ENOMEM : 0; }
static void zsmalloc_unmount(void) {}
static int zs_cpu_prepare, zs_cpu_dead;
static int cpuhp_setup_state(int state, const char *name, int up, int down) {
    (void)state; (void)name; (void)up; (void)down;
    return fail_at == 4 ? -ENOMEM : 0;
}
static void zs_stat_init(void) {}
'''
body = '\n'.join(extract_function(source, name) for name in (
    'create_cache', 'destroy_cache', 'zs_create_pool', 'zs_init'))
tests = r'''
int main(int argc, char **argv) {
    struct zs_pool *pool;
    int ret;
    assert(argc == 2);
    fail_at = atoi(argv[1]);
    ret = zs_init();
    pool = zs_create_pool("regression");
    if (fail_at) {
        assert(ret == -ENOMEM);
        assert(pool == NULL);
    } else {
        assert(ret == 0 && pool != NULL);
        assert(!strcmp(pool->name, "regression"));
        zs_destroy_pool(pool);
        destroy_cache();
    }
    puts("PASS: pool API rejects failed initialization and accepts success");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='zsmalloc-pool-') as temp:
    test, binary = Path(temp) / 'test.c', Path(temp) / 'test'
    test.write_text(preamble + body + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Wno-unused-variable', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined'),
                    '-o', str(binary), str(test)], check=True)
    for failure in (1, 2, 3, 4, 0):
        subprocess.run([str(binary), str(failure)], check=True, timeout=30)
