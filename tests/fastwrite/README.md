# Fast write file size regression

Run from the repository root on Linux with Python 3, GCC and pthreads:

```sh
python3 tests/fastwrite/check_fastwrite.py . --build-dir build/fastwrite-tests
python3 tests/fastwrite/check_fastwrite.py . --build-dir build/fastwrite-tests-asan --sanitize
```

The sanitizer run also requires the GCC AddressSanitizer and
UndefinedBehaviorSanitizer runtimes.

To reproduce the failures against the original source:

```sh
python3 tests/fastwrite/check_fastwrite.py . --revision 3fa5cf21 --build-dir build/fastwrite-tests-original
```

The original source passes 4 of the 9 scenarios.

The harness extracts the callbacks the driver registers for FastIoWrite,
PrepareMdlWrite, MdlWriteComplete, FastIoQueryStandardInfo,
AcquireForCcFlush, ReleaseForCcFlush and the lazy-write pair, any static
helpers they call, and write_file and write_file2. ERESOURCE mocks use real
pthreads and hand ownership to waiters on release. FsRtlCopyWrite and
FsRtlPrepareMdlWriteDev follow the ReactOS implementations: a write that
extends the file sets Header.FileSize under the FCB resource and releases the
resource before returning. A successful paging write leaves its page clean
whatever length was written. Each scenario runs in a separate process with a
ten-second timeout.

Four scenarios write past the end of a file while write-behind, CcFlushCache
or a cached write IRP waits for the FCB resource. The writer pauses after
FsRtlCopyWrite releases the resource, until the waiting thread finishes or
blocks. One scenario extends the file through an MDL write. In another, the
file is truncated when a fast overwrite calls FsRtlCopyWrite; the truncation
follows set_end_of_file_information's locking and size updates, and either
order is accepted. Three controls cover overwrites through fast I/O and MDL
writes, and a write-behind pass that runs after the write returns. Every
scenario requires that the file size and inode size match and that every byte
written reaches the disk once no page is dirty. All but the truncation require
a size query after the write to report the new size.

Windows scheduler behavior, the real FsRtl and cache manager routines, and
SMB server MDL writes require Windows testing with Driver Verifier.
