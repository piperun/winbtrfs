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
ioctl=(repo/'src/btrfsioctl.h').read_text();end=ioctl.index('} btrfs_balance_opts;')+len('} btrfs_balance_opts;');start=ioctl.rindex('typedef struct {',0,end)
(out/'balance-types.inc').write_text('\n'.join(line for line in ioctl.splitlines() if line.startswith('#define BTRFS_BALANCE_OPTS_') or line.startswith('#define BTRFS_AVAIL_ALLOC_BIT_SINGLE'))+'\n'+ioctl[start:end])
balance=(repo/'src/balance-worker.c').read_text()
(out/'balance.inc').write_text('\n'.join(function(balance,n) for n in ['static __inline uint64_t get_chunk_dup_type(','static uint16_t get_chunk_size_factor(','static bool should_balance_chunk(','static NTSTATUS regenerate_space_list(']))
with (out/'balance.inc').open('a') as stream:stream.write(function((repo/'src/balance-device.c').read_text(),'static NTSTATUS check_device_removal('))
removal=function((repo/'src/balance-device.c').read_text(),'NTSTATUS finish_removing_device(')
removal=removal[removal.index('    if (Vcb->need_write)'):removal.index('    // remove entry in chunk tree')]
startup=function(balance,'void __stdcall balance_thread(')
startup=startup[startup.index('    InitializeListHead(&chunks);')+len('    InitializeListHead(&chunks);'):startup.index('    // FIXME - what are we supposed to do with limit_start?')]
with (out/'balance.inc').open('a') as stream:
 stream.write('static NTSTATUS removal_preamble(device_extension* Vcb,device* dev) {\nNTSTATUS Status;\n'+removal+'return Status;\n}\n')
 stream.write('static NTSTATUS balance_start(device_extension* Vcb) {\nNTSTATUS Status=STATUS_SUCCESS;\n'+startup+'end:\nreturn Status;\n}\n')
dismount=(repo/'src/volume-dismount.c').read_text()
handoff=[function(dismount,n) for n in ['NTSTATUS pnp_remove_device(','NTSTATUS pnp_surprise_removal(','NTSTATUS dismount_volume(']]
handoff.append('static NTSTATUS invalidate_handoff(device_extension* Vcb,PDEVICE_OBJECT devobj,PIRP Irp) {\nNTSTATUS Status=STATUS_SUCCESS;\n'+function(dismount,'if (Vcb->Vpb == devobj->Vpb)')+'end:\nreturn Status;\n}\n')
handoff.append(function((repo/'src/volume-wakeup.c').read_text(),'void __stdcall check_after_wakeup('))
uninit=function((repo/'src/volume-lifecycle.c').read_text(),'void uninit(')
prefix=uninit[:uninit.index('    // FIXME - needs global_loading_lock')]
prefix=prefix.replace('void uninit(', 'static void uninit_preamble(').replace('    NTSTATUS Status;\n','').replace('    LIST_ENTRY* le;\n','')
handoff.append(prefix+'}\n')
(out/'handoff.inc').write_text('\n'.join(handoff))
source=(repo/'src/free-space.c').read_text()
names=['static void order_space_entry(','static void place_space_entry(','static NTSTATUS space_list_add_from(','NTSTATUS space_list_add2(','static void order_space_entries(','NTSTATUS space_list_merge(','NTSTATUS space_list_subtract2(']
(out/'spaces.inc').write_text('\n'.join(function(source,n) for n in names))
for src,dest in [('src/trim-state.h','queue-types.inc'),('src/trim-queue.c','queue.inc'),('src/trim-reservations.c','reservations.inc'),('src/trim-map-queue.c','mapping.inc')]:
 path=repo/src
 if not path.exists():raise SystemExit('FAIL: asynchronous discard ownership is not implemented; synchronous baseline blocks ordinary opens (retained lock-regression/results.log)')
 (out/dest).write_text('\n'.join(line for line in path.read_text().splitlines() if not line.startswith('#include') and line!='#pragma once'))
clean = (repo/'src/trim-mapping.c').read_text()
(out/'clean.inc').write_text(function(clean, 'NTSTATUS clean_space_cache('))
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
