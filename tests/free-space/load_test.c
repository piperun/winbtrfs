/* Run the production free-space loaders against modelled trees and a modelled cache file. */
#include "host.h"
#include "btrfs.h"

typedef uint32_t ULONG;
typedef void* PIRP;
typedef struct { ULONG SizeOfBitMap; ULONG* Buffer; } RTL_BITMAP;
typedef struct {
    struct btrfs_key key;
    uint16_t size;
    uint8_t* data;
} tree_data;
typedef struct tree { tree_data* items; size_t count; } tree; /* one leaf, in key order */
typedef struct { tree t; } root;
typedef struct { tree* tree; tree_data* item; } traverse_ptr;
typedef struct {
    struct btrfs_inode_item inode_item;
    bool deleted;
} fcb;
struct chunk {
    struct btrfs_chunk* chunk_item;
    uint64_t offset, used;
    fcb *cache, *old_cache;
    LIST_ENTRY space, space_size, deleting;
    bool cache_loaded, changed, space_changed, using_fst_bitmaps;
};
typedef struct {
    struct btrfs_super_block superblock;
    unsigned int sector_shift;
    root* root_root;
    root* space_root;
    root* extent_root;
} device_extension;

#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define RtlCopyMemory(d, s, n) memcpy(d, s, n)
#define RtlZeroMemory(p, n) memset(p, 0, n)
#define keycmp(key1, key2) compare_keys(&(key1), &(key2))
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static int compare_keys(const struct btrfs_key* a, const struct btrfs_key* b) {
    if (a->objectid != b->objectid) return a->objectid < b->objectid ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    return 0;
}

static uint64_t sector_align(uint64_t n, uint64_t a) {
    return n & (a - 1) ? (n + a) & ~(a - 1) : n;
}

static unsigned long long allocations, fail_allocation, pool_live;

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK(pool == PagedPool && tag == ALLOC_TAG && size > 0);
    if (++allocations == fail_allocation) return NULL;
    void* p = malloc(size);
    CHECK(p != NULL);
    memset(p, 0xa5, size);
    pool_live++;
    return p;
}

static void ExFreePool(void* p) {
    CHECK(p != NULL && pool_live > 0);
    pool_live--;
    free(p);
}

static NTSTATUS add_rollback_space(LIST_ENTRY* rollback, bool add, LIST_ENTRY* list,
    LIST_ENTRY* list_size, uint64_t address, uint64_t length, chunk* c) {
    (void)rollback; (void)add; (void)list; (void)list_size; (void)address; (void)length; (void)c;
    CHECK(false); /* no call in these tests passes a rollback list */
    return STATUS_INSUFFICIENT_RESOURCES;
}

/* The Windows bitmap routines: bit i is bit i % 32 of the ULONG i / 32. */
static void RtlInitializeBitMap(RTL_BITMAP* b, ULONG* p, ULONG bits) { b->Buffer = p; b->SizeOfBitMap = bits; }

static bool bit_set(const RTL_BITMAP* b, ULONG i) { return b->Buffer[i / 32] >> (i % 32) & 1; }

/* The length of the first run of clear bits at or after from, or 0 if there is none. */
static ULONG RtlFindNextForwardRunClear(RTL_BITMAP* b, ULONG from, ULONG* start) {
    ULONG i = from;
    while (i < b->SizeOfBitMap && bit_set(b, i)) i++;
    if (i >= b->SizeOfBitMap) return 0;
    *start = i;
    while (i < b->SizeOfBitMap && !bit_set(b, i)) i++;
    return i - *start;
}

static ULONG RtlFindFirstRunClear(RTL_BITMAP* b, ULONG* start) { return RtlFindNextForwardRunClear(b, 0, start); }

#define SECTOR 0x1000
#define NODE 0x4000
#define BASE 0x100000000ULL /* above 4 GiB, so truncated addresses show */
#define BITMAP_LENGTH 0x800000ULL /* 2048 sectors, as Linux writes them */
#define BITMAPS 16
#define LENGTH (BITMAPS * BITMAP_LENGTH) /* the v1 loader discards caches of chunks under 100 MB */
#define USED 0x34000 /* the extents below */
#define SUPERBLOCK (BASE + 0x4000000) /* a superblock copy, in the last free range */
#define STRIPE 0x10000 /* the stripe around a superblock copy, which allocations must skip */
#define CACHE_INODE 0x101
#define CACHE_GENERATION 41

typedef struct { uint64_t start, end; } range; /* half-open */

/* The chunk's extents and its block group item, between items of the chunks around it. */
static tree_data extent_items[] = {
    { { BASE - NODE, BTRFS_METADATA_ITEM_KEY, 0 }, 0, NULL },
    { { BASE, BTRFS_EXTENT_ITEM_KEY, 0x10000 }, 0, NULL },
    { { BASE, BTRFS_BLOCK_GROUP_ITEM_KEY, LENGTH }, 0, NULL },
    { { BASE + 0x14000, BTRFS_METADATA_ITEM_KEY, 0 }, 0, NULL },
    { { BASE + 0x20000, BTRFS_EXTENT_ITEM_KEY, 0x20000 }, 0, NULL },
    { { BASE + LENGTH, BTRFS_BLOCK_GROUP_ITEM_KEY, LENGTH }, 0, NULL },
};
/* The space the extent tree leaves free in the chunk. */
static const range free_ranges[] = {
    { BASE + 0x10000, BASE + 0x14000 },
    { BASE + 0x18000, BASE + 0x20000 },
    { BASE + 0x40000, BASE + LENGTH },
};
/* The free ranges without the stripe around the superblock copy. */
static const range protected_ranges[] = {
    { BASE + 0x10000, BASE + 0x14000 },
    { BASE + 0x18000, BASE + 0x20000 },
    { BASE + 0x40000, SUPERBLOCK },
    { SUPERBLOCK + STRIPE, BASE + LENGTH },
};

