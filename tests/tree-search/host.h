/* Kernel boundary doubles for executing the production ordered-tree search. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef void* PIRP;
typedef struct { unsigned held; } FAST_MUTEX;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
struct btrfs_key { uint64_t objectid; uint8_t type; uint64_t offset; };
struct btrfs_header {
    uint64_t bytenr, generation, owner, flags;
    uint32_t nritems;
    uint8_t level;
};
#include "tree-types.h"
typedef struct _root {
    uint64_t id;
    tree_holder treeholder;
} root;
typedef struct _device_extension {
    struct { uint64_t generation; uint32_t nodesize; } superblock;
    FAST_MUTEX trees_list_mutex;
    LIST_ENTRY trees, trees_hash;
    LIST_ENTRY* trees_ptrs[256];
    unsigned tree_data_lookaside;
} device_extension;
typedef struct { tree* tree; tree_data* item; } traverse_ptr;
struct btrfs_item { struct btrfs_key key; uint32_t offset, size; };
struct btrfs_key_ptr { struct btrfs_key key; uint64_t blockptr, generation; };

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225u)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5u)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009au)
#define NT_SUCCESS(s) ((s) >= 0)
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define _In_
#define _Out_
#define _In_opt_
#define _Requires_lock_held_(x)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); abort(); } } while (0)
#define PagedPool 1
#define NonPagedPool 0
#define ALLOC_TAG 0
#define RtlCopyMemory memcpy

static uint64_t comparisons;
static unsigned allocations, releases;
static uint64_t allocation_bytes;
static bool fail_allocation;
static size_t fail_bytes;
static void* ExAllocatePoolWithTag(int pool, size_t bytes, unsigned tag) {
    (void)pool; (void)tag;
    if (fail_allocation || (fail_bytes && fail_bytes == bytes)) return NULL;
    void* pointer = malloc(bytes);
    CHECK(pointer);
    memset(pointer, 0xa5, bytes);
    allocations++;
    allocation_bytes += bytes;
    return pointer;
}
static void ExFreePool(void* pointer) { CHECK(pointer); releases++; free(pointer); }
static int keycmp(struct btrfs_key a, struct btrfs_key b) {
    comparisons++;
    if (a.objectid != b.objectid) return a.objectid < b.objectid ? -1 : 1;
    if (a.type != b.type) return a.type < b.type ? -1 : 1;
    return a.offset < b.offset ? -1 : a.offset > b.offset ? 1 : 0;
}
static bool IsListEmpty(const LIST_ENTRY* list) { return list->Flink == list; }
static void InitializeListHead(LIST_ENTRY* list) { list->Flink = list->Blink = list; }
static void InsertTailList(LIST_ENTRY* list, LIST_ENTRY* item) {
    item->Flink = list; item->Blink = list->Blink;
    list->Blink->Flink = item; list->Blink = item;
}
static void InsertHeadList(LIST_ENTRY* list, LIST_ENTRY* item) {
    item->Blink = list; item->Flink = list->Flink;
    list->Flink->Blink = item; list->Flink = item;
}
static void RemoveEntryList(LIST_ENTRY* item) {
    item->Blink->Flink = item->Flink;
    item->Flink->Blink = item->Blink;
}
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* list) {
    LIST_ENTRY* item = list->Flink;
    RemoveEntryList(item);
    return item;
}
static void* ExAllocateFromPagedLookasideList(unsigned* list) {
    (void)list;
    return ExAllocatePoolWithTag(PagedPool, sizeof(tree_data), ALLOC_TAG);
}
static void ExFreeToPagedLookasideList(unsigned* list, void* pointer) {
    (void)list;
    ExFreePool(pointer);
}
static void ExInitializeFastMutex(FAST_MUTEX* mutex) { mutex->held = 0; }
static void ExAcquireFastMutex(FAST_MUTEX* mutex) { CHECK(!mutex->held); mutex->held = 1; }
static void ExReleaseFastMutex(FAST_MUTEX* mutex) { CHECK(mutex->held); mutex->held = 0; }
static uint32_t calc_crc32c(uint32_t crc, const uint8_t* data, size_t bytes) {
    (void)crc; (void)data; (void)bytes; return 0x80000000u;
}
static unsigned load_calls;
static NTSTATUS load_status;
static tree* loaded_child;
static NTSTATUS do_load_tree(device_extension* vcb, tree_holder* holder, root* r,
                             tree* parent, tree_data* item, PIRP irp) {
    (void)vcb; (void)r; (void)irp;
    load_calls++;
    if (!NT_SUCCESS(load_status)) return load_status;
    CHECK(loaded_child);
    holder->tree = loaded_child;
    loaded_child->parent = parent;
    loaded_child->paritem = item;
    return STATUS_SUCCESS;
}
NTSTATUS find_item(device_extension*, root*, traverse_ptr*, const struct btrfs_key*, bool, PIRP);
NTSTATUS find_item_to_level(device_extension*, root*, traverse_ptr*, const struct btrfs_key*, bool, uint8_t, PIRP);
bool find_prev_item(device_extension*, const traverse_ptr*, traverse_ptr*, PIRP);
bool find_next_item(device_extension*, const traverse_ptr*, traverse_ptr*, bool, PIRP);
#if HAVE_INDEX
#include "tree-search.h"
#endif

static void prepare_tree(tree* t, tree_data* items, unsigned count, uint8_t level) {
    memset(t, 0, sizeof(*t));
    t->header.level = level;
    t->header.nritems = count;
    InitializeListHead(&t->itemlist);
    for (unsigned i = 0; i < count; i++) {
        memset(&items[i], 0, sizeof(items[i]));
        items[i].key = (struct btrfs_key){42, 108, i * 16};
        InsertTailList(&t->itemlist, &items[i].list_entry);
    }
}
static void build_index(tree* t) {
#if HAVE_INDEX
    cache_tree_search(t);
#else
    (void)t;
#endif
}
static void release_index(tree* t) {
#if HAVE_INDEX
    invalidate_tree_search(t);
#else
    (void)t;
#endif
}
