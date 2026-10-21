# Free-space list, loader and cache-writer regressions

Run from the repository root with Python 3 and GCC supporting
`-ftrivial-auto-var-init`:

```sh
python3 tests/free-space/check_free_space.py . --build-dir /tmp/winbtrfs-free-space/fixed
python3 tests/free-space/check_free_space.py . --init zero --build-dir /tmp/winbtrfs-free-space/zero
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/free-space/check_free_space.py . --sanitize --build-dir /tmp/winbtrfs-free-space/asan
python3 tests/free-space/check_free_space.py . --bench --build-dir /tmp/winbtrfs-free-space/bench
```

Add `--revision` to run the sources before each fix. Each revision also fails
the cases of the fixes after it.

- `96fe76d4` merges by searching from the head of the list for every freed
  range: 29/55 cases pass. The four step-count cases that merge many ranges
  fail; the large merge takes 400,020,000 address-list steps against a limit
  of 160,000.
- `23c741d8` runs `update_chunk_cache` before it freed the lists it copies:
  33/55 cases pass. A written cache leaves 8 allocations behind, the 5 merged
  entries and the 3 copied freed ranges, and so does a failed flush. Failing an
  allocation while copying either list leaves the entries copied before it, and
  failing one in the merge leaves both copies.
- `5c9895e7` runs `update_chunk_cache_tree` before it freed its copy of the
  space list on failure: 36/55 cases pass. Failing the second or third
  allocation, in the copy, leaves 1 or 2 entries behind, and failing the fourth
  or fifth, in the merge, leaves 3 or 4.
- `550f234e` runs `add_space_entry` before it linked an entry at the last
  entry's address into the address list: 37/55 cases pass. With a size list the
  entry is only on the size list, which still offers its range after an
  allocation took it; without one the entry is on neither list.
- `03af14bb` runs `load_stored_free_space_tree` before it freed what it had
  loaded when it rejects a bitmap item: 43/55 cases pass. The entries of the
  first bitmap stay in front of the free space regenerated from the extent tree:
  the address list holds 6 entries instead of 3, each loaded entry overlapping a
  regenerated one, and the bitmap buffer stays allocated.
- `872b041a` keeps overlapping stored ranges as separate entries: 45/55 cases
  pass. Two entries cover the same space, and after an allocation inside the
  overlap the second one still offers it. The v1 cache whose entries add up to
  the chunk's free space despite the overlap is kept, not discarded.
- `469ce25c` returns errors from loading a chunk's free space with the entries
  it loaded still on the chunk's lists: 49/55 cases pass. Failing an allocation
  in the free space tree or v1 cache loader, or in taking the superblock stripe
  out of the loaded entries, leaves the entries loaded before it, and so does
  failing to delete a discarded v1 cache. The next load adds the same entries
  again, so a stored tree or v1 cache is rejected as overlapping, the free
  space is regenerated from the extent tree and the v1 cache deleted. Free
  space regenerated from the extent tree is appended to the old entries
  without a check, so the list holds each free range twice; after a failed
  `protect_superblocks` one copy still covers the superblock stripe, and the
  chunk keeps the `space_changed` mark the failed load set.

The runner extracts the production `space` type, `order_space_entry`,
`space_list_add2`, `space_list_merge`, `add_space_entry` and
`space_list_subtract2`, plus the `place_space_entry`, `space_list_add_from` and
`order_space_entries` helpers when present. The host models are:

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

The loaders add the entries of a stored cache with `add_space_entry`. Its trials
add up to 40 entries, with and without a size list, at addresses drawn from 12,
so many entries share an address. After each add the address list must hold
exactly the added ranges in non-decreasing address order, and the size list the
same entries in non-increasing size order. The trials must add into an empty
list, after the last entry, at the last entry's address, at another entry's
address and below the last entry's address. Two more cases add an entry at the
last entry's address and take its range with `space_list_subtract2`, as an
allocation does; neither list may offer any of that range afterwards.

The large step-count cases merge 20,000 freed ranges interleaved with 20,000
separate entries, with and without a size list. The small ones merge 1, 16 or
64 freed ranges into random gaps between 20,000 entries with a size list; every
second range extends the entry before its gap. The runner adds a counter to each
list-cursor step (`le = le->Flink`) in the extracted functions. Address-list
steps must stay within 4(n + m), and size-list steps within 2(n + m⌈log2 m⌉).

`cache_test` runs the production `update_chunk_cache`, which writes a chunk's
v1 free-space cache, and `update_chunk_cache_tree`, which writes its entries in
the free space tree, with `copy_space_list` and the merge functions without
counters or calls into the test. It uses the same list primitives, and a pool
allocator that can fail any chosen allocation and frees memory at once. The
free-space header item, an empty free space tree, `flush_fcb`,
`do_write_file`, the checksum and `update_free_space_info` are native models.
The chunk has three free entries and three freed ranges; one range extends an
entry and two join nothing, so the merged list has five entries.

