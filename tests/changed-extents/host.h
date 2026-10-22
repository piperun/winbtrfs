#pragma once
/* Replay rationalize_extents' reference calls against the real
 * get_changed_extent_item, update_changed_extent_ref and add_changed_extent_ref,
 * with each chunk's changed_extents list holding `prefill` other entries. */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "btrfs.h"

typedef int32_t NTSTATUS;
typedef struct { unsigned marker; } IRP, *PIRP;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { int unused; } ERESOURCE;
typedef struct _root { uint64_t id; } root;
typedef struct { root* extent_root; } device_extension;
typedef struct {
    LIST_ENTRY changed_extents;
#if HAVE_INDEX
    struct changed_extent* changed_extents_root;
#endif
    ERESOURCE changed_extents_lock;
    uint64_t used;
} chunk;

#include "extent-type.inc"

typedef struct { struct btrfs_key key; uint16_t size; uint8_t* data; } tree_data;
typedef struct { void* tree; tree_data* item; } traverse_ptr;
enum rollback_type { ROLLBACK_UPDATE_CHANGED_EXTENT_REF };
typedef struct {
    LIST_ENTRY list_entry;
    enum rollback_type type;
    union {
        struct {
            uint64_t address, size, root, objid, offset;
            int32_t count;
        } changed_extent_ref;
    };
} rollback_item;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define NT_SUCCESS(s) ((s) >= 0)
#define PagedPool 1
#define ALLOC_TAG 0
#define ERR(...) ((void)0)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static inline void InitializeListHead(LIST_ENTRY* h) { h->Flink = h->Blink = h; }
static inline bool IsListEmpty(const LIST_ENTRY* h) { return h->Flink == h; }
static inline void InsertTailList(LIST_ENTRY* h, LIST_ENTRY* e) {
    e->Blink = h->Blink; e->Flink = h; h->Blink->Flink = e; h->Blink = e;
}
static inline void RemoveEntryList(LIST_ENTRY* e) { e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink; }
static inline LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) {
    LIST_ENTRY* e = h->Flink; RemoveEntryList(e); return e;
}
#ifdef CE_TEST
static void* ce_allocate(size_t size);
static void ce_release(void* pointer);
#endif
static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    (void)pool; (void)tag;
#ifdef CE_TEST
    return ce_allocate(size);
#else
    return malloc(size);
#endif
}
static void ExFreePool(void* p) {
#ifdef CE_TEST
    ce_release(p);
#else
    free(p);
#endif
}
static void ExAcquireResourceExclusiveLite(ERESOURCE* r, bool wait) { (void)r; (void)wait; }
static void ExReleaseResourceLite(ERESOURCE* r) { (void)r; }

unsigned long long loop_steps;
static uint64_t looked_up_size;
static struct btrfs_extent_item item_data = { .refs = 1 };
static tree_data found;
#ifdef CE_TEST
static unsigned lookup_fault;
static uint32_t prior_refs = 1;
#endif

static NTSTATUS find_item(device_extension* Vcb, root* r, traverse_ptr* tp, const struct btrfs_key* searchkey,
                          bool ignore, PIRP Irp) {
    (void)Vcb; (void)r; (void)ignore; (void)Irp;
    found.key = (struct btrfs_key){ .objectid = searchkey->objectid, .type = BTRFS_EXTENT_ITEM_KEY,
                                    .offset = looked_up_size };
    found.size = sizeof(item_data);
    found.data = (uint8_t*)&item_data;
    tp->item = &found;
#ifdef CE_TEST
    if (lookup_fault == 1) return STATUS_INTERNAL_ERROR;
    if (lookup_fault == 2) found.key.objectid++;
    if (lookup_fault == 3) found.key.type++;
    if (lookup_fault == 4) found.key.offset++;
    if (lookup_fault == 5) found.size = 0;
    if (lookup_fault == 6) found.size = sizeof(struct btrfs_extent_item_v0);
#endif
    return STATUS_SUCCESS;
}

static uint32_t find_extent_data_refcount(device_extension* Vcb, uint64_t address, uint64_t size, uint64_t root,
                                          uint64_t objid, uint64_t offset, PIRP Irp) {
    (void)Vcb; (void)address; (void)size; (void)root; (void)objid; (void)offset; (void)Irp;
#ifdef CE_TEST
    return prior_refs;
#else
    return 1;
#endif
}

