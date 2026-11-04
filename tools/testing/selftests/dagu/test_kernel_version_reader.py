#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check the production reader with controlled cat/uname command responses.

Host fixtures do not prove procfs registration, rundown or target identity.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import tarfile

reader = Path(__file__).resolve().parents[3] / 'kernel-version/read.sh'
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    if not reader.is_file():
        with tarfile.open(Path(__file__).with_name('kernel-sources.tar')) as archive:
            source = archive.extractfile('tools/kernel-version/read.sh').read()
        reader = root / 'read.sh'
        reader.write_bytes(source)
    (root / 'cat').write_text('#!/bin/sh\n[ "$1" = /proc/chiruno_kernel_version ] || exit 9\n[ "$FAIL_CAT" != 1 ] || exit 1\nexec /bin/cat "$FIXTURE"\n')
    (root / 'uname').write_text('#!/bin/sh\n[ "$1" = -r ] || exit 9\nprintf "4.19.325-dagu\\n"\n')
    for name in ('cat', 'uname'):
        (root / name).chmod(0o755)
    env = dict(os.environ, PATH=str(root), FIXTURE=str(root / 'release'))
    for payload, valid in [(b'4.19.325-dagu\n', True), (b'', False),
                           (b'4.19.325-dagu', False),
                           (b'4.19.325-dagu\n\n', False),
                           (b'4.19.325-dagu\nother\n', False),
                           (b'4.19.999-other\n', False)]:
        (root / 'release').write_bytes(payload)
        result = subprocess.run(['/bin/sh', str(reader)], env=env,
                                capture_output=True)
        assert (result.returncode == 0) == valid, (payload, result)
        if valid:
            assert b'4.19.325-dagu' in result.stdout
            assert b'/proc/chiruno_kernel_version' in result.stdout
    result = subprocess.run(['/bin/sh', str(reader)],
                            env=dict(env, FAIL_CAT='1'), capture_output=True)
    assert result.returncode != 0
    assert b'not enabled or inaccessible' in result.stderr
print('PASS: production release reader validation and unavailable path')
