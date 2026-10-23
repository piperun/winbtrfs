#!/usr/bin/env python3
"""Exercise production asynchronous discard ownership and real space-list merges."""
import argparse,os,resource,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('repo',type=Path);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--init',choices=['zero','pattern'],default='pattern');p.add_argument('--sanitize',action='store_true');args=p.parse_args()
repo=args.repo.resolve();out=args.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
def function(source,name):
 a=source.index(name);b=source.index('{',a);e=b+1;depth=1
 while depth:depth+=(source[e]=='{')-(source[e]=='}');e+=1
 return source[a:e]+'\n'
header=(repo/'src/btrfs_drv.h').read_text();end=header.index('} space;')+len('} space;');(out/'types.inc').write_text(header[header.rindex('typedef struct {',0,end):end])
source=(repo/'src/free-space.c').read_text()
names=['static void order_space_entry(','static void place_space_entry(','static NTSTATUS space_list_add_from(','NTSTATUS space_list_add2(','static void order_space_entries(','NTSTATUS space_list_merge(','NTSTATUS space_list_subtract2(']
(out/'spaces.inc').write_text('\n'.join(function(source,n) for n in names))
for src,dest in [('src/trim-state.h','queue-types.inc'),('src/trim-queue.c','queue.inc'),('src/trim-reservations.c','reservations.inc'),('src/trim-map-queue.c','mapping.inc')]:
 path=repo/src
 if not path.exists():raise SystemExit('FAIL: asynchronous discard ownership is not implemented; synchronous baseline blocks ordinary opens (retained lock-regression/results.log)')
 (out/dest).write_text('\n'.join(line for line in path.read_text().splitlines() if not line.startswith('#include') and line!='#pragma once'))
flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'] if args.sanitize else []
binary=out/'trim_queue_test'
subprocess.run(['gcc','-std=c11','-O0','-g','-Wall','-Wextra','-Werror',f'-ftrivial-auto-var-init={args.init}',*flags,'-I',str(out),'-I',str(repo/'tests/free-space'),str(Path(__file__).with_name('queue_test.c')),'-o',str(binary)],check=True)
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
cases=subprocess.check_output([str(binary),'--list'],text=True).splitlines();failed=[]
for case in cases:
 result=subprocess.run([str(binary),case],timeout=30,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
 if result.returncode:failed.append(case)
print(f'{len(cases)-len(failed)}/{len(cases)} scenarios passed (locals: {args.init})',flush=True)
raise SystemExit(bool(failed))
