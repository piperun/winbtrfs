#!/usr/bin/env python3
"""Exercise production reparse deletion and symlink setting with modeled persistence services."""
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
if args.revision:
    source = subprocess.check_output(['git', '-C', str(repo), 'show', f'{args.revision}:src/reparse.c'],
                                     text=True, timeout=10)
else:
    source = (repo / 'src/reparse.c').read_text()

def block(start):
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

(out / 'delete.inc').write_text(block(source.index('NTSTATUS delete_reparse_point(')))
lx_start = source.index('typedef struct {\n    uint32_t unknown;')
lx_end = source.index('} REPARSE_DATA_BUFFER_LX_SYMLINK;', lx_start) + len('} REPARSE_DATA_BUFFER_LX_SYMLINK;')
(out / 'symlink.inc').write_text(source[lx_start:lx_end] + '\n\n' + block(source.index('static NTSTATUS set_symlink(')))
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
failed = False
for name in ['reparse_test', 'symlink_test']:
    binary = out / name
    subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                    f'-ftrivial-auto-var-init={args.init}', *flags, '-I', str(out),
                    str(Path(__file__).with_name(f'{name}.c')), '-o', str(binary)], check=True, timeout=30)
    failed |= subprocess.run([str(binary)], env=env, timeout=10).returncode != 0
raise SystemExit(failed)
