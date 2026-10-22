/* Deterministic VM-style extent fixture and service-call recorder.
 * Retains the layout and native stubs needed to generate the original replay,
 * without depending on another test suite or a fork-only Git object. */
#define SERVICE extern
#include "trace-host.h"

enum operation { LOOKUP, UPDATE, ADD, CHECKSUM, LOAD, SPACE, LOCK, UNLOCK, OPERATIONS };
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

#define POOL_MAGIC 0x6c6f6f7065737421ULL
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

#define rationalize_extents candidate_rationalize_extents
#include "ranges.inc"

int main(int argc, char** argv) {
    static run r;
    layout l;
    CHECK(argc == 3);
    unsigned count = (unsigned)atoi(argv[1]);
    make_vm_layout(&l, count, gib, count);
    start(&r, &l);
    current = &r;
    r.status = candidate_rationalize_extents(&r.file, &request);
    CHECK(r.status == STATUS_SUCCESS && r.live == 0);
    FILE* f = fopen(argv[2], "w");
    CHECK(f != NULL);
    fprintf(f, "%u 0.000\n", l.num_chunks);
    for (size_t i = 0; i < r.num_calls; i++) {
        const call* c = &r.calls[i];
        if (c->op == UPDATE || c->op == ADD)
            fprintf(f, "%c %d %llu %llu %llu %llu %llu %lld\n", c->op == UPDATE ? 'U' : 'A', c->chunk,
                    (unsigned long long)c->address, (unsigned long long)c->size,
                    (unsigned long long)c->root, (unsigned long long)c->inode,
                    (unsigned long long)c->offset, (long long)c->count);
    }
    CHECK(fclose(f) == 0);
    free(r.calls); free(r.extents); free(r.items); free(r.chunks);
    free(l.extents); free(l.chunks);
    return 0;
}
