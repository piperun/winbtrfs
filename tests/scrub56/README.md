# RAID56 scrub batch ownership regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/scrub56/check_scrub56.py . --build-dir build/scrub56-tests
python3 tests/scrub56/check_scrub56.py . --init zero --build-dir build/scrub56-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/scrub56/check_scrub56.py . --sanitize --build-dir build/scrub56-tests-asan
```

Add `--revision 2f2306c8` to reproduce the original batch ownership bugs.
On the original 120 scenarios, that baseline passes 45/120; the corrections
in `6e902189` make all 120 pass with pattern/zero locals and address,
undefined-behavior and leak sanitizers. This covers items 3 and 4 of
[upstream issue #816](https://github.com/maharmstone/btrfs/issues/816).

Add `--revision 6e902189` to reproduce three subsequent batch and cleanup
defects with the current runner. That baseline passes 206/298 scenarios:
32 cases skip a final row, 40 failed repair writes retain I/O resources,
and 20 failed pool allocations retain earlier error bitmaps. The corrected
source passes all 298 with both initialization modes and sanitizers.

A page-lock exception frees the unlocked MDL immediately. Clearing the
IRP's MDL pointer prevents common cleanup from unlocking and freeing it
again. Initializing every stripe's IRP pointer makes cleanup safe for
devices that preparation never reaches. Resetting every repair flag
before each batch prevents an earlier repair from authorizing writes of
healthy, failed or partially read later data. Failure statuses remain
unchanged, while later verified batches can still request fresh repairs.

The batch loop includes the final stripe row even when a full batch leaves
only one row. Common cleanup releases every batch IRP and MDL before any
repair write, so a failed write can return its original error without
leaving later devices' I/O resources attached to freed buffers. Both partial
allocation unwind paths release earlier error bitmaps as well as buffers.

The runner extracts the production `scrub_chunk_raid56_stripe_run`, its
completion callback, context types and real `btrfs.h` layouts. It supports
both the original `scrub.c` layout and the extracted sources. Pool
allocations start with nonzero bytes. MDL services track allocated,
locked and freed states; `setjmp`/`longjmp` models an exception at the
production page-lock boundary. IRP submission executes the real
completion callback inline or in deferred reverse order.

Parity verification is a controlled service model that selects repairs
only after all reads complete successfully. It checks the read bytes and
changes a known byte in the requested batch. These tests execute actual
batch setup, I/O, repair writes and cleanup; they do not test parity
arithmetic, checksums or reconstruction algorithms. Memory disks and
full-image comparisons check exact write counts, addresses, sizes and
unchanged bytes outside the selected repair.

The original 120 cases cover three-device RAID5 and four-device RAID6, direct and
neither-I/O reads, runs of 2, 18 and 35 rows, healthy batches, repairs in the
first/second/all/alternating batches, missing devices, and page-lock,
IRP-allocation, MDL-allocation and partial-read failures at every device
in the first or second batch. Assertions require balanced pool, IRP,
MDL and range-lock ownership, complete submitted I/O and the original
error status. Each case has a five-second timeout with core dumps disabled.

Another 178 cases cover healthy runs of 1/15/16/17/31/32/33 rows, repairs in
the final row of 17/33-row runs, failed repair writes at every device in the
first or second batch, and every pool allocation during data/metadata setup
for both RAID levels. Write failures run with both I/O modes and completion
orders. The failed-write model returns an error without changing that disk;
earlier successful repairs must persist and later writes must not occur.
Allocation failures must return insufficient resources, submit no reads and
retain no pools. Pool frees also reject buffers still referenced by a live MDL.

Buffered I/O, actual SEH, Windows kernel scheduling and on-disk persistence
are not exercised. Windows runtime testing remains outstanding.
LeakSanitizer may require execution outside a tracing sandbox.
