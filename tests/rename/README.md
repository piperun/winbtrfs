# Rename dispatch ownership regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/rename/check_rename.py . --build-dir build/rename-tests
python3 tests/rename/check_rename.py . --init zero --build-dir build/rename-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/rename/check_rename.py . --sanitize --build-dir build/rename-tests-asan
```

Add `--revision 57c97b1f` to reproduce the UTF-8 buffer leak. That source passes
16/92 cases: 76 cases allocate a temporary name that dispatch never releases.
The 16 passing cases exit before allocation or fail the allocation itself.
The fixed source passes all 92 with both initialization modes and sanitizers.

The original 66 cases also retain coverage for the same-directory parent leak
reported in [upstream issue #817](https://github.com/maharmstone/btrfs/issues/817).
Before buffer-release checks were added, the fixture in `57c97b1f` passed
44/66 against `e56845d0`, exposing 22 retained parent references. The current
suite requires both parent and buffer ownership to balance.

The runner extracts production `set_rename_information`,
`rename_in_same_directory`, `free_fileref`, filename validation and software
CRC32C. Both the original `fileinfo.c` layout and the extracted `rename.c`
layout are supported. The same-directory helper executes its actual name
updates, metadata changes, allocation cleanup and notification calls.

Both legacy and extended requests cover relative names, target-directory
handles, parent lookup, files, directories, renaming to self, replacement,
long and Unicode names, collisions, access failures and open children.
Seven allocation failures and four filename-query failures cover the request
buffer and every allocation in the same-directory helper. Additional cases
cover UTF-8 sizing/fill conversion failures, replacement validation, destination
deletion failures and deletion-access denial. Root rejection, stream dispatch
and cache-flush failures exercise cleanup before the buffer is allocated.
Repeated-renames and repeated-collisions cases execute 100 requests on the
same fileref and parent. Two more cases close the child and require the
parent's actual reference count to reach zero.

Every request requires the dispatcher to release its temporary UTF-8 buffer
exactly once before returning, or not release anything when allocation did not
succeed. The fixture no longer frees that temporary. Only the current directory
entry's three name buffers and its optional old name may remain allocated;
fixture teardown releases those owned buffers and requires zero live allocations.
Cross-directory service models require the borrowed buffer to remain live
while the helper runs.

Requests also check source/destination/collision reference counts, balanced
resources and subject contexts, and rollback selection. Successful same-directory
requests also verify the changed name, inode metadata and notifications.
Modeled cross-directory and cross-subvolume helpers consume the destination
reference on success and return early on failure; these controls catch an
incorrect unconditional parent release. Their actual namespace algorithms and
failures after ownership transfer are outside this regression.

Windows lookup, permissions, Unicode uppercasing, locking and notifications
are service models. Uppercasing models ASCII only; the UTF-8 encoder covers
the ASCII/BMP fixtures. Cases run in separate processes with a five-second
timeout and core dumps disabled. LeakSanitizer may require running outside a
tracing sandbox. Actual Windows IRP dispatch, kernel resources, FCB/fileref
reaping, persistence, concurrent execution and rename rollback atomicity are
not exercised. Windows runtime testing remains outstanding.
