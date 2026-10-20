# Failed-mount FCB cleanup regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/mount/check_mount.py . --build-dir build/mount-tests
python3 tests/mount/check_mount.py . --init zero --build-dir build/mount-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/mount/check_mount.py . --sanitize --build-dir build/mount-tests-asan
```

Add `--revision 4a99d1c5` to reproduce the missing dummy-FCB cleanup. It
passes the 40 earlier controls but fails all 36 new cases because the dummy
FCB, its nonpaged component, resources and allocated name-hash arrays survive
device deletion. The corrected source passes all 76 with pattern/zero locals
and address, undefined-behavior and leak sanitizers.

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
helper when present, and production `free_fcb` and `reap_fcb`.
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

Fixtures construct partial-mount state directly. They do not execute disk
discovery, full mount initialization, root cache-file close, concurrent
kernel activity or Windows pools. The root-file and root-fileref cleanup
branches are not exercised. Other #817 mount-unwind leaks (roots, trees,
chunks, system chunks and batch entries) remain separate work.
No complete mount-failure cleanup or #832 hang fix is claimed.

Each case runs separately with a five-second timeout and core dumps disabled.
Windows runtime validation remains outstanding. LeakSanitizer may require
execution outside a tracing sandbox.
