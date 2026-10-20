# Failed-mount lookaside lifetime regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/mount/check_mount.py . --build-dir build/mount-tests
python3 tests/mount/check_mount.py . --init zero --build-dir build/mount-tests-zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/mount/check_mount.py . --sanitize --build-dir build/mount-tests-asan
```

Add `--revision 774da4b3` to reproduce the original inline mount failure
cleanup, or `--revision 0a1d140d` for the unchanged extracted helper. Both
pass 16/40 cases. The other 24 fail when production `reap_fcb` returns its
nonpaged allocation to a deleted lookaside list. The fixed source passes
all 40 with pattern/zero locals and address, undefined-behavior and leak
sanitizers.

Keep lookaside lists initialized until the cleanup's FCB destruction has
finished, then delete them. Mount's existing flag still prevents deletion
of never-initialized lists. This fixes the premature lookaside deletion
reported in [issue #817, item 5](https://github.com/maharmstone/btrfs/issues/817).

The runner executes the actual failure exit from `mount_vol`, the extracted
helper when present, and production `free_fcb` and `reap_fcb`.
It supports both source layouts. Native services track lookaside lifetime,
allocation ownership, resource release/deletion and the final device-object
deletion. Returning an allocation to a deleted list fails immediately.
Each case must preserve its original error, destroy resources exactly once
and release every allocation owned by its fixture.

The 40 cases cover failure without a device, early volume initialization,
initialized device lists and empty lookasides, a volume FCB, a provisional
root FCB with or without a subvolume, and an owned-metadata destructor
control. Cases use zero/one/three device-list entries and two distinct
failure statuses. All nine volume resources and eight lookaside deletions
are tracked. FCB fixtures use nonpaged storage and a lookaside-backed
nonpaged component, as mount does. The root has not yet acquired a
fileref or cache-file owner.

Fixtures construct partial-mount state directly. They do not execute disk
discovery, full mount initialization, root cache-file close, concurrent
kernel activity or Windows pools. The root-file and root-fileref cleanup
branches are not exercised. Other #817 mount-unwind leaks (roots, trees,
chunks, system chunks, dummy FCBs and batch entries) remain separate work.
No complete mount-failure cleanup or #832 hang fix is claimed.

Each case runs separately with a five-second timeout and core dumps disabled.
Windows runtime validation remains outstanding. LeakSanitizer may require
execution outside a tracing sandbox.
