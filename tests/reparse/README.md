# Reparse deletion regressions

```sh
python3 tests/reparse/check_reparse.py . --build-dir /tmp/winbtrfs-reparse
python3 tests/reparse/check_reparse.py . --init zero --sanitize --build-dir /tmp/winbtrfs-reparse
```

The runner extracts the production deletion handler without rewriting its
statements. File and directory reparse points must reject a different tag or
third-party GUID without changing data or metadata. Native symlinks use the
same tag that `FSCTL_GET_REPARSE_POINT` returns to the caller, including LX
symlinks. Controls cover matching deletes, missing reparse points, invalid
headers, short stored data, and a failed file read. All scenarios run with
patterned and zero initialization and ASan/UBSan.

The dirty-attribute persistence service, rollback, notifications and locks are
modeled. The host fixture follows the actual DOSATTRIB dirty predicate in
`flushthread.c`; it does not execute that serializer or reload an on-disk inode.
Windows verification must include close/reopen and a fresh mount or reboot
after removing a symlink with previously persisted ordinary attributes. Each
run has a ten-second limit, disables core dumps and disables LeakSanitizer
under tracing.
