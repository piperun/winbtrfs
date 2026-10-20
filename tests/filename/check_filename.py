#!/usr/bin/env python3
"""Exercise production filename queries with host file-reference fixtures."""
import argparse
import os
from pathlib import Path
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
        result = subprocess.run(['git', '-C', str(repo), 'show', f'{args.revision}:{path}'],
                                text=True, capture_output=True)
        return result.stdout if result.returncode == 0 else ''
    p = repo / path
    return p.read_text() if p.exists() else ''

def function(source, marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

source = read('src/file-name.c') or read('src/fileinfo.c')
(out / 'filename.inc').write_text('\n'.join(function(source, f'NTSTATUS {name}(')
    for name in ['fileref_get_filename', 'fill_in_file_name_information']))
binary = out / 'filename_test'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                '-fshort-wchar', f'-ftrivial-auto-var-init={args.init}', *flags, '-I', str(out),
                str(Path(__file__).with_name('filename_test.c')), '-o', str(binary)], check=True)
env = os.environ.copy()
if args.sanitize:
    env['ASAN_OPTIONS'] = 'detect_leaks=0'  # LeakSanitizer cannot run under ptrace.
raise SystemExit(subprocess.run([str(binary)], env=env, timeout=10).returncode)
