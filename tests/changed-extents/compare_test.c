/* Compare production indexed updates with the original list implementation. */
#define CE_TEST 1
#include "host.h"

enum { HEAP_CAPACITY = 4096, WORLD_CHUNKS = 4 };
typedef struct {
    chunk chunks[WORLD_CHUNKS];
    LIST_ENTRY rollback;
    void* heap[HEAP_CAPACITY];
    unsigned calls, frees, live, fail_at;
    bool indexed;
} world;

static world* current;

static void* ce_allocate(size_t size) {
    unsigned id = current->calls++;
    CHECK(id < HEAP_CAPACITY);
    if (current->calls == current->fail_at)
        return NULL;
    void* value = malloc(size);
    CHECK(value != NULL);
    memset(value, 0xa5, size);
    current->heap[id] = value;
    current->live++;
    return value;
}

static void ce_release(void* pointer) {
    CHECK(pointer != NULL);
    for (unsigned i = 0; i < current->calls; i++)
        if (current->heap[i] == pointer) {
            current->heap[i] = NULL;
            current->live--;
            current->frees++;
            free(pointer);
            return;
        }
    CHECK(false);
}

static unsigned identity(const world* w, const void* pointer) {
    for (unsigned i = 0; i < w->calls; i++)
        if (w->heap[i] == pointer)
            return i + 1;
    CHECK(false);
    return 0;
}

static chunk* get_chunk_from_address(device_extension* vcb, uint64_t address) {
    (void)vcb;
    return &current->chunks[address % WORLD_CHUNKS];
}

static unsigned flush_fault;
static unsigned external_calls;
static NTSTATUS external_status(unsigned stage) {
    external_calls++;
    return flush_fault == stage ? STATUS_INTERNAL_ERROR : STATUS_SUCCESS;
}
static NTSTATUS increase_extent_refcount_data(device_extension* v, uint64_t a, uint64_t s,
        uint64_t r, uint64_t i, uint64_t o, uint32_t n, PIRP p) {
    (void)v; (void)a; (void)s; (void)r; (void)i; (void)o; (void)n; (void)p;
    return external_status(1);
}
static NTSTATUS decrease_extent_refcount_data(device_extension* v, uint64_t a, uint64_t s,
        uint64_t r, uint64_t i, uint64_t o, uint32_t n, bool x, PIRP p) {
    (void)v; (void)a; (void)s; (void)r; (void)i; (void)o; (void)n; (void)x; (void)p;
    return external_status(2);
}
static NTSTATUS space_list_add(chunk* c, uint64_t a, uint64_t s, LIST_ENTRY* rb) {
    (void)c; (void)a; (void)s; (void)rb; return external_status(3);
}
static int keycmp(struct btrfs_key a, struct btrfs_key b) {
    return a.objectid != b.objectid || a.type != b.type || a.offset != b.offset;
}
#define RtlCopyMemory memcpy
static NTSTATUS insert_tree_item(device_extension* v, root* r, uint64_t a, uint8_t t,
        uint64_t s, void* d, uint16_t n, void* rb, PIRP p) {
    (void)v; (void)r; (void)a; (void)t; (void)s; (void)n; (void)rb; (void)p;
    NTSTATUS status = external_status(4);
    if (NT_SUCCESS(status) && d)
        ExFreePool(d);
    return status;
}
static NTSTATUS delete_tree_item(device_extension* v, traverse_ptr* tp) {
    (void)v; (void)tp; return external_status(5);
}

#include "refs.inc"
#include "flush.inc"
#include "rollback.inc"
#include "terminal.inc"
#include "index-check.h"

#define get_changed_extent_item baseline_get_changed_extent_item
#define update_changed_extent_ref baseline_update_changed_extent_ref
#define add_changed_extent_ref baseline_add_changed_extent_ref
#define flush_changed_extent baseline_flush_changed_extent
#define rollback_one baseline_rollback_one
#include "baseline.inc"
#include "baseline-flush.inc"
#include "baseline-rollback.inc"
#undef get_changed_extent_item
#undef update_changed_extent_ref
#undef add_changed_extent_ref
#undef flush_changed_extent
#undef rollback_one

static void start(world* w, bool indexed) {
    memset(w, 0, sizeof(*w));
    w->indexed = indexed;
    for (unsigned i = 0; i < WORLD_CHUNKS; i++)
        initialize_changed_extents(&w->chunks[i]);
    InitializeListHead(&w->rollback);
}

