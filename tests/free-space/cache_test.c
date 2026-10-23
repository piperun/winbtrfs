/* Run the production free-space cache writers against modelled trees and pools. */
#include "host.h"
#include "btrfs.h"

typedef uint32_t ULONG;
typedef void* PIRP;
typedef struct { uint64_t id; } root;
typedef struct tree tree;
typedef struct {
    struct btrfs_key key;
    uint16_t size;
    uint8_t* data;
} tree_data;
typedef struct { tree* tree; tree_data* item; } traverse_ptr;
typedef struct { struct btrfs_inode_item inode_item; } fcb;
struct chunk {
    struct btrfs_chunk* chunk_item;
    uint64_t offset;
    fcb* cache;
    LIST_ENTRY space, space_size, deleting, pending_trims;
    bool using_fst_bitmaps;
};
typedef struct {
    struct btrfs_super_block superblock;
    unsigned int sector_shift;
    root* root_root;
    root* space_root;
} device_extension;

#define NonPagedPool 0
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define RtlZeroMemory(p, n) memset(p, 0, n)
#define keycmp(key1, key2) compare_keys(&(key1), &(key2))

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
    CHECK(tag == ALLOC_TAG && (pool == NonPagedPool || (pool == PagedPool && size == sizeof(space))));
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
    CHECK(false); /* the cache writers pass no rollback list */
    return STATUS_INSUFFICIENT_RESOURCES;
}

#define SECTOR 4096
#define BASE 0x100000000ULL /* above 4 GiB, so truncated addresses show */
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

typedef struct { uint64_t start, end; } range; /* half-open */

/* The chunk's free entries, the ranges freed since the last flush, and their union. */
static const range space_ranges[] = {
    { BASE + 0x10000, BASE + 0x11000 },
    { BASE + 0x13000, BASE + 0x14000 },
    { BASE + 0x20000, BASE + 0x22000 },
};
static const range freed_ranges[] = {
    { BASE + 0x11000, BASE + 0x12000 }, /* extends the first entry */
    { BASE + 0x16000, BASE + 0x17000 }, /* joins nothing */
    { BASE + 0x30000, BASE + 0x31000 }, /* joins nothing */
};
static const range merged_ranges[] = {
    { BASE + 0x10000, BASE + 0x12000 },
    { BASE + 0x13000, BASE + 0x14000 },
    { BASE + 0x16000, BASE + 0x17000 },
    { BASE + 0x20000, BASE + 0x22000 },
    { BASE + 0x30000, BASE + 0x31000 },
};
/* The cache buffer, a copy of each entry and of each freed range, and an entry for
   each freed range that joins nothing. */
#define CACHE_ALLOCATIONS (1 + COUNT(space_ranges) + COUNT(freed_ranges) + 2)
/* A copy of each entry, and an entry for each freed range that joins nothing. */
#define TREE_ALLOCATIONS (COUNT(space_ranges) + 2)

static device_extension volume;
static root root_tree, space_tree;
static struct btrfs_chunk chunk_item;
static fcb cache_file;
static chunk cache_chunk;
static struct btrfs_free_space_header header;
static tree_data header_item;
static uint8_t written[2 * SECTOR];
static unsigned flushes, writes;
static NTSTATUS flush_status;
static range inserted[2 * COUNT(merged_ranges)];
static unsigned insert_count, info_updates;
static uint32_t info_extent_count;

static NTSTATUS flush_fcb(fcb* f, bool cache, LIST_ENTRY* batchlist, PIRP Irp) {
    (void)Irp;
    CHECK(f == &cache_file && cache && batchlist);
    flushes++;
    return flush_status;
}

static NTSTATUS find_item(device_extension* Vcb, root* r, traverse_ptr* tp,
                          const struct btrfs_key* searchkey, bool ignore, PIRP Irp) {
    (void)Irp;
    CHECK(Vcb == &volume && !ignore);
    if (r == &space_tree) {
        /* The free space tree holds no items for the chunk yet. */
        CHECK(searchkey->objectid == cache_chunk.offset && searchkey->type == BTRFS_FREE_SPACE_EXTENT_KEY &&
              searchkey->offset == UINT64_MAX);
        return STATUS_NOT_FOUND;
    }
    CHECK(r == &root_tree);
    CHECK(searchkey->objectid == BTRFS_FREE_SPACE_OBJECTID && searchkey->type == 0 &&
          searchkey->offset == cache_chunk.offset);
    tp->tree = NULL;
    tp->item = &header_item;
    return STATUS_SUCCESS;
}

