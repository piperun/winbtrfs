# RAID10 scrub repair offset regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/scrub/check_scrub.py . --build-dir build/scrub-tests
python3 tests/scrub/check_scrub.py . --init zero --build-dir build/scrub-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/scrub/check_scrub.py . --sanitize --build-dir build/scrub-tests-asan
```

Add `--revision 113f44b1` to reproduce the original code in `scrub.c`, or
`--revision dfae8bbb` for the identical extracted RAID10 helper. Both pass
72/172 scenarios with pattern/zero locals. The 100 failures write repairs at
the logical chunk offset instead of the physical stripe offset. In the
four-disk example, chunk offset `0x30000` reads at device offset `0x10000`,
then repairs at `0x30000`: damaged data remains and unrelated data changes.
The fixed source passes all 172 scenarios, including address,
undefined-behavior and leak sanitizers.

This addresses [upstream issue #816, item 1](https://github.com/maharmstone/btrfs/issues/816).
Both whole-mirror and sector-by-sector reconstruction writes now use the
destination stripe's physical `start`, already calculated for its read,
plus that device's stripe base. Other findings in that issue are separate.

The runner extracts the production RAID10 helper, `scrub_extent` caller,
`scrub_read_completion`, shared context types and `get_raid0_offset`. It uses
the revision's real `btrfs.h` layouts. Production code maps and submits reads,
waits for its real completion callback, verifies data/metadata, reconstructs
damaged buffers, submits repairs and frees its resources. Lower-driver I/O,
pool services, checksums and error logging are deterministic native models.
Checksums use a simple test hash rather than a production checksum algorithm;
metadata includes real tree headers with the expected logical `bytenr`.

Each virtual disk has a distinct base above 4 GiB and 512 KiB of memory-backed
storage. Fixtures independently enumerate logical sectors to derive expected
read windows and fill each physical region using the inverse stripe mapping.
The lower-driver model requires reads to use those exact windows, then applies
repair writes at the actual address production requests. Tests compare every
byte of every disk against its expected image, proving that the damaged
region is repaired and unrelated bytes stay intact. Failed writes are also
required to target the correct physical range.

The 160-case matrix covers two, four, six and eight disks with two mirrors,
six disks with three mirrors, four starting offsets, short/cross-stripe ranges,
data/metadata and both repair branches. Whole-mirror cases leave another mirror
fully healthy. Reconstruction cases damage every present mirror at different
sectors or metadata nodes while keeping a valid copy of each block. Completions
run inline or in deferred reverse order.

Twelve controls cover healthy data, read-only repair destinations, missing
mirrors, degraded reconstruction, read/write failures, scratch/context
allocation failures and twelve-disk metadata layouts. Assertions check exact
read/write sizes and addresses, return statuses, scrubbed byte counts,
completion accounting, error reporting and balanced pool/IRP ownership.
The maximum fixture uses about 13 MiB of RAM; it creates no disk images.

Each case runs in its own process with a five-second timeout and core dumps
disabled. LeakSanitizer may require execution outside a tracing sandbox.
The native I/O model exercises neither-I/O reads; Windows buffered/direct I/O,
MDLs, SEH, real kernel scheduling, RAID0/DUP and RAID56 are not exercised.
Unrecoverable blocks with no valid mirror are outside these repair cases.
Windows runtime testing remains outstanding.