static device_extension volume;
static root root_tree, space_tree, extent_tree;
static struct btrfs_free_space_header header;
/* The v1 cache header items of the chunk and of the chunks around it. */
static const tree_data root_template[] = {
    { { BTRFS_FREE_SPACE_OBJECTID, 0, BASE - LENGTH }, 0, NULL },
    { { BTRFS_FREE_SPACE_OBJECTID, 0, BASE }, sizeof(header), (uint8_t*)&header },
    { { BTRFS_FREE_SPACE_OBJECTID, 0, BASE + LENGTH }, 0, NULL },
};
static tree_data root_items[COUNT(root_template)];
static tree_data space_items[BITMAPS + 4];
static struct btrfs_free_space_info info;
static uint8_t bitmaps[BITMAPS][BITMAP_LENGTH / SECTOR / 8];
static struct btrfs_chunk chunk_item;
static chunk load_chunk;
static fcb cache_file;
static uint8_t cache_data[2 * SECTOR];
static unsigned extent_searches, header_deletes, cache_excises, dirty_marks, failures;
/* What deleting the header item and excising the cache's extents return. */
static NTSTATUS delete_status, excise_status;
/* The stripe around a superblock copy in the chunk, or an empty range. */
static range superblock_stripe;
/* Whether a nocow write marks the chunk changed, without its chunk lock, while
   protect_superblocks runs. */
static bool write_during_protect;

/* As the production search: the item with the search key, else the last item before
   it, else the first item. */
static NTSTATUS find_item(device_extension* Vcb, root* r, traverse_ptr* tp,
                          const struct btrfs_key* searchkey, bool ignore, PIRP Irp) {
    (void)Irp;
    CHECK(Vcb == &volume && (r == &root_tree || r == &space_tree || r == &extent_tree) && !ignore);
    if (r == &extent_tree) extent_searches++;
    if (r->t.count == 0) return STATUS_NOT_FOUND;
    size_t i = 0;
    while (i + 1 < r->t.count && compare_keys(&r->t.items[i + 1].key, searchkey) <= 0) i++;
    tp->tree = &r->t;
    tp->item = &r->t.items[i];
    return STATUS_SUCCESS;
}

static bool find_next_item(device_extension* Vcb, const traverse_ptr* tp, traverse_ptr* next_tp,
                           bool ignore, PIRP Irp) {
    (void)Irp;
    CHECK(Vcb == &volume && !ignore);
    CHECK(tp->item >= tp->tree->items && tp->item < tp->tree->items + tp->tree->count);
    if (tp->item + 1 == tp->tree->items + tp->tree->count) return false;
    *next_tp = (traverse_ptr){ tp->tree, tp->item + 1 };
    return true;
}

static NTSTATUS open_fcb(device_extension* Vcb, root* subvol, uint64_t inode, uint8_t type, void* utf8,
                         bool always_add_hl, fcb* parent, fcb** pfcb, unsigned pooltype, PIRP Irp) {
    (void)Irp;
    CHECK(Vcb == &volume && subvol == &root_tree && inode == CACHE_INODE && type == BTRFS_FT_REG_FILE);
    CHECK(!utf8 && !always_add_hl && !parent && pooltype == PagedPool);
    *pfcb = &cache_file;
    return STATUS_SUCCESS;
}

/* The loader releases the cache file it opened when it cannot read it. */
static void free_fcb(fcb* f) {
    CHECK(f == &cache_file && load_chunk.cache == f);
}

static NTSTATUS read_file(fcb* f, uint8_t* data, uint64_t start, uint64_t length, ULONG* pbr, PIRP Irp) {
    CHECK(f == &cache_file && start == 0 && length == sizeof(cache_data) && !pbr && !Irp);
    memcpy(data, cache_data, sizeof(cache_data));
    return STATUS_SUCCESS;
}

/* Every buffer checksums to the seed, so the loader expects a stored checksum of 0. */
static uint32_t calc_crc32c(uint32_t seed, uint8_t* msg, ULONG msglen) {
    (void)msg; (void)msglen;
    return seed;
}

/* As the production function, it allocates an entry for each superblock stripe in the
   chunk while it counts them. */
static NTSTATUS get_superblock_size(chunk* c, uint64_t* size) {
    CHECK(c == &load_chunk);
    if (superblock_stripe.end > superblock_stripe.start) {
        uint64_t* stripe = ExAllocatePoolWithTag(PagedPool, sizeof(*stripe), ALLOC_TAG);
        if (!stripe) return STATUS_INSUFFICIENT_RESOURCES;
        ExFreePool(stripe);
    }
    *size = superblock_stripe.end - superblock_stripe.start;
    return STATUS_SUCCESS;
}

NTSTATUS space_list_subtract(chunk* c, uint64_t address, uint64_t length, LIST_ENTRY* rollback);

/* As the production function, it takes the stripe around each superblock copy in the
   chunk off its lists with space_list_subtract. */
