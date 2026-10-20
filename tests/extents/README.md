# Extent rationalization status regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/extents/check_extents.py . --build-dir build/extents-tests
python3 tests/extents/check_extents.py . --init zero --build-dir build/extents-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/extents/check_extents.py . --sanitize --build-dir build/extents-tests-asan
```

Add `--revision ad9ecb63` to reproduce the original code in `flushthread.c`,
or `--revision 548b845f` for the identical extracted helper. Both baselines
pass 26/98 cases. All 72 injected merge-reference failures return the wrong
status. Without trimming, pattern-initialized locals produce `0xfefefefe`;
after successful prefix/suffix trimming, the function returns stale success.
Zero-initialized locals also return success on every merge failure.

The merge loop declared an inner `Status`, shadowing the status returned at
the common cleanup label. Both failed reference calls jumped out of that
scope without updating the returned status. The compiler reported this as
`Status may be used uninitialized` in `flush_fcb` after inlining the helper.
Removing that shadow propagates the exact error through cleanup to the caller.

The runner extracts the production `extent_range` type and
`rationalize_extents` function and uses the revision's real `btrfs.h` on-disk
structures. The 84-case merge matrix covers regular/preallocated extents,
checksums enabled/disabled, no trimming, prefix trimming and suffix trimming.
For each, three physically adjacent extents merge successfully or fail at
each of the six reference removal/addition calls. Assertions require the
exact error, the complete expected callback arguments, no work after the
failure, the expected extent updates and freeing every temporary range once.

Fourteen controls exercise empty/single-extent lists, nonunique/compressed/
hole/inline extents, nonadjacent ranges, all three range-allocation failure
positions, missing chunks and existing truncation failures at reference
update, cache load and free-space insertion. Chunk lock balance is checked,
including failures while locked. Success cases check the merged physical
address, length, extent offsets, file lengths and chunk usage.

Each case runs in its own process with a five-second timeout and core dumps
disabled. Windows pool, chunk, reference, checksum and free-space services are
deterministic native models. These tests execute the actual merge/error path,
but do not execute `flush_fcb`'s later persistence work or exercise real Windows
kernel allocation, I/O or scheduling. Windows runtime testing remains outstanding.
