#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Behavior contract for the bounded read/mmap observation probe."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile


binary = str(pathlib.Path(sys.argv[1]).resolve())


def run(*args):
    return subprocess.run([binary, *map(str, args)], capture_output=True,
                          text=True, timeout=10)


def fields(output):
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", output))


def expect_ok(result, **expected):
    assert result.returncode == 0, (result.returncode, result.stdout,
                                   result.stderr)
    actual = fields(result.stdout)
    for key, value in expected.items():
        assert actual.get(key) == str(value), (key, value, actual)
    if 'hint' not in expected:
        assert actual.get('hint') == 'none', actual
        assert actual.get('hint_api') == 'none', actual
        assert actual.get('hint_status') == 'not-applicable', actual
    assert actual.get('elapsed_ns', '').isdigit(), actual
    for name in ('mincore_file_before', 'mincore_file_after',
                 'mincore_request_before', 'mincore_request_after'):
        resident, separator, pages = actual.get(name, '').partition('/')
        assert separator and resident.isdigit() and pages.isdigit(), actual
        assert int(resident) <= int(pages), actual
    assert actual.get('status') == 'pass', actual
    return actual


with tempfile.TemporaryDirectory() as directory:
    fixture = pathlib.Path(directory) / 'fixture'
    empty = pathlib.Path(directory) / 'empty'
    content = bytes(range(256)) * 48
    page_size = os.sysconf('SC_PAGE_SIZE')
    empty.touch()
    fixture.write_bytes(content)

    result = run('read', empty, 0, 1)
    expect_ok(result, pread_return=0, data='ok',
              mincore_file_before='0/0', mincore_file_after='0/0',
              mincore_request_before='0/0', mincore_request_after='0/0')

    result = run('read', empty, 0, 1, '--dontneed')
    assert result.returncode != 0
    actual = fields(result.stdout)
    assert actual.get('cold') == 'not-confirmed', result.stdout
    assert actual.get('operation') == 'skipped', result.stdout
    assert actual.get('mincore_file_before') == '0/0', result.stdout
    assert actual.get('mincore_file_after') == 'not-run', result.stdout

    result = run('read', fixture, 4096, 4096)
    expect_ok(result, mode='read', request_offset=4096,
              request_length=4096, pread_offset=4096,
              pread_length=4096, pread_return=4096, data='ok')

    result = run('mmap', fixture, 4103, 17)
    expect_ok(result, mode='mmap', request_offset=4103,
              request_length=17, mmap_offset=4103 - 4103 % page_size,
              mmap_length=(4103 % page_size) + 17,
              touch_offset=4103, touched_byte=7, data='ok')

    result = run('mmap', fixture, 4103, 17, '--fd-random', '--sequential')
    expect_ok(result, hint='sequential', hint_api='madvise',
              hint_status='applied', fd_random='applied', data='ok')
    assert run('read', fixture, 0, 1, '--fd-random').returncode == 2
    assert run('mmap', fixture, 0, 1, '--fd-random',
               '--fd-random').returncode == 2

    for hint in ('sequential', 'random'):
        result = run('read', fixture, 4096, 4096, f'--{hint}')
        expect_ok(result, hint=hint, hint_api='posix_fadvise',
                  hint_status='applied', pread_offset=4096,
                  pread_length=4096, pread_return=4096, data='ok')

        result = run('mmap', fixture, 4103, 17, f'--{hint}')
        expect_ok(result, hint=hint, hint_api='madvise',
                  hint_status='applied', touch_offset=4103,
                  touched_byte=7, data='ok')

    result = run('read', fixture, 4096, 4096,
                 '--random', '--sequential')
    assert result.returncode == 2
    assert 'mutually exclusive' in result.stderr, result.stderr

    result = run('mmap', fixture, 4103, 17,
                 '--dontneed', '--random')
    actual = fields(result.stdout)
    assert actual.get('cache_prep') == 'dontneed', result.stdout
    assert actual.get('hint') == 'random', result.stdout
    assert actual.get('hint_api') == 'madvise', result.stdout
    if result.returncode == 0:
        assert actual.get('hint_status') == 'applied', result.stdout
    else:
        assert actual.get('operation') == 'skipped', result.stdout
        assert actual.get('hint_status') == 'not-applied', result.stdout

    result = run('read', fixture, len(content) - 2, 32)
    expect_ok(result, pread_return=2, data='ok')

    result = run('read', fixture, len(content), 32)
    expect_ok(result, pread_return=0, data='ok',
              mincore_request_before='0/0', mincore_request_after='0/0')

    result = run('read', fixture, len(content), 32, '--dontneed')
    actual = fields(result.stdout)
    if result.returncode == 0:
        assert actual.get('cold') == 'confirmed', result.stdout
        assert actual.get('mincore_file_before', '').startswith('0/'), result.stdout
        assert actual.get('mincore_request_before') == '0/0', result.stdout
        assert actual.get('pread_return') == '0', result.stdout
    else:
        assert actual.get('cold') in ('not-confirmed', 'unknown'), result.stdout
        assert actual.get('operation') == 'skipped', result.stdout
        assert actual.get('mincore_request_before') == '0/0', result.stdout
        assert actual.get('mincore_file_after') == 'not-run', result.stdout

    result = run('mmap', fixture, len(content) - 1, 1)
    expect_ok(result, mmap_offset=len(content) - 1 -
              (len(content) - 1) % page_size,
              mmap_length=(len(content) - 1) % page_size + 1,
              touch_offset=len(content) - 1,
              touched_byte=(len(content) - 1) % 256)

    result = run('mmap', fixture, len(content) - 1, 2)
    assert result.returncode != 0
    assert 'mmap range crosses EOF' in result.stderr, result.stderr

    result = run('read', fixture, 16777216, 1)
    assert result.returncode != 0
    assert 'request range exceeds 16 MiB bound' in result.stderr, result.stderr

    damaged = bytearray(content)
    damaged[4103] ^= 0xff
    fixture.write_bytes(damaged)
    result = run('mmap', fixture, 4103, 17)
    assert result.returncode != 0
    assert fields(result.stdout).get('data') == 'bad', result.stdout
    fixture.write_bytes(content)

    # DONTNEED is advisory. A run may proceed only if mincore verifies an
    # empty whole-file range after preparation; otherwise it must skip.
    result = run('read', fixture, 4096, 4096, '--dontneed')
    actual = fields(result.stdout)
    assert actual.get('cache_prep') == 'dontneed', result.stdout
    if result.returncode == 0:
        assert actual.get('cold') == 'confirmed', result.stdout
        assert actual.get('mincore_file_before', '').startswith('0/'), result.stdout
        assert actual.get('mincore_request_before', '').startswith('0/'), result.stdout
        assert actual.get('status') == 'pass', result.stdout
    else:
        assert actual.get('cold') in ('not-confirmed', 'unknown'), result.stdout
        assert actual.get('operation') == 'skipped', result.stdout
        assert actual.get('mincore_file_after') == 'not-run', result.stdout

    missing = pathlib.Path(directory) / 'missing'
    result = run('read', missing, 0, 1)
    assert result.returncode != 0
    assert result.stderr.startswith('open:'), result.stderr

print('PASS: bounded requests, bytes, EOF, mmap alignment, mincore and cache preparation')
