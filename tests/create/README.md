# File and stream creation reference regression

Run from the repository root on Linux with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/create/check_create.py . --build-dir build/create-tests
python3 tests/create/check_create.py . --debug-refs --build-dir build/create-tests-debug
python3 tests/create/check_create.py . --init zero --build-dir build/create-tests-zero
python3 tests/create/check_create.py . --sanitize --build-dir build/create-tests-asan
```

The compiler uses `-fshort-wchar` for the production UTF-16 string literals.
The sanitizer run needs GCC's ASan and UBSan runtimes; LeakSanitizer may
require execution outside a tracing sandbox. Each case runs in a separate
process with a five-second timeout and core dumps disabled.

Reproduce the parent FCB leaks before the fix:

```sh
python3 tests/create/check_create.py . --revision 4260d931 --build-dir build/create-tests-before
python3 tests/create/check_create.py . --revision 4260d931 --debug-refs --build-dir build/create-tests-before-debug
```

These baselines reproduce 40 parent-reference failures after the child
has been closed and reaped. Older revisions also fail the mixed-case
collision checks below.
The fixture checks both parents when stream creation first creates its
missing base file. That path exercises both reference acquisitions.

Reproduce the mixed-case collision bug after the parent-reference fix:

```sh
python3 tests/create/check_create.py . --revision f87278b4 --build-dir build/create-tests-case-before
```

In the original 64-case matrix, this baseline passes 58/64 cases. The
expanded matrix also exercises the cached-entry gap below. Six insensitive
rechecks create a new entry instead of returning `STATUS_OBJECT_NAME_COLLISION` and the existing
fileref. The cases cover lowercase and mixed-case cached names, different
request capitalization, directory creation and paging allocation.

The fixed recheck compares the cached uppercase name length and buffer
with the uppercase request. Controls cover uppercase cached names,
case-sensitive distinct names, genuinely distinct insensitive names,
uppercase conversion failure and two real CRC32C collisions. The hash fixtures
`572BSHK`/`AFNB7ZH` have equal lengths and hash `0xba8c7cc2`;
`6KKS16`/`IM921VL31U` have different lengths and hash `0xc84cda84`.
Hashes use the driver's seed `0xffffffff` over UTF-16LE uppercase names.
Equal hashes must not cause different names to collide. Collision returns
must preserve the existing entry and restore parent directory size while
reclaiming the attempted child through the normal reference cleanup.

Reproduce the cached-entry collision gap after the uppercase fix:

```sh
python3 tests/create/check_create.py . --revision e0435e1c --build-dir build/create-tests-cached-before
```

This baseline passes 53/80 cases: 12 cached-entry collision failures and
15 failures because rejected attempts remain marked live. The 12 collision
cases fail after production `reap_filerefs` clears the existing entry's fileref. Creation must still
reject that cached name and return a referenced fileref for the existing
inode. The fixture keeps an independent FCB hold, as another hardlink can.
The fixed path uses production `find_file_in_dir` and `open_fileref_child`;
it does not manufacture the returned fileref in a mock.

The cases cover sensitive and insensitive names, directory/file type
mismatches, paging allocation, preallocation, FCB-open failure, fileref
allocation failure and uppercase conversion failure during reopening.
A deterministic callback installs another fileref during FCB opening to
exercise the child-open helper's existing revalidation and loser cleanup;
this models that interleaving, not actual concurrent execution. Controls
allow distinct names and both real hash collisions after reaping. Failed
reopens must return no fileref and must never attempt entry insertion.
Rejected ordinary creations must also leave their dirty attempted inode
marked deleted, so the flush worker cannot persist an unlinked live inode.

The harness extracts the actual `file_create2`, `create_stream`, FCB and
fileref constructors, dirty-queue insertion, reference releases, cache
reapers, directory-entry creation and lookup, child fileref opening, FCB
cache insertion and software CRC32C.
It checks successful file/directory/stream creation, nonpaged allocation,
preallocation, case folding, existing-file collision, and injected errors
in conversion, allocation, security, EA parsing, extension and metadata
lookup. Stream creation on an existing base and a newly created base are
both covered. `DEBUG_FCB_REFCOUNTS` exercises the alternate acquisition
and release code as well.

The caller closes the returned references, then modeled dirty-queue
completion releases its legitimate holds. Production reapers must return
both parent counts and FCB cache buckets to their initial values, return
every child lookaside allocation and destroy each initialized resource
exactly once. Namespace entries remain cached after child eviction; their
fixture teardown is separate from reference and child-ownership checks.

Windows locks, lookaside storage, security assignment, access checks,
name conversion, EA parsing, file extension, stream base lookup and
disk/FCB opening are host models. Conversion fixtures use ASCII names. Dirty completion does
not run the flush worker, persistence, or actual transaction rollback.
The test calls the creation helpers directly and models their callers'
returned-reference releases; it does not drive the outer create dispatch
or Windows handle-close code. Sanitizer results cover host memory, not
Windows pool behavior or concurrency. Windows runtime testing is still
required.

The stream duplicate-entry collision cleanup path has a separate
pre-existing defect and is outside this matrix. The missing-base access-denied path
also retains a name buffer; that separate cleanup defect is unchanged.
