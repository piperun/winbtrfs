# File notification ownership regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/notify/check_notify.py . --build-dir build/notify-tests
python3 tests/notify/check_notify.py . --init zero --build-dir build/notify-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/notify/check_notify.py . --sanitize --build-dir build/notify-tests-asan
```

Add `--revision 37604350` to reproduce the deleted-parent reference leak.
That baseline passes 60/104 scenarios with pattern/zero locals; 44 fail
because a successful lookup of a deleted hard-link parent retains its
reference. Sixty-four notifications leave 64 additional references per
deleted parent, including parents that originally had no references.
Three links sharing one deleted parent retain three references per call.
The fixed source passes all 104 scenarios, including AddressSanitizer,
UndefinedBehaviorSanitizer and leak checks.

The hard-link loop previously released the lookup reference only when it
used the parent to build a notification path. Every successful lookup now
releases the parent whether it is used or skipped as deleted. Failed lookups
leave the output untouched and acquire no reference. Existing early exits
for path lookup, length and allocation failures still release exactly once.

The original 28 stream-name cases remain included. Before their fix, at
`78dd84d3` (or the identical extraction at `926ee9a2`), 20 of those cases
failed because the queued worker received a changed or retired stream name.

The stream-name fix addresses the delayed-notification name lifetime bug in
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

The 76 hard-link cases execute both direct notification calls with the tree
lock held and queued production workers. They cover all 27 combinations of
live, deleted and missing parents across three links; the live paths include
root and nested directories, with exact UTF-16 path bytes and name offsets
checked. Lookup models retain parents only on success and leave output
parameters unchanged on failure. Tests compare every parent's final reference
count with its initial count and check the number/order of lookup and path
calls. They verify that skipped parents do not suppress later valid links.

Additional cases repeat notifications 64 times with zero/one initial parent
references, share a deleted parent among three links, handle an empty hard-link
list and inject path sizing, path filling, pool allocation and oversized path
failures. Error cases require the loop to stop before visiting subsequent
links and balance all parent references, buffers, work items and locks.

Each case runs in its own process with a five-second timeout and core dumps
disabled. LeakSanitizer may require execution outside a tracing sandbox.
Single-link and hard-link notification construction execute production code.
Parent lookup and path discovery are deterministic models; real Windows
notification delivery, cache reaping and concurrent kernel scheduling are not
exercised. Windows runtime testing remains outstanding.
