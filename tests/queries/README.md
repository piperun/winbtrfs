# Filesystem query regressions

Run from the repository root, using a bounded scratch directory:

```sh
python3 tests/queries/check_queries.py . --build-dir /tmp/winbtrfs-queries
python3 tests/queries/check_queries.py . --init zero --sanitize --build-dir /tmp/winbtrfs-queries
```

The runner extracts the production checksum and allocated-range functions,
along with the actual compiler cleanup macros from `btrfs_drv.h`. It never
translates `try`, `finally`, or `leave` into different host control flow.
`--revision` selects committed production source for before/after checks.
Each scenario runs in a separate process with a five-second deadline.
ASan/UBSan runs disable LeakSanitizer because sandbox tracing prevents its
process inspection. Pool allocations and resource acquisition are counted
by the host models.

The checksum fixtures provide enough backing storage and declared capacity
for two complete checksum slots. They check compressed placeholders followed
by ordinary checksums, compiler-dependent early exits, inline files, sparse
checksum allocation failure and allocation beyond EOF. Guards also check that
successful output ends at the reported sector count. Sector hashing is a
constant host model; these tests exercise output placement and status handling,
not the checksum algorithms themselves.

The allocated-range fixtures check full and partial intervals, ordinary files,
sparse holes (implicit and explicit), zero length, EOF, negative and overflowing
inputs, partial output and continuation, inline data, preallocation, ignored
extents and adjacent allocations. A page containing one range returns
`STATUS_BUFFER_OVERFLOW` when a second disjoint result cannot fit. Resuming at
the returned range's end returns the later allocation. Empty intersections,
including requests beginning at or beyond EOF, return success with zero bytes.
Additional guards cover zero/undersized output capacity, zero-length extents,
extent lengths spanning the unsigned 64-bit limit, and extents touching either
request boundary. These preserve the existing query behavior while the pure
clipping helper is tested independently under `tests/range-math`.

These are native contract checks with modeled kernel services. See the
[Microsoft FSCTL_QUERY_ALLOCATED_RANGES documentation](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_query_allocated_ranges)
for the requested input interval and returned ranges.

For Windows API checks on the disposable RAID6 fixture:

```sh
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -municode -I src \
  tests/queries/windows.c -lntdll -o /tmp/query-regression.exe
```

Run `query-regression.exe H:` in the test guest. The client requires a Btrfs
volume labeled `WB_RAID6` with 4 KiB sectors and CRC32C checksums. It creates
exclusive fixture files in its own directory and removes them after success.
Checks cover independently calculated ordinary checksums, compressed checksum
placeholders followed by ordinary checksums, sparse and ordinary subranges,
EOF clipping, zero length and empty files. The mixed fixture must have proven
compressed and ordinary mappings; failure to establish that layout returns
exit code 3 rather than passing the checksum assertion.

For allocated-range checks on the isolated healthy `R:` test volume:

```sh
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -municode \
  tests/queries/windows-ranges.c -lntdll -o /tmp/windows-ranges.exe
```

The client requires a Btrfs volume labeled `WBTRFS_TEST`. It creates an exclusive
task directory and sparse/ordinary files, checks twelve range queries, then
removes its fixtures. Coverage includes sparse holes, clipped subranges, EOF,
empty requests, one-slot partial output and zero-slot output. Its deterministic
status, byte-count and range output can be compared between driver builds.