static NTSTATUS protect_superblocks(chunk* c) {
    CHECK(c == &load_chunk);
    if (write_during_protect) c->changed = true;
    if (superblock_stripe.end == superblock_stripe.start) return STATUS_SUCCESS;
    return space_list_subtract(c, superblock_stripe.start, superblock_stripe.end - superblock_stripe.start,
                               NULL);
}

/* Takes the chunk's header item out of the root tree, so a later search does not find it. */
static NTSTATUS delete_tree_item(device_extension* Vcb, traverse_ptr* tp) {
    CHECK(Vcb == &volume && tp->tree == &root_tree.t && tp->item == &root_items[1]);
    if (delete_status != STATUS_SUCCESS) return delete_status;
    root_items[1] = root_items[2];
    root_tree.t.count--;
    header_deletes++;
    return STATUS_SUCCESS;
}

static NTSTATUS excise_extents(device_extension* Vcb, fcb* f, uint64_t start, uint64_t end, PIRP Irp,
                               LIST_ENTRY* rollback) {
    (void)Irp;
    CHECK(Vcb == &volume && f == &cache_file && start == 0 && end == sizeof(cache_data) && rollback);
    if (excise_status != STATUS_SUCCESS) return excise_status;
    cache_excises++;
    return STATUS_SUCCESS;
}

static void do_rollback(device_extension* Vcb, LIST_ENTRY* rollback) {
    CHECK(Vcb == &volume && IsListEmpty(rollback)); /* excise_extents adds nothing to roll back */
}

static void clear_rollback(LIST_ENTRY* rollback) { CHECK(IsListEmpty(rollback)); }

static void mark_fcb_dirty(fcb* f) {
    CHECK(f == &cache_file && f->deleted);
    dirty_marks++;
}

#include "space.inc"
#include "load.inc"

static void add_item(uint64_t objectid, uint8_t type, uint64_t offset, void* data, uint16_t size) {
    CHECK(space_tree.t.count < COUNT(space_items));
    space_items[space_tree.t.count++] = (tree_data){ { objectid, type, offset }, size, data };
}

static void add_extent(uint64_t start, uint64_t end) {
    add_item(start, BTRFS_FREE_SPACE_EXTENT_KEY, end - start, NULL, 0);
}

/* Bitmap i covers BITMAP_LENGTH from BASE + i * BITMAP_LENGTH; a set bit marks a free
   sector. */
static void add_bitmap(unsigned i, uint16_t size) {
    uint64_t start = BASE + i * BITMAP_LENGTH;
    memset(bitmaps[i], 0, sizeof(bitmaps[i]));
    for (size_t j = 0; j < COUNT(free_ranges); j++) {
        for (uint64_t a = free_ranges[j].start; a < free_ranges[j].end; a += SECTOR) {
            if (a < start || a >= start + BITMAP_LENGTH) continue;
            uint64_t bit = (a - start) / SECTOR;
            bitmaps[i][bit / 8] |= 1 << (bit % 8);
        }
    }
    add_item(start, BTRFS_FREE_SPACE_BITMAP_KEY, BITMAP_LENGTH, bitmaps[i], size);
}

static void add_bitmaps(unsigned from, unsigned to) {
    for (unsigned i = from; i < to; i++) add_bitmap(i, sizeof(bitmaps[i]));
}

static void set_up_chunk(void) {
    volume = (device_extension){ .sector_shift = 12, .root_root = &root_tree, .space_root = &space_tree,
                                 .extent_root = &extent_tree };
    volume.superblock.sectorsize = SECTOR;
    volume.superblock.nodesize = NODE;
    chunk_item = (struct btrfs_chunk){ .length = LENGTH };
    load_chunk = (chunk){ .chunk_item = &chunk_item, .offset = BASE, .used = USED };
    InitializeListHead(&load_chunk.space);
    InitializeListHead(&load_chunk.space_size);
    InitializeListHead(&load_chunk.deleting);
    memcpy(root_items, root_template, sizeof(root_items));
    root_tree.t = (tree){ root_items, COUNT(root_items) };
    space_tree.t = (tree){ space_items, 0 };
    extent_tree.t = (tree){ extent_items, COUNT(extent_items) };
    extent_searches = header_deletes = cache_excises = dirty_marks = 0;
    allocations = fail_allocation = 0;
    delete_status = excise_status = STATUS_SUCCESS;
    superblock_stripe = (range){ 0, 0 };
    write_during_protect = false;
}

/* A free space tree whose info item for the chunk has the given flags; each case adds
   the items that describe the chunk's free space. */
static void set_up_tree(uint32_t flags) {
    set_up_chunk();
    volume.superblock.compat_ro_flags = BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
                                        BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID;
    info = (struct btrfs_free_space_info){ .extent_count = COUNT(free_ranges), .flags = flags };
    add_item(BASE, BTRFS_FREE_SPACE_INFO_KEY, LENGTH, &info, sizeof(info));
}

static void end_tree(void) {
    add_item(BASE + LENGTH, BTRFS_FREE_SPACE_INFO_KEY, LENGTH, &info, sizeof(info)); /* the chunk after */
}

static NTSTATUS load_tree(void) {
    end_tree();
    return load_free_space_cache(&volume, &load_chunk, NULL);
}

/* A v1 cache written in the last transaction, holding the ranges as extent entries:
   two sector checksums, the generation, then the entries. */
