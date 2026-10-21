/* Run the production free-space loaders against a modelled free space tree and extent tree. */
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
struct chunk {
    struct btrfs_chunk* chunk_item;
    uint64_t offset;
    LIST_ENTRY space, space_size;
    bool using_fst_bitmaps;
};
typedef struct {
    struct btrfs_super_block superblock;
    unsigned int sector_shift;
    root* space_root;
    root* extent_root;
} device_extension;

#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225)
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define RtlCopyMemory(d, s, n) memcpy(d, s, n)
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

static unsigned long long pool_live;

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK(pool == PagedPool && tag == ALLOC_TAG && size > 0);
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
#define LENGTH (2 * BITMAP_LENGTH)

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

static device_extension volume;
static root space_tree, extent_tree;
static tree_data space_items[8];
static struct btrfs_free_space_info info;
static uint8_t bitmaps[2][BITMAP_LENGTH / SECTOR / 8];
static struct btrfs_chunk chunk_item;
static chunk load_chunk;
static unsigned extent_searches, failures;

/* As the production search: the item with the search key, else the last item before
   it, else the first item. */
static NTSTATUS find_item(device_extension* Vcb, root* r, traverse_ptr* tp,
                          const struct btrfs_key* searchkey, bool ignore, PIRP Irp) {
    (void)Irp;
    CHECK(Vcb == &volume && (r == &space_tree || r == &extent_tree) && !ignore);
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

static NTSTATUS load_stored_free_space_cache(device_extension* Vcb, chunk* c, bool load_only, PIRP Irp) {
    (void)Vcb; (void)c; (void)load_only; (void)Irp;
    CHECK(false); /* the superblock flags choose the free space tree */
    return STATUS_NOT_FOUND;
}

#include "space.inc"
#include "load.inc"

static void add_item(uint64_t objectid, uint8_t type, uint64_t offset, void* data, uint16_t size) {
    CHECK(space_tree.t.count < COUNT(space_items));
    space_items[space_tree.t.count++] = (tree_data){ { objectid, type, offset }, size, data };
}

/* Bitmap i covers BITMAP_LENGTH from start; a set bit marks a free sector. */
static void add_bitmap(unsigned i, uint64_t start, uint16_t size) {
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

/* A chunk whose free space tree items are in bitmaps; each case adds the bitmaps. */
static void set_up(void) {
    volume = (device_extension){ .sector_shift = 12, .space_root = &space_tree, .extent_root = &extent_tree };
    volume.superblock.compat_ro_flags = BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
                                        BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID;
    volume.superblock.sectorsize = SECTOR;
    volume.superblock.nodesize = NODE;
    chunk_item = (struct btrfs_chunk){ .length = LENGTH };
    load_chunk = (chunk){ .chunk_item = &chunk_item, .offset = BASE };
    InitializeListHead(&load_chunk.space);
    InitializeListHead(&load_chunk.space_size);
    extent_tree.t = (tree){ extent_items, COUNT(extent_items) };
    space_tree.t = (tree){ space_items, 0 };
    info = (struct btrfs_free_space_info){ .extent_count = COUNT(free_ranges),
                                           .flags = BTRFS_FREE_SPACE_USING_BITMAPS };
    add_item(BASE, BTRFS_FREE_SPACE_INFO_KEY, LENGTH, &info, sizeof(info));
    extent_searches = failures = 0;
}

static NTSTATUS load(void) {
    add_item(BASE + LENGTH, BTRFS_FREE_SPACE_INFO_KEY, LENGTH, &info, sizeof(info)); /* the chunk after */
    return load_free_space_cache(&volume, &load_chunk, NULL);
}

static void print_ranges(const char* what, const range* r, size_t count) {
    fprintf(stderr, "%s", what);
    for (size_t i = 0; i < count; i++)
        fprintf(stderr, " %llx-%llx", (unsigned long long)r[i].start, (unsigned long long)r[i].end);
    fprintf(stderr, "\n");
}

/* Reports an address list that does not hold exactly the given ranges, in address
   order. The size list must hold the same entries, in non-increasing size order. */
static void check_space(const range* want, size_t count) {
    range got[32];
    size_t n = 0;
    for (LIST_ENTRY* le = load_chunk.space.Flink; le != &load_chunk.space; le = le->Flink) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        CHECK(n < COUNT(got));
        got[n++] = (range){ s->address, s->address + s->size };
    }
    if (n != count || memcmp(got, want, n * sizeof(*got)) != 0) {
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

/* Frees the chunk's entries; anything still allocated after that is a leak. */
static void tear_down(void) {
    while (!IsListEmpty(&load_chunk.space)) {
        space* s = CONTAINING_RECORD(RemoveHeadList(&load_chunk.space), space, list_entry);
        RemoveEntryList(&s->list_entry_size);
        ExFreePool(s);
    }
    CHECK(IsListEmpty(&load_chunk.space_size));
    if (pool_live) {
        fprintf(stderr, "pool allocations not freed: %llu\n", pool_live);
        failures++;
    }
    CHECK(failures == 0);
}

/* The two bitmaps give the free ranges, and the extent tree is not searched. */
static void tree_loaded(void) {
    set_up();
    add_bitmap(0, BASE, sizeof(bitmaps[0]));
    add_bitmap(1, BASE + BITMAP_LENGTH, sizeof(bitmaps[1]));
    CHECK(load() == STATUS_SUCCESS);
    CHECK(extent_searches == 0);
    check_space(free_ranges, COUNT(free_ranges));
    tear_down();
}

/* A second bitmap shorter than its range needs makes the loader regenerate the chunk's
   free space from the extent tree. The entries of the first bitmap must not stay. */
static void tree_short_bitmap(void) {
    set_up();
    add_bitmap(0, BASE, sizeof(bitmaps[0]));
    add_bitmap(1, BASE + BITMAP_LENGTH, sizeof(bitmaps[1]) / 2);
    CHECK(load() == STATUS_SUCCESS);
    CHECK(extent_searches == 1);
    check_space(free_ranges, COUNT(free_ranges));
    tear_down();
}

/* The same for an empty bitmap item over 4 sectors. The loader expects 4 / 8 bytes,
   rounded down to 0, so the item reaches its check for an empty item. */
static void tree_empty_bitmap(void) {
    set_up();
    add_bitmap(0, BASE, sizeof(bitmaps[0]));
    add_item(BASE + BITMAP_LENGTH, BTRFS_FREE_SPACE_BITMAP_KEY, 4 * SECTOR, bitmaps[1], 0);
    CHECK(load() == STATUS_SUCCESS);
    CHECK(extent_searches == 1);
    check_space(free_ranges, COUNT(free_ranges));
    tear_down();
}

static const struct { const char* name; void (*run)(void); } cases[] = {
    { "load-tree", tree_loaded },
    { "load-tree-short-bitmap", tree_short_bitmap },
    { "load-tree-empty-bitmap", tree_empty_bitmap },
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
