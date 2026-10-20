# TRIM allocation-failure regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/trim/check_trim.py . --build-dir build/trim-tests
python3 tests/trim/check_trim.py . --init zero --build-dir build/trim-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/trim/check_trim.py . --sanitize --build-dir build/trim-tests-asan
```

Add `--revision 4a8bf1a9` to reproduce before the fix. The baseline passes
21/167 cases. All 146 failures reach an unsignaled wait after every submitted
IRP has completed: devices skipped after DSM-buffer or IRP allocation failure
still own a pending slot. A single failing device can leave no submitted I/O
and no possible future completion to wake the flush thread. The fixed source
passes all 167 cases with pattern/zero locals and sanitizers. This addresses
the production TRIM hang in [upstream issue #815](https://github.com/maharmstone/btrfs/issues/815).

The runner extracts production `clean_space_cache`, TRIM context types,
`trim_completion`, and the extracted `trim_devices` helper when present.
Historical sources with all submission code in `flushthread.c` remain supported.
Allocation and I/O services are deterministic native models; submission and
completion accounting execute the actual production code.

The 156-case matrix covers all combinations of successful allocation,
DSM-buffer failure and IRP allocation failure for one or three devices, plus
mixed eligible, read-only, missing, unsupported and empty devices. Each runs
with inline completions, deferred FIFO completions, deferred reverse completions
and mixed inline/deferred completions. Eleven controls cover no work, disabled
TRIM, the no-TRIM mount option, context-array allocation failure, multiple
ranges, failed/cancelled device requests and successful space merging before I/O.

The wait model executes every deferred production completion, then requires
the event to be signaled with no remaining pending slots. If all possible
completions have run without signaling the event, it reports the deterministic
hang immediately. It does not add a timeout to production code. Event signaling
must not happen while requests remain pending or eligible devices remain
unvisited. Pool buffers and IRPs must stay alive through completion and be
released exactly once afterward. Skipped/disabled devices retain or discard
their queues according to the existing best-effort TRIM behavior.

Checks verify request flags, completion registration on success/error/cancel,
input lengths, aligned DSM ranges and their exact 64-bit offsets and lengths.
Layout assertions match the installed MinGW Windows DSM headers. Resource
ownership, device queue counts, pool ownership, IRP ownership and the volume's
writable state are checked after each call. Fixture teardown releases only
the queues that production intentionally retained.

Each case runs in its own process with a five-second timeout and core dumps
disabled. LeakSanitizer may require execution outside a tracing sandbox.
These tests do not run real storage drivers, Windows scheduling, kernel pools
or concurrent kernel threads. Chunk-to-device range mapping and space merging
are modeled. Debug TRIM emulation is moved unchanged and syntax-checked only;
its separate write/MDL path is outside these production-TRIM tests. Windows
runtime testing remains outstanding.
