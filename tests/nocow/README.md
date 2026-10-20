# NOCOW write-boundary regressions

From the repository root:

```sh
python3 tests/nocow/check_nocow.py . --build-dir /tmp/winbtrfs-nocow/fixed
python3 tests/nocow/check_nocow.py . --init zero --build-dir /tmp/winbtrfs-nocow/zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/nocow/check_nocow.py . --sanitize --build-dir /tmp/winbtrfs-nocow/asan
```

Add `--revision 333652ff` to reproduce the overrun: 4/12 cases pass. The
corrected traversal passes all 12 with 512-byte and 4 KiB sectors.

The runner extracts actual `do_write_file` code and the real Btrfs extent
layout. Physical writes target a bounded memory disk with sentinel bytes
outside the owned extents. Assertions check every write address, length
and byte; a write starting inside an extent must continue into the next
extent or the allocation service. Controls cover writes starting at an
extent boundary, writes ending within an extent, and first/second physical
write failures. Each scenario has a five-second timeout and no core dumps.

Allocation, checksumming and lower-device writes are modeled services.
Preallocated extent conversion and actual Windows write dispatch are not
exercised. LeakSanitizer must run outside a tracing sandbox.
