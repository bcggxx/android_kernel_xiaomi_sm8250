#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Public file-access seam: known bytes, EOF and mixed file descriptions."""
import pathlib
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as directory:
    fixture = pathlib.Path(directory) / 'fixture'
    for size in (0, 1, 4095, 4096, 4097, 1048589):
        fixture.write_bytes((bytes(range(256)) * ((size + 255) // 256))[:size])
        for mode in ('read', 'mmap', 'mixed', 'sequential', 'random'):
            subprocess.run([binary, mode, str(fixture)], check=True)
    fixture.write_bytes(b'wrong')
    result = subprocess.run([binary, 'read', str(fixture)])
    assert result.returncode != 0, 'corrupt fixture must fail'
print('PASS: known data, EOF, hints, mmap and shared-description mixed access')
