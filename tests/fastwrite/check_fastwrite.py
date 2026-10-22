#!/usr/bin/env python3
"""Exercise the driver's fast I/O and MDL write callbacks against write-behind,
CcFlushCache and cached write IRPs, with pthread kernel mocks."""
import argparse
import os
from pathlib import Path
import re
import resource
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
        return subprocess.check_output(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                       text=True, timeout=10)
    return (repo / path).read_text()


def matching(source, start, opening, closing):
    depth, end = 0, start
    while True:
        depth += (source[end] == opening) - (source[end] == closing)
        end += 1
        if depth == 0:
            return end


def definitions(source):
    """Map each function defined at the top level of source to (offset, text)."""
    found = {}
    for match in re.finditer(r"^[A-Za-z_][\w \t*]*?\b([A-Za-z_]\w*)[ \t]*\(", source, re.M):
        parameters_end = matching(source, match.end() - 1, "(", ")")
        if source[parameters_end:].lstrip().startswith("{"):
            body = source.index("{", parameters_end)
            found.setdefault(match.group(1), (match.start(), source[match.start():matching(source, body, "{", "}")]))
    return found


def callbacks(source, table):
    return dict(re.findall(rf"^\s*{table}\.(\w+)\s*=\s*(\w+);", source, re.M))


def closure(source, roots):
    """The functions in source that roots reach, in source order."""
    defined = definitions(source)
    chosen, pending = {}, [name for name in roots if name in defined]
    while pending:
        name = pending.pop()
        if name in chosen:
            continue
        chosen[name] = defined[name]
        pending += [callee for callee in re.findall(r"\b([A-Za-z_]\w*)\s*\(", defined[name][1])
                    if callee in defined and callee not in chosen]
    return [text + "\n" for _, text in sorted(chosen.values())]


fastio = read("src/fastio.c")
cache = read("src/cache.c")
write = read("src/write.c")

# The FsRtl routines fall back to the FsRtl*Dev implementations when the
# driver registers no callback.
dispatch = {"FastIoWrite": None, "PrepareMdlWrite": "FsRtlPrepareMdlWriteDev",
            "MdlWriteComplete": "FsRtlMdlWriteCompleteDev", "AcquireForCcFlush": None,
            "ReleaseForCcFlush": None, "FastIoQueryStandardInfo": None}
registered = callbacks(fastio, "FastIoDispatch")
for member in dispatch:
    dispatch[member] = registered.get(member, dispatch[member])
    if dispatch[member] is None:
        raise SystemExit(f"src/fastio.c registers no {member} callback")
lazy_write = {member: callbacks(cache, "cache_callbacks")[member]
              for member in ("AcquireForLazyWrite", "ReleaseFromLazyWrite")}

write_paths = definitions(write)
pieces = closure(fastio, dispatch.values()) + closure(cache, lazy_write.values())
pieces += [write_paths["write_file2"][1] + "\n", write_paths["write_file"][1] + "\n"]
(out / "driver.inc").write_text("\n".join(pieces))
(out / "dispatch.inc").write_text("".join(f"#define DISPATCH_{member} {name}\n"
                                          for member, name in {**dispatch, **lazy_write}.items()))

binary = out / "fast_write_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-pthread", *flags, "-I", str(out),
                str(Path(__file__).with_name("fast_write_test.c")), "-o", str(binary)], check=True, timeout=60)

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
cases = ["write-behind-after-extend", "write-behind-during-overwrite", "write-behind-during-extend",
         "write-behind-during-extend-preallocated", "flush-during-extend", "cached-write-during-extend",
         "truncate-during-overwrite", "mdl-overwrite", "mdl-extend"]
failed = 0
for case in cases:
    try:
        result = subprocess.run([str(binary), case], env=env, timeout=10)
        failed += result.returncode != 0
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: timed out", flush=True)
        failed += 1
print(f"{len(cases) - failed}/{len(cases)} scenarios passed", flush=True)
raise SystemExit(bool(failed))
