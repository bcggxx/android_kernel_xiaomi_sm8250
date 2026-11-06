#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the path checker through its CLI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CHECK = Path(__file__).with_name('check_sm8250_ci_paths.py')


class PathCoverage(unittest.TestCase):
    def test_each_required_path_must_trigger(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('mm/readahead.c', 'include/linux/readahead_limit.h'):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.touch()
            manifest = root / 'dependencies.json'
            manifest.write_text(json.dumps(['mm/readahead.c',
                                            'include/linux/readahead_limit.h']))
            workflow = root / 'workflow.yml'
            for paths, expected in ((['mm/**'], 1),
                                    (['mm/**', 'include/linux/**'], 0)):
                workflow.write_text('on:\n  pull_request:\n    paths:\n' +
                                    ''.join('      - ' + x + '\n' for x in paths))
                result = subprocess.run([sys.executable, str(CHECK),
                                         '--root', str(root), '--manifest',
                                         str(manifest), '--workflow', str(workflow)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_real_manifest_rejects_each_missing_trigger(self):
        source = CHECK.resolve().parents[2]
        required = json.loads((CHECK.parent / 'sm8250-dependencies.json').read_text())
        with tempfile.TemporaryDirectory() as directory:
            workflow = Path(directory) / 'workflow.yml'
            for missing in required:
                with self.subTest(missing=missing):
                    workflow.write_text('on:\n  pull_request:\n    paths:\n' +
                                        ''.join('      - ' + x + '\n'
                                                for x in required if x != missing))
                    result = subprocess.run([sys.executable, str(CHECK),
                                             '--root', str(source), '--workflow',
                                             str(workflow)], capture_output=True,
                                            text=True)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn('uncovered dependency: ' + missing, result.stderr)


if __name__ == '__main__':
    unittest.main()