static void set_up_cache(const range* r, size_t count) {
    set_up_chunk();
    volume.superblock.generation = CACHE_GENERATION + 1;
    volume.superblock.cache_generation = CACHE_GENERATION;
    header = (struct btrfs_free_space_header){ .location = { CACHE_INODE, BTRFS_INODE_ITEM_KEY, 0 },
                                               .generation = CACHE_GENERATION, .num_entries = count };
    cache_file = (fcb){ .inode_item = { .size = sizeof(cache_data) } };
    memset(cache_data, 0, sizeof(cache_data));
    uint64_t generation = CACHE_GENERATION;
    memcpy(cache_data + 2 * sizeof(uint32_t), &generation, sizeof(generation));
    uint8_t* at = cache_data + 2 * sizeof(uint32_t) + sizeof(uint64_t);
    for (size_t i = 0; i < count; i++, at += sizeof(struct btrfs_free_space_entry)) {
        struct btrfs_free_space_entry e = { r[i].start, r[i].end - r[i].start, BTRFS_FREE_SPACE_EXTENT };
        memcpy(at, &e, sizeof(e));
    }
}

static NTSTATUS load_cache(const range* r, size_t count) {
    set_up_cache(r, count);
    return load_free_space_cache(&volume, &load_chunk, NULL);
}

/* Printed before each failure the checks report, to name the step of a case that
   loads the chunk many times. */
static const char* step = "";

static void print_ranges(const char* what, const range* r, size_t count) {
    fprintf(stderr, "%s", what);
    for (size_t i = 0; i < count; i++)
        fprintf(stderr, " %llx-%llx", (unsigned long long)r[i].start, (unsigned long long)r[i].end);
    fprintf(stderr, "\n");
}

/* The ranges of the chunk's address list, in list order. */
static size_t list_ranges(range* got, size_t max) {
    size_t n = 0;
    for (LIST_ENTRY* le = load_chunk.space.Flink; le != &load_chunk.space; le = le->Flink) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        CHECK(n < max);
        got[n++] = (range){ s->address, s->address + s->size };
    }
    return n;
}

/* Reports an address list that does not hold exactly the given ranges, in address
   order. The size list must hold the same entries, in non-increasing size order. */
static void check_space(const range* want, size_t count) {
    range got[32];
    size_t n = list_ranges(got, COUNT(got));
    if (n != count || memcmp(got, want, n * sizeof(*got)) != 0) {
        fprintf(stderr, "%s", step);
        print_ranges("address list holds", got, n);
        print_ranges("expected", want, count);
        failures++;
        return;
    }
    size_t sizes = 0;
    uint64_t last = UINT64_MAX;
    for (LIST_ENTRY* le = load_chunk.space_size.Flink; le != &load_chunk.space_size; le = le->Flink, sizes++) {
        space* s = CONTAINING_RECORD(le, space, list_entry_size);
        bool listed = false;
        for (LIST_ENTRY* le2 = load_chunk.space.Flink; le2 != &load_chunk.space; le2 = le2->Flink)
            listed |= le2 == &s->list_entry;
        CHECK(listed && s->size <= last);
        last = s->size;
    }
    CHECK(sizes == count);
}

/* Reports a load that regenerated the free space from the extent tree when it should
   not have, or the reverse. */
static void check_regenerated(bool regenerated) {
    unsigned want = regenerated;
    if (extent_searches != want) {
        fprintf(stderr, "%sextent tree searched %u times, expected %u\n", step, extent_searches, want);
        failures++;
    }
}

/* Reports a v1 cache kept when it should have been discarded, or the reverse. A
   discarded cache loses its header item and its extents, and the chunk keeps the
   file only as its old cache. */
static void check_cache_discarded(bool discarded) {
    bool gone = header_deletes == 1 && cache_excises == 1 && cache_file.deleted && dirty_marks == 1 &&
                !load_chunk.cache && load_chunk.old_cache == &cache_file;
    bool kept = !header_deletes && !cache_excises && !cache_file.deleted && !dirty_marks &&
                load_chunk.cache == &cache_file && !load_chunk.old_cache;
    CHECK(gone || kept);
    if (gone != discarded) {
        fprintf(stderr, "%scache %s, expected it %s\n", step, gone ? "discarded" : "kept",
                discarded ? "discarded" : "kept");
        failures++;
    }
}

/* Reports a failed load that left entries or other allocations behind, or changed the
   chunk's space_changed from what it was before the load. The next load must start
   from an empty list, and the flush writes the lists of a chunk with space_changed
   set out as its free space. */
static void check_unloaded(bool space_changed) {
    CHECK(!load_chunk.cache_loaded);
    if (!IsListEmpty(&load_chunk.space)) {
        range got[32];
        size_t n = list_ranges(got, COUNT(got));
        fprintf(stderr, "%s", step);
        print_ranges("address list holds", got, n);
        fprintf(stderr, "expected none\n");
        failures++;
    } else if (pool_live) {
        fprintf(stderr, "%spool allocations not freed: %llu\n", step, pool_live);
        failures++;
    }
    if (load_chunk.space_changed != space_changed) {
        fprintf(stderr, "%sspace_changed %s, expected %s\n", step,
                load_chunk.space_changed ? "set" : "clear", space_changed ? "set" : "clear");
        failures++;
    }
}

/* Takes a range off the chunk's lists as an allocation does. The allocators pick from
   the size list, which may offer none of the range afterwards. */