static void compare_refs(const world* a, const LIST_ENTRY* ah,
                         const world* b, const LIST_ENTRY* bh) {
    const LIST_ENTRY *x = ah->Flink, *y = bh->Flink;
    while (x != ah && y != bh) {
        const changed_extent_ref* ca = CONTAINING_RECORD(x, changed_extent_ref, list_entry);
        const changed_extent_ref* cb = CONTAINING_RECORD(y, changed_extent_ref, list_entry);
        CHECK(identity(a, ca) == identity(b, cb));
        CHECK(ca->type == cb->type);
        if (ca->type == BTRFS_EXTENT_DATA_REF_KEY) {
            CHECK(ca->edr.root == cb->edr.root && ca->edr.objectid == cb->edr.objectid);
            CHECK(ca->edr.offset == cb->edr.offset && ca->edr.count == cb->edr.count);
        } else {
            CHECK(ca->offset == cb->offset && ca->sdr.count == cb->sdr.count);
        }
        x = x->Flink; y = y->Flink;
    }
    CHECK(x == ah && y == bh);
}

static void compare_worlds(const world* a, const world* b) {
    CHECK(a->calls == b->calls && a->frees == b->frees && a->live == b->live);
    for (unsigned c = 0; c < WORLD_CHUNKS; c++) {
        validate_changed_extents(&a->chunks[c]);
        const LIST_ENTRY *ah = &a->chunks[c].changed_extents, *bh = &b->chunks[c].changed_extents;
        const LIST_ENTRY *x = ah->Flink, *y = bh->Flink;
        CHECK(a->chunks[c].used == b->chunks[c].used);
        while (x != ah && y != bh) {
            const changed_extent* ca = CONTAINING_RECORD(x, changed_extent, list_entry);
            const changed_extent* cb = CONTAINING_RECORD(y, changed_extent, list_entry);
            CHECK(identity(a, ca) == identity(b, cb));
            CHECK(ca->address == cb->address && ca->size == cb->size && ca->old_size == cb->old_size);
            CHECK(ca->count == cb->count && ca->old_count == cb->old_count);
            CHECK(ca->no_csum == cb->no_csum && ca->superseded == cb->superseded);
            compare_refs(a, &ca->refs, b, &cb->refs);
            compare_refs(a, &ca->old_refs, b, &cb->old_refs);
            x = x->Flink; y = y->Flink;
        }
        CHECK(x == ah && y == bh);
    }
    const LIST_ENTRY *x = a->rollback.Flink, *y = b->rollback.Flink;
    while (x != &a->rollback && y != &b->rollback) {
        const rollback_item* ra = CONTAINING_RECORD(x, rollback_item, list_entry);
        const rollback_item* rb = CONTAINING_RECORD(y, rollback_item, list_entry);
        CHECK(identity(a, ra) == identity(b, rb) && ra->type == rb->type);
        CHECK(ra->changed_extent_ref.address == rb->changed_extent_ref.address);
        CHECK(ra->changed_extent_ref.size == rb->changed_extent_ref.size);
        CHECK(ra->changed_extent_ref.root == rb->changed_extent_ref.root);
        CHECK(ra->changed_extent_ref.objid == rb->changed_extent_ref.objid);
        CHECK(ra->changed_extent_ref.offset == rb->changed_extent_ref.offset);
        CHECK(ra->changed_extent_ref.count == rb->changed_extent_ref.count);
        x = x->Flink; y = y->Flink;
    }
    CHECK(x == &a->rollback && y == &b->rollback);
}

static void finish(world* w) {
    current = w;
    for (unsigned c = 0; c < WORLD_CHUNKS; c++)
        while (!IsListEmpty(&w->chunks[c].changed_extents)) {
            changed_extent* ce = CONTAINING_RECORD(w->chunks[c].changed_extents.Flink, changed_extent, list_entry);
            while (!IsListEmpty(&ce->refs))
                ExFreePool(CONTAINING_RECORD(RemoveHeadList(&ce->refs), changed_extent_ref, list_entry));
            while (!IsListEmpty(&ce->old_refs))
                ExFreePool(CONTAINING_RECORD(RemoveHeadList(&ce->old_refs), changed_extent_ref, list_entry));
            if (w->indexed) remove_changed_extent(&w->chunks[c], ce);
            else RemoveEntryList(&ce->list_entry);
            ExFreePool(ce);
        }
    while (!IsListEmpty(&w->rollback))
        ExFreePool(CONTAINING_RECORD(RemoveHeadList(&w->rollback), rollback_item, list_entry));
    CHECK(w->live == 0);
}

typedef struct {
    unsigned operation, fail, fault;
    uint64_t address, size, root, inode, offset;
    int32_t count;
    bool rollback, no_csum, superseded;
} command;

