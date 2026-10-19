# Volume thread lifecycle regression

Run from the repository root on Linux with Python 3, GCC and pthreads:

```sh
python3 tests/threads/check_threads.py . --build-dir build/thread-tests
python3 tests/threads/check_threads.py . --build-dir build/thread-tests-asan --sanitize
```

The sanitizer run also requires the GCC AddressSanitizer and
UndefinedBehaviorSanitizer runtimes. On systems where LeakSanitizer cannot
inspect traced processes, run outside the tracing sandbox.

To reproduce the failures against the original source:

```sh
python3 tests/threads/check_threads.py . --revision 3fa5cf21 --build-dir build/thread-tests-original
```

The harness extracts the actual lifecycle functions, worker entrypoints,
worker types and mount activation statements. Kernel API mocks use real
pthreads, notification events, blocking jobs and delayed termination. Each
scenario runs in a separate process with a five-second timeout.

The 19 scenarios cover allocation failure, failure at every calculation
thread slot, flush thread creation failure, active and delayed workers,
single-worker startup, repeated cleanup, retry after shutdown, VPB
publication and the flush timer rearm race. The checks require every
created handle to be joined and closed before the worker array is freed,
and preserve the original startup error.

Windows scheduler behavior, IRQL restrictions and real kernel object
lifetimes require Windows mount/unmount and Driver Verifier testing.