static void take(range taken) {
    CHECK(space_list_subtract2(&load_chunk.space, &load_chunk.space_size, taken.start,
                               taken.end - taken.start, &load_chunk, NULL) == STATUS_SUCCESS);
    for (LIST_ENTRY* le = load_chunk.space_size.Flink; le != &load_chunk.space_size; le = le->Flink) {
        space* s = CONTAINING_RECORD(le, space, list_entry_size);
        if (s->address < taken.end && taken.start < s->address + s->size) {
            fprintf(stderr, "size list still offers %llx-%llx after %llx-%llx was taken\n",
                    (unsigned long long)s->address, (unsigned long long)(s->address + s->size),
                    (unsigned long long)taken.start, (unsigned long long)taken.end);
            failures++;
        }
    }
}

/* Frees the chunk's entries; anything still allocated after that is a leak. */
static void free_entries(void) {
    while (!IsListEmpty(&load_chunk.space)) {
        space* s = CONTAINING_RECORD(RemoveHeadList(&load_chunk.space), space, list_entry);
        RemoveEntryList(&s->list_entry_size);
        ExFreePool(s);
    }
    CHECK(IsListEmpty(&load_chunk.space_size) && IsListEmpty(&load_chunk.deleting));
    if (pool_live) {
        fprintf(stderr, "%spool allocations not freed: %llu\n", step, pool_live);
        pool_live = 0;
        failures++;
    }
}

static void tear_down(void) {
    free_entries();
    CHECK(failures == 0);
}

/* Loads the free space tree the case built, which the loader must reject: the lists must
   hold the free space regenerated from the extent tree, and nothing else may stay
   allocated. */
static void load_rejected_tree(void) {
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    check_regenerated(true);
    tear_down();
}

