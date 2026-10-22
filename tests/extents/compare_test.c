/* Run rationalize_extents on generated extent lists: compare each result with
 * another revision of the function, bound the loop conditions it evaluates on
 * large files, and time it. */
#define _POSIX_C_SOURCE 200809L
#define SERVICE extern
#include <time.h>
#include "host.h"

NTSTATUS candidate_rationalize_extents(fcb* fcb, PIRP Irp);
#if HAVE_BASELINE
NTSTATUS baseline_rationalize_extents(fcb* fcb, PIRP Irp);
#endif

typedef NTSTATUS (*implementation)(fcb*, PIRP);

enum operation { LOOKUP, UPDATE, ADD, CHECKSUM, LOAD, SPACE, LOCK, UNLOCK, OPERATIONS };
static const char* const operation_names[OPERATIONS] = {
    "lookup", "update", "add", "checksum", "load", "space", "lock", "unlock" };

/* One service call with all its arguments. Chunks are recorded by index, so runs
 * on separate copies of a layout record equal calls. */
typedef struct {
    enum operation op;
    int chunk;
    uint64_t address, size, root, inode, offset;
    int64_t count;
    bool no_csum, superseded;
    const void* irp;
    const void* rollback;
    const void* csum;
} call;

typedef struct { uint64_t offset, length; bool cache_loaded; } chunk_spec;
typedef struct { uint64_t offset; bool unique; struct btrfs_file_extent_item data; } extent_spec;
typedef struct {
    unsigned num_chunks, num_extents;
    chunk_spec* chunks;
    extent_spec* extents;
    bool no_csum;
} layout;

/* One call of an implementation on its own copy of a layout. */
typedef struct {
    device_extension volume;
    root subvol;
    fcb file;
    extent* extents;
    struct btrfs_chunk* items;
    chunk* chunks;
    unsigned num_chunks, num_extents;
    call* calls;
    size_t num_calls, capacity;
    uint64_t counts[OPERATIONS];
    unsigned failable, fail_call, allocations, fail_allocation;
    NTSTATUS failure, status;
    bool failed;
    long live;
    unsigned long long steps;
} run;

typedef struct {
    unsigned merged, merged_to_limit, trimmed_start, trimmed_end, missing_chunk, nothing_allocated;
    unsigned failures[OPERATIONS];
} coverage;

#define POOL_MAGIC 0x6c6f6f7065737421ULL
#define RANDOM_CASES 16
#define TRIALS 250

static const uint64_t page = 4096, gib = 0x40000000;
static IRP request = { .marker = 0x5678 };
static run* current;
static bool recording = true;
static uint64_t random_state;
unsigned long long loop_steps;

