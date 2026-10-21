# Free-space list merge regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/free-space/check_free_space.py . --build-dir /tmp/winbtrfs-free-space/fixed
python3 tests/free-space/check_free_space.py . --init zero --build-dir /tmp/winbtrfs-free-space/zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/free-space/check_free_space.py . --sanitize --build-dir /tmp/winbtrfs-free-space/asan
python3 tests/free-space/check_free_space.py . --bench --build-dir /tmp/winbtrfs-free-space/bench
```

Add `--revision 96fe76d4` to run the merge that searched from the head of the
list for every freed range: 25/29 cases pass. The four step-count cases that
merge many ranges fail; the large merge takes 400,020,000 address-list steps
against a limit of 160,000.

The runner extracts the production `space` type, `order_space_entry`,
`space_list_add2` and `space_list_merge`, plus the `place_space_entry`,
`space_list_add_from` and `order_space_entries` helpers when present. The host
models are:

- a pool allocator that can fail any chosen allocation;
- `ExFreePool` that poisons a freed entry, which then reads as an empty range at
  address 0 with poisoned links, and keeps it unused until the trial ends; it
  fails if the entry was written after it was freed. Sanitizer builds free the
  memory at once and let AddressSanitizer check later use;
- list primitives with the integrity checks of the Windows ones: the links of
  the head or entry and of its neighbours must agree, and none may be poisoned;
- list heads inside a record that covers every address, so code that takes a
  head for an entry absorbs ranges into it and the result check fails.

Random trials with fixed seeds build a valid space list and a sorted, coalesced
list of freed ranges, then merge them with and without a size list. Each freed
range misses, touches, bridges, overlaps, contains, equals or lies inside the
existing entries, or joins the entry the previous range merged into. Every case
must cover each of these relationships. After the merge:

- the address list equals the union of the half-open ranges, sorted, with a gap
  between neighbouring entries;
- the size list holds exactly the address-list entries, in non-increasing size
  order, and both lists have consistent forward and backward links;
- one entry was allocated for each freed range that joined no existing range;
- the freed-range list is unchanged and no allocation leaks.

Further cases merge freed ranges given out of address order, fail each
allocation of a merge in turn, do both together, and add ranges one at a time
through `space_list_add2`. After a failed allocation the merge must return
`STATUS_INSUFFICIENT_RESOURCES`, allocate nothing more, and leave both lists
consistent and holding exactly the ranges merged before the failure.

When the helpers are present, the runner also adds calls into the test where
each insert starts, where an entry takes a range, where entries are resized or
freed, where the merge restarts from the head and where an allocation fails.
Each insert must start at the entry that took the previous range, or at the
first entry for the first range and after a restart. Sorted input must never
restart. Each kind of case must also show the paths it exists for: inserts
starting after the first entry, restarts for out-of-order ranges, entries freed
after taking the previous range, entries on the list of changed entries resized
again or freed, and failed allocations while that list is not empty.

The large step-count cases merge 20,000 freed ranges interleaved with 20,000
separate entries, with and without a size list. The small ones merge 1, 16 or
64 freed ranges into random gaps between 20,000 entries with a size list; every
second range extends the entry before its gap. The runner adds a counter to each
list-cursor step (`le = le->Flink`) in the extracted functions. Address-list
steps must stay within 4(n + m), and size-list steps within 2(n + m⌈log2 m⌉).

`--bench` builds the functions without counters or calls into the test, with
`-O2`, and prints `CLOCK_MONOTONIC` timings of a merge: the best of five for
m = 5,000 to 40,000 freed ranges between as many entries, and the best of twenty
for m = 1 to 64 into random gaps between 33,015 entries. The timings illustrate
the change; the step counts are the regression check.

Each case runs in its own process with core dumps disabled, a 30-second limit,
or 120 seconds for the large step-count cases. Rollback lists, chunk locking
and real Windows pool allocation are not exercised.
