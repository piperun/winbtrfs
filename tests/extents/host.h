/* Native service models for the production extent rationalization helper. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef struct { unsigned marker; } IRP, *PIRP;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned sector_shift; } device_extension;
typedef struct {
    uint64_t offset, used;
    struct btrfs_chunk* chunk_item;
    bool cache_loaded;
    unsigned locks;
} chunk;
typedef struct {
    uint64_t offset;
    bool unique;
    LIST_ENTRY list_entry;
    struct btrfs_file_extent_item extent_data;
} extent;
typedef struct { uint64_t id; } root;
typedef struct {
    device_extension* Vcb;
    root* subvol;
    uint64_t inode;
    struct btrfs_inode_item inode_item;
    LIST_ENTRY extents;
} fcb;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define NT_SUCCESS(s) ((s) >= 0)
#define PagedPool 1
#define ALLOC_TAG 0
#define MAX_EXTENT_SIZE 0x8000000
#define ERR(...) ((void)0)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

static inline void InitializeListHead(LIST_ENTRY* h) { h->Flink = h->Blink = h; }
static inline bool IsListEmpty(const LIST_ENTRY* h) { return h->Flink == h; }
static inline void InsertHeadList(LIST_ENTRY* h, LIST_ENTRY* e) {
    e->Flink = h->Flink; e->Blink = h;
    h->Flink->Blink = e; h->Flink = e;
}
static inline void RemoveEntryList(LIST_ENTRY* e) {
    e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink;
}
static inline LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) {
    LIST_ENTRY* e = h->Flink; CHECK(e != h); RemoveEntryList(e); return e;
}

/* extents_test.c defines the services in the same file as the function. The
 * comparison build defines them once, with SERVICE set to extern, and links two
 * copies of the function against them. */
#ifndef SERVICE
#define SERVICE static
#endif

SERVICE void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag);
SERVICE void ExFreePool(void* p);
SERVICE chunk* get_chunk_from_address(device_extension* v, uint64_t address);
SERVICE NTSTATUS update_changed_extent_ref(device_extension* v, chunk* c, uint64_t address,
    uint64_t size, uint64_t root_id, uint64_t inode, uint64_t offset, int32_t count,
    bool no_csum, bool superseded, PIRP irp, LIST_ENTRY* rollback);
SERVICE NTSTATUS add_changed_extent_ref(chunk* c, uint64_t address, uint64_t size,
    uint64_t root_id, uint64_t inode, uint64_t offset, uint32_t count,
    bool no_csum, LIST_ENTRY* rollback);
SERVICE NTSTATUS add_checksum_entry(device_extension* v, uint64_t address, ULONG length,
    void* csum, PIRP irp);
SERVICE void acquire_chunk_lock(chunk* c, device_extension* v);
SERVICE void release_chunk_lock(chunk* c, device_extension* v);
SERVICE NTSTATUS load_cache_chunk(device_extension* v, chunk* c, PIRP irp);
SERVICE NTSTATUS space_list_add(chunk* c, uint64_t address, uint64_t length,
    LIST_ENTRY* rollback);
