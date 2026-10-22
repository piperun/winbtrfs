#include "host.h"
#include "refs.inc"


typedef struct { char op; int chunk; uint64_t address, size, root, inode, offset; long long count; } call;

static double milliseconds(const struct timespec* a, const struct timespec* b) {
    return (double)(b->tv_sec - a->tv_sec) * 1e3 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

int main(int argc, char** argv) {
    CHECK(argc == 3);
    unsigned prefill = (unsigned)atoi(argv[2]), num_chunks;
    double rationalize_ms;
    FILE* f = fopen(argv[1], "r");
    CHECK(f && fscanf(f, "%u %lf", &num_chunks, &rationalize_ms) == 2);
    size_t num_calls = 0, capacity = 1024;
    call* calls = malloc(capacity * sizeof(call));
    call c;
    while (fscanf(f, " %c %d %llu %llu %llu %llu %llu %lld", &c.op, &c.chunk,
                  (unsigned long long*)&c.address, (unsigned long long*)&c.size, (unsigned long long*)&c.root,
                  (unsigned long long*)&c.inode, (unsigned long long*)&c.offset, &c.count) == 8) {
        if (num_calls == capacity)
            calls = realloc(calls, (capacity *= 2) * sizeof(call));
        calls[num_calls++] = c;
    }
    fclose(f);

    root extent_root = { .id = 2 };
    device_extension vcb = { .extent_root = &extent_root };
    IRP irp = { 0 };
    chunk* chunks = calloc(num_chunks, sizeof(chunk));
    for (unsigned i = 0; i < num_chunks; i++) {
        InitializeListHead(&chunks[i].changed_extents);
        for (unsigned j = 0; j < prefill; j++) {
            changed_extent* ce = calloc(1, sizeof(changed_extent));
            changed_extent_ref* cer = calloc(1, sizeof(changed_extent_ref));
            ce->address = 0x100000000000ULL + ((uint64_t)i * prefill + j) * 4096;
            ce->size = 4096;
            ce->count = 1;
            InitializeListHead(&ce->refs);
            InitializeListHead(&ce->old_refs);
            cer->type = BTRFS_EXTENT_DATA_REF_KEY;
            cer->edr.count = 1;
            InsertTailList(&ce->refs, &cer->list_entry);
            InsertTailList(&chunks[i].changed_extents, &ce->list_entry);
        }
    }

    struct timespec t0, t1;
    loop_steps = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < num_calls; i++) {
        const call* e = &calls[i];
        NTSTATUS status;
        CHECK(e->chunk >= 0 && (unsigned)e->chunk < num_chunks);
        looked_up_size = e->size;
        if (e->op == 'U')
            status = update_changed_extent_ref(&vcb, &chunks[e->chunk], e->address, e->size, e->root, e->inode,
                                               e->offset, (int32_t)e->count, false, true, &irp, NULL);
        else
            status = add_changed_extent_ref(&chunks[e->chunk], e->address, e->size, e->root, e->inode,
                                            e->offset, (uint32_t)e->count, false, NULL);
        CHECK(NT_SUCCESS(status));
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("%.9f\n", milliseconds(&t0, &t1) / 1000.0);
    for (unsigned i = 0; i < num_chunks; i++) {
        while (!IsListEmpty(&chunks[i].changed_extents)) {
            changed_extent* ce = CONTAINING_RECORD(chunks[i].changed_extents.Flink, changed_extent, list_entry);
            while (!IsListEmpty(&ce->refs)) {
                changed_extent_ref* ref = CONTAINING_RECORD(ce->refs.Flink, changed_extent_ref, list_entry);
                RemoveEntryList(&ref->list_entry); ExFreePool(ref);
            }
            while (!IsListEmpty(&ce->old_refs)) {
                changed_extent_ref* ref = CONTAINING_RECORD(ce->old_refs.Flink, changed_extent_ref, list_entry);
                RemoveEntryList(&ref->list_entry); ExFreePool(ref);
            }
            RemoveEntryList(&ce->list_entry); ExFreePool(ce);
        }
    }
    free(chunks); free(calls);
    return 0;
}
