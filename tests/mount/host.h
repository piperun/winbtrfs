/* Minimal native layout for failed-mount cleanup and production reap_fcb. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { bool live; unsigned held; } ERESOURCE;
typedef struct { bool live, nonpaged; unsigned outstanding, returned; } LOOKASIDE;
typedef struct { void* Buffer; size_t Length; } STRING;
typedef struct { ERESOURCE load_tree_lock; } root_nonpaged;
typedef struct _tree tree;
typedef struct { uint64_t address, generation; tree* tree; } tree_holder;
typedef struct {
    LIST_ENTRY list_entry, fcbs; bool dropped; LIST_ENTRY* fcbs_ptrs[256]; root_nonpaged* nonpaged; tree_holder treeholder;
} root;
typedef struct device_extension device_extension;
typedef struct {
    LIST_ENTRY list_entry; bool ignore, inserted;
    union { tree_holder treeholder; struct { uint16_t size; uint8_t* data; }; };
} tree_data;
struct _tree {
    void* nonpaged; struct { uint8_t level; } header; uint32_t hash; device_extension* Vcb;
    tree* parent; tree_data* paritem; root* root; LIST_ENTRY itemlist, list_entry, list_entry_hash; uint8_t* buf;
};
typedef struct { ERESOURCE resource, paging_resource, dir_children_lock; } fcb_nonpaged;
typedef struct _fcb {
    uint32_t hash;
    root* subvol;
    struct _file_ref* fileref; bool ads;
    device_extension* Vcb;
    LONG refcount; bool dirty; LIST_ENTRY list_entry_dirty;
    fcb_nonpaged* nonpaged;
    LIST_ENTRY list_entry, list_entry_all, extents, hardlinks, xattrs, dir_children_index;
    void* sd;
    STRING adsxattr, reparse_xattr, ea_xattr, adsdata;
    LIST_ENTRY **hash_ptrs, **hash_ptrs_uc;
    int Header, lock, oplock, pool_type;
} fcb;
typedef struct _file_ref {
    LONG refcount; fcb* fcb; struct _file_ref* parent; LIST_ENTRY list_entry;
    struct { uint64_t size; struct _file_ref* fileref; }* dc;
    STRING oldutf8;
} file_ref;
typedef struct { LIST_ENTRY list_entry; void* csum; } extent;
typedef struct { LIST_ENTRY list_entry; STRING name, utf8; } hardlink;
typedef struct { LIST_ENTRY list_entry; } xattr;
typedef struct { LIST_ENTRY list_entry_index; STRING utf8, name, name_uc; } dir_child;
typedef struct { LIST_ENTRY list_entry, space; } device;
typedef struct { LIST_ENTRY list_entry; } space;
typedef struct { LIST_ENTRY list_entry, refs, old_refs; } changed_extent;
typedef struct { LIST_ENTRY list_entry; } changed_extent_ref;
typedef struct { LIST_ENTRY list_entry; } batch_item;
typedef struct { LIST_ENTRY list_entry, items; } batch_item_ind;
typedef struct { LIST_ENTRY list_entry, items_ind; } batch_root;
typedef struct { LIST_ENTRY list_entry; void* data; } sys_chunk;
typedef struct {
    LIST_ENTRY list_entry, space, deleting, changed_extents; fcb* cache; device** devices; void* chunk_item;
    ERESOURCE lock, changed_extents_lock, range_locks_lock, partial_stripes_lock;
} chunk;
struct device_extension {
    bool need_write;
    LOOKASIDE tree_data_lookaside, traverse_ptr_lookaside, batch_item_lookaside, fileref_lookaside,
              fcb_lookaside, name_bit_lookaside, range_lock_lookaside, fcb_np_lookaside;
    ERESOURCE tree_lock, load_lock, fcb_lock, fileref_lock, chunk_lock,
              dirty_fcbs_lock, dirty_filerefs_lock, dirty_subvols_lock;
    struct { ERESOURCE stats_lock; } scrub;
    void* root_file;
    void* NotifySync;
    file_ref* root_fileref;
    fcb* volume_fcb;
    fcb* dummy_fcb;
    LIST_ENTRY devices, roots, sys_chunks, chunks, trees, trees_hash, all_fcbs, dirty_fcbs;
    LIST_ENTRY* trees_ptrs[256];
};
typedef struct { bool deleted; device_extension* DeviceExtension; } DEVICE_OBJECT, *PDEVICE_OBJECT;

void mark_fcb_dirty(fcb* f);

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define NT_SUCCESS(s) ((s) >= 0)
#define PagedPool 1
#define NonPagedPool 0
#define ALLOC_TAG 0
#define RtlZeroMemory(p,n) memset(p, 0, n)
#define _In_
#define _Inout_
#define _In_opt_
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define fcb_oplock(f) (&(f)->oplock)
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

static void InitializeListHead(LIST_ENTRY* head) { head->Flink = head->Blink = head; }
static bool IsListEmpty(const LIST_ENTRY* head) { CHECK(head->Flink && head->Blink); return head->Flink == head; }
static void RemoveEntryList(LIST_ENTRY* entry) {
    entry->Flink->Blink = entry->Blink; entry->Blink->Flink = entry->Flink;
}
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* head) {
    CHECK(!IsListEmpty(head)); LIST_ENTRY* entry = head->Flink; RemoveEntryList(entry); return entry;
}
static void InsertTailList(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Blink = head->Blink; entry->Flink = head; head->Blink->Flink = entry; head->Blink = entry;
}
