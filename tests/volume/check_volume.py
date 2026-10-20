#!/usr/bin/env python3
"""Exercise production volume status and connected-device enumeration."""
import argparse
import os
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('repo', type=Path)
parser.add_argument('--revision')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--init', choices=['pattern', 'zero'], default='pattern')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def read(path):
    if args.revision:
        p = subprocess.run(['git', '-C', str(repo), 'show', f'{args.revision}:{path}'],
                           capture_output=True, text=True, timeout=10)
        return p.stdout if p.returncode == 0 else ''
    p = repo / path
    return p.read_text() if p.exists() else ''

def function(source, name):
    start = source.index(f'static NTSTATUS {name}(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

source = read('src/volume-ioctl.c') or read('src/volume.c')
(out / 'queries.inc').write_text(function(source, 'vol_is_writable') +
                                function(read('src/devctrl.c'), 'query_filesystems'))
header = read('src/btrfsioctl.h')
begin = header.index('typedef struct {\n    uint8_t uuid[16];\n    BOOL missing;')
end = header.index('} btrfs_filesystem;', begin) + len('} btrfs_filesystem;')
(out / 'types.inc').write_text(header[begin:end])
binary = out / 'volume_test'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                f'-ftrivial-auto-var-init={args.init}', *flags, '-I', str(out),
                str(Path(__file__).with_name('volume_test.c')), '-o', str(binary)], check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
raise SystemExit(subprocess.run([str(binary)], env=env, timeout=10).returncode)
