# RAID10 scrub regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/scrub/check_scrub.py . --build-dir build/scrub-tests
python3 tests/scrub/check_scrub.py . --init zero --build-dir build/scrub-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/scrub/check_scrub.py . --sanitize --build-dir build/scrub-tests-asan
```

Add `--revision 113f44b1` to reproduce the original code in `scrub.c`, or
`--revision dfae8bbb` for the identical extracted RAID10 helper. On the original
172 repair scenarios, both pass 72/172 with pattern/zero locals. The 100
failures write repairs at the logical chunk offset instead of the physical
stripe offset. In the
four-disk example, chunk offset `0x30000` reads at device offset `0x10000`,
then repairs at `0x30000`: damaged data remains and unrelated data changes.
The physical-offset fix makes all 172 repair scenarios pass, including
address, undefined-behavior and leak sanitizers.

This addresses [upstream issue #816, item 1](https://github.com/maharmstone/btrfs/issues/816).
Both whole-mirror and sector-by-sector reconstruction writes now use the
destination stripe's physical `start`, already calculated for its read,
plus that device's stripe base.

Add `--revision e104f35d` to reproduce the device-index typo from item 2 of
the same issue. The all-mirrors-bad data branch tested `devices[j + j]`
instead of the current mirror, `devices[j + k]`. This could read past the
device array, skip reports for present mirrors, or report absent mirrors.
That revision passes 184/234 scenarios; the corrected index makes all 234
pass. The fixture allocates exactly `num_stripes` device pointers so address
sanitizers detect an out-of-range index rather than reading unused slots.

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

Another 62 scenarios damage the same data sector on every present mirror.
They cover every mirror group in two-, four-, six-, eight- and twelve-disk
layouts, two or three mirrors, each possible missing mirror, unrelated
missing devices, read-only devices and requests spanning multiple groups.
Checks require one unrecoverable report per damaged present device at its
exact logical address, the existing corruption-counter updates, no recovery
reports and no writes. The existing successful scrub return status is
preserved; the errors are reported through the scrub error log.

Each case runs in its own process with a five-second timeout and core dumps
disabled. LeakSanitizer may require execution outside a tracing sandbox.
The native I/O model exercises neither-I/O reads; Windows buffered/direct I/O,
MDLs, SEH, real kernel scheduling, RAID0/DUP and RAID56 are not exercised.
Unrecoverable metadata blocks are outside these data-index regressions.
Windows runtime testing remains outstanding.
