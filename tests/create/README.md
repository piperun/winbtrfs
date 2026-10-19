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

In the original 80-case matrix, this baseline passes 53/80 cases:
12 cached-entry collision failures and 15 failures because rejected attempts remain marked live. The 12 collision
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

Reproduce the stream collision cleanup errors:

```sh
python3 tests/create/check_create.py . --revision bdc861b6 --build-dir build/create-tests-stream-before
```

In the original 92-case matrix, this baseline passes 77/92 cases. Five collisions return with the base
file's directory lock held, one temporary uppercase buffer still allocated
and a discarded ADS FCB on the dirty queue. Ten injected failures also
leave an unpublished ADS FCB dirty. The production flush path interprets
a dirty deleted ADS FCB as deletion of its named xattr, so a failed attempt
must never enter that queue. The fixed helper queues the FCB only after
insertion succeeds and frees all three temporary entry-name buffers on
collision, releasing the directory lock after referencing the existing
fileref.

The fixture caches a stream with a live fileref and checks the existing
entry, spelling, length, references and FCB cache buckets after duplicate
attempt cleanup. Cases cover sensitive and insensitive names, nonpaged
allocation, a directory base file, different names and lengths, and failure
before the collision recheck. The tests also require successful creates
to remain dirty and reclaim all discarded allocations and resources.
These assertions examine dirty-list eligibility, not actual disk writes.

Reproduce cached-stream reopening failures:

```sh
python3 tests/create/check_create.py . --revision 09bb62c0 --build-dir build/create-tests-reopen-before
```

In the original 112-case matrix, this baseline passes 94/112 cases. Fourteen cached-stream create attempts
terminate with SIGSEGV after the production reaper clears their fileref.
Three direct stream opens leak the xattr data buffer on metadata errors;
a fourth leaks an FCB reference when another fileref wins publication.

The new cases use production `open_fcb_stream` and `open_fileref_child`,
not a mock reopening result. The fixture constructs a real stream FCB,
closes its real fileref and runs production reapers. Warm cases retain an
independent FCB hold; cold cases also evict that FCB. Reopening must return
one referenced fileref with the original cached xattr name, seven bytes of
known stream data and the correct file size. The discarded create FCB
shares that cache key and must be removed before opening the existing
stream, otherwise its empty data can shadow the real stream.

Coverage includes case folding, paging allocation, directory bases,
distinct-name controls, uppercase and allocation failures, xattr read
failure, metadata lookup and validation failures, and deterministic
competing fileref publication. Metadata failures must release the read
buffer; losing publication must release the acquired FCB reference. The
callback models that interleaving and does not run concurrent threads.
Returned references, parent holds, cache buckets, initialized resources
and all tracked allocations must balance after normal reaping.

Reproduce collision forwarding and parent-ownership failures in the caller:

```sh
python3 tests/create/check_create.py . --revision f5c4cc7d --build-dir build/create-tests-dispatch-before
```

This baseline passes 116/135 cases. All 112 earlier helper cases pass.
Eleven dispatch collisions return no existing fileref, including live and
reaped streams, case folding, paging allocation, directory bases and
`:$DATA` suffixes. Eight success/error paths leave parent references
unbalanced: the caller overwrites its original lookup parent, then frees
the stream parent under the original parent's ownership rules.

The harness now runs production `file_create` as well as the creation
helpers. It verifies that collisions return a caller-owned existing
fileref without creating a CCB, assigning a FileObject or setting share
access. Both acquired lookup parents and borrowed related parents are
covered, including relative `:stream` paths. Successful new streams on
existing and newly created bases must also balance references, as must
errors before and after base lookup. Ordinary file creation and collision
are controls. Success cases build real CCBs, then modeled handle closure
releases their storage and fileref holds before production cache reaping.

The original lookup parent stays separate from `create_stream`'s output
parent. The helper returns an extra parent hold on success; the caller
releases that hold once, transfers collision filerefs unchanged, and
cleans up the original lookup parent according to its own ownership.

The harness extracts the actual `file_create2`, `create_stream`, FCB and
fileref constructors, dirty-queue insertion, reference releases, cache
reapers, directory-entry creation and lookup, child fileref opening,
stream FCB reconstruction, FCB cache insertion and software CRC32C.
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
ordinary FCB opening, xattr reads and tree lookups are host models.
Stream FCB reconstruction and fileref publication use production code.
Conversion fixtures use ASCII names. Dirty completion does
not run the flush worker, persistence, or actual transaction rollback.
The test runs file creation dispatch and helpers, modeling returned-reference
releases and handle closure. It does not run `open_file`, `open_file2` or
Windows IRP/handle-close dispatch. ECP discovery is disabled, LXSS detection
is modeled false, and share-access operations are counted host services. Sanitizer results cover host memory, not
Windows pool behavior or concurrency. Windows runtime testing is still
required.

Stream FCB cache matching still uses the ADS hash without the full xattr
name. The missing-base access-denied path also retains a copied filename
buffer. Those pre-existing defects are unchanged.
