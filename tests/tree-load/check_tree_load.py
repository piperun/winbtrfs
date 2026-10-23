#!/usr/bin/env python3
"""Fault-inject the actual unpublished metadata node constructor."""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('repo', type=Path)
p.add_argument('--build-dir', type=Path, required=True)
p.add_argument('--revision')
p.add_argument('--init', choices=['pattern', 'zero'], default='pattern')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
repo, out = a.repo.resolve(), a.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
here = Path(__file__).resolve().parent
def read(name):
    if a.revision:
        return subprocess.check_output(['git', '-C', str(repo), 'show', a.revision + ':' + name], text=True)
    return (repo / name).read_text()
source = read('src/tree-load.c')
source = source[:source.index('__attribute__((nonnull(1,2,3,4)))')]
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
(out / 'loader.inc').write_text(source)
(out / 'search.inc').write_text(re.sub(r'^#include[^\n]*\n', '', read('src/tree-search.c'), flags=re.M))
(out / 'tree-types.h').write_text(read('src/tree-types.h'))
(out / 'tree-search.h').write_text(read('src/tree-search.h'))
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-DHAVE_INDEX=1', '-DPOOL_PATTERN=' + ('0' if a.init == 'zero' else '0xa5'),
         '-ftrivial-auto-var-init=' + a.init]
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
binary = out / 'tree-load-test'
subprocess.run(['gcc', *flags, '-I', str(out), '-I', str(here.parent / 'tree-search'), str(here / 'load_test.c'), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
