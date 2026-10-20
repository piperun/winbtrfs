# RAID56 scrub batch ownership regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/scrub56/check_scrub56.py . --build-dir build/scrub56-tests
python3 tests/scrub56/check_scrub56.py . --init zero --build-dir build/scrub56-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/scrub56/check_scrub56.py . --sanitize --build-dir build/scrub56-tests-asan
```

Add `--revision 2f2306c8` to reproduce the unchanged batch code before the
fixes. That baseline passes 45/120 scenarios. The corrected code passes
all 120 with pattern/zero locals and address, undefined-behavior and leak
sanitizers. This covers items 3 and 4 of
[upstream issue #816](https://github.com/maharmstone/btrfs/issues/816).

A page-lock exception frees the unlocked MDL immediately. Clearing the
IRP's MDL pointer prevents common cleanup from unlocking and freeing it
again. Initializing every stripe's IRP pointer makes cleanup safe for
devices that preparation never reaches. Resetting every repair flag
before each batch prevents an earlier repair from authorizing writes of
healthy, failed or partially read later data. Failure statuses remain
unchanged, while later verified batches can still request fresh repairs.

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

The 120 cases cover three-device RAID5 and four-device RAID6, direct and
neither-I/O reads, runs of 2, 18 and 35 rows, healthy batches, repairs in the
first/second/all/alternating batches, missing devices, and page-lock,
IRP-allocation, MDL-allocation and partial-read failures at every device
in the first or second batch. Assertions require balanced pool, IRP,
MDL and range-lock ownership, complete submitted I/O and the original
error status. Each case has a five-second timeout with core dumps disabled.

Buffered I/O, pool-allocation failures, failed repair writes, exact
one-row batch remainders, actual SEH, Windows kernel scheduling and
on-disk persistence are not exercised. Windows runtime testing remains
outstanding. LeakSanitizer may require execution outside a tracing sandbox.
