#!/usr/bin/env python3
"""Compile actual directory serializer/scan bodies against native fixtures."""
import argparse
import hashlib
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
parser.add_argument('--baseline', action='store_true', help='Check extraction compiles, without expecting bugs fixed')
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def read(path, optional=False):
    if args.revision:
        p = subprocess.run(['git', '-C', str(repo), 'show', f'{args.revision}:{path}'], capture_output=True, text=True)
        if p.returncode and not optional:
            raise RuntimeError(p.stderr)
        return p.stdout if not p.returncode else ''
    p = repo / path
    return p.read_text() if p.exists() or not optional else ''

def function(source, name):
    # Exact source slice; no substituted implementation or edited body.
    marker = (f'LONG {name}(' if name == 'directory_entry_header_size' else f'NTSTATUS {name}(')
    start = source.index(marker)
    if source[max(0, start-7):start] == 'static ':
        start -= 7
    brace = source.index('{', start)
    depth, end = 1, brace+1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

original = read('src/dirctrl.c')
entry = read('src/directory-entry.c', True) or original
query = read('src/directory-query.c', True) or original
types = read('src/directory-query.h', True) or original
start = types.index('typedef struct _FILE_ID_EXTD_DIR_INFORMATION')
end = types.index('\n#endif', start)
(out/'types.inc').write_text(types[start:end])
start = types.index('enum DirEntryType')
end = types.index('} dir_entry;', start)+len('} dir_entry;')
(out/'entry.inc').write_text(types[start:end])
bodies = (function(entry, 'directory_entry_header_size') if 'static LONG directory_entry_header_size(' in entry else '') + function(entry, 'query_dir_item')+function(query, 'next_dir_entry')+function(query, 'query_directory')
(out/'queries.inc').write_text(bodies)
print('Extracted source SHA256:', hashlib.sha256(bodies.encode()).hexdigest(), flush=True)
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if args.sanitize else []
binary = out/'directory_test'
subprocess.run(['gcc','-std=c11','-fshort-wchar','-O0','-g','-Wall','-Wextra','-Werror',f'-ftrivial-auto-var-init={args.init}',*flags,'-I',str(out),str(Path(__file__).with_name('directory_test.c')),'-o',str(binary)],check=True)
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
if not args.baseline:
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    subprocess.run([str(binary)],check=True,env=env,timeout=15)