static NTSTATUS apply(world* w, const command* op) {
    current = w;
    w->fail_at = op->fail ? w->calls + op->fail : 0;
    lookup_fault = op->fault;
    looked_up_size = op->size;
    root r = {.id = 2};
    device_extension v = {.extent_root = &r};
    chunk* c = &w->chunks[op->address % WORLD_CHUNKS];
    LIST_ENTRY* rb = op->rollback ? &w->rollback : NULL;
    if (op->operation == 0) {
        if (w->indexed)
            return add_changed_extent_ref(c, op->address, op->size, op->root, op->inode,
                                           op->offset, (uint32_t)op->count, op->no_csum, rb);
        return baseline_add_changed_extent_ref(c, op->address, op->size, op->root, op->inode,
                                               op->offset, (uint32_t)op->count, op->no_csum, rb);
    }
    if (w->indexed)
        return update_changed_extent_ref(&v, c, op->address, op->size, op->root, op->inode,
                                          op->offset, op->count, op->no_csum, op->superseded, NULL, rb);
    return baseline_update_changed_extent_ref(&v, c, op->address, op->size, op->root, op->inode,
                                              op->offset, op->count, op->no_csum, op->superseded, NULL, rb);
}

static uint64_t random_value(uint64_t* state) {
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17; return *state;
}

static void compare_apply(world* candidate, world* baseline, const command* op) {
    NTSTATUS expected = apply(baseline, op);
    NTSTATUS actual = apply(candidate, op);
    CHECK(actual == expected);
    compare_worlds(candidate, baseline);
}

static void differential(void) {
    static world candidate, baseline;
    unsigned operations = 0;
    for (unsigned seed = 1; seed <= 32; seed++) {
        start(&candidate, true); start(&baseline, false);
        uint64_t state = seed;
        for (unsigned step = 0; step < 256; step++) {
            command op = {0};
            op.operation = (unsigned)(random_value(&state) % 2);
            op.address = random_value(&state) % 24;
            op.size = 4096 * (1 + random_value(&state) % 2);
            op.root = random_value(&state) % 3;
            op.inode = random_value(&state) % 4;
            op.offset = random_value(&state) % 8;
            op.count = op.operation && (random_value(&state) & 1) ? -1 : 1;
            op.fail = (unsigned)(random_value(&state) % 5);
            op.fault = (unsigned)(random_value(&state) % 7);
            op.rollback = (random_value(&state) & 1) != 0;
            op.no_csum = (random_value(&state) & 1) != 0;
            op.superseded = (random_value(&state) & 1) != 0;
            prior_refs = (uint32_t)(random_value(&state) % 3);
            NTSTATUS expected = apply(&baseline, &op);
            NTSTATUS actual = apply(&candidate, &op);
            CHECK(actual == expected);
            compare_worlds(&candidate, &baseline);
            operations++;
        }
        finish(&candidate); finish(&baseline);
    }
    printf("%u production update/add fault comparisons passed\n", operations);
}

static void allocation_ordinals(void) {
    static world candidate, baseline;
    unsigned cases = 0;
    for (unsigned operation = 0; operation < 2; operation++)
        for (unsigned rollback = 0; rollback < 2; rollback++)
            for (unsigned old_refs = 0; old_refs < 2; old_refs++)
                for (unsigned failure = 0; failure <= 4; failure++)
                    for (unsigned fault = 0; fault < 7; fault++) {
                        start(&candidate, true); start(&baseline, false);
                        command op = {.operation = operation, .address = 8, .size = 4096,
                                      .count = 1, .rollback = rollback != 0,
                                      .fail = failure, .fault = fault};
                        prior_refs = old_refs * 2;
                        compare_apply(&candidate, &baseline, &op);
                        finish(&candidate); finish(&baseline);
                        cases++;
                    }
    start(&candidate, true); start(&baseline, false);
    for (unsigned ref = 0; ref < 128; ref++) {
        command op = {.address = 8, .size = 4096, .offset = ref,
                      .count = 1, .rollback = true};
        compare_apply(&candidate, &baseline, &op);
    }
    for (unsigned ref = 0; ref < 128; ref++) {
        command op = {.address = 8, .size = 4096, .offset = ref,
                      .count = 1, .rollback = true, .fail = 1 + ref % 4};
        compare_apply(&candidate, &baseline, &op);
    }
    finish(&candidate); finish(&baseline);
    printf("%u exhaustive allocation/stub failure cases and long ref-list comparisons passed\n", cases);
}

