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
allocation failure covered. Deferred jobs retain borrowed compressed input
until modeled completion. Two discontiguous compressed extents cover queued
and running jobs, a later read failure, allocation failures while submitting
the second job, and preservation of the original error. The ownership checks
compare allocation ranges and never read freed input. Use `--revision 5ed6211f`
to reproduce the cleanup ordering bug: 146/194 scenarios pass. The corrected
implementation passes 194/194 with patterned and zero initialization and
ASan/UBSan.

Uncompressed inline extents are read from an item whose `ram_bytes` may claim
more data than the item stores, as a damaged or crafted image can. The bytes
after the stored data are filled with a marker, so a copy past the item shows
up in the output. A read must return the stored bytes and zeros for the rest
of `ram_bytes`, wherever it starts. A regular extent after a short inline
extent must still be read from its own disk address into its own file range.
An inline extent that an earlier extent has already read past must fail with
`STATUS_INTERNAL_ERROR` without copying.

LZO reads that start in the second 4 KiB of output skip the first segment.
Fixtures place the second segment header after zero to three bytes of page
padding and give the first segment lengths that end at, or one to three bytes
past, the end of the extent. The decompression job must receive exactly the
bytes left in the extent after the skip. Decompression in these reader fixtures
is modeled.

The runner also extracts the actual LZO segment parser for 15 boundary cases:
valid literal output, partial reads, unaligned headers, multiple segments with
page padding, and incomplete headers or payloads. It checks the returned status,
output bytes, and untouched bytes beyond the requested output. These cases run
against the guarded working-tree decoder only; `--revision` compares modeled
reader behavior and does not run a historical decoder. Source inspection showed
that the previous parser read a segment header before checking its span, then
checked the payload before accounting for the header and used wrapping addition.
The parser now checks the remaining header, copies it without an alignment
assumption, and checks the payload against the remaining input after the header.

`--revision 96fe76d4` has neither the inline-extent nor the LZO correction:
178/194 scenarios pass.

Cache, disk, hashing, decompression and worker execution are modeled services.
The compressed fixtures test the production reader's resource ownership, not
compression algorithms or Windows scheduling. The model completes ordinary
jobs at submission, queued jobs when the reader assists them, and running jobs
when the reader waits. Waiting on an unstarted queued job is recorded as an
error rather than hanging the host. A live kernel pool measurement is not
claimed. Pool balance is counted explicitly; LeakSanitizer is disabled under
sandbox tracing. Each run has a ten-second limit and core dumps are disabled.
Windows API checks must
also exercise an aligned uncached read crossing EOF in a zero-filled tail and
ordinary compressed-file reads on the finished driver.
