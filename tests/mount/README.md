# Failed-mount cleanup regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/mount/check_mount.py . --build-dir build/mount-tests
python3 tests/mount/check_mount.py . --init zero --build-dir build/mount-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/mount/check_mount.py . --sanitize --build-dir build/mount-tests-asan
```

Add `--revision 96fe76d4` to reproduce the missing metadata cleanup: 76/112
cases pass and all 36 metadata cases fail, because trees, roots, chunks,
system chunks and chunk free-space caches survive device deletion. Add
`--revision 4a99d1c5` to reproduce the missing dummy-FCB cleanup as well. It
passes the 40 earlier controls but fails the 36 dummy-FCB cases because the
dummy FCB, its nonpaged component, resources and allocated name-hash arrays
survive device deletion. The corrected source passes all 112 with pattern/zero
locals and address, undefined-behavior and leak sanitizers.

The original 40-case lookaside-lifetime regression is also retained. Before
that correction, revisions `774da4b3` (inline cleanup) and `0a1d140d`
(unchanged extracted helper) passed 16 of those 40 cases. The other 24
returned an FCB's nonpaged allocation to a deleted lookaside list.

Keep lookaside lists initialized until the cleanup's FCB destruction has
finished, then delete them. Mount's existing flag still prevents deletion
of never-initialized lists. This fixes the premature lookaside deletion
reported in [issue #817, item 5](https://github.com/maharmstone/btrfs/issues/817).
The failure cleanup also reaps the dummy FCB before deleting the lookasides,
using the same destructor as normal volume teardown. Its subvolume must
remain alive until this destruction finishes.

The runner executes the actual failure exit from `mount_vol`, the extracted
helper when present, and production `free_fcb`, `reap_fcb`, `reap_fcbs`,
`free_tree` and `free_trees_root`.
It supports both source layouts. Native services track lookaside lifetime,
allocation ownership, resource release/deletion and the final device-object
deletion. Returning an allocation to a deleted list fails immediately.
Each case must preserve its original error, destroy resources exactly once
and release every allocation owned by its fixture.

The original 40 cases cover failure without a device, early volume initialization,
initialized device lists and empty lookasides, a volume FCB, a provisional
root FCB with or without a subvolume, and an owned-metadata destructor
control. Cases use zero/one/three device-list entries and two distinct
failure statuses. All nine volume resources and eight lookaside deletions
are tracked. FCB fixtures use nonpaged storage and a lookaside-backed
nonpaged component, as mount does. The root has not yet acquired a
fileref or cache-file owner.

The 36 additional cases construct the states after dummy-FCB creation with
zero, one or both name-hash arrays allocated, followed by provisional root-FCB
creation with or without a subvolume and owned metadata. Each runs with
zero/one/three device entries and both failure statuses. All dummy FCBs use
the root-of-roots subvolume and zero hash, as mount initializes them. Tests
require exact destructor and nonpaged-lookaside return counts, no remaining
allocations or live resources, and preservation of the original failure.

The 36 metadata cases construct what mount loads before it creates its FCBs,
in load order. Failure after the system chunks are read leaves the chunk
root and two system chunks, with the tree, chunk and FCB lists and the
lookasides not yet initialized. Failure after the chunk tree is read adds a
cached leaf holding an inserted item and two chunks with free-space,
deleting and device entries and four live resources. Failure after the
roots are read adds three roots with two-level trees in the hash list.
Failure after free-space caches are loaded adds three cache FCBs from the
paged FCB lookaside in the root tree's subvolume: one attached to a chunk,
one released but still listed for reaping, and one shared by two chunks
whose free-space headers name the same inode, as `open_fcb` returns an
already open FCB with another reference. The next stage also has the volume,
dummy and root FCBs, with the dummy FCB's subvolume the loaded root tree and
the root FCB's the loaded default subvolume. The last stage fails after the
root stream file was created: the root FCB is linked into its subvolume and
the FCB list with its fileref, and the stream file still points at it after
mount drops its reference, so the FCB, its nonpaged part and the fileref
must survive the cleanup. Tests require every tree item back on its
lookaside before deletion, each FCB destroyed exactly once, exact FCB
counts, and no other remaining allocations or live resources.

Fixtures construct partial-mount state directly. They do not execute disk
discovery, full mount initialization, root cache-file close, concurrent
kernel activity or Windows pools. The root-fileref branch is not exercised.
Still leaking on some failure paths: batch entries left when
`commit_batch_list` fails; FCBs marked dirty and changed-extent records
created while clearing the free-space cache; the root fileref and root FCB
on the root-fileref branch; the notify synchronization object set up with
`FsRtlNotifyInitializeSync`; and device free-space lists after a failure in
or after `find_disk_holes`. The first device is allocated without zeroing,
so the cleanup cannot tell whether its free-space list was initialized. No
complete mount-failure cleanup or #832 hang fix is claimed.

Each case runs separately with a five-second timeout and core dumps disabled.
Windows runtime validation remains outstanding. LeakSanitizer may require
execution outside a tracing sandbox.
