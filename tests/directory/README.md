# Directory query regression tests

The native harness extracts `query_dir_item`, `next_dir_entry`, and
`query_directory` directly from the driver sources. It replaces Windows service
boundaries with deterministic fixtures, without substituting query logic.
`--revision` runs the same fixtures against a committed version, including
versions before the directory query modules were extracted.

```sh
python3 tests/directory/check_directory.py . --build-dir /tmp/winbtrfs-directory
python3 tests/directory/check_directory.py . --build-dir /tmp/winbtrfs-directory --init zero
python3 tests/directory/check_directory.py . --build-dir /tmp/winbtrfs-directory --sanitize
```

The fixtures cover both extended file ID formats, ordinary children, `.` and
`..`, subvolume and unavailable-subvolume entries, all supported directory
information classes, every buffer length residue for their required alignment,
partial initial output, larger-buffer retries, paginated wildcard scans,
case-sensitive matching, and the expression captured on the first request.

Windows layouts for the exercised records are checked with static assertions.
The wildcard boundary implements only `*` and `?` for these ASCII fixtures;
Windows handles the complete wildcard and Unicode semantics in the driver.
These checks exercise production code on the host, not Windows I/O dispatch.

Microsoft documents the query and record contracts in
[NtQueryDirectoryFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntquerydirectoryfile),
[FILE_NAMES_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_names_information),
and [FILE_ID_EXTD_DIR_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_extd_dir_info).

## Windows driver check

`windows.c` uses normal directory and file information queries. It refuses
volumes other than Btrfs volumes labeled `WB_RAID5` or `WB_RAID6`, creates a
unique directory and six files with `CREATE_NEW`, and caps directory requests
at 64. Successful runs delete only those fixtures. A failed run prints the
fixture directory and retains it for diagnosis.

```sh
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -O2 -municode tests/directory/windows.c -o /tmp/winbtrfs-directory/directory-windows.exe
```

Run `directory-windows.exe D:` only inside the disposable Windows test VM
with the candidate driver loaded. The client verifies both extended directory
ID formats against each opened child's `FileIdInformation`, all alignment
residues for names and extended records, partial output, a subsequent short
buffer followed by a larger retry, captured exact and wildcard patterns, and
case-sensitive native directory handles. Host tests cover the other record
classes and special subvolume entries.
