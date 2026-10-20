#!/usr/bin/env python3
"""Execute production rename dispatch and same-directory rename natively."""
import argparse
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision", help="Git revision; omit for working-tree sources")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def read(path, optional=False):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                text=True, capture_output=True)
        if result.returncode and not optional:
            raise RuntimeError(result.stderr)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() or not optional else ""

def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

original = read("src/fileinfo.c")
source = read("src/rename.c", optional=True) or original
types = read("src/rename.h", optional=True) or original
start = types.index("typedef struct _FILE_RENAME_INFORMATION_EX")
end = types.index("} FILE_RENAME_INFORMATION_EX, *PFILE_RENAME_INFORMATION_EX;", start)
end += len("} FILE_RENAME_INFORMATION_EX, *PFILE_RENAME_INFORMATION_EX;")
flags = [line for line in types.splitlines() if line.startswith("#define FILE_RENAME_") and "INFORMATION" not in line]
(out / "types.inc").write_text(types[start:end] + "\n" + "\n".join(flags) + "\n")
(out / "same_directory.inc").write_text(function(source, "static NTSTATUS rename_in_same_directory("))
(out / "dispatch.inc").write_text(function(source, "NTSTATUS set_rename_information("))
btrfs = read("src/btrfs.c")
(out / "free_fileref.inc").write_text(function(btrfs, "void free_fileref("))
(out / "valid_name.inc").write_text(function(btrfs, "NTSTATUS check_file_name_valid("))
(out / "btrfs.h").write_text(read("src/btrfs.h"))
crc = read("src/crc32c.c")
(out / "crc.inc").write_text(crc[crc.index("const uint32_t crctable[]"):])
binary = out / "rename_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror", "-DDEBUG_FCB_REFCOUNTS",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("rename_test.c")), "-o", str(binary)], check=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
failed = 0
for case in cases:
    try:
        result = subprocess.run([str(binary), case], timeout=5)
        if result.returncode:
            print(f"FAIL {case}: exit {result.returncode}", flush=True)
            failed += 1
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: five-second timeout", flush=True)
        failed += 1
print(f"{len(cases)-failed}/{len(cases)} scenarios passed (locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
