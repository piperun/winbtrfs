# File extent load validation

`check_fcb_load.py` compiles the regular/preallocated extent acceptance branch
from the production `open_fcb` source, along with its local validator, against
small host models. It checks rejection at the load boundary; malformed values
are never passed to read, write, or extent-range operations.

The cases cover zero and overflowing lengths, ranges beyond uncompressed disk
or compressed RAM backing, and overlapping logical ranges. Valid regular,
preallocated, compressed, sparse, adjacent, gapped, and shared-physical extents
remain accepted. Each rejected item must reap its owned FCB and return
`STATUS_INTERNAL_ERROR`.

Run from the fork checkout in WSL:

```sh
python3 tests/fcb-load/check_fcb_load.py . --build-dir /tmp/winbtrfs-fcb-load/extent-pattern --init pattern
```

From a Windows-created worktree in WSL, set `GIT_DIR` to the main repository's
`.git` for a historical `--revision 96fe76d4` run. The historical source passes
8/19 scenarios; the validated source passes 19/19 in pattern and zero
initialization, with and without ASan, UBSan, and leak checks.
