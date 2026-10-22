# Extent rationalization regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/extents/check_extents.py . --build-dir build/extents-tests
python3 tests/extents/check_extents.py . --init zero --build-dir build/extents-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/extents/check_extents.py . --sanitize --build-dir build/extents-tests-asan
python3 tests/extents/check_extents.py . --compare 96fe76d4 --build-dir build/extents-compare
python3 tests/extents/check_extents.py . --bench --compare 96fe76d4 --build-dir build/extents-bench
```

`--revision` takes the sources from a git revision instead of the working
tree, and `--compare` also builds `rationalize_extents` from a second revision;
both read files with `git show`. The runner extracts the production
`extent_range` type, the helpers defined after it and `rationalize_extents`,
and uses the revision's real `btrfs.h` on-disk structures.

## Merge reference failures

Add `--revision ad9ecb63` to reproduce the original code in `flushthread.c`,
or `--revision 548b845f` for the identical extracted helper. Both baselines
pass 26/119 cases. All 72 injected merge-reference failures return the wrong
status. Without trimming, pattern-initialized locals produce `0xfefefefe`;
after successful prefix/suffix trimming, the function returns stale success.
Zero-initialized locals also return success on every merge failure. The
random cases fail for the same reason, and the step-count cases as described
below.

The merge loop declared an inner `Status`, shadowing the status returned at
the common cleanup label. Both failed reference calls jumped out of that
scope without updating the returned status. The compiler reported this as
`Status may be used uninitialized` in `flush_fcb` after inlining the helper.
Removing that shadow propagates the exact error through cleanup to the caller.

The 84-case merge matrix in `extents_test.c` covers regular/preallocated
extents, checksums enabled/disabled, no trimming, prefix trimming and suffix
trimming. For each, three physically adjacent extents merge successfully or
fail at each of the six reference removal/addition calls. Assertions require
the exact error, the complete expected callback arguments, no work after the
failure, the expected extent updates and freeing every temporary allocation
once.

Fourteen controls exercise empty/single-extent lists, nonunique/compressed/
hole/inline extents, nonadjacent ranges, a failure at each allocation in turn,
missing chunks, existing truncation failures at reference update, cache load
and free-space insertion, and one disk extent used by two extents. Only the
first of those two in the list decides whether the function looks up the chunk
to trim: when it uses the whole disk extent there is no lookup, and when it
uses only part there is one. Chunk lock balance is checked, including failures
while locked. Success cases check the merged physical address, length, extent
offsets, file lengths and chunk usage.

## Random extent lists

`compare_test.c` runs 16 cases of 250 random layouts each. A layout has one
to four chunks, adjacent or with gaps between them, and up to 300 disk
extents, often placed directly after the one before. One layout in eight uses
extents of up to 128 MiB, the merge limit. One to three file extents refer to
parts of each disk extent, in roughly physical order or shuffled, with holes,
an inline extent and compressed, shared or preallocated extents mixed in. Some
layouts put a disk extent outside every chunk, or have no extent the function
may change. No two disk extents overlap, as on a consistent filesystem.

Each layout runs once without failures, then with a failure at up to 12 of
the reference, checksum, cache-load and free-space calls, and with each of up
to four allocations failing. Every run must:

- return the injected status, `STATUS_INSUFFICIENT_RESOURCES` without making
  any call if an allocation failed, `STATUS_INTERNAL_ERROR` after a lookup that
  finds no chunk, or success;
- make no call after a failed one other than releasing the chunk lock, free
  every allocation and leave no chunk locked;
- keep every extent it may change on the same bytes on disk
  (`disk_bytenr + offset` and `num_bytes` unchanged) and leave every other
  extent unchanged; after success, each extent's used part must lie inside its
  disk extent, with `ram_bytes` equal to `disk_num_bytes`.

With `--compare REVISION`, every run is repeated with `rationalize_extents`
from that revision, built as a separate object file against the same service
models and the same `btrfs.h`. Both must return the same status,
make the same lookup, reference, checksum, lock, cache-load and free-space
calls with the same arguments in the same order, and leave the same extents
and chunk usage. Allocation failures are checked for each revision on its own,
because the two may allocate differently. Each case must include merged
extents, trimmed starts and ends, a missing chunk, a layout with nothing to
allocate for, and failures at each kind of call.

## Loop steps and timings

The runner adds one to a counter each time the extracted code evaluates a
`while` or `for` condition. `steps-5000` to `steps-40000` run a file written
like a VM disk image: each extent of 1-16 pages has its own disk extent at a
random free 64-page slot of 1 GiB chunks, one in eight follows the extent
before it on disk, so the two merge, and one in sixteen leaves 1-4 pages of its
disk extent unused. `steps-small-chunks` puts 20,000 such extents in 625
chunks of 16 MiB, so code that walks the later ranges after each chunk lookup
also exceeds the limit. Each chunk must be looked up once. For E extents the
count must stay within 4E⌈log2 E⌉ + 16E: sorting the extents and looking each
one up take about E⌈log2 E⌉ steps each, and the other passes a few steps per
extent. The list scans in 96fe76d4 take 28,059,734 steps for 5,000 extents and
1,794,470,288 for 40,000, against limits of 340,000 and 3,200,000.

`--bench` builds the function at `-O2` without counters and prints the best of
five runs on the same layouts, for the working tree or `--revision`, and with
`--compare` for that revision too. The timings illustrate the change; the step
counts are the regression check.

## Limits

Each case runs in its own process with core dumps disabled, and a five-second
timeout in `extents_test.c` or 300 seconds in `compare_test.c`. Windows pool,
chunk, reference, checksum and free-space services are deterministic native
models. These tests execute the actual merge/error path, but do not execute
`flush_fcb`'s later persistence work or exercise real Windows kernel
allocation, I/O or scheduling. Windows runtime testing remains outstanding.