static uint32_t calc_crc32c(uint32_t seed, uint8_t* msg, ULONG msglen) {
    (void)seed; (void)msg; (void)msglen;
    return 0;
}

static NTSTATUS do_write_file(fcb* f, uint64_t start, uint64_t end, void* data, PIRP Irp,
                              bool file_write, uint32_t irp_offset, LIST_ENTRY* rollback) {
    (void)Irp; (void)irp_offset;
    CHECK(f == &cache_file && start == 0 && end == sizeof(written) && !file_write && !rollback);
    memcpy(written, data, sizeof(written));
    writes++;
    return STATUS_SUCCESS;
}

static bool should_write_fst_bitmaps(device_extension* Vcb, chunk* c, LIST_ENTRY* space_list) {
    CHECK(Vcb == &volume && c == &cache_chunk && space_list);
    return false;
}

static NTSTATUS update_chunk_cache_tree_bitmaps(device_extension* Vcb, chunk* c, PIRP Irp,
                                                LIST_ENTRY* space_list) {
    (void)Vcb; (void)c; (void)Irp; (void)space_list;
    CHECK(false); /* should_write_fst_bitmaps chooses extent items */
    return STATUS_INTERNAL_ERROR;
}

static bool find_next_item(device_extension* Vcb, const traverse_ptr* tp, traverse_ptr* next_tp,
                           bool ignore, PIRP Irp) {
    (void)Vcb; (void)tp; (void)next_tp; (void)ignore; (void)Irp;
    CHECK(false); /* the free space tree holds no items to walk */
    return false;
}

static NTSTATUS delete_tree_item(device_extension* Vcb, traverse_ptr* tp) {
    (void)Vcb; (void)tp;
    CHECK(false); /* the free space tree holds no items to delete */
    return STATUS_INTERNAL_ERROR;
}

static NTSTATUS insert_tree_item(device_extension* Vcb, root* r, uint64_t objectid, uint8_t type,
                                 uint64_t offset, void* data, uint16_t size, traverse_ptr* ptp, PIRP Irp) {
    (void)Irp;
    CHECK(Vcb == &volume && r == &space_tree && type == BTRFS_FREE_SPACE_EXTENT_KEY);
    CHECK(!data && !size && !ptp && insert_count < COUNT(inserted));
    inserted[insert_count++] = (range){ objectid, objectid + offset };
    return STATUS_SUCCESS;
}

static NTSTATUS update_free_space_info(device_extension* Vcb, chunk* c, uint32_t extent_count,
                                       uint32_t flags, PIRP Irp) {
    (void)Irp;
    CHECK(Vcb == &volume && c == &cache_chunk && flags == 0);
    info_updates++;
    info_extent_count = extent_count;
    return STATUS_SUCCESS;
}

#include "space.inc"
#include "pending.inc"
#include "cache.inc"

static void build_list(LIST_ENTRY* list, const range* r, size_t count) {
    InitializeListHead(list);
    for (size_t i = 0; i < count; i++) {
        space* s = malloc(sizeof(space));
        CHECK(s != NULL);
        memset(s, 0xa5, sizeof(space));
        s->address = r[i].start;
        s->size = r[i].end - r[i].start;
        InsertTailList(list, &s->list_entry);
        pool_live++;
    }
}

static void check_list(LIST_ENTRY* list, const range* r, size_t count) {
    size_t i = 0;
    for (LIST_ENTRY* le = list->Flink; le != list; le = le->Flink, i++) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        CHECK(i < count && le->Flink->Blink == le);
        CHECK(s->address == r[i].start && s->size == r[i].end - r[i].start);
    }
    CHECK(i == count);
}

static void free_list(LIST_ENTRY* list) {
    while (!IsListEmpty(list)) ExFreePool(CONTAINING_RECORD(RemoveHeadList(list), space, list_entry));
}

