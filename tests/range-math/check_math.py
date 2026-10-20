#!/usr/bin/env python3
"""Check the portable production byte-range helper with a widened oracle."""
import argparse
import os
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('repo', type=Path)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--init', choices=['pattern', 'zero'], default='pattern')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
binary = out / 'range_math_test'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
subprocess.run(['gcc', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                f'-ftrivial-auto-var-init={args.init}', *flags, '-I', str(repo / 'src'),
                str(Path(__file__).with_name('range_math_test.c')),
                str(repo / 'src/range-math.c'), '-o', str(binary)],
               check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
raise SystemExit(subprocess.run([str(binary)], env=env, timeout=10).returncode)
