#!/usr/bin/env python3
"""Run real checksum preloading with deterministic pthread/read boundaries."""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('repo', type=Path)
p.add_argument('--build-dir', type=Path, required=True)
p.add_argument('--revision')
p.add_argument('--sanitize', action='store_true')
p.add_argument('--init', choices=['pattern', 'zero'], default='pattern')
p.add_argument('--baseline', action='store_true')
a = p.parse_args()
repo, out = a.repo.resolve(), a.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
here = Path(__file__).resolve().parent
def read(name, optional=False):
    if a.revision:
        result = subprocess.run(['git', '-C', str(repo), 'show', a.revision + ':' + name], text=True, capture_output=True)
        if not result.returncode: return result.stdout
        if optional: return ''
        raise RuntimeError(result.stderr)
    path = repo / name
    return path.read_text() if path.exists() or not optional else ''
def strip(text):
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)
def function(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'
for name in ['tree-types.h', 'tree-search.h']:
    (out / name).write_text(read('src/' + name))
disk = read('src/btrfs.h')
(out / 'disk-types.inc').write_text('#define BTRFS_UUID_SIZE 16\n#pragma pack(push,1)\n' +
    '\n'.join(function(disk, 'struct ' + name + ' {').rstrip() + ';' for name in
              ['btrfs_key', 'btrfs_header', 'btrfs_item', 'btrfs_key_ptr', 'btrfs_file_extent_item']) + '\n#pragma pack(pop)\n')
driver = read('src/btrfs_drv.h')
start = driver.index('#define keycmp(')
(out / 'keycmp.inc').write_text(driver[start:driver.index('\n\n', start)] + '\n')
reads = read('src/read.c')
(out / 'io-types.inc').write_text(reads[reads.index('enum read_data_status {'):reads.index('extern bool diskacc;')])
(out / 'loader.inc').write_text(strip(read('src/tree-load.c')))
(out / 'search.inc').write_text(strip(read('src/tree-search.c')))
(out / 'free.inc').write_text(function(read('src/treefuncs.c'), 'void free_tree('))
prefetch = '\n'.join(read('src/' + name, True) for name in
                     ['metadata-prefetch-private.h', 'metadata-prefetch-plan.c', 'metadata-prefetch-read.c', 'metadata-prefetch.c'])
(out / 'prefetch.inc').write_text(strip(prefetch))
(out / 'flush.inc').write_text(function(read('src/threads.c'), 'static void do_flush('))
flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-function', '-Wno-unused-variable', '-ftrivial-auto-var-init=' + a.init,
         '-DPOOL_PATTERN=' + ('0' if a.init == 'zero' else '0xa5'), '-DHAVE_PREFETCH=' + str(int(bool(prefetch.strip())))]
if a.sanitize: flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
binary = out / 'metadata-prefetch-test'
subprocess.run(['gcc', *flags, '-I', str(out), '-I', str(here), str(here / 'prefetch_test.c'), '-o', str(binary)], check=True)
subprocess.run([str(binary), *(['baseline'] if a.baseline else [])], check=True, timeout=90)
