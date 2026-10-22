# Changed extent host checks

These tests compile the production exact lookup and AVL lifecycle helpers with
deterministic allocator, lock and extent-tree service models. Differential checks
compare production update/add functions with the original list implementation,
including statuses, allocation ordinals, entry identity, list/reference/rollback
order, field values, and index membership/parent/height/balance after failures.

Run the candidate checked out in this repository:

```sh
export GIT_DIR=/mnt/e/Programming/winbtrfs/.git
python3 tests/changed-extents/check_changed_extents.py . \
  --build-dir /tmp/wbr-ce/fork-check --init pattern
python3 tests/changed-extents/check_changed_extents.py . \
  --build-dir /tmp/wbr-ce/fork-check --init zero
python3 tests/changed-extents/check_changed_extents.py . \
  --build-dir /tmp/wbr-ce/fork-check --init pattern --sanitize
```

Coverage includes a logarithmic lookup bound, reverse-ordered keys, 256 equal-key
root removals, 48,000 randomized insertion/removal/same-chunk/cross-chunk movement
operations, 8,192 randomized production update/add comparisons, 280 exhaustive
allocation/stub-failure cases, and long reference lists. Twelve comparisons run
the actual production changed-reference rollback case and per-extent flush
function, including early failures and terminal cleanup errors. Where available,
the actual mount-drain block verifies an empty list and NULL root without extra
allocations. Host tests cannot exercise kernel synchronization or pool pressure.

## Statistical comparison

The list baseline is `9da8aa72`; select the candidate by its exact checked-out
commit. The optimized benchmark needs pyperf 2.10.0, a development-only tool.
Ordinary checks do not need it. The examples use the existing task-owned tool
installation; install a verified release separately if it is absent.

```sh
candidate=$(git rev-parse HEAD)
python3 tests/changed-extents/check_changed_extents.py . \
  --revision 9da8aa72 --build-dir /tmp/wbr-ce/fork-before --bench
python3 tests/changed-extents/check_changed_extents.py . \
  --revision "$candidate" --build-dir /tmp/wbr-ce/fork-after --bench
PYTHONPATH=/tmp/wbr-ce/tools python3 tests/changed-extents/bench.py \
  --build-dir /tmp/wbr-ce/fork-after --baseline-dir /tmp/wbr-ce/fork-before \
  --processes 4 --values 5 --warmups 2 --loops 1 \
  -o /tmp/wbr-ce/fork-paired.json
PYTHONPATH=/tmp/wbr-ce/tools python3 -m pyperf show /tmp/wbr-ce/fork-paired.json
```

Before/after cases interleave in one suite with 20 measured values each. pyperf
reports mean, standard deviation and stability warnings. Only the uninstrumented
C replay interval is timed; reading, prefill allocation and final cleanup are
excluded on both sides. Counters belong to separate correctness builds.

`trace.c` and `trace-host.h` retain only the original deterministic VM layout and
required service models. They are self-contained and do not import another test
suite. `--trace-revision` selects the production rationalization function (default
`9da8aa72`). Each build regenerates both 40,000/87,000-extent traces; the unused
timing header is canonicalized. Provenance records the source, type, fixture and
trace hashes. Paired measurement refuses unequal trace hashes. `--compare`
selects the original production reference implementation for differential tests.

The intrusive node costs 32 bytes per changed extent on the exercised 64-bit host
layout (96 to 128 bytes), plus one pointer per chunk. It adds no allocation calls;
larger pool allocations can still fail under different physical memory pressure.

Keep task output under the declared 32 MiB scratch budget; reuse build paths
between local-initialization and sanitizer runs. No build outputs belong in Git.
These checks do not establish Windows linking or compatibility. A supported WDK
build, including an XP-compatible x86 target, and disposable Windows VM write,
flush and balance checks remain necessary before claiming native correctness or
reduced live stalls. Do not install or unload the host driver for this benchmark.
