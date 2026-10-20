#!/usr/bin/env python3
"""Check production volume probe allocation ownership with modeled services."""
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
    source = subprocess.run(['git', '-C', str(repo), 'show',
                             f'{args.revision}:src/devctrl.c'], check=True,
                            capture_output=True, text=True, timeout=10).stdout
else:
    source = (repo / 'src/devctrl.c').read_text()
start = source.index('static NTSTATUS probe_volume(')
end = source.index('static NTSTATUS ioctl_unload(', start)
(out / 'probe.inc').write_text(source[start:end])
binary = out / 'probe_test'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                f'-ftrivial-auto-var-init={args.init}', *flags, '-I', str(out),
                str(Path(__file__).with_name('probe_test.c')), '-o', str(binary)],
               check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
raise SystemExit(subprocess.run([str(binary)], env=env, timeout=10).returncode)
