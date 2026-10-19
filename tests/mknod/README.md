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

Eight directory scenarios fail at security assignment: `SeAssignSecurityEx`
receives `IsDirectoryObject=FALSE` because the new FCB type is still zero. The harness
checks this argument for all seven supported node types, the parent security
descriptor and existing inheritance flags, plus security-assignment failure
for both a regular file and directory. It verifies arguments passed to the
Windows API, not the resulting ACL contents.

Reproduce the remaining parent-reference leaks after the security fix:

```sh
python3 tests/mknod/check_mknod.py . --revision 71555232 --build-dir build/mknod-tests-parent
```

This baseline fails 28 of 42 scenarios: 14 successful creations and 14
failed creations retain an extra parent FCB reference. Success must keep
the legitimate dirty-queue reference and the separate parent fileref
reference; failure must leave both caller-owned parent counts unchanged.
Older baselines also fail these parent-reference checks.

Success must also release the local child fileref reference. After modeled
dirty-queue completion, the real reapers must reclaim the child fileref
and FCB, detach the cached directory entry and restore both parent counts.

The directory-entry failure cases cover all three allocation failures and
uppercase conversion failure, each for a regular file and directory.
These must immediately reap the unpublished fileref and its FCB: merely
decrementing the fileref leaves it unreachable by the normal reaper.

The harness extracts the real `mknod`, FCB and fileref constructors,
reference releases and reapers, dirty-queue insertion, name lookup,
inode collision check, directory-entry creation, FCB cache insertion
and software CRC32C code.
It checks automatic and explicit inode selection, 64-bit inode values,
all supported node types, parent-subvolume scope, cached and on-disk
collisions, permissions, error propagation and allocation failures.

Lookaside allocation/return, resource initialization/destruction, security
descriptor buffers and parent FCB references are tracked. Rejected nodes
must return both FCB allocations, destroy their resources and release the
security descriptor exactly once, without consuming caller-owned parent
references. Inode rejection preserves status, inode counters, existing
cache entries and root metadata.

Windows APIs and disk reads remain mocked; lookaside allocations are
fixtures and security descriptors are host buffers. Completing dirty queues
is modeled by removing entries and calling the real reference releases;
it does not run the flush worker or persist metadata. The normal fileref
and FCB reapers then run on the successfully published child.
Host sanitizer results do not prove Windows pool behavior or ACL
inheritance. Windows driver runtime and filesystem persistence still
require Windows testing.
