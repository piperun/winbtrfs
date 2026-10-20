# EA packing regression

Run from the repository root on Linux with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/ea/check_ea.py . --build-dir build/ea-tests
python3 tests/ea/check_ea.py . --init zero --build-dir build/ea-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/ea/check_ea.py . --sanitize --build-dir build/ea-tests-asan
```

Reproduce the overlap before the fix:

```sh
python3 tests/ea/check_ea.py . --revision 23ebf477 --build-dir build/ea-tests-before
```

The baseline passes 50/64 cases. Fourteen fail because an outgoing
`NextEntryOffset` omits the name's terminating NUL before four-byte alignment.
An eight-byte header, one-byte name, NUL and three-byte value occupy
13 bytes, requiring a 16-byte next offset. The old offset is 12 and the
following header overwrites the last value byte. An empty value can instead
lose its name terminator. Including the NUL matches the existing allocation
size calculation; the last record still has a zero next offset.

The harness extracts the unchanged production `file_create_parse_ea`
function and EA list types/constants from the selected source revision.
It calls that function directly with valid, independently packed inputs.
The Windows EA header model asserts an eight-byte name offset, matching
`FILE_FULL_EA_INFORMATION` in the Windows headers. Native pool allocation
is tracked, filled with a nonzero pattern, and individually fault-injected;
the input buffer remains caller-owned.

The 64 cases cover all four alignment residues across name/value lengths,
empty and binary values, singletons, two/three/sixteen-record chains,
255-byte names, larger values, case-insensitive duplicate names with growing
and shrinking values, duplicate flags and stable order, LXSS removal and
LXSS-only input, and invalid short LXSS values. Eleven allocation failures
exercise every allocation in three-entry, duplicate and LXSS-filtered
lists, including final output allocation after temporary entries are removed.

Checks follow the actual published offsets and require four-byte alignment,
nonoverlap, complete names and terminators, exact binary payloads and flags,
the final zero offset, exact allocation/EA lengths, and balanced ownership.
Failures must free every temporary entry and publish no EA buffer or changed
flag; successes leave only the FCB-owned output buffer, which the fixture
then frees. Input headers and values must remain intact.

Each scenario runs in a separate process with a five-second timeout and
core dumps disabled. ASan/LeakSanitizer/UBSan check native memory; LeakSanitizer
may require execution outside a tracing sandbox. The uppercase service models
ASCII only. This suite does not run Windows IRPs, `IoCheckEaBufferValidity`,
kernel pools, FCB destruction, persistence or concurrent execution. Oversized
EA-list rejection and aggregate-length overflow are outside this regression.
Windows runtime testing remains necessary.
