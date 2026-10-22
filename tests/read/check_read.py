#!/usr/bin/env python3
"""Run production file-read functions with modeled kernel services."""
import argparse
import os
from pathlib import Path
import resource
import re
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
    match = re.search(rf'^(?:static )?(?:NTSTATUS|uint8_t|int|void) {name}\(', source, re.MULTILINE)
    if match is None:
        raise ValueError(f'function {name} missing')
    start = match.start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

source = read('src/file-read.c') or read('src/read.c')
begin = source.index('typedef struct {\n    uint64_t off;')
end = source.index('} comp_calc_job;', begin) + len('} comp_calc_job;')
(out / 'read_types.inc').write_text(source[begin:end])
header = read('src/btrfs_drv.h')
begin = header.index('#if defined(_MSC_VER) || defined(__clang__)')
end = header.index('#endif', begin) + len('#endif')
(out / 'read_functions.inc').write_text(header[begin:end] + '\n' + '\n'.join(
    function(source, name) for name in ['read_stream', 'read_file', 'do_read']))
(out / 'btrfs.h').write_text(read('src/btrfs.h'))
binary = out / 'read_test'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                f'-ftrivial-auto-var-init={args.init}', *flags, '-I', str(out),
                str(Path(__file__).with_name('read_test.c')), '-o', str(binary)], check=True, timeout=30)
binaries = [binary]
if not args.revision:
    # Historical comparisons cover the modeled reader, not the real decoder.
    lzo_source = read('src/compress.c')
    begin = lzo_source.index('typedef struct {')
    end = lzo_source.index('} lzo_stream;', begin) + len('} lzo_stream;')
    (out / 'lzo_functions.inc').write_text(lzo_source[begin:end] + '\n' + '\n'.join(
        function(lzo_source, name) for name in ['lzo_nextbyte', 'lzo_len', 'lzo_copy',
                                              'lzo_copyback', 'do_lzo_decompress', 'lzo_decompress']))
    lzo_binary = out / 'lzo_test'
    subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', f'-ftrivial-auto-var-init={args.init}', *flags,
                    '-I', str(out), str(Path(__file__).with_name('lzo_test.c')),
                    '-o', str(lzo_binary)], check=True, timeout=30)
    binaries.append(lzo_binary)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
results = [subprocess.run([str(executable)], env=env, timeout=10) for executable in binaries]
raise SystemExit(any(result.returncode for result in results))