static void set_up(void) {
    volume = (device_extension){ .sector_shift = 12, .root_root = &root_tree, .space_root = &space_tree };
    volume.superblock.generation = 42;
    volume.superblock.sectorsize = SECTOR;
    chunk_item = (struct btrfs_chunk){ .length = 0x40000000 };
    cache_file = (fcb){ .inode_item = { .size = sizeof(written) } };
    cache_chunk = (chunk){ .chunk_item = &chunk_item, .offset = BASE, .cache = &cache_file };
    build_list(&cache_chunk.space, space_ranges, COUNT(space_ranges));
    InitializeListHead(&cache_chunk.space_size);
    InitializeListHead(&cache_chunk.pending_trims);
    build_list(&cache_chunk.deleting, freed_ranges, COUNT(freed_ranges));
    header = (struct btrfs_free_space_header){ .num_entries = 99 };
    header_item = (tree_data){ .key = { BTRFS_FREE_SPACE_OBJECTID, 0, BASE },
                               .size = sizeof(header), .data = (uint8_t*)&header };
    memset(written, 0, sizeof(written));
    allocations = fail_allocation = 0;
    flushes = writes = 0;
    flush_status = STATUS_SUCCESS;
    insert_count = info_updates = 0;
}

static unsigned leaks;

/* The chunk's own lists must be unchanged. Frees them; anything still allocated
   after that is a leak, reported here and failed at the end of the case. */
static void tear_down(const char* step) {
    check_list(&cache_chunk.space, space_ranges, COUNT(space_ranges));
    check_list(&cache_chunk.deleting, freed_ranges, COUNT(freed_ranges));
    free_list(&cache_chunk.space);
    free_list(&cache_chunk.deleting);
    if (pool_live) {
        fprintf(stderr, "%s: pool allocations not freed: %llu\n", step, pool_live);
        pool_live = 0;
        leaks++;
    }
}

static NTSTATUS write_cache(void) {
    struct btrfs_timespec now = { 1000, 5 };
    LIST_ENTRY batchlist;
    InitializeListHead(&batchlist);
    return update_chunk_cache(&volume, &cache_chunk, &now, &batchlist, NULL);
}

static void cache_written(void) {
    set_up();
    CHECK(write_cache() == STATUS_SUCCESS);
    CHECK(allocations == CACHE_ALLOCATIONS && flushes == 1 && writes == 1);
    CHECK(header.generation == 42 && header.num_entries == COUNT(merged_ranges) && header.num_bitmaps == 0);
    /* Two sector checksums and the cache generation come before the entries. */
    const uint8_t* at = written + 2 * sizeof(uint32_t) + sizeof(uint64_t);
    for (size_t i = 0; i < COUNT(merged_ranges); i++, at += sizeof(struct btrfs_free_space_entry)) {
        const struct btrfs_free_space_entry* e = (const void*)at;
        CHECK(e->type == BTRFS_FREE_SPACE_EXTENT && e->offset == merged_ranges[i].start &&
              e->bytes == merged_ranges[i].end - merged_ranges[i].start);
    }
    tear_down("written cache");
}

/* Fails each allocation in turn. */
static void cache_allocation_failures(void) {
    for (unsigned k = 1; k <= CACHE_ALLOCATIONS; k++) {
        char step[48];
        snprintf(step, sizeof(step), "allocation %u of %zu failed", k, CACHE_ALLOCATIONS);
        set_up();
        fail_allocation = k;
        CHECK(write_cache() == STATUS_INSUFFICIENT_RESOURCES);
        CHECK(allocations == k && flushes == 0 && writes == 0);
        tear_down(step);
    }
}

static void cache_flush_failure(void) {
    set_up();
    flush_status = STATUS_IO_DEVICE_ERROR;
    CHECK(write_cache() == STATUS_IO_DEVICE_ERROR);
    CHECK(allocations == CACHE_ALLOCATIONS && flushes == 1 && writes == 0);
    tear_down("flush failed");
}

static void tree_written(void) {
    set_up();
    cache_chunk.using_fst_bitmaps = true;
    CHECK(update_chunk_cache_tree(&volume, &cache_chunk, NULL) == STATUS_SUCCESS);
    CHECK(allocations == TREE_ALLOCATIONS && !cache_chunk.using_fst_bitmaps);
    CHECK(insert_count == COUNT(merged_ranges) && memcmp(inserted, merged_ranges, sizeof(merged_ranges)) == 0);
    CHECK(info_updates == 1 && info_extent_count == COUNT(merged_ranges));
    tear_down("written tree");
}

