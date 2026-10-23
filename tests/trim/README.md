# Explicit TRIM allocation-failure regressions

Run with Python 3 and GCC supporting `-ftrivial-auto-var-init`:

```sh
python3 tests/trim/check_trim.py . --build-dir build/trim-tests
python3 tests/trim/check_trim.py . --init zero --sanitize --build-dir build/trim-tests
```

The 166 scenarios execute the production synchronous `trim_devices` helper
used for explicit balance discard. The 156-case matrix covers successful
allocation, DSM-buffer failure and IRP failure for one or three devices,
plus mixed read-only, missing, unsupported and empty devices. Completion
orders include inline, deferred FIFO, deferred reverse and mixed timing.
Controls cover opt-outs, multiple ranges, allocation failure, device errors
and cancellation. Exact 64-bit offsets, DSM layout, IRP ownership and pool
cleanup are checked.

The wait model completes all submitted requests before requiring the event
to be signaled. A skipped device must retire its completion slot; otherwise
it reports a deterministic hang. Each scenario runs in its own process with
a five-second test timeout. Production code gains no timeout.

Automatic transaction discard is exercised separately by
`tests/trim-queue/check_trim_queue.py`. Those tests cover nonblocking
submission, range reservation, transaction merge errors, partial reclaim,
shutdown and completion races. `tests/free-space/check_free_space.py`
executes both cache writers with pending reservations and allocation faults.

`--revision` selects historical sources containing the standalone
`trim_devices` helper. Earlier inline-only implementations require the
matching historical test runner.
