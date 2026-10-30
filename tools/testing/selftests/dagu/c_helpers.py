# SPDX-License-Identifier: GPL-2.0
"""Read tested C sources in-tree or from the installed source bundle."""
from pathlib import Path
import re
import tarfile


def read_source(path):
    here = Path(__file__).resolve().parent
    source_root = here.parent.parent.parent.parent
    if (here == source_root / 'tools/testing/selftests/dagu' and
            (source_root / 'mm/zsmalloc.c').is_file()):
        return (source_root / path).read_text()
    with tarfile.open(here / 'kernel-sources.tar') as archive:
        member = archive.extractfile(path)
        if member is None:
            raise ValueError('Missing source file: ' + path)
        return member.read().decode()


def extract_function(source, name):
    """Extract a definition, ignoring braces in C comments and literals."""
    tokens = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    masked = re.sub(tokens, lambda match: re.sub(r'[^\n]', ' ', match[0]),
                    source, flags=re.DOTALL)
    definition = re.search(r'^[\w \t*]+\b' + re.escape(name) +
                           r'\s*\([^;{}]*\)\s*\{', masked, re.MULTILINE)
    if definition is None:
        raise ValueError('Missing function definition: ' + name)
    brace = definition.end() - 1
    end, depth = brace + 1, 1
    while depth and end < len(masked):
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    if depth:
        raise ValueError('Unclosed function definition: ' + name)
    return source[definition.start():end] + '\n'


def function(path, name):
    return extract_function(read_source(path), name)
