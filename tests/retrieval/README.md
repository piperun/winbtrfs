# Retrieval-pointer regressions

From the repository root:

```sh
python3 tests/retrieval/check_retrieval.py . --build-dir /tmp/winbtrfs-retrieval/fixed
python3 tests/retrieval/check_retrieval.py . --init zero --build-dir /tmp/winbtrfs-retrieval/zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/retrieval/check_retrieval.py . --sanitize --build-dir /tmp/winbtrfs-retrieval/asan
```

Add `--revision 333652ff` to reproduce the original query: 122/480 cases
pass. The corrected query passes 480/480. The matrix checks exact run
boundaries, physical offsets, copied counts, byte counts, forward progress,
output guards and balanced resource acquisition. It covers 512-byte and
4 KiB sectors, zero to three output slots, and requests before, inside and
after each extent.

Fixtures include contiguous and sliced extents, leading/internal/trailing
holes, explicit sparse extents, compressed extents, ignored records,
inline files, preallocation beyond EOF and a partial final sector.

The runner extracts the actual query and real Btrfs extent layout. Only
Windows structured cleanup syntax is translated to a host cleanup label;
resource acquisition is modeled. Each scenario has a five-second timeout
and core dumps are disabled. These tests do not execute Windows kernel
buffer probing, scheduling or filesystem-control dispatch. LeakSanitizer
must run outside a tracing sandbox.