static uint64_t next_random(void) {
    uint64_t z = (random_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint64_t rnd(uint64_t n) { return n ? next_random() % n : 0; }

void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK(pool == PagedPool && tag == ALLOC_TAG && size > 0);
    if (++current->allocations == current->fail_allocation)
        return NULL;
    uint64_t* p = malloc(size + 2 * sizeof(uint64_t));
    CHECK(p != NULL);
    p[0] = POOL_MAGIC;
    p[1] = size;
    if (recording)
        memset(p + 2, 0xa5, size);
    current->live++;
    return p + 2;
}

void ExFreePool(void* q) {
    CHECK(q != NULL);
    uint64_t* p = (uint64_t*)q - 2;
    CHECK(p[0] == POOL_MAGIC);
    p[0] = 0;
    if (recording)
        memset(q, 0x5a, p[1]);
    current->live--;
    free(p);
}

static int chunk_index(const chunk* c) {
    if (!c)
        return -1;
    CHECK(c >= current->chunks && c < current->chunks + current->num_chunks);
    return (int)(c - current->chunks);
}

static call* record(enum operation op, const chunk* c) {
    int index = chunk_index(c);
    current->counts[op]++;
    if (!recording)
        return NULL;
    if (current->num_calls == current->capacity) {
        current->capacity = current->capacity ? 2 * current->capacity : 64;
        current->calls = realloc(current->calls, current->capacity * sizeof(call));
        CHECK(current->calls != NULL);
    }
    call* e = &current->calls[current->num_calls++];
    memset(e, 0, sizeof(*e));
    e->op = op;
    e->chunk = index;
    return e;
}

static NTSTATUS result(void) {
    if (++current->failable == current->fail_call) {
        current->failed = true;
        return current->failure;
    }
    return STATUS_SUCCESS;
}

static bool any_chunk_locked(void) {
    for (unsigned i = 0; i < current->num_chunks; i++) {
        if (current->chunks[i].locks)
            return true;
    }
    return false;
}

chunk* get_chunk_from_address(device_extension* v, uint64_t address) {
    chunk* found = NULL;
    CHECK(v == &current->volume);
    for (unsigned i = 0; i < current->num_chunks && !found; i++) {
        chunk* c = &current->chunks[i];
        if (address >= c->offset && address < c->offset + c->chunk_item->length)
            found = c;
    }
    call* e = record(LOOKUP, found);
    if (e)
        e->address = address;
    return found;
}

NTSTATUS update_changed_extent_ref(device_extension* v, chunk* c, uint64_t address,
    uint64_t size, uint64_t root_id, uint64_t inode, uint64_t offset, int32_t count,
    bool no_csum, bool superseded, PIRP irp, LIST_ENTRY* rollback) {
    CHECK(v == &current->volume && c != NULL && !any_chunk_locked());
    call* e = record(UPDATE, c);
    if (e) {
        e->address = address; e->size = size; e->root = root_id; e->inode = inode;
        e->offset = offset; e->count = count; e->no_csum = no_csum;
        e->superseded = superseded; e->irp = irp; e->rollback = rollback;
    }
    return result();
}

NTSTATUS add_changed_extent_ref(chunk* c, uint64_t address, uint64_t size,
    uint64_t root_id, uint64_t inode, uint64_t offset, uint32_t count,
    bool no_csum, LIST_ENTRY* rollback) {
    CHECK(c != NULL && !any_chunk_locked());
    call* e = record(ADD, c);
    if (e) {
        e->address = address; e->size = size; e->root = root_id; e->inode = inode;
        e->offset = offset; e->count = count; e->no_csum = no_csum; e->rollback = rollback;
    }
    return result();
}

NTSTATUS add_checksum_entry(device_extension* v, uint64_t address, ULONG length,
    void* csum, PIRP irp) {
    CHECK(v == &current->volume && !any_chunk_locked());
    call* e = record(CHECKSUM, NULL);
    if (e) {
        e->address = address; e->size = length; e->csum = csum; e->irp = irp;
    }
    return result();
}

void acquire_chunk_lock(chunk* c, device_extension* v) {
    CHECK(v == &current->volume && c != NULL && !any_chunk_locked());
    c->locks++;
    record(LOCK, c);
}

void release_chunk_lock(chunk* c, device_extension* v) {
    CHECK(v == &current->volume && c != NULL && c->locks == 1);
    c->locks--;
    record(UNLOCK, c);
}

NTSTATUS load_cache_chunk(device_extension* v, chunk* c, PIRP irp) {
    CHECK(v == &current->volume && c != NULL && c->locks == 1);
    call* e = record(LOAD, c);
    if (e)
        e->irp = irp;
    NTSTATUS status = result();
    if (NT_SUCCESS(status))
        c->cache_loaded = true;
    return status;
}

NTSTATUS space_list_add(chunk* c, uint64_t address, uint64_t length,
    LIST_ENTRY* rollback) {
    CHECK(c != NULL && c->locks == 1);
    call* e = record(SPACE, c);
    if (e) {
        e->address = address; e->size = length; e->rollback = rollback;
    }
    return result();
}

static bool qualifies(const extent_spec* e) {
    return (e->data.type == BTRFS_FILE_EXTENT_REG || e->data.type == BTRFS_FILE_EXTENT_PREALLOC) &&
           e->data.compression == BTRFS_COMPRESS_NONE && e->unique && e->data.disk_num_bytes != 0;
}

static extent_spec* add_extent(layout* l, unsigned* capacity) {
    if (l->num_extents == *capacity) {
        *capacity = *capacity ? 2 * *capacity : 16;
        l->extents = realloc(l->extents, *capacity * sizeof(extent_spec));
        CHECK(l->extents != NULL);
    }
    extent_spec* e = &l->extents[l->num_extents++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void free_layout(layout* l) {
    free(l->chunks);
    free(l->extents);
}

/* A disk extent of a random layout. */
typedef struct {
    uint64_t address, length;
    uint8_t type, compression;
    bool unique;
} disk_spec;

typedef struct { unsigned disk; uint64_t offset, num_bytes; } reference;

static uint64_t big_length(void) {
    static const uint64_t lengths[] = {
        MAX_EXTENT_SIZE / 2, MAX_EXTENT_SIZE / 2 - 4096, MAX_EXTENT_SIZE / 2 + 4096,
        MAX_EXTENT_SIZE / 4, MAX_EXTENT_SIZE - 4096, MAX_EXTENT_SIZE };
    return rnd(4) == 0 ? (1 + rnd(8)) * page : lengths[rnd(sizeof(lengths) / sizeof(lengths[0]))];
}

/* Disk extents laid out in 1 to 4 chunks, adjacent or with gaps between them,
 * some placed straight after the one before; one to three file extents refer to
 * parts of each, in roughly physical order, with holes, an inline extent and
 * compressed, shared or preallocated extents mixed in. */
static void make_random_layout(layout* l, uint64_t seed) {
    unsigned capacity = 0, num_disks = 0, num_refs = 0;
    random_state = seed;
    memset(l, 0, sizeof(*l));
    bool big = rnd(8) == 0, shared = rnd(32) == 0, outside = rnd(32) == 0;
    unsigned target = rnd(10) == 0 ? 100 + (unsigned)rnd(200) : 1 + (unsigned)rnd(24);
    l->no_csum = rnd(2);
    l->num_chunks = 1 + (unsigned)rnd(4);
    l->chunks = calloc(l->num_chunks, sizeof(chunk_spec));
    CHECK(l->chunks != NULL);
    uint64_t address = (1 + rnd(4)) * gib;
    for (unsigned i = 0; i < l->num_chunks; i++) {
        l->chunks[i].offset = address;
        l->chunks[i].length = big ? gib : (16 + rnd(64)) * (target / 16 + 1) * page;
        l->chunks[i].cache_loaded = rnd(3) != 0;
        address += l->chunks[i].length + (rnd(2) ? 0 : (1 + rnd(16)) * page);
    }

    disk_spec* disks = calloc(target + 1, sizeof(disk_spec));
    CHECK(disks != NULL);
    for (unsigned i = 0; i < l->num_chunks && num_disks < target; i++) {
        uint64_t end = l->chunks[i].offset + l->chunks[i].length;
        uint64_t pos = l->chunks[i].offset + (rnd(3) ? 0 : rnd(4) * page);
        while (num_disks < target && pos < end) {
            uint64_t length = big ? big_length() : (1 + rnd(8)) * page;
            if (pos + length > end) {
                /* Sometimes fill the chunk to its end, next to the next chunk's first extent. */
                if (rnd(2))
                    break;
                length = end - pos;
            }
            disks[num_disks++] = (disk_spec){ .address = pos, .length = length };
            pos += length + (rnd(2) ? 0 : (1 + rnd(3)) * page);
        }
    }
    if (outside)
        disks[num_disks++] = (disk_spec){ .address = address + rnd(4) * page, .length = (1 + rnd(4)) * page };
    for (unsigned i = 0; i < num_disks; i++) {
        disks[i].type = rnd(8) == 0 ? BTRFS_FILE_EXTENT_PREALLOC : BTRFS_FILE_EXTENT_REG;
        disks[i].compression = rnd(16) == 0 ? BTRFS_COMPRESS_ZLIB : BTRFS_COMPRESS_NONE;
        disks[i].unique = !shared && rnd(16) != 0;
    }

    reference* refs = calloc(3 * num_disks + 1, sizeof(reference));
    CHECK(refs != NULL);
    for (unsigned i = 0; i < num_disks; i++) {
        unsigned count = rnd(10) < 7 ? 1 : rnd(10) < 7 ? 2 : 3;
        uint64_t pages = disks[i].length / page;
        for (unsigned j = 0; j < count; j++) {
            uint64_t first = 0, number = pages;
            if (count > 1 || rnd(3) == 0) {
                first = rnd(pages);
                number = 1 + rnd(pages - first);
            }
            refs[num_refs++] = (reference){ i, first * page, number * page };
        }
    }
    if (rnd(4) == 0) {
        for (unsigned i = num_refs; i > 1; i--) {
            unsigned j = (unsigned)rnd(i);
            reference t = refs[i - 1]; refs[i - 1] = refs[j]; refs[j] = t;
        }
    } else {
        for (unsigned i = 0; i + 1 < num_refs; i++) {
            if (rnd(5) == 0) {
                reference t = refs[i]; refs[i] = refs[i + 1]; refs[i + 1] = t;
            }
        }
    }

    uint64_t file_pos = 0;
    if (rnd(32) == 0) {
        extent_spec* e = add_extent(l, &capacity);
        e->unique = true;
        e->data = (struct btrfs_file_extent_item){ .generation = 1, .ram_bytes = 100,
            .type = BTRFS_FILE_EXTENT_INLINE, .disk_bytenr = next_random(),
            .disk_num_bytes = next_random(), .offset = next_random(), .num_bytes = next_random() };
        file_pos = page;
    }
    for (unsigned i = 0; i < num_refs; i++) {
        if (rnd(8) == 0) {
            extent_spec* hole = add_extent(l, &capacity);
            uint64_t length = (1 + rnd(4)) * page;
            hole->offset = file_pos;
            hole->unique = true;
            hole->data = (struct btrfs_file_extent_item){ .generation = 1, .ram_bytes = length,
                .type = BTRFS_FILE_EXTENT_REG, .num_bytes = length };
            file_pos += length;
        }
        const disk_spec* d = &disks[refs[i].disk];
        extent_spec* e = add_extent(l, &capacity);
        e->offset = file_pos;
        e->unique = d->unique && rnd(32) != 0;
        e->data = (struct btrfs_file_extent_item){ .generation = 1 + rnd(1000),
            .ram_bytes = d->compression ? 2 * d->length : d->length, .compression = d->compression,
            .type = d->type, .disk_bytenr = d->address, .disk_num_bytes = d->length,
            .offset = refs[i].offset, .num_bytes = refs[i].num_bytes };
        file_pos += refs[i].num_bytes;
    }
    free(refs);
    free(disks);
}

/* A file written like a VM disk image: `count` extents of 1-16 pages in file
 * order, each in its own disk extent at a random free 64-page slot of chunks of
 * chunk_size bytes, enough of them for twice as many slots as extents. One in
 * eight is allocated straight after the extent before it, so the two merge, and
 * one in sixteen leaves 1-4 pages of its disk extent unused at the start, the
 * end or both. */
static void make_vm_layout(layout* l, unsigned count, uint64_t chunk_size, uint64_t seed) {
    const uint64_t slot = 64 * page;
    const unsigned slots_per_chunk = (unsigned)(chunk_size / slot);
    random_state = seed;
    memset(l, 0, sizeof(*l));
    l->num_chunks = (2 * count + slots_per_chunk - 1) / slots_per_chunk;
    l->chunks = calloc(l->num_chunks, sizeof(chunk_spec));
    CHECK(l->chunks != NULL);
    for (unsigned i = 0; i < l->num_chunks; i++)
        l->chunks[i] = (chunk_spec){ .offset = gib + i * chunk_size, .length = chunk_size, .cache_loaded = true };

    unsigned num_slots = l->num_chunks * slots_per_chunk, next_slot = 0;
    unsigned* slots = malloc(num_slots * sizeof(unsigned));
    CHECK(slots != NULL);
    for (unsigned i = 0; i < num_slots; i++)
        slots[i] = i;
    for (unsigned i = num_slots; i > 1; i--) {
        unsigned j = (unsigned)rnd(i), t = slots[i - 1];
        slots[i - 1] = slots[j]; slots[j] = t;
    }

    l->num_extents = count;
    l->extents = calloc(count, sizeof(extent_spec));
    CHECK(l->extents != NULL);
    uint64_t file_pos = 0, slot_used = 0;
    for (unsigned k = 0; k < count; k++) {
        uint64_t num_bytes = (1 + rnd(16)) * page;
        uint64_t unused = rnd(16) == 0 ? (1 + rnd(4)) * page : 0;
        uint64_t skip = rnd(unused / page + 1) * page;
        uint64_t length = num_bytes + unused, address;
        if (k > 0 && rnd(8) == 0 && slot_used + length <= slot) {
            const extent_spec* prev = &l->extents[k - 1];
            address = prev->data.disk_bytenr + prev->data.disk_num_bytes;
            slot_used += length;
        } else {
            address = gib + slots[next_slot++] * slot;
            slot_used = length;
        }
        l->extents[k] = (extent_spec){ .offset = file_pos, .unique = true, .data = {
            .generation = 1, .ram_bytes = length, .compression = BTRFS_COMPRESS_NONE,
            .type = BTRFS_FILE_EXTENT_REG, .disk_bytenr = address, .disk_num_bytes = length,
            .offset = skip, .num_bytes = num_bytes } };
        file_pos += num_bytes;
    }
    free(slots);
}

static void start(run* r, const layout* l) {
    memset(r, 0, sizeof(*r));
    r->volume.sector_shift = 12;
    r->subvol.id = 0x1234;
    r->file.Vcb = &r->volume;
    r->file.subvol = &r->subvol;
    r->file.inode = 0xabcdef;
    r->file.inode_item.flags = l->no_csum ? BTRFS_INODE_NODATASUM : 0;
    InitializeListHead(&r->file.extents);
    r->num_extents = l->num_extents;
    r->extents = calloc(l->num_extents + 1, sizeof(extent));
    CHECK(r->extents != NULL);
    for (unsigned i = 0; i < l->num_extents; i++) {
        extent* ext = &r->extents[i];
        ext->offset = l->extents[i].offset;
        ext->unique = l->extents[i].unique;
        ext->extent_data = l->extents[i].data;
        InsertHeadList(r->file.extents.Blink, &ext->list_entry);
    }
    r->num_chunks = l->num_chunks;
    r->items = calloc(l->num_chunks + 1, sizeof(struct btrfs_chunk));
    r->chunks = calloc(l->num_chunks + 1, sizeof(chunk));
    CHECK(r->items != NULL && r->chunks != NULL);
    for (unsigned i = 0; i < l->num_chunks; i++) {
        r->items[i].length = l->chunks[i].length;
        r->chunks[i] = (chunk){ .offset = l->chunks[i].offset, .used = l->chunks[i].length,
                                .chunk_item = &r->items[i], .cache_loaded = l->chunks[i].cache_loaded };
    }
}

static void finish(run* r) {
    free(r->extents);
    free(r->items);
    free(r->chunks);
    free(r->calls);
    memset(r, 0, sizeof(*r));
}

static void execute(run* r, implementation f, const layout* l, unsigned fail_call,
                    NTSTATUS failure, unsigned fail_allocation) {
    start(r, l);
    r->fail_call = fail_call;
    r->failure = failure;
    r->fail_allocation = fail_allocation;
    current = r;
    loop_steps = 0;
    r->status = f(&r->file, &request);
    r->steps = loop_steps;
    CHECK(r->live == 0 && !any_chunk_locked());
    current = NULL;
}

static void describe(const char* who, size_t index, const call* c) {
    fprintf(stderr, "  %s call %zu: %s chunk %d address %#llx size %#llx root %#llx inode %#llx "
            "offset %#llx count %lld no_csum %d superseded %d irp %p rollback %p csum %p\n",
            who, index, operation_names[c->op], c->chunk, (unsigned long long)c->address,
            (unsigned long long)c->size, (unsigned long long)c->root, (unsigned long long)c->inode,
            (unsigned long long)c->offset, (long long)c->count, c->no_csum, c->superseded,
            c->irp, c->rollback, c->csum);
}

static void describe_extent(const char* who, unsigned index, const extent* ext) {
    const struct btrfs_file_extent_item* d = &ext->extent_data;
    fprintf(stderr, "  %s extent %u at %#llx: disk %#llx+%#llx offset %#llx bytes %#llx ram %#llx\n",
            who, index, (unsigned long long)ext->offset, (unsigned long long)d->disk_bytenr,
            (unsigned long long)d->disk_num_bytes, (unsigned long long)d->offset,
            (unsigned long long)d->num_bytes, (unsigned long long)d->ram_bytes);
}

#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); exit(1); } while (0)

static bool failable(enum operation op) {
    return op == UPDATE || op == ADD || op == CHECKSUM || op == LOAD || op == SPACE;
}

/* Check one run against the layout it started from. A failed allocation must
 * return success without making any call or changing any extent or chunk, so
 * the flush goes on; a baseline from before that change returns
 * STATUS_INSUFFICIENT_RESOURCES instead. */
static void check_run(const run* r, const layout* l, const char* label, bool is_baseline) {
    bool allocation_failed = r->fail_allocation && r->allocations >= r->fail_allocation;
    if (allocation_failed) {
        bool status_ok = r->status == STATUS_SUCCESS ||
                         (is_baseline && r->status == STATUS_INSUFFICIENT_RESOURCES);
        if (!status_ok || r->num_calls != 0)
            FAIL("%s: allocation %u failed, returned %08x after %zu calls\n", label,
                 r->fail_allocation, (unsigned)r->status, r->num_calls);
        /* start() makes every chunk full. */
        for (unsigned i = 0; i < l->num_chunks; i++)
            CHECK(r->chunks[i].used == l->chunks[i].length && r->chunks[i].cache_loaded == l->chunks[i].cache_loaded);
    } else if (r->failed) {
        if (r->status != r->failure)
            FAIL("%s: call %u failed with %08x, returned %08x\n", label, r->fail_call,
                 (unsigned)r->failure, (unsigned)r->status);
        CHECK(r->failable == r->fail_call);
        size_t i = r->num_calls;
        while (i > 0 && r->calls[i - 1].op == UNLOCK)
            i--;
        CHECK(i > 0 && failable(r->calls[i - 1].op));
    } else if (r->status == STATUS_INTERNAL_ERROR) {
        CHECK(r->num_calls > 0 && r->calls[r->num_calls - 1].op == LOOKUP);
        CHECK(r->calls[r->num_calls - 1].chunk == -1);
    } else if (r->status != STATUS_SUCCESS) {
        FAIL("%s: returned %08x\n", label, (unsigned)r->status);
    }

    const LIST_ENTRY* le = r->file.extents.Flink;
    for (unsigned i = 0; i < l->num_extents; i++) {
        const extent* ext = &r->extents[i];
        const struct btrfs_file_extent_item* d = &ext->extent_data;
        const struct btrfs_file_extent_item* s = &l->extents[i].data;
        CHECK(le == &ext->list_entry && le->Flink->Blink == le);
        le = le->Flink;
        CHECK(ext->offset == l->extents[i].offset && ext->unique == l->extents[i].unique);
        if (!qualifies(&l->extents[i]) || allocation_failed) {
            CHECK(memcmp(d, s, sizeof(*d)) == 0);
            continue;
        }
        /* Trimming and merging keep each extent on the same bytes on disk. */
        if (d->disk_bytenr + d->offset != s->disk_bytenr + s->offset || d->num_bytes != s->num_bytes ||
            d->generation != s->generation || d->type != s->type || d->compression != s->compression ||
            d->encryption != s->encryption || d->other_encoding != s->other_encoding) {
            describe_extent("before", i, &(extent){ .offset = l->extents[i].offset, .extent_data = *s });
            describe_extent("after", i, ext);
            FAIL("%s: extent %u moved\n", label, i);
        }
        if (r->status == STATUS_SUCCESS) {
            CHECK(d->offset + d->num_bytes <= d->disk_num_bytes);
            CHECK(d->ram_bytes == d->disk_num_bytes);
        }
    }
    CHECK(le == &r->file.extents);
}

static void compare_runs(const run* a, const run* b, const layout* l, const char* label) {
    if (a->status != b->status)
        FAIL("%s: baseline returned %08x, candidate %08x\n", label, (unsigned)a->status, (unsigned)b->status);
    size_t common = min(a->num_calls, b->num_calls);
    for (size_t i = 0; i < common; i++) {
        if (memcmp(&a->calls[i], &b->calls[i], sizeof(call)) != 0) {
            describe("baseline", i, &a->calls[i]);
            describe("candidate", i, &b->calls[i]);
            FAIL("%s: call %zu differs\n", label, i);
        }
    }
    if (a->num_calls != b->num_calls) {
        if (a->num_calls > common)
            describe("baseline", common, &a->calls[common]);
        else
            describe("candidate", common, &b->calls[common]);
        FAIL("%s: baseline made %zu calls, candidate %zu\n", label, a->num_calls, b->num_calls);
    }
    for (unsigned i = 0; i < l->num_extents; i++) {
        if (memcmp(&a->extents[i].extent_data, &b->extents[i].extent_data, sizeof(struct btrfs_file_extent_item))) {
            describe_extent("baseline", i, &a->extents[i]);
            describe_extent("candidate", i, &b->extents[i]);
            FAIL("%s: extent %u differs\n", label, i);
        }
    }
    for (unsigned i = 0; i < l->num_chunks; i++) {
        if (a->chunks[i].used != b->chunks[i].used || a->chunks[i].cache_loaded != b->chunks[i].cache_loaded)
            FAIL("%s: chunk %u differs\n", label, i);
    }
}

static void note_coverage(coverage* cov, const run* r, const layout* l) {
    if (r->failed) {
        size_t i = r->num_calls;
        while (i > 0 && r->calls[i - 1].op == UNLOCK)
            i--;
        cov->failures[r->calls[i - 1].op]++;
        return;
    }
    if (r->status == STATUS_INTERNAL_ERROR)
        cov->missing_chunk++;
    if (r->allocations == 0)
        cov->nothing_allocated++;
    if (r->status != STATUS_SUCCESS)
        return;
    for (unsigned i = 0; i < l->num_extents; i++) {
        const struct btrfs_file_extent_item* d = &r->extents[i].extent_data;
        const struct btrfs_file_extent_item* s = &l->extents[i].data;
        if (!qualifies(&l->extents[i]))
            continue;
        if (d->disk_bytenr > s->disk_bytenr)
            cov->trimmed_start++;
        if (d->disk_bytenr + d->disk_num_bytes < s->disk_bytenr + s->disk_num_bytes)
            cov->trimmed_end++;
        if (d->disk_num_bytes > s->disk_num_bytes)
            cov->merged++;
        if (d->disk_num_bytes > s->disk_num_bytes && d->disk_num_bytes == MAX_EXTENT_SIZE)
            cov->merged_to_limit++;
    }
}

/* Up to `limit` numbers from 1 to n: all of them, or 1, n and others at random. */
static unsigned sample(unsigned n, unsigned limit, unsigned* out) {
    if (n <= limit) {
        for (unsigned i = 0; i < n; i++)
            out[i] = i + 1;
        return n;
    }
    out[0] = 1;
    out[1] = n;
    for (unsigned i = 2; i < limit; i++)
        out[i] = 2 + (unsigned)rnd(n - 2);
    return limit;
}

static void trial(uint64_t seed, implementation baseline, coverage* cov) {
    static run a, b;
    unsigned points[12];
    char label[96];
    layout l;
    make_random_layout(&l, seed);

    snprintf(label, sizeof(label), "seed %llu", (unsigned long long)seed);
    execute(&b, candidate_rationalize_extents, &l, 0, 0, 0);
    check_run(&b, &l, label, false);
    note_coverage(cov, &b, &l);
    unsigned failable_calls = b.failable, candidate_allocations = b.allocations;
    unsigned baseline_allocations = 0;
    if (baseline) {
        execute(&a, baseline, &l, 0, 0, 0);
        check_run(&a, &l, label, true);
        compare_runs(&a, &b, &l, label);
        baseline_allocations = a.allocations;
        finish(&a);
    }
    finish(&b);

    unsigned n = sample(failable_calls, 12, points);
    for (unsigned i = 0; i < n; i++) {
        NTSTATUS failure = rnd(2) ? STATUS_IO_DEVICE_ERROR : STATUS_INSUFFICIENT_RESOURCES;
        snprintf(label, sizeof(label), "seed %llu, call %u failing", (unsigned long long)seed, points[i]);
        execute(&b, candidate_rationalize_extents, &l, points[i], failure, 0);
        check_run(&b, &l, label, false);
        note_coverage(cov, &b, &l);
        if (baseline) {
            execute(&a, baseline, &l, points[i], failure, 0);
            check_run(&a, &l, label, true);
            compare_runs(&a, &b, &l, label);
            finish(&a);
        }
        finish(&b);
    }

    /* The two revisions may allocate differently, so each one's allocation
     * failures are checked on their own: no call is made and nothing changes. */
    n = sample(candidate_allocations, 4, points);
    for (unsigned i = 0; i < n; i++) {
        snprintf(label, sizeof(label), "seed %llu, candidate allocation %u failing", (unsigned long long)seed, points[i]);
        execute(&b, candidate_rationalize_extents, &l, 0, 0, points[i]);
        check_run(&b, &l, label, false);
        finish(&b);
    }
    n = sample(baseline_allocations, 4, points);
    for (unsigned i = 0; i < n; i++) {
        snprintf(label, sizeof(label), "seed %llu, baseline allocation %u failing", (unsigned long long)seed, points[i]);
        execute(&a, baseline, &l, 0, 0, points[i]);
        check_run(&a, &l, label, true);
        finish(&a);
    }
    free_layout(&l);
}

static void random_case(unsigned n, implementation baseline) {
    coverage cov = { 0 };
    for (unsigned t = 0; t < TRIALS; t++)
        trial(100000 * (uint64_t)n + t, baseline, &cov);
    printf("random-%u: merged %u, trimmed at start %u, at end %u, merged to 128 MiB %u, "
           "missing chunk %u, nothing allocated %u, failed calls: update %u, add %u, checksum %u, "
           "load %u, space %u\n", n, cov.merged, cov.trimmed_start, cov.trimmed_end, cov.merged_to_limit,
           cov.missing_chunk, cov.nothing_allocated, cov.failures[UPDATE], cov.failures[ADD],
           cov.failures[CHECKSUM], cov.failures[LOAD], cov.failures[SPACE]);
    CHECK(cov.merged && cov.trimmed_start && cov.trimmed_end && cov.missing_chunk && cov.nothing_allocated);
    CHECK(cov.failures[UPDATE] && cov.failures[ADD] && cov.failures[CHECKSUM]);
    CHECK(cov.failures[LOAD] && cov.failures[SPACE]);
}

static unsigned ceil_log2(unsigned n) {
    unsigned bits = 0;
    while ((1u << bits) < n)
        bits++;
    return bits;
}

/* Sorting the extents by address and looking each one up in the sorted ranges
 * takes about E*ceil(log2 E) loop steps each; the rest is a few passes over the
 * extents and ranges. */
static unsigned long long step_limit(unsigned count) {
    return 4ULL * count * ceil_log2(count) + 16ULL * count;
}

static void steps_case(const char* name, unsigned count, uint64_t chunk_size) {
    static run r;
    layout l;
    make_vm_layout(&l, count, chunk_size, count);
    execute(&r, candidate_rationalize_extents, &l, 0, 0, 0);
    check_run(&r, &l, name, false);
    /* Ranges were trimmed and merged and every chunk was looked up, so every
     * part of the function ran. */
    CHECK(r.status == STATUS_SUCCESS && r.counts[SPACE] > 0 && r.counts[UPDATE] > r.counts[SPACE]);
    CHECK(r.counts[LOOKUP] == l.num_chunks);
    printf("%s: %llu loop steps, limit %llu, %u chunks\n", name, r.steps, step_limit(count), l.num_chunks);
    CHECK(r.steps <= step_limit(count));
    finish(&r);
    free_layout(&l);
}

static double milliseconds(const struct timespec* a, const struct timespec* b) {
    return (double)(b->tv_sec - a->tv_sec) * 1e3 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

static void bench(implementation baseline) {
    static run r;
    struct { const char* name; implementation f; } impls[] = {
        { "baseline", baseline }, { "candidate", candidate_rationalize_extents } };
    recording = false;
    printf("%-10s %8s %8s %8s %8s %14s\n", "", "extents", "updates", "checksum", "space", "best of 5");
    for (unsigned count = 5000; count <= 40000; count *= 2) {
        layout l;
        make_vm_layout(&l, count, gib, count);
        for (unsigned i = 0; i < 2; i++) {
            double best = 0;
            if (!impls[i].f)
                continue;
            for (unsigned rep = 0; rep < 5; rep++) {
                struct timespec t0, t1;
                start(&r, &l);
                current = &r;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                r.status = impls[i].f(&r.file, &request);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                CHECK(r.status == STATUS_SUCCESS && r.live == 0);
                double ms = milliseconds(&t0, &t1);
                if (rep == 0 || ms < best)
                    best = ms;
                if (rep == 4)
                    printf("%-10s %8u %8llu %8llu %8llu %11.3f ms\n", impls[i].name, count,
                           (unsigned long long)r.counts[UPDATE], (unsigned long long)r.counts[CHECKSUM],
                           (unsigned long long)r.counts[SPACE], best);
                current = NULL;
                finish(&r);
            }
            fflush(stdout);
        }
        free_layout(&l);
    }
}

int main(int argc, char** argv) {
    implementation baseline = NULL;
#if HAVE_BASELINE
    baseline = baseline_rationalize_extents;
#endif
    CHECK(argc == 2);
    if (strcmp(argv[1], "--bench") == 0) {
        bench(baseline);
        return 0;
    }
    bool list = strcmp(argv[1], "--list") == 0;
    for (unsigned n = 0; n < RANDOM_CASES; n++) {
        char name[32];
        snprintf(name, sizeof(name), "random-%u", n);
        if (list)
            puts(name);
        else if (strcmp(argv[1], name) == 0) {
            random_case(n, baseline);
            return 0;
        }
    }
    for (unsigned count = 5000; count <= 40000; count *= 2) {
        char name[32];
        snprintf(name, sizeof(name), "steps-%u", count);
        if (list)
            puts(name);
        else if (strcmp(argv[1], name) == 0) {
            steps_case(name, count, gib);
            return 0;
        }
    }
    /* 625 chunks of 16 MiB, so code that walks the ranges after each chunk
     * lookup exceeds the limit. */
    if (list)
        puts("steps-small-chunks");
    else if (strcmp(argv[1], "steps-small-chunks") == 0) {
        steps_case(argv[1], 20000, 16 * 1024 * 1024);
        return 0;
    }
    CHECK(list);
    return 0;
}
