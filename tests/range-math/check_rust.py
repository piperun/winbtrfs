#!/usr/bin/env python3
"""Compare C and Rust production clipping through the same host fixture."""
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
parser.add_argument('--rustc', default=os.environ.get('RUSTC', 'rustc'))
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
rust_object = out / 'range-math-rust.o'
subprocess.run([args.rustc, '--edition=2024', '--crate-name=btrfs_range_math', '--crate-type=rlib',
                '--emit=obj', '-C', 'opt-level=2', '-C', 'codegen-units=1',
                '-C', 'panic=abort', '-C', 'overflow-checks=yes',
                str(repo / 'rust/range-math/lib.rs'), '-o', str(rust_object)],
               check=True, timeout=30)
undefined = subprocess.check_output(['nm', '-u', str(rust_object)], text=True, timeout=10)
if undefined.strip():
    raise SystemExit(f'Rust leaf has unresolved runtime symbols:\n{undefined}')

flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
compiler = ['gcc', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
            f'-ftrivial-auto-var-init={args.init}', *flags, '-I', str(repo / 'src')]
objects = []
for source, name, defines in [
    (repo / 'src/range-math.c', 'range-math-c.o', ['-Dbtrfs_clip_byte_range=btrfs_clip_byte_range_c']),
    (Path(__file__).with_name('range_math_test.c'), 'fixture.o',
     ['-Dbtrfs_clip_byte_range=btrfs_compare_clip_byte_range']),
    (Path(__file__).with_name('differential.c'), 'differential.o', [])
]:
    output = out / name
    subprocess.run([*compiler, *defines, '-c', str(source), '-o', str(output)], check=True, timeout=30)
    objects.append(str(output))
binary = out / 'range_math_differential'
subprocess.run(['gcc', *flags, *objects, str(rust_object), '-o', str(binary)], check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
result = subprocess.run([str(binary)], env=env, timeout=10)
print(f'Rust object for query checks: {rust_object}', flush=True)
raise SystemExit(result.returncode)
