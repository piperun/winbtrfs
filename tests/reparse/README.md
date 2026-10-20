# Reparse deletion regressions

```sh
python3 tests/reparse/check_reparse.py . --build-dir /tmp/winbtrfs-reparse
python3 tests/reparse/check_reparse.py . --init zero --sanitize --build-dir /tmp/winbtrfs-reparse
```

The runner extracts the production deletion handler without rewriting its
statements. Symlink deletion must mark the cleared DOS attributes for
persistence. Ordinary files and directories must reject deletion of a
nonexistent reparse point without truncating data or changing metadata.
Controls cover real file/directory reparse points and a mismatched symlink tag.
The unchanged `be01e7d0` passes 3/6 scenarios; the fix passes 6/6 with patterned
and zero initialization and ASan/UBSan.

The dirty-attribute persistence service, rollback, notifications and locks are
modeled. The host fixture follows the actual DOSATTRIB dirty predicate in
`flushthread.c`; it does not execute that serializer or reload an on-disk inode.
Windows verification must include close/reopen and a fresh mount or reboot
after removing a symlink with previously persisted ordinary attributes. Each
run has a ten-second limit, disables core dumps and disables LeakSanitizer
under tracing.