static void lifecycle_paths(void) {
    static world candidate, baseline;
    for (unsigned scenario = 0; scenario < 12; scenario++) {
        start(&candidate, true); start(&baseline, false);
        command op = {.address = 8, .size = 4096, .count = 1, .rollback = scenario == 0};
        lookup_fault = 0; prior_refs = 1;
        CHECK(apply(&baseline, &op) == STATUS_SUCCESS);
        CHECK(apply(&candidate, &op) == STATUS_SUCCESS);
        compare_worlds(&candidate, &baseline);
        NTSTATUS statuses[2];
        unsigned calls[2];
        world* worlds[] = {&candidate, &baseline};
        for (unsigned i = 0; i < 2; i++) {
            world* w = worlds[i]; current = w;
            root r = {.id = 2}; device_extension v = {.extent_root = &r};
            w->fail_at = 0; lookup_fault = 0; flush_fault = 0; external_calls = 0;
            if (scenario == 0) {
                rollback_item* ri = CONTAINING_RECORD(RemoveHeadList(&w->rollback), rollback_item, list_entry);
                statuses[i] = w->indexed ? rollback_one(&v, ri) : baseline_rollback_one(&v, ri);
            } else {
                changed_extent* ce = CONTAINING_RECORD(w->chunks[0].changed_extents.Flink, changed_extent, list_entry);
                if (scenario == 2) flush_fault = 1; // early failure must leave entry indexed
                if (scenario == 3 || scenario == 4) {
                    ce->count = 0; w->chunks[0].used = ce->size;
                    if (scenario == 4) flush_fault = 3; // terminal free still occurs
                }
                if (scenario == 5) {
                    ce->old_count = 2;
                    changed_extent_ref* ref = ExAllocatePoolWithTag(PagedPool, sizeof(*ref), ALLOC_TAG);
                    memset(ref, 0, sizeof(*ref)); ref->type = BTRFS_EXTENT_DATA_REF_KEY; ref->edr.count = 2;
                    InsertTailList(&ce->old_refs, &ref->list_entry);
                    flush_fault = 2; // second-pass failure after old-ref removal
                }
                if (scenario >= 6) {
                    ce->old_count = 1;
                    ce->old_size = 2048;
                    looked_up_size = ce->old_size;
                    if (scenario == 7) w->fail_at = w->calls + 1;
                    if (scenario == 8) flush_fault = 4;
                    if (scenario == 9) flush_fault = 5;
                    if (scenario == 10) lookup_fault = 1;
                    if (scenario == 11) lookup_fault = 2;
                }
                statuses[i] = w->indexed ? flush_changed_extent(&v, &w->chunks[0], ce, NULL)
                                         : baseline_flush_changed_extent(&v, &w->chunks[0], ce, NULL);
            }
            calls[i] = external_calls;
        }
        CHECK(statuses[0] == statuses[1] && calls[0] == calls[1]);
        compare_worlds(&candidate, &baseline);
        if (!IsListEmpty(&candidate.chunks[0].changed_extents)) {
            changed_extent* ce = CONTAINING_RECORD(candidate.chunks[0].changed_extents.Flink, changed_extent, list_entry);
            current = &candidate;
            CHECK(get_changed_extent_item(&candidate.chunks[0], ce->address, ce->size, false) == ce);
        } else CHECK(candidate.chunks[0].changed_extents_root == NULL);
        finish(&candidate); finish(&baseline);
    }
    puts("12 production rollback/flush lifecycle comparisons passed");
}

static void terminal_cleanup(void) {
#if HAVE_TERMINAL_DRAIN
    static world w;
    start(&w, true);
    prior_refs = 2;
    command op = {.operation = 1, .address = 8, .size = 4096, .count = 1};
    CHECK(apply(&w, &op) == STATUS_SUCCESS);
    op.operation = 0; op.address = 12;
    CHECK(apply(&w, &op) == STATUS_SUCCESS);
    unsigned allocations = w.calls, frees = w.frees, live = w.live;
    current = &w;
    terminal_drain(&w.chunks[0]);
    validate_changed_extents(&w.chunks[0]);
    CHECK(IsListEmpty(&w.chunks[0].changed_extents));
    CHECK(w.chunks[0].changed_extents_root == NULL);
    CHECK(w.calls == allocations && w.frees == frees + live && w.live == 0);
    finish(&w);
    puts("production terminal drain clears list/root without allocating");
#endif
}

int main(void) {
    differential();
    allocation_ordinals();
    lifecycle_paths();
    terminal_cleanup();
    return 0;
}
