#pragma once
/* Replay rationalize_extents' reference calls against the real
 * get_changed_extent_item, update_changed_extent_ref and add_changed_extent_ref,
 * with each chunk's changed_extents list holding `prefill` other entries. */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
typedef struct { LIST_ENTRY changed_extents; ERESOURCE changed_extents_lock; } chunk;

typedef struct {
    uint64_t address;
    uint64_t size;
    uint64_t old_size;
    uint64_t count;
    uint64_t old_count;
    bool no_csum;
    bool superseded;
    LIST_ENTRY refs;
    LIST_ENTRY old_refs;
    LIST_ENTRY list_entry;
} changed_extent;

typedef struct {
    uint8_t type;
    union {
        struct btrfs_extent_data_ref edr;
        struct {
            uint64_t offset;
            struct btrfs_shared_data_ref sdr;
        };
    };
    LIST_ENTRY list_entry;
} changed_extent_ref;

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
static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    (void)pool; (void)tag; return malloc(size);
}
static void ExFreePool(void* p) { free(p); }
static void ExAcquireResourceExclusiveLite(ERESOURCE* r, bool wait) { (void)r; (void)wait; }
static void ExReleaseResourceLite(ERESOURCE* r) { (void)r; }

unsigned long long loop_steps;
static uint64_t looked_up_size;
static struct btrfs_extent_item item_data = { .refs = 1 };
static tree_data found;

static NTSTATUS find_item(device_extension* Vcb, root* r, traverse_ptr* tp, const struct btrfs_key* searchkey,
                          bool ignore, PIRP Irp) {
    (void)Vcb; (void)r; (void)ignore; (void)Irp;
    found.key = (struct btrfs_key){ .objectid = searchkey->objectid, .type = BTRFS_EXTENT_ITEM_KEY,
                                    .offset = looked_up_size };
    found.size = sizeof(item_data);
    found.data = (uint8_t*)&item_data;
    tp->item = &found;
    return STATUS_SUCCESS;
}

static uint32_t find_extent_data_refcount(device_extension* Vcb, uint64_t address, uint64_t size, uint64_t root,
                                          uint64_t objid, uint64_t offset, PIRP Irp) {
    (void)Vcb; (void)address; (void)size; (void)root; (void)objid; (void)offset; (void)Irp;
    return 1;
}

