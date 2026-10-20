# File read regressions

```sh
python3 tests/read/check_read.py . --build-dir /tmp/winbtrfs-read
python3 tests/read/check_read.py . --init zero --sanitize --build-dir /tmp/winbtrfs-read
```

The runner extracts the actual file-read functions, their private record types,
the Btrfs extent layout and the driver's compiler cleanup macros. It checks
cached and uncached zero-filled reads around valid-data length and EOF, byte
counts and unchanged output beyond the returned bytes. Compressed-read fixtures
check completed calculation-job ownership on success and failure, with each
allocation failure covered. Use `--revision be01e7d0` to reproduce the unchanged
implementation: 114/132 scenarios pass. The corrected implementation passes
132/132 with patterned and zero initialization and ASan/UBSan.

Cache, disk, hashing, decompression and worker execution are modeled services.
The compressed fixtures test the production reader's resource ownership, not
compression algorithms or Windows scheduling. Jobs complete synchronously in
the model; a live kernel pool measurement is not claimed. Pool balance is
counted explicitly; LeakSanitizer is disabled under sandbox tracing. Each run
has a ten-second limit and core dumps are disabled. Windows API checks must
also exercise an aligned uncached read crossing EOF in a zero-filled tail and
ordinary compressed-file reads on the finished driver.
