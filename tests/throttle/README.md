# Fast write throttle deadlock regression

Run from the repository root on Linux with Python 3, GCC and pthreads:

```sh
python3 tests/throttle/check_throttle.py . --build-dir build/throttle-tests
python3 tests/throttle/check_throttle.py . --build-dir build/throttle-tests-asan --sanitize
```

To reproduce the deadlock against the original source:

```sh
python3 tests/throttle/check_throttle.py . --revision 96fe76d4 --build-dir build/throttle-tests-original
```

The harness extracts the real `fast_io_write` (src/fastio.c), `acquire_for_lazy_write`
and `release_from_lazy_write` (src/cache.c) and `do_flush` (src/threads.c, or
src/flushthread.c in upstream's layout), and runs them on three pthreads: a writer, the
flush thread and the lazy writer. This is the deadlock reported in maharmstone/btrfs#345
and #822: a fast write holds `tree_lock` shared while `FsRtlCopyWrite` waits in the
`CcCanIWrite` throttle, the flush thread waits for `tree_lock` exclusive, and the lazy
writer, the only thread that can open the throttle, waits for `tree_lock` shared behind
the flush.

The mocks model:

- ERESOURCE as documented for `ExAcquireResourceSharedLite`,
  `ExAcquireSharedStarveExclusive` and `ExAcquireResourceExclusiveLite`: an owner is
  granted the resource again; a shared request from a thread that does not own it waits
  while the resource is owned exclusive or an exclusive request is queued; a
  starve-exclusive request waits only while it is owned exclusive.
- `FsRtlCopyWrite` as documented, in the order ReactOS uses: `CcCanIWrite` with the
  caller's Wait first, then the file's main resource, shared for a write that ends
  within ValidDataLength and exclusive otherwise.
- `CcCanIWrite` holding writers while dirty pages are over a threshold; only the lazy
  writer lowers the count.
- The IRP the I/O manager sends when fast I/O declines, in `write_file2`'s order:
  `CcCanIWrite` with no lock held, then `tree_lock` shared and the file's resource
  exclusive.

Scenarios, each in its own process with a ten-second timeout:

- `throttled`: the throttle is closed before the write starts.
- `throttle-closes`: the throttle closes after `fast_io_write` has taken `tree_lock`,
  so a `CcCanIWrite` check made before taking the lock does not prevent the deadlock.
- `unthrottled`: the write goes through fast I/O.

In each scenario the flush thread asks for `tree_lock` while the writer is inside
`FsRtlCopyWrite`, and the lazy writer starts once the writer is waiting or done. A
scenario fails if any thread makes no progress for two seconds, and prints what each
thread is waiting for and who owns `tree_lock`. It also checks that every lock is
released, the flush ran once and the data was written once.

Cache manager timing, the real dirty page threshold and the real ERESOURCE hand-off
order are not modeled; they need a Windows test with sustained cached writes.
