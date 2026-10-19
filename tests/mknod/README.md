# mknod inode regression

Run from the repository root on Linux with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/mknod/check_mknod.py . --build-dir build/mknod-tests
python3 tests/mknod/check_mknod.py . --init zero --build-dir build/mknod-tests-zero
python3 tests/mknod/check_mknod.py . --sanitize --build-dir build/mknod-tests-asan
```

The sanitizer command requires GCC's AddressSanitizer and
UndefinedBehaviorSanitizer runtimes. Run outside a tracing sandbox if
LeakSanitizer cannot inspect the test processes.

Reproduce the original bug with the same harness:

```sh
python3 tests/mknod/check_mknod.py . --revision 3fa5cf21 --build-dir build/mknod-tests-original
```

This command is expected to fail. Pattern initialization makes accidental
reads of uninitialized automatic variables deterministic without modifying
the tested C routines. Each case runs in a separate process with a
five-second timeout and core dumps disabled.

The harness extracts the real `mknod`, name lookup, inode collision check,
directory-entry creation, FCB cache insertion and software CRC32C code.
It checks automatic and explicit inode selection, 64-bit inode values,
all supported node types, parent-subvolume scope, cached and on-disk
collisions, permissions, error propagation and allocation failures.

Windows locks, security descriptors, object allocation and metadata reads
are mocked. Host sanitizer results do not prove Windows pool ownership or
ACL inheritance. The explicit inode rejection cases record an existing
unreaped FCB on those paths; that leak is outside this inode-value fix.
Windows driver runtime and filesystem persistence still require Windows
testing.
