# mknod regression

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

Reproduce the rejected-inode leak after the inode identity fix:

```sh
python3 tests/mknod/check_mknod.py . --revision a4b0140a --build-dir build/mknod-tests-leak
```

The leak affects cached collision, on-disk collision and metadata-read
error, each for a file and directory. Older revisions also fail the
directory-security checks below before reaching the directory leak checks.

Reproduce the directory security argument bug after the rejection cleanup:

```sh
python3 tests/mknod/check_mknod.py . --revision 3eed7ee9 --build-dir build/mknod-tests-security
```

This baseline fails eight directory scenarios: `SeAssignSecurityEx` receives
`IsDirectoryObject=FALSE` because the new FCB type is still zero. The harness
checks this argument for all seven supported node types, the parent security
descriptor and existing inheritance flags, plus security-assignment failure
for both a regular file and directory. It verifies arguments passed to the
Windows API, not the resulting ACL contents.

The harness extracts the real `mknod`, FCB constructor/destructor, name
lookup, inode collision check, directory-entry creation, FCB cache
insertion and software CRC32C code.
It checks automatic and explicit inode selection, 64-bit inode values,
all supported node types, parent-subvolume scope, cached and on-disk
collisions, permissions, error propagation and allocation failures.

Lookaside allocation/return, resource initialization/destruction, security
descriptor buffers and parent FCB references are tracked. Rejected nodes
must return both FCB allocations, destroy their resources and release the
security descriptor and borrowed parent reference exactly once. Status,
inode counters, existing cache entries and root metadata must stay intact.

Windows APIs and disk reads remain mocked; lookaside allocations are
fixtures and security descriptors are host buffers. Fileref rollback
remains modeled and does not validate the real rollback implementation.
Host sanitizer results do not prove Windows pool behavior or ACL
inheritance. Windows driver runtime and filesystem persistence still
require Windows testing.
