# EA size, packing and failed-update regressions

Run from the repository root on Linux with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/ea/check_ea.py . --build-dir build/ea-tests
python3 tests/ea/check_ea.py . --init zero --build-dir build/ea-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/ea/check_ea.py . --sanitize --build-dir build/ea-tests-asan
```

Run the same cases against the source before the size and update fixes:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/ea/check_ea.py . --revision 2a1fbbff --sanitize --build-dir build/ea-tests-before
```

The sanitized baseline passes 92/117 cases; the fixed source passes all 117.
The failures reproduce oversized lists being accepted, 16-bit aggregate-size
wraparound, unaligned LXSS value reads, overlapping packed records in the set
path, and inode changes surviving a failed update. Size checks apply after
duplicate replacement and LXSS filtering, so large requests can still succeed
when their final stored attributes fit.

The harness extracts the production `file_create_parse_ea` and `drv_set_ea`
functions and EA list types/constants from the selected source revision.
It calls those functions directly with valid, independently packed inputs.
The Windows EA header model asserts an eight-byte name offset, matching
`FILE_FULL_EA_INFORMATION` in the Windows headers. Native pool allocation
is tracked, filled with a nonzero pattern, and individually fault-injected;
the input buffer remains caller-owned.

The original 64 packing cases cover all four alignment residues across name/value lengths,
empty and binary values, singletons, two/three/sixteen-record chains,
255-byte names, larger values, case-insensitive duplicate names with growing
and shrinking values, duplicate flags and stable order, LXSS removal and
LXSS-only input, and invalid short LXSS values. Eleven allocation failures
exercise every allocation in three-entry, duplicate and LXSS-filtered
lists, including final output allocation after temporary entries are removed.

The additional 53 cases cover sizes below, at and above the stored-attribute
limit for 4 KiB, 16 KiB and 64 KiB nodes; alignment at the limit; aggregate
lengths crossing 65535; and large requests reduced by replacement or filtering.
Update cases cover each allocation in old-list copying and new-list creation,
final allocation failures, deletion, replacement and metadata-only updates.
UID, GID and mode changes must all wait until validation and allocation succeed.
Invalid later metadata, a size error or a failed allocation must preserve the
old EA buffer's contents, pointer and lengths, the complete inode item and its
dirty flags. Failed updates must not mark the FCB dirty or notify observers.

Checks follow the actual published offsets and require four-byte alignment,
nonoverlap, complete names and terminators, exact binary payloads and flags,
the final zero offset, exact allocation/EA lengths, and balanced ownership.
Failures must free every temporary entry without publishing a new EA buffer
or changed flag; successes leave only the FCB-owned output buffer, which the
fixture then frees. The caller owns the input buffer; production functions
normalize its names in place. Lock ownership, filesystem entry/exit and IRP
completion are checked on the update path.

Each scenario runs in a separate process with a five-second timeout and
core dumps disabled. ASan/LeakSanitizer/UBSan check native memory; LeakSanitizer
may require execution outside a tracing sandbox. The uppercase service models
ASCII only. The IRP and `IoCheckEaBufferValidity` services are native models;
the fixtures do not test the Windows implementations. This suite does not run
kernel pools, FCB destruction, persistence or concurrent execution. Windows
runtime testing remains necessary.