/* Fails each allocation in turn. */
static void tree_allocation_failures(void) {
    for (unsigned k = 1; k <= TREE_ALLOCATIONS; k++) {
        char step[48];
        snprintf(step, sizeof(step), "allocation %u of %zu failed", k, TREE_ALLOCATIONS);
        set_up();
        fail_allocation = k;
        CHECK(update_chunk_cache_tree(&volume, &cache_chunk, NULL) == STATUS_INSUFFICIENT_RESOURCES);
        CHECK(allocations == k && insert_count == 0 && info_updates == 0);
        tear_down(step);
    }
}

static const range pending_ranges[] = {
    { BASE + 0x14000, BASE + 0x15000 },
    { BASE + 0x40000, BASE + 0x41000 },
};
static const range pending_union[] = {
    { BASE + 0x10000, BASE + 0x12000 },
    { BASE + 0x13000, BASE + 0x15000 },
    { BASE + 0x16000, BASE + 0x17000 },
    { BASE + 0x20000, BASE + 0x22000 },
    { BASE + 0x30000, BASE + 0x31000 },
    { BASE + 0x40000, BASE + 0x41000 },
};
static void pending_written(bool tree_mode, bool fail) {
    unsigned count = (tree_mode ? TREE_ALLOCATIONS : CACHE_ALLOCATIONS) + 1;
    for (unsigned k = fail ? 1 : 0; k <= (fail ? count : 0); k++) {
        set_up();trim_chunk pending = {0};
        build_list(&pending.ranges, pending_ranges, COUNT(pending_ranges));
        InsertTailList(&cache_chunk.pending_trims, &pending.chunk_entry);
        fail_allocation = k;
        NTSTATUS status = tree_mode ? update_chunk_cache_tree(&volume, &cache_chunk, NULL) : write_cache();
        if (fail) { CHECK(status == STATUS_INSUFFICIENT_RESOURCES); CHECK(!writes && !insert_count); }
        else if (tree_mode) {CHECK(status == STATUS_SUCCESS && insert_count == COUNT(pending_union));CHECK(!memcmp(inserted,pending_union,sizeof(pending_union)));}
        else {
            CHECK(status == STATUS_SUCCESS && header.num_entries == COUNT(pending_union));
            const uint8_t* at = written + 2 * sizeof(uint32_t) + sizeof(uint64_t);
            for(size_t i=0;i<COUNT(pending_union);i++,at+=sizeof(struct btrfs_free_space_entry)) {
                const struct btrfs_free_space_entry* e=(const void*)at;
                CHECK(e->offset==pending_union[i].start && e->bytes==pending_union[i].end-pending_union[i].start);
            }
        }
        check_list(&pending.ranges,pending_ranges,COUNT(pending_ranges));
        free_list(&pending.ranges);RemoveEntryList(&pending.chunk_entry);tear_down("pending reservations");
    }
}
static void pending_cache(void){pending_written(false,false);}
static void pending_tree(void){pending_written(true,false);}
static void pending_cache_oom(void){pending_written(false,true);}
static void pending_tree_oom(void){pending_written(true,true);}

static const struct { const char* name; void (*run)(void); } cases[] = {
    { "cache-includes-pending-reservations", pending_cache },
    { "tree-includes-pending-reservations", pending_tree },
    { "cache-pending-reservation-oom", pending_cache_oom },
    { "tree-pending-reservation-oom", pending_tree_oom },
    { "update-chunk-cache-written", cache_written },
    { "update-chunk-cache-allocation-failures", cache_allocation_failures },
    { "update-chunk-cache-flush-failure", cache_flush_failure },
    { "update-chunk-cache-tree-written", tree_written },
    { "update-chunk-cache-tree-allocation-failures", tree_allocation_failures },
};

int main(int argc, char** argv) {
    CHECK(argc == 2);
    for (size_t i = 0; i < COUNT(cases); i++) {
        if (strcmp(argv[1], "--list") == 0) puts(cases[i].name);
        else if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            CHECK(leaks == 0);
            return 0;
        }
    }
    CHECK(strcmp(argv[1], "--list") == 0);
    return 0;
}
