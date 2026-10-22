# Changed extent host checks

The runner extracts the production changed-extent get/update/add functions. The
host stubs only the extent-tree lookup, pool allocator, and locks; this isolates
the list/index lookup cost from I/O and kernel scheduling. It is not a driver or
Windows compatibility check.

Generate the fixed rationalization workload and compile an optimized replay:

```sh
export GIT_DIR=/mnt/e/Programming/winbtrfs/.git
python3 tests/changed-extents/check_changed_extents.py . \
  --revision 9da8aa72 --build-dir /tmp/wbr-ce/fork-before --bench
PYTHONPATH=/tmp/wbr-ce/tools python3 tests/changed-extents/bench.py \
  --build-dir /tmp/wbr-ce/fork-before --processes 4 --values 5 \
  --warmups 2 --loops 1 -o /tmp/wbr-ce/fork-before.json
```

The benchmark uses pyperf 2.10.0 (a development-only tool, not required for
ordinary checks). It repeatedly runs the uninstrumented production functions,
reporting mean and standard deviation across worker processes. Only the C replay
interval is timed; trace reading, unrelated-entry setup and cleanup are excluded
on both sides. Provenance records source revisions and frozen trace SHA-256s.
Compare matching JSON files with `python3 -m pyperf compare_to --table`.

Without `--bench`, the runner uses `--init pattern` or `--init zero` and accepts
`--sanitize` for ASan/UBSan. Successful replay requires every production call to
succeed and frees all allocated entries and reference lists.

The source trace generator and rationalization function are both read from
`--trace-revision` (default `9da8aa72`), so later changes do not change the fixture.
The optional private on-disk refcount reader is replaced by a deterministic stub;
its production code and visibility remain unchanged by the lookup optimization.
