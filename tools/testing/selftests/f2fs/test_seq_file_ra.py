#!/usr/bin/env python3
"""Test the actual f2fs fadvise wrapper with userspace VFS stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'fs/f2fs/file.c').read_text()
start = source.index('static int f2fs_file_fadvise(')
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
wrapper = source[start:end]
preamble = r'''
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_RANDOM 1
#define READ_ONCE(x) (x)
struct backing_dev_info { unsigned long ra_pages; };
static struct backing_dev_info noop_backing_dev_info;
struct f2fs_sb_info { unsigned int seq_file_ra_mul; };
struct inode { int dax; struct backing_dev_info *bdi; struct f2fs_sb_info *sbi; };
struct address_space { struct inode *host; };
struct file { struct inode *inode; struct address_space *f_mapping;
              struct { unsigned int ra_pages; } f_ra; };
#define file_inode(f) ((f)->inode)
#define IS_DAX(i) ((i)->dax)
#define F2FS_I_SB(i) ((i)->sbi)
#define inode_to_bdi(i) ((i)->bdi)
static int generic_result, generic_calls;
static int generic_fadvise(struct file *f, loff_t offset, loff_t len, int advice) {
    (void)offset;
    generic_calls++;
    if (generic_result) return generic_result;
    if (!f->f_mapping || len < 0) return -EINVAL;
    if (f->inode->dax || f->inode->bdi == &noop_backing_dev_info) return 0;
    f->f_ra.ra_pages = advice == POSIX_FADV_SEQUENTIAL ? f->inode->bdi->ra_pages * 2 : 777;
    return 0;
}
'''
tests = r'''
int main(void) {
    struct f2fs_sb_info sbi = {.seq_file_ra_mul = 2};
    struct backing_dev_info bdi = {.ra_pages = 32};
    struct inode inode = {.bdi = &bdi, .sbi = &sbi};
    struct address_space mapping = {.host = &inode};
    struct file file = {.inode = &inode, .f_mapping = &mapping};
    unsigned int values[] = {2, 3, 4, 16, 256};
    for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        sbi.seq_file_ra_mul = values[i];
        assert(f2fs_file_fadvise(&file, 0, 4096, POSIX_FADV_SEQUENTIAL) == 0);
        assert(file.f_ra.ra_pages == 32UL * values[i]);
    }
    assert(f2fs_file_fadvise(&file, 0, 4096, POSIX_FADV_RANDOM) == 0);
    assert(file.f_ra.ra_pages == 777);
    generic_result = -ESPIPE;
    assert(f2fs_file_fadvise(&file, 0, 4096, POSIX_FADV_SEQUENTIAL) == -ESPIPE);
    assert(file.f_ra.ra_pages == 777);
    generic_result = 0;
    assert(f2fs_file_fadvise(&file, 0, -1, POSIX_FADV_SEQUENTIAL) == -EINVAL);
    file.f_mapping = NULL;
    assert(f2fs_file_fadvise(&file, 0, 4096, POSIX_FADV_SEQUENTIAL) == -EINVAL);
    file.f_mapping = &mapping;
    inode.dax = 1;
    assert(f2fs_file_fadvise(&file, 0, 4096, POSIX_FADV_SEQUENTIAL) == 0);
    assert(file.f_ra.ra_pages == 777);
    inode.dax = 0; inode.bdi = &noop_backing_dev_info;
    assert(f2fs_file_fadvise(&file, 0, 4096, POSIX_FADV_SEQUENTIAL) == 0);
    assert(file.f_ra.ra_pages == 777);
    inode.bdi = &bdi; bdi.ra_pages = UINT_MAX / 256 + 1;
    assert(f2fs_file_fadvise(&file, 0, 4096, POSIX_FADV_SEQUENTIAL) == 0);
    assert(file.f_ra.ra_pages == UINT_MAX);
    assert(generic_calls == 12);
    puts("PASS: multipliers, generic advice/errors, DAX/noop, missing mapping, overflow");
}
'''
with tempfile.TemporaryDirectory(prefix='f2fs-readahead-') as temp:
    test, binary = Path(temp) / 'test.c', Path(temp) / 'test'
    test.write_text(preamble + wrapper + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-Wall', '-Wextra', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined'),
                    '-o', str(binary), str(test)], check=True)
    subprocess.run([str(binary)], check=True)
