# Filename query regression

Run production filename queries against root, file, and named-stream fixtures:

```sh
python tests/filename/check_filename.py . --build-dir /tmp/winbtrfs-filename --init pattern
python tests/filename/check_filename.py . --build-dir /tmp/winbtrfs-filename --init zero
```

The test extracts both query functions without rewriting their statements and
uses Windows-sized `WCHAR`. It checks reported lengths, status, remaining space,
output prefixes, and output bounds across capacities below and above the complete
name length, including buffers that fit the stream path but truncate `:$DATA`.
`--revision REV` tests an earlier source revision.

Add `--sanitize` to enable AddressSanitizer and UndefinedBehaviorSanitizer.
LeakSanitizer is disabled for debugger/ptrace compatibility.

## Windows runtime check

Cross-compile the bounded real-driver client:

```sh
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -municode tests/filename/windows.c -lntdll -o /tmp/filename-windows.exe
```

Run `filename-windows.exe D:` with the test-volume drive letter. It requires a
Btrfs volume labeled `WB_RAID5` or `WB_RAID6`, exclusively creates one uniquely
named directory and a file with a named stream, then queries nine buffer
capacities spanning the stream path and `:$DATA` suffix. It checks the returned
status, required filename length, and copied prefix, and removes its own fixture.
It refuses other volume labels and preserves existing files.