/* The same for a v1 cache holding the given ranges, which the loader must also discard. */
static void load_rejected_cache(const range* entries, size_t count) {
    CHECK(load_cache(entries, count) == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    check_regenerated(true);
    check_cache_discarded(true);
    tear_down();
}

/* The bitmaps give the free ranges; the last one spans all 16 bitmaps and becomes one
   entry. */
static void tree_loaded(void) {
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_bitmaps(0, BITMAPS);
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    check_regenerated(false);
    tear_down();
}

/* Extent items give the free ranges; the two that touch become one entry. */
static void tree_extents_loaded(void) {
    set_up_tree(0);
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_extent(BASE + 0x40000, BASE + 0x80000);
    add_extent(BASE + 0x80000, BASE + LENGTH);
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    check_regenerated(false);
    tear_down();
}

/* A second bitmap shorter than its range needs makes the loader regenerate the chunk's
   free space from the extent tree. The entries of the first bitmap must not stay. */
static void tree_short_bitmap(void) {
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_bitmap(0, sizeof(bitmaps[0]));
    add_bitmap(1, sizeof(bitmaps[1]) / 2);
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    check_regenerated(true);
    tear_down();
}

/* The same for an empty bitmap item over half a sector. It covers no whole sector, so
   the loader expects no bytes and the item reaches its check for an empty item; over a
   sector or more it would fail the size check first. */
static void tree_empty_bitmap(void) {
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_bitmap(0, sizeof(bitmaps[0]));
    add_item(BASE + BITMAP_LENGTH, BTRFS_FREE_SPACE_BITMAP_KEY, SECTOR / 2, bitmaps[1], 0);
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    check_regenerated(true);
    tear_down();
}

/* The same for a bitmap item over the chunk's first 12 sectors, inside an extent, that
   holds 1 byte where its bits need 2. Loaded, sectors 8 to 11 would take their bits from
   the rest of the loader's buffer, which the model's allocator fills with 0xa5, marking
   sectors 8 and 10 free. */
static void tree_bitmap_missing_bits(void) {
    static uint8_t bits[1];
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_item(BASE, BTRFS_FREE_SPACE_BITMAP_KEY, 12 * SECTOR, bits, sizeof(bits));
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_extent(BASE + 0x40000, BASE + LENGTH);
    load_rejected_tree();
}

/* Extent items whose ranges overlap make the loader regenerate the chunk's free space.
   An allocation inside the overlap must then leave none of its range free. */
static void tree_overlapping_extents(void) {
    set_up_tree(0);
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_extent(BASE + 0x1c000, BASE + 0x20000);
    add_extent(BASE + 0x40000, BASE + LENGTH);
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    take((range){ BASE + 0x1c000, BASE + 0x1d000 });
    check_regenerated(true);
    tear_down();
}

/* The same for two extent items that start at the same address. */
static void tree_same_start_extents(void) {
    set_up_tree(0);
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x1c000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_extent(BASE + 0x40000, BASE + LENGTH);
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    take((range){ BASE + 0x19000, BASE + 0x1a000 });
    check_regenerated(true);
    tear_down();
}

/* The same for an extent item over free space that a bitmap item also gives. */
static void tree_extent_over_bitmap(void) {
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_bitmap(0, sizeof(bitmaps[0]));
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_bitmaps(1, BITMAPS);
    CHECK(load_tree() == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    take((range){ BASE + 0x19000, BASE + 0x1a000 });
    check_regenerated(true);
    tear_down();
}

/* An extent item that reaches 4 sectors past the end of the chunk, with nothing after it
   that it could overlap, makes the loader regenerate the chunk's free space. */
static void tree_extent_past_chunk(void) {
    set_up_tree(0);
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_extent(BASE + 0x40000, BASE + LENGTH + 0x4000);
    load_rejected_tree();
}

/* The same for an extent item of 2^64 - 0x1000 bytes, whose end wraps to a sector before
   its start, so the item after it does not seem to overlap it. */
static void tree_extent_end_wraps(void) {
    set_up_tree(0);
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_item(BASE + 0x30000, BTRFS_FREE_SPACE_EXTENT_KEY, UINT64_MAX - 0xfff, NULL, 0);
    add_extent(BASE + 0x40000, BASE + LENGTH);
    load_rejected_tree();
}

/* The same for an extent item of size 0, between two of the chunk's extents. */
static void tree_empty_extent(void) {
    set_up_tree(0);
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_extent(BASE + 0x30000, BASE + 0x30000);
    add_extent(BASE + 0x40000, BASE + LENGTH);
    load_rejected_tree();
}

/* The same for a last bitmap item over twice its range, whose bits mark the space after
   the chunk free as well. */
static void tree_bitmap_past_chunk(void) {
    static uint8_t bits[2 * BITMAP_LENGTH / SECTOR / 8];
    memset(bits, 0xff, sizeof(bits));
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_bitmaps(0, BITMAPS - 1);
    add_item(BASE + LENGTH - BITMAP_LENGTH, BTRFS_FREE_SPACE_BITMAP_KEY, 2 * BITMAP_LENGTH, bits,
             sizeof(bits));
    load_rejected_tree();
}

/* The cache's entries give the free ranges, the two that touch becoming one entry, and
   the cache is kept. */
static void cache_loaded(void) {
    static const range entries[] = {
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
        { BASE + 0x40000, BASE + 0x80000 },
        { BASE + 0x80000, BASE + LENGTH },
    };
    CHECK(load_cache(entries, COUNT(entries)) == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    check_regenerated(false);
    check_cache_discarded(false);
    tear_down();
}

/* An entry inside another, with the last entry short by as much, so the entries still
   add up to the chunk's free space. The loader must discard the cache and regenerate
   the chunk's free space, and an allocation inside the overlap must leave none of its
   range free. */
static void cache_overlapping_entries(void) {
    static const range entries[] = {
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
        { BASE + 0x1c000, BASE + 0x20000 },
        { BASE + 0x40000, BASE + LENGTH - 0x4000 },
    };
    CHECK(load_cache(entries, COUNT(entries)) == STATUS_SUCCESS);
    check_space(free_ranges, COUNT(free_ranges));
    take((range){ BASE + 0x1c000, BASE + 0x1d000 });
    check_regenerated(true);
    check_cache_discarded(true);
    tear_down();
}

/* An entry for the 4 sectors before the chunk, where the chunk before it has a tree
   block, with the last entry short by as much. The loader must discard the cache and
   regenerate the chunk's free space. */
static void cache_entry_before_chunk(void) {
    static const range entries[] = {
        { BASE - 0x4000, BASE },
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
        { BASE + 0x40000, BASE + LENGTH - 0x4000 },
    };
    load_rejected_cache(entries, COUNT(entries));
}

/* The same for a last entry moved 4 sectors on, past the end of the chunk. */
static void cache_entry_past_chunk(void) {
    static const range entries[] = {
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
        { BASE + 0x44000, BASE + LENGTH + 0x4000 },
    };
    load_rejected_cache(entries, COUNT(entries));
}

/* The same for an entry that starts 4 sectors after the end of the chunk, with the last
   entry in the chunk short by as much. */
static void cache_entry_after_chunk(void) {
    static const range entries[] = {
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
        { BASE + 0x40000, BASE + LENGTH - 0x4000 },
        { BASE + LENGTH + 0x4000, BASE + LENGTH + 0x8000 },
    };
    load_rejected_cache(entries, COUNT(entries));
}

/* The same for an entry of size 0, between two of the chunk's extents. */
static void cache_empty_entry(void) {
    static const range entries[] = {
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
        { BASE + 0x30000, BASE + 0x30000 },
        { BASE + 0x40000, BASE + LENGTH },
    };
    load_rejected_cache(entries, COUNT(entries));
}

/* The same for an entry of 2^64 - 0x1000 bytes, whose end wraps to a sector before its
   start, with the last entry a sector longer, so the sizes still add up to the chunk's
   free space and the entry after it does not seem to overlap it. */
static void cache_entry_end_wraps(void) {
    static const range entries[] = {
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
        { BASE + 0x30000, BASE + 0x2f000 },
        { BASE + 0x3f000, BASE + LENGTH },
    };
    load_rejected_cache(entries, COUNT(entries));
}

/* The bitmap items of load-tree, with a superblock copy in the chunk. */
static void set_up_bitmap_tree(void) {
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_bitmaps(0, BITMAPS);
    end_tree();
    superblock_stripe = (range){ SUPERBLOCK, SUPERBLOCK + STRIPE };
}

/* Extent items over the first bitmap's span and bitmap items for the rest, so the
   loader allocates its bitmap buffer after loading entries. */
static void set_up_mixed_tree(void) {
    set_up_tree(BTRFS_FREE_SPACE_USING_BITMAPS);
    add_extent(BASE + 0x10000, BASE + 0x14000);
    add_extent(BASE + 0x18000, BASE + 0x20000);
    add_extent(BASE + 0x40000, BASE + BITMAP_LENGTH);
    add_bitmaps(1, BITMAPS);
    end_tree();
    superblock_stripe = (range){ SUPERBLOCK, SUPERBLOCK + STRIPE };
}

/* A v1 cache that leaves out the superblock stripe, which the loader counts with
   get_superblock_size instead. */
static void set_up_protected_cache(void) {
    set_up_cache(protected_ranges, COUNT(protected_ranges));
    superblock_stripe = (range){ SUPERBLOCK, SUPERBLOCK + STRIPE };
}

/* The same with the last two ranges in a bitmap entry after the extent entries. The
   bitmap fills the sector after the entries and covers the chunk, with a set bit for
   each free sector. */
static void set_up_bitmap_cache(void) {
    _Static_assert(LENGTH == SECTOR * 8 * SECTOR, "a sector of bitmap covers the chunk");
    set_up_cache(protected_ranges, 2);
    struct btrfs_free_space_entry e = { BASE, 0, BTRFS_FREE_SPACE_BITMAP };
    uint8_t* bitmap = cache_data + SECTOR;
    for (size_t i = 2; i < COUNT(protected_ranges); i++) {
        for (uint64_t a = protected_ranges[i].start; a < protected_ranges[i].end; a += SECTOR) {
            uint64_t bit = (a - BASE) / SECTOR;
            bitmap[bit / 8] |= 1 << (bit % 8);
            e.bytes += SECTOR;
        }
    }
    memcpy(cache_data + 2 * sizeof(uint32_t) + sizeof(uint64_t) + 2 * sizeof(e), &e, sizeof(e));
    header.num_entries = 3;
    header.num_bitmaps = 1;
    superblock_stripe = (range){ SUPERBLOCK, SUPERBLOCK + STRIPE };
}

/* No free space tree and no v1 cache from the last transaction, so the loader
   regenerates the free space from the extent tree. */
static void set_up_no_stored_space(void) {
    set_up_chunk();
    superblock_stripe = (range){ SUPERBLOCK, SUPERBLOCK + STRIPE };
}

/* The bitmap buffer, 3 entries from the first bitmap and 1 from each other, and the
   entry split off below the superblock stripe. */
#define BITMAP_TREE_ALLOCATIONS (1 + 3 + (BITMAPS - 1) + 1)
/* An entry for each extent item, the bitmap buffer, 1 entry from each bitmap and the
   entry split off below the superblock stripe. */
#define MIXED_TREE_ALLOCATIONS (3 + 1 + (BITMAPS - 1) + 1)
/* The buffer the cache file is read into, its 4 entries, and the superblock stripe
   while it is counted. */
#define CACHE_ALLOCATIONS (1 + 4 + 1)
/* The buffer, the 2 extent entries, an entry for each of the bitmap's 2 runs, and the
   superblock stripe while it is counted. */
#define BITMAP_CACHE_ALLOCATIONS (1 + 2 + 2 + 1)
/* An entry for each free range and the entry split off below the superblock stripe. */
#define REGENERATED_ALLOCATIONS (3 + 1)

/* The chunk must be loaded with the free ranges less the superblock stripe, from the
   extent tree or not, and keep its v1 cache if it has one. It must keep the
   space_changed mark protect_superblocks sets: only a failed load takes it back. */
static void check_loaded(bool regenerated, bool cached) {
    CHECK(load_chunk.cache_loaded);
    check_space(protected_ranges, COUNT(protected_ranges));
    check_regenerated(regenerated);
    if (cached) check_cache_discarded(false);
    if (!load_chunk.space_changed) {
        fprintf(stderr, "%sspace_changed clear after the load, expected set\n", step);
        failures++;
    }
}

/* Fails each allocation of a load in turn, then loads the chunk again, as callers do
   after a failure. The failed load must return the failure and leave the chunk as it
   found it, and the next load must give what a load that failed nothing gives. A
   load that succeeds despite the failure is reported with what it loaded. */
static void fail_each_allocation(void (*set_up)(void), unsigned count, bool regenerated, bool cached) {
    char text[64];
    set_up();
    CHECK(load_cache_chunk(&volume, &load_chunk, NULL) == STATUS_SUCCESS);
    CHECK(allocations == count);
    check_loaded(regenerated, cached);
    free_entries();
    for (unsigned k = 1; k <= count; k++) {
        set_up();
        fail_allocation = k;
        snprintf(text, sizeof(text), "allocation %u of %u failed: ", k, count);
        step = text;
        NTSTATUS status = load_cache_chunk(&volume, &load_chunk, NULL);
        if (status == STATUS_SUCCESS) {
            fprintf(stderr, "%sload succeeded, expected STATUS_INSUFFICIENT_RESOURCES\n", step);
            failures++;
            check_loaded(regenerated, cached);
            free_entries();
            continue;
        }
        CHECK(status == STATUS_INSUFFICIENT_RESOURCES);
        CHECK(allocations == k);
        check_unloaded(false);
        fail_allocation = 0;
        extent_searches = 0;
        snprintf(text, sizeof(text), "load after allocation %u of %u failed: ", k, count);
        CHECK(load_cache_chunk(&volume, &load_chunk, NULL) == STATUS_SUCCESS);
        check_loaded(regenerated, cached);
        free_entries();
    }
    step = "";
    CHECK(failures == 0);
}

static void tree_allocation_failures(void) {
    fail_each_allocation(set_up_bitmap_tree, BITMAP_TREE_ALLOCATIONS, false, false);
}

static void mixed_tree_allocation_failures(void) {
    fail_each_allocation(set_up_mixed_tree, MIXED_TREE_ALLOCATIONS, false, false);
}

static void cache_allocation_failures(void) {
    fail_each_allocation(set_up_protected_cache, CACHE_ALLOCATIONS, false, true);
}

static void bitmap_cache_allocation_failures(void) {
    fail_each_allocation(set_up_bitmap_cache, BITMAP_CACHE_ALLOCATIONS, false, true);
}

static void regenerated_allocation_failures(void) {
    fail_each_allocation(set_up_no_stored_space, REGENERATED_ALLOCATIONS, true, false);
}

/* Loads that fail in protect_superblocks, as it takes the stripe out of the free space
   regenerated from the extent tree. A chunk marked space_changed before the load must
   stay marked, and so must a chunk a nocow write marks changed while
   protect_superblocks runs. */
static void protect_failure_marks(void) {
    set_up_no_stored_space();
    load_chunk.space_changed = true;
    fail_allocation = REGENERATED_ALLOCATIONS;
    step = "chunk marked before the load: ";
    CHECK(load_cache_chunk(&volume, &load_chunk, NULL) == STATUS_INSUFFICIENT_RESOURCES);
    check_unloaded(true);
    free_entries();
    set_up_no_stored_space();
    write_during_protect = true;
    fail_allocation = REGENERATED_ALLOCATIONS;
    step = "chunk marked during the load: ";
    CHECK(load_cache_chunk(&volume, &load_chunk, NULL) == STATUS_INSUFFICIENT_RESOURCES);
    check_unloaded(false);
    if (!load_chunk.changed) {
        fprintf(stderr, "%schanged clear, expected set\n", step);
        failures++;
    }
    free_entries();
    step = "";
    CHECK(failures == 0);
}

/* The cache's entries add up to less than the chunk's free space, so the loader
   discards the cache after loading them. Deleting its header item or excising its
   extents fails in turn: the load must return the failure and leave the chunk as it
   found it. The next load regenerates the free space from the extent tree, discarding
   the cache again or finding its header item gone. */
static void cache_discard_failures(void) {
    static const range entries[] = {
        { BASE + 0x10000, BASE + 0x14000 },
        { BASE + 0x18000, BASE + 0x20000 },
    };
    struct { const char* what; NTSTATUS* status; NTSTATUS failure; } calls[] = {
        { "deleting the header item", &delete_status, STATUS_INTERNAL_ERROR },
        { "excising the cache's extents", &excise_status, STATUS_INSUFFICIENT_RESOURCES },
    };
    char text[64];
    for (size_t i = 0; i < COUNT(calls); i++) {
        set_up_cache(entries, COUNT(entries));
        *calls[i].status = calls[i].failure;
        snprintf(text, sizeof(text), "%s failed: ", calls[i].what);
        step = text;
        CHECK(load_cache_chunk(&volume, &load_chunk, NULL) == calls[i].failure);
        check_unloaded(false);
        *calls[i].status = STATUS_SUCCESS;
        extent_searches = 0;
        snprintf(text, sizeof(text), "load after %s failed: ", calls[i].what);
        CHECK(load_cache_chunk(&volume, &load_chunk, NULL) == STATUS_SUCCESS);
        CHECK(load_chunk.cache_loaded);
        check_space(free_ranges, COUNT(free_ranges));
        check_regenerated(true);
        free_entries();
    }
    step = "";
    CHECK(failures == 0);
}

static const struct { const char* name; void (*run)(void); } cases[] = {
    { "load-tree", tree_loaded },
    { "load-tree-extents", tree_extents_loaded },
    { "load-tree-short-bitmap", tree_short_bitmap },
    { "load-tree-empty-bitmap", tree_empty_bitmap },
    { "load-tree-bitmap-missing-bits", tree_bitmap_missing_bits },
    { "load-tree-overlapping-extents", tree_overlapping_extents },
    { "load-tree-same-start-extents", tree_same_start_extents },
    { "load-tree-extent-over-bitmap", tree_extent_over_bitmap },
    { "load-tree-extent-past-chunk", tree_extent_past_chunk },
    { "load-tree-extent-end-wraps", tree_extent_end_wraps },
    { "load-tree-empty-extent", tree_empty_extent },
    { "load-tree-bitmap-past-chunk", tree_bitmap_past_chunk },
    { "load-cache", cache_loaded },
    { "load-cache-overlapping-entries", cache_overlapping_entries },
    { "load-cache-entry-before-chunk", cache_entry_before_chunk },
    { "load-cache-entry-past-chunk", cache_entry_past_chunk },
    { "load-cache-entry-after-chunk", cache_entry_after_chunk },
    { "load-cache-empty-entry", cache_empty_entry },
    { "load-cache-entry-end-wraps", cache_entry_end_wraps },
    { "load-tree-allocation-failures", tree_allocation_failures },
    { "load-tree-mixed-allocation-failures", mixed_tree_allocation_failures },
    { "load-cache-allocation-failures", cache_allocation_failures },
    { "load-cache-bitmap-allocation-failures", bitmap_cache_allocation_failures },
    { "load-cache-discard-failures", cache_discard_failures },
    { "load-regenerated-allocation-failures", regenerated_allocation_failures },
    { "load-protect-failure-marks", protect_failure_marks },
};

int main(int argc, char** argv) {
    CHECK(argc == 2);
    for (size_t i = 0; i < COUNT(cases); i++) {
        if (strcmp(argv[1], "--list") == 0) puts(cases[i].name);
        else if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            return 0;
        }
    }
    CHECK(strcmp(argv[1], "--list") == 0);
    return 0;
}
