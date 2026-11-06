#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check the deliberately simple literal PR paths list without a YAML dependency."""
import argparse
import json
from pathlib import Path
import re
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--workflow', type=Path)
    args = parser.parse_args()
    manifest = args.manifest or args.root / 'tools/ci/sm8250-dependencies.json'
    workflow = args.workflow or args.root / '.github/workflows/sm8250-host-check.yml'
    try:
        dependencies = json.loads(manifest.read_text())
        if not isinstance(dependencies, list) or not dependencies:
            raise ValueError('dependency manifest must be a nonempty list')
        lines = workflow.read_text().splitlines()
        # Accept only the repo's literal pull_request/paths block. Fail closed
        # on schema changes instead of pretending to parse general YAML.
        start = lines.index('  pull_request:') + 1
        if lines[start] != '    paths:':
            raise ValueError('expected literal pull_request paths block')
        patterns = []
        for line in lines[start + 1:]:
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            if not line.startswith('      '):
                break
            match = re.fullmatch(r'      - ([A-Za-z0-9_./*?-]+)', line)
            if not match:
                raise ValueError('unsupported path pattern: ' + line)
            pattern = match[1]
            if any(c in pattern for c in '*?') and not (
                    pattern.endswith('/**') and '*' not in pattern[:-3]
                    and '?' not in pattern):
                raise ValueError('only literal paths and directory/** are supported')
            patterns.append(pattern)
        for path in dependencies:
            if (not isinstance(path, str) or path.startswith('/') or
                    '..' in Path(path).parts or not (args.root / path).is_file()):
                raise ValueError('missing/invalid dependency: ' + str(path))
            covered = any(path.startswith(pattern[:-2]) if pattern.endswith('/**')
                          else path == pattern for pattern in patterns)
            if not covered:
                raise ValueError('uncovered dependency: ' + path)
    except (OSError, ValueError, IndexError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print('SM8250 CI dependency paths covered')
    return 0


if __name__ == '__main__':
    sys.exit(main())
