# Boot option cleanup regression

Run the production boot-option parser against mocked registry and pool APIs:

```sh
python tests/boot/check_boot.py . --build-dir /tmp/winbtrfs-boot
```

The test extracts `check_boot_options` without rewriting its statements and uses
Windows-sized `WCHAR`. It checks parsing and balanced allocations and handles on
successful queries with and without `SUBVOL`, query failure, open failure, and
allocation failure. `--revision REV` tests an earlier source revision.

Add `--sanitize` to enable AddressSanitizer and UndefinedBehaviorSanitizer.
LeakSanitizer is disabled for debugger/ptrace compatibility; the boot fixture
tracks pool allocations explicitly.
