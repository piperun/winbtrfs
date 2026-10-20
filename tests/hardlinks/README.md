# Hard-link query regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/hardlinks/check_hardlinks.py . --build-dir build/hardlink-tests
python3 tests/hardlinks/check_hardlinks.py . --init zero --build-dir build/hardlink-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/hardlinks/check_hardlinks.py . --sanitize --build-dir build/hardlink-tests-asan
```

To reproduce before the fixes, add `--revision 19432b2e`. The baseline passes
39/52 cases. Eight cases retain references to deleted parents, three full-ID
cases dereference missing directory data, and two cases underreport
`BytesNeeded` when no entries fit in the output buffer. The fixed source passes
all 52. Deleted-parent lifetime and cached dummy-entry failures are reported
in [upstream issue #817](https://github.com/maharmstone/btrfs/issues/817).

The harness extracts both production query functions, the full-ID compatibility
types and production `free_fileref`. Historical sources before extraction from
`fileinfo.c` remain supported. Windows services are native models: successful
parent opens take an actual fixture reference, failed opens take none, and every
query must release its locks and leave both parent reference counts unchanged.
Two cases repeat the query 100 times to check reference retention.

Both formats cover root and subvolume entries, ordinary and alternate-stream
queries, cached renamed/deleted links, uncached names, dummy children, deleted
parents, mixed parents, multiple links to one parent, lookup/allocation failures,
Unicode names and 64-bit inode/subvolume identifiers. Output checks verify
names, IDs, entry counts, aligned offsets, total required size and consumed
length with full, exact, one-byte-short, header-only and partial buffers.
Canaries surround each caller buffer.

The x64 ordinary structure model follows the installed MinGW Windows headers;
layout assertions check its size and field offsets. Full-ID types come from the
production source. Each scenario runs in its own process with a five-second
timeout and core dumps disabled. LeakSanitizer may require execution outside a
tracing sandbox.

These tests execute the production query and reference-release functions with
modeled parent lookup and locking. They do not exercise Windows IRP dispatch,
kernel resources, reference reaping, disk-backed lookup or concurrent threads.
Windows runtime testing remains outstanding.
