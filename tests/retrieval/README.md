# Retrieval-pointer regressions

From the repository root:

```sh
python3 tests/retrieval/check_retrieval.py . --build-dir /tmp/winbtrfs-retrieval/fixed
python3 tests/retrieval/check_retrieval.py . --init zero --build-dir /tmp/winbtrfs-retrieval/zero
python3 tests/retrieval/check_retrieval.py . --mingw --build-dir /tmp/winbtrfs-retrieval/mingw
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

The `--mingw` mode compiles the actual cleanup macros from `btrfs_drv.h`.
Against revision `a496f584`, it passes 356/480 cases: early EOF and
short-buffer returns continue because GCC's `leave` is empty. Explicit
exits to the cleanup path restore 480/480 passing cases. Run this mode
alongside the translated cleanup mode to cover compiler differences.

The runner extracts the actual query and real Btrfs extent layout. Only
Windows structured cleanup syntax is translated to a host cleanup label;
resource acquisition is modeled. Each scenario has a five-second timeout
and core dumps are disabled. These tests do not execute Windows kernel
buffer probing, scheduling or filesystem-control dispatch. LeakSanitizer
must run outside a tracing sandbox.

For real-driver checks on a disposable RAID fixture, cross-compile:

```sh
x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -municode -I src \
  tests/retrieval/windows.c -lntdll -o /tmp/extent-regression.exe
```

Run `extent-regression.exe D:` inside Windows. It requires a Btrfs volume
named `WB_RAID5` or `WB_RAID6`, creates files with exclusive new names,
and removes them after successful checks. Sparse and sliced extent queries
must return exact full-buffer results and make progress with one run per
page. The NOCOW check reopens both files after a boundary write and verifies
the target bytes and an unchanged neighboring file. It does not guarantee
that the allocator placed those files next to each other; physical overrun
coverage comes from the native memory-disk regression.
