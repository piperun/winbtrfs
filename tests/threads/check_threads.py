#!/usr/bin/env python3
"""Exercise extracted WinBtrfs thread lifecycle C with pthread kernel mocks."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
repo = args.repo.resolve()
out = args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def read(path):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                text=True, capture_output=True)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() else ""

def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

btrfs = read("src/btrfs.c")
lifecycle = read("src/threads.c") or btrfs
flush = read("src/threads.c") or read("src/flushthread.c")
header = read("src/btrfs_drv.h")
start = header.index("typedef struct {\n    PDEVICE_OBJECT DeviceObject;\n    HANDLE handle;\n    KEVENT finished;")
end = header.index("} drv_calc_threads;", start) + len("} drv_calc_threads;")
license_header = header[:header.index("#pragma once")]
(out / "thread_types.inc").write_text(license_header + header[start:end] + "\n")
pieces = [function(lifecycle, "uint32_t get_num_of_processors("),
          function(lifecycle, "NTSTATUS create_calc_threads("),
          function(read("src/calcthread.c"), "void __stdcall calc_thread("),
          function(flush, "void __stdcall flush_thread(")]
uninit = function(btrfs, "void uninit(")
if "void stop_calc_threads(" in lifecycle:
    pieces.append(function(lifecycle, "void stop_calc_threads("))
else:
    start = uninit.index("    for (i = 0; i < Vcb->calcthreads.num_threads;")
    end = uninit.index("    ExFreePool(Vcb->calcthreads.threads);", start) + len("    ExFreePool(Vcb->calcthreads.threads);")
    pieces.append("void stop_calc_threads(device_extension* Vcb) {\n    ULONG i;\n" + uninit[start:end] + "\n}\n")
if "void stop_flush_thread(" in lifecycle:
    pieces.append(function(lifecycle, "void stop_flush_thread("))
else:
    start = uninit.index("    time.QuadPart = 0;")
    end = uninit.index("\n", uninit.index("KeWaitForSingleObject(&Vcb->flush_thread_finished", start))
    close_start = uninit.index("    ZwClose(Vcb->flush_thread_handle);")
    close_end = uninit.index("\n", close_start)
    pieces.append("void stop_flush_thread(device_extension* Vcb) {\n    LARGE_INTEGER time;\n" +
                  uninit[start:end] + "\n" + uninit[close_start:close_end] + "\n}\n")
calc_pos = uninit.find("stop_calc_threads(Vcb)")
flush_pos = uninit.find("stop_flush_thread(Vcb)")
if calc_pos == -1:
    calc_pos = uninit.index("Vcb->calcthreads.num_threads")
if flush_pos == -1:
    flush_pos = uninit.index("time.QuadPart = 0;")
calls = ["stop_calc_threads(Vcb);", "stop_flush_thread(Vcb);"]
if flush_pos < calc_pos:
    calls.reverse()
if "void stop_flush_thread(" in lifecycle and "ZwClose(Vcb->flush_thread_handle);" in uninit:
    calls.append("ZwClose(Vcb->flush_thread_handle);")
pieces.append("void stop_volume_threads(device_extension* Vcb) {\n" + "\n".join(calls) + "\n}\n")
(out / "lifecycle.inc").write_text(btrfs[:btrfs.index("#include")] + "\n" + "\n".join(pieces))

# The actual mount activation statements, including each error branch and goto.
# Only unrelated disk/tree discovery before this point and registry bookkeeping
# after it are excluded. This guards startup/publication ordering as well as helpers.
mount = function(btrfs, "static NTSTATUS mount_vol(")
start = mount.index("find_disk_holes(Vcb,")
start = mount.index("        le = le->Flink;", start)
start = mount.index("\n    }", start) + len("\n    }")
end = mount.index("    Status = registry_mark_volume_mounted(", start)
(out / "mount_activation.inc").write_text(license_header + mount[start:end])
binary = out / "threads_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-pthread", *flags, "-I", str(out),
                str(Path(__file__).with_name("threads_test.c")), "-o", str(binary)], check=True)
cases = ["allocation", "empty", "success", "single", "busy", "termination", "restart",
         "mount-success", "mount-allocation", "flush-failure", "flush-rearm"]
cases += [f"calc-failure-{i}" for i in range(4)]
cases += [f"mount-failure-{i}" for i in range(4)]
failed = 0
for case in cases:
    try:
        result = subprocess.run([str(binary), case], timeout=5)
        failed += result.returncode != 0
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: timed out", flush=True)
        failed += 1
print(f"{len(cases) - failed}/{len(cases)} scenarios passed", flush=True)
raise SystemExit(bool(failed))
