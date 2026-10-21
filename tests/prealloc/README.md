# Preallocated-extent write regressions

From the repository root:

```sh
python3 tests/prealloc/check_prealloc.py . --build-dir /tmp/winbtrfs-prealloc/fixed
python3 tests/prealloc/check_prealloc.py . --init zero --build-dir /tmp/winbtrfs-prealloc/zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/prealloc/check_prealloc.py . --sanitize --build-dir /tmp/winbtrfs-prealloc/asan
```

The runner extracts the actual `do_write_file_prealloc` and the real Btrfs
extent layout. A write into a four-sector preallocated extent replaces all of
it, its start, its end or its middle. Each case runs with checksums enabled
and disabled, once with a successful data write and once with the first data
write failing.

After a failed write the function must return the write error and free every
extent it allocated, leaving the file's extent list, the written count, the
rollback list and extent references unchanged. After a successful write the
live extents must cover the old range in order, regular where written and
preallocated elsewhere, with checksums only on the written part. Every
allocation must belong to one of those extents.

`--revision 96fe76d4` leaks the replacement extent when a write over the whole
extent fails: 14/16 scenarios pass. The corrected function passes 16/16 with
patterned and zero locals and ASan/UBSan.

Disk writes, checksum calculation, rollback entries, extent references and
extent removal are modeled services. Each scenario has a five-second timeout
and no core dumps. LeakSanitizer must run outside a tracing sandbox.
