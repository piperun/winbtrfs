# Delayed stream-notification ownership regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/notify/check_notify.py . --build-dir build/notify-tests
python3 tests/notify/check_notify.py . --init zero --build-dir build/notify-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/notify/check_notify.py . --sanitize --build-dir build/notify-tests-asan
```

Add `--revision 78dd84d3` to reproduce the original code in `btrfs.c`, or
`--revision 926ee9a2` for the identical extracted notification code. Both
baselines pass 8/28 scenarios with pattern/zero locals. Twenty fail because
the queued worker receives a changed or retired stream name instead of the
name at queue time. The fixed source passes all 28 scenarios, including
AddressSanitizer, UndefinedBehaviorSanitizer and leak checks.

This addresses the delayed-notification name lifetime bug in
[upstream issue #820, item 3](https://github.com/maharmstone/btrfs/issues/820).
The queue retained a reference to the notified file's fileref, but borrowed
the stream's `dir_child.name` descriptor and buffer. Callers can delete the
stream immediately after queueing; retaining the parent does not retain that
child name. The queue now copies the descriptor and exactly `Length` bytes
into its context allocation. Existing worker/failure cleanup releases the
entire snapshot once. No second allocation or ownership reference is needed.

The runner extracts production `send_notification_fileref`,
`send_notification_fcb`, the work-item context, queue and worker, and
`free_fileref`. These execute with native models for pool allocation, file
path lookup, work-item scheduling, locks and the final Windows notification
service. Names are Windows UTF-16 counted strings, not host `wchar_t` strings.

Twenty cases cover four name lengths (2, 10, 514 and 65534 bytes) with an
unchanged name, in-place edits, descriptor/buffer replacement, buffer release
and release of both descriptor and buffer before dispatch. Inputs have no
terminator within `Length`, include non-ASCII characters and can have spare
capacity. Two batches queue three names from a changing caller buffer, retire
it, then execute workers in FIFO or reverse order. Six controls cover an
absent stream, both forms of empty stream, context allocation failure,
work-item allocation failure and shutdown suppression.

Assertions check the exact stream bytes, length, filter, action, filename,
notification presence, file reference counts, lock ordering and balanced
pool/work-item ownership. The caller releases its own fileref reference
before dispatch, so the final worker must release the last queue reference.
Before reading stream memory, the reporting model verifies that the descriptor
and bytes still occupy live allocations; this makes retirement failures
deterministic without depending on allocator reuse or dereferencing freed
memory. Mutation/replacement tests independently check the reported bytes.

Each case runs in its own process with a five-second timeout and core dumps
disabled. LeakSanitizer may require execution outside a tracing sandbox.
These tests cover the single-link notification path; hard-link path lookup,
real Windows notification delivery and concurrent kernel scheduling are not
exercised. Windows runtime testing remains outstanding.
