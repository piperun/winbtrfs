# Same-directory rename parent-reference regression

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/rename/check_rename.py . --build-dir build/rename-tests
python3 tests/rename/check_rename.py . --init zero --build-dir build/rename-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/rename/check_rename.py . --sanitize --build-dir build/rename-tests-asan
```

Add `--revision e56845d0` to reproduce before the fix. That source passes
44/66 cases; 22 successful same-directory cases retain the dispatcher's extra
parent reference. The fixed source passes all 66. The leak is reported in
[upstream issue #817](https://github.com/maharmstone/btrfs/issues/817).

The runner extracts production `set_rename_information`,
`rename_in_same_directory`, `free_fileref`, filename validation and software
CRC32C. Both the original `fileinfo.c` layout and the extracted `rename.c`
layout are supported. The same-directory helper executes its actual name
updates, metadata changes, allocation cleanup and notification calls.

Both legacy and extended requests cover relative names, target-directory
handles, parent lookup, files, directories, renaming to self, replacement,
long and Unicode names, collisions, access failures and open children.
Seven allocation failures and four filename-query failures cover the request
buffer and every allocation in the same-directory helper. Parent lookup errors
and stream dispatch are controls. Repeated-renames cases execute 100 requests
on the same fileref and parent. Two more cases close the child and require
the parent's actual reference count to reach zero.

Every request checks source/destination/collision reference counts, balanced
resources and subject contexts, and rollback selection. Successful same-directory
requests also verify the changed name, inode metadata and notifications.
Modeled cross-directory and cross-subvolume helpers consume the destination
reference on success and return early on failure; these controls catch an
incorrect unconditional parent release. Their actual namespace algorithms and
failures after ownership transfer are outside this regression.

Known separate leak: dispatch allocates a temporary UTF-8 name that it does
not release. The fixture records that allocation and explicitly frees it after
each request, separately from the fileref's owned names. This suite therefore
does not claim production pool ownership is balanced; it checks the requested
parent-reference fix without hiding or changing the existing UTF-8 leak.

Windows lookup, permissions, Unicode uppercasing, locking and notifications
are service models. Uppercasing models ASCII only; the UTF-8 encoder covers
the ASCII/BMP fixtures. Cases run in separate processes with a five-second
timeout and core dumps disabled. LeakSanitizer may require running outside a
tracing sandbox. Actual Windows IRP dispatch, kernel resources, FCB/fileref
reaping, persistence, concurrent execution and rename rollback atomicity are
not exercised. Windows runtime testing remains outstanding.
