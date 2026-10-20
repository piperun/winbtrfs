#!/usr/bin/env python3
"""Exercise the production FastIoQueryNetworkOpenInfo callback with modeled locks."""
import argparse
import os
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('repo', type=Path)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--revision')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
if args.revision:
    source = subprocess.check_output(['git', '-C', str(repo), 'show',
                                      f'{args.revision}:src/fastio.c'], text=True, timeout=10)
else:
    source = (repo / 'src/fastio.c').read_text()
start = source.index('static BOOLEAN __stdcall fast_io_query_network_open_info(')
brace = source.index('{', start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
(out / 'network_open.inc').write_text(source[start:end] + '\n')
binary = out / 'network_open_test'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                *flags, '-I', str(out), str(Path(__file__).with_name('network_open_test.c')),
                '-o', str(binary)], check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
raise SystemExit(subprocess.run([str(binary)], env=env, timeout=10).returncode)