- A successful `update_chunk_cache` must set five entries in the header and
  write them in address order after the two sector checksums and the cache
  generation. A successful `update_chunk_cache_tree` must insert the five
  entries as extent items in address order and record five extents.
- Failing each allocation in turn, nine for the cache and five for the tree,
  must return `STATUS_INSUFFICIENT_RESOURCES` and write nothing. A failed
  `flush_fcb` must return its status.
- In every case the chunk's lists must be unchanged, and nothing else may stay
  allocated. A case reports each step that leaves allocations behind before it
  fails.

`load_test` runs the production `load_free_space_cache`, which loads a chunk's
free space with `load_stored_free_space_tree` from the free space tree or with
`load_stored_free_space_cache` from a v1 cache, and otherwise regenerates it
from the extent tree, with the plain `add_space_entry`. Some cases run it
through the production `load_cache_chunk`, which then takes the superblock
stripes out of the loaded entries with `protect_superblocks`. It uses the same
list primitives and a pool allocator that can fail any chosen allocation and
frees memory at once. The trees are native models searched as the production
`find_item` and `find_next_item` search them. The v1 cache file, its header
item and the calls that discard a cache are native models too; deleting the
header item takes it out of the root tree, and deleting it or excising the
cache's extents can be made to fail. The checksum is not under test.
`protect_superblocks` and `get_superblock_size` are native models of a chunk
with no superblock copy, or with one whose 64 KiB stripe lies in the last free
range: `protect_superblocks` takes the stripe off the chunk's lists with the
production `space_list_subtract`, and `get_superblock_size` allocates an entry
while it counts the stripe, as the production function does. A case can make
`protect_superblocks` first mark the chunk `changed`, as a nocow write can at
the same time without the chunk lock. The extent tree
holds the 128 MiB chunk's block group item, two data extents and a tree block,
between items of the chunks before and after it, and leaves three ranges of the
chunk free. The free space tree describes the chunk in extent items or in 16
bitmap items of 2,048 sectors.

- With valid bitmap items, with extent items, or with a v1 cache, each giving the
  last free range in pieces that touch, the address list must hold the three
  free ranges in address order and the size list the same entries in
  non-increasing size order. The extent tree must not be searched, and the
  cache must be kept.
- A second bitmap item shorter than its range needs, or an empty one, makes the
  loader reject the free space tree. The lists must then hold the three ranges
  regenerated from the extent tree, and nothing else may stay allocated.
- Overlapping stored ranges make the loaders reject what they loaded: an extent
  item inside another, two extent items with the same start, an extent item
  over free space a bitmap item also gives, and a v1 entry inside another with
  the last entry short by as much, so the entries still add up to the chunk's
  free space. The lists must hold the regenerated ranges, and the v1 cache must
  be discarded. After an allocation inside the overlap, the size list may offer
  none of it.
- `load_cache_chunk` loads a chunk with the superblock stripe from the 16 bitmap
  items; from extent items over the first bitmap's span and bitmap items for
  the rest, so the bitmap buffer is allocated after entries were loaded; from a
  v1 cache that leaves out the stripe; and from the extent tree. The last
  allocation of each load takes the stripe out of the loaded entries, or for the
  v1 cache counts it. Failing each allocation in turn must return
  `STATUS_INSUFFICIENT_RESOURCES` and leave the chunk as it found it: no
  entries, nothing else allocated, and `space_changed` clear. Loading it again
  must give the free ranges less the stripe, from the same source as a load
  that failed nothing, keep the v1 cache, and leave `space_changed` set, as
  `protect_superblocks` sets it.
- A v1 cache whose entries add up to less than the chunk's free space is
  discarded after its entries are loaded. If deleting its header item or
  excising its extents fails, the load must return the failure and leave the
  chunk as it found it, and the next load must regenerate the free ranges from
  the extent tree.
- A load that fails in `protect_superblocks` must keep the `space_changed` mark
  of a chunk marked before the load, and the `changed` mark of a nocow write
  while `protect_superblocks` runs.

`--bench` builds the functions without counters or calls into the test, with
`-O2`, and prints `CLOCK_MONOTONIC` timings of a merge: the best of five for
m = 5,000 to 40,000 freed ranges between as many entries, and the best of twenty
for m = 1 to 64 into random gaps between 33,015 entries. The timings illustrate
the change; the step counts are the regression check.

Each case runs in its own process with core dumps disabled, a 30-second limit,
or 120 seconds for the large step-count cases. Rollback lists, chunk locking,
bitmaps in a v1 cache, real tree searches and file writes, and real Windows pool
allocation are not exercised.
