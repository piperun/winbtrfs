/* Kernel and storage boundaries; planning, loading and publication stay real. */
#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); abort(); } } while (0)
typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef int LONG;
typedef signed char CCHAR;
typedef void* PIRP;
typedef struct { pthread_mutex_t lock; } FAST_MUTEX;
typedef struct { unsigned held; } ERESOURCE;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
#include "disk-types.inc"
#include "tree-types.h"
typedef struct { ERESOURCE load_tree_lock; } root_nonpaged;
typedef struct _root { uint64_t id; tree_holder treeholder; root_nonpaged* nonpaged; struct { uint8_t level; } root_item; } root;
typedef struct { LIST_ENTRY list_entry; bool ignore, inserted; void* csum; struct btrfs_file_extent_item extent_data; } extent;
typedef struct _fcb { struct { ERESOURCE* Resource; } Header; bool deleted, ads, extents_changed; LIST_ENTRY extents, list_entry_dirty; } fcb;
typedef struct { struct { unsigned StackSize; }* devobj; } device;
struct btrfs_chunk { uint64_t type, length; uint16_t num_stripes; };
typedef struct { uint64_t offset; struct btrfs_chunk* chunk_item; device** devices; } chunk;
typedef struct _device_extension {
    struct { uint64_t generation; uint32_t nodesize; } superblock;
    FAST_MUTEX trees_list_mutex;
    LIST_ENTRY trees, trees_hash;
    LIST_ENTRY* trees_ptrs[256];
    unsigned tree_data_lookaside;
    ERESOURCE tree_lock, dirty_fcbs_lock;
    LIST_ENTRY dirty_fcbs;
    root* checksum_root;
    bool need_write, readonly, log_to_phys_loaded;
    unsigned sector_shift;
} device_extension;
typedef struct { tree* tree; tree_data* item; } traverse_ptr;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225u)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5u)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009au)
#define STATUS_CRC_ERROR ((NTSTATUS)0xc000003fu)
#define NT_SUCCESS(s) ((s) >= 0)
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define _In_
#define _Out_
#define _In_opt_
#define _Requires_lock_held_(x)
#define _Function_class_(x)
#define __stdcall
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define PagedPool 1
#define NonPagedPool 0
#define ALLOC_TAG 0
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset(p,0,n)
#define BTRFS_FILE_EXTENT_REG 1
#define BTRFS_COMPRESS_NONE 0
#define BTRFS_EXTENT_CSUM_OBJECTID UINT64_C(0xfffffffffffffff6)
#define BTRFS_EXTENT_CSUM_KEY 0x80
#define BTRFS_BLOCK_GROUP_DATA 1
#define BTRFS_BLOCK_GROUP_METADATA 4
#define BTRFS_BLOCK_GROUP_SYSTEM 2
#define BTRFS_BLOCK_GROUP_DUP 32
#define BTRFS_BLOCK_GROUP_RAID0 8
#define BTRFS_BLOCK_GROUP_RAID1 16
#define BTRFS_BLOCK_GROUP_RAID10 64
#define BTRFS_BLOCK_GROUP_RAID5 128
#define BTRFS_BLOCK_GROUP_RAID6 256
#define BTRFS_BLOCK_GROUP_RAID1C3 512
#define BTRFS_BLOCK_GROUP_RAID1C4 1024
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define NormalPagePriority 0
typedef uintptr_t PFN_NUMBER;
typedef struct { uint8_t data[48]; } MDL;
typedef MDL* PMDL;
typedef struct { uintptr_t status, information; } IO_STATUS_BLOCK;
static unsigned IoSizeOfIrp(unsigned stacks) { return 208 + stacks * 72; }
static pthread_t owner;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t live_bytes, peak_bytes;
static unsigned allocations, releases, allocation_attempts, fail_at;
typedef union { max_align_t align; size_t size; } allocation_header;
static void* ExAllocatePoolWithTag(int pool, size_t bytes, unsigned tag) {
    (void)pool; (void)tag;
    pthread_mutex_lock(&pool_lock);
    allocation_attempts++;
    if (fail_at && allocation_attempts == fail_at) { pthread_mutex_unlock(&pool_lock); return NULL; }
    allocation_header* h = malloc(sizeof(*h) + bytes);
    CHECK(h); h->size = bytes; allocations++; live_bytes += bytes;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    pthread_mutex_unlock(&pool_lock);
    memset(h + 1, POOL_PATTERN, bytes);
    return h + 1;
}
static void ExFreePool(void* pointer) {
    CHECK(pointer);
    allocation_header* h = (allocation_header*)pointer - 1;
    pthread_mutex_lock(&pool_lock); live_bytes -= h->size; releases++; pthread_mutex_unlock(&pool_lock);
    free(h);
}
static void* ExAllocateFromPagedLookasideList(unsigned* list) { (void)list; return ExAllocatePoolWithTag(PagedPool, sizeof(tree_data), ALLOC_TAG); }
static void ExFreeToPagedLookasideList(unsigned* list, void* p) { (void)list; ExFreePool(p); }
#include "keycmp.inc"
static bool IsListEmpty(const LIST_ENTRY* p) { return p->Flink == p; }
static void InitializeListHead(LIST_ENTRY* p) { p->Flink = p->Blink = p; }
static void InsertTailList(LIST_ENTRY* p, LIST_ENTRY* e) { e->Flink=p; e->Blink=p->Blink; p->Blink->Flink=e; p->Blink=e; }
static void InsertHeadList(LIST_ENTRY* p, LIST_ENTRY* e) { e->Blink=p; e->Flink=p->Flink; p->Flink->Blink=e; p->Flink=e; }
static void RemoveEntryList(LIST_ENTRY* e) { e->Blink->Flink=e->Flink; e->Flink->Blink=e->Blink; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* p) { LIST_ENTRY* e=p->Flink; RemoveEntryList(e); return e; }
static void ExInitializeFastMutex(FAST_MUTEX* p) { CHECK(!pthread_mutex_init(&p->lock, NULL)); }
static void ExAcquireFastMutex(FAST_MUTEX* p) { CHECK(pthread_equal(owner,pthread_self())); CHECK(!pthread_mutex_lock(&p->lock)); }
static void ExReleaseFastMutex(FAST_MUTEX* p) { CHECK(!pthread_mutex_unlock(&p->lock)); }
static void ExAcquireResourceExclusiveLite(ERESOURCE* p, bool wait) { (void)wait; CHECK(pthread_equal(owner,pthread_self())); CHECK(!p->held); p->held=1; }
static void ExReleaseResourceLite(ERESOURCE* p) { CHECK(p->held); p->held=0; }
static uint32_t calc_crc32c(uint32_t crc, const uint8_t* b, size_t n) { (void)crc; (void)b; (void)n; return 0x80000000u; }
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; bool signaled, automatic; } KEVENT;
#define NotificationEvent 0
#define SynchronizationEvent 1
#define Executive 0
#define KernelMode 0
static void KeInitializeEvent(KEVENT* e, int type, bool state) { pthread_mutex_init(&e->lock,NULL); pthread_cond_init(&e->cond,NULL); e->signaled=state; e->automatic=type==SynchronizationEvent; }
static void KeSetEvent(KEVENT* e, int increment, bool wait) { (void)increment; (void)wait; pthread_mutex_lock(&e->lock); e->signaled=true; pthread_cond_signal(&e->cond); pthread_mutex_unlock(&e->lock); }
static void KeClearEvent(KEVENT* e) { pthread_mutex_lock(&e->lock); e->signaled=false; pthread_mutex_unlock(&e->lock); }
static void KeWaitForSingleObject(KEVENT* e, int why, int mode, bool alert, void* timeout) { (void)why; (void)mode; (void)alert; (void)timeout; pthread_mutex_lock(&e->lock); while(!e->signaled) pthread_cond_wait(&e->cond,&e->lock); if(e->automatic)e->signaled=false; pthread_mutex_unlock(&e->lock); }
typedef struct { pthread_t thread; void (*routine)(void*); void* arg; } *HANDLE;
typedef struct { unsigned flags; } OBJECT_ATTRIBUTES;
#define OBJ_KERNEL_HANDLE 1
#define SYNCHRONIZE 1
#define InitializeObjectAttributes(p,n,f,r,s) ((p)->flags=(f))
static unsigned creates, joins, closes, fail_create, alive;
static void* thread_start(void* arg) { HANDLE h=arg; h->routine(h->arg); return NULL; }
static NTSTATUS PsCreateSystemThread(HANDLE* out, unsigned access, OBJECT_ATTRIBUTES* oa, void* pid, void* cid, void (*routine)(void*), void* arg) {
    (void)access; (void)pid; (void)cid; CHECK(oa->flags==OBJ_KERNEL_HANDLE); creates++;
    if (creates==fail_create) return STATUS_INSUFFICIENT_RESOURCES;
    HANDLE h=calloc(1,sizeof(*h)); CHECK(h); h->routine=routine; h->arg=arg; *out=h;
    CHECK(!pthread_create(&h->thread,NULL,thread_start,h)); alive++; return STATUS_SUCCESS;
}
static void ZwWaitForSingleObject(HANDLE h, bool alert, void* timeout) { (void)alert; (void)timeout; CHECK(!pthread_join(h->thread,NULL)); joins++; alive--; }
static void ZwClose(HANDLE h) { closes++; free(h); }
static void PsTerminateSystemThread(NTSTATUS s) { CHECK(NT_SUCCESS(s)); pthread_exit(NULL); }
NTSTATUS do_load_tree(device_extension*, tree_holder*, root*, tree*, tree_data*, PIRP);
NTSTATUS read_data(device_extension*,uint64_t,uint32_t,void*,bool,uint8_t*,chunk*,chunk**,PIRP,uint64_t,bool,ULONG);
static chunk* get_chunk_from_address(device_extension*,uint64_t);
NTSTATUS find_item(device_extension*,root*,traverse_ptr*,const struct btrfs_key*,bool,PIRP);
NTSTATUS find_item_to_level(device_extension*,root*,traverse_ptr*,const struct btrfs_key*,bool,uint8_t,PIRP);
bool find_prev_item(device_extension*,const traverse_ptr*,traverse_ptr*,PIRP);
bool find_next_item(device_extension*,const traverse_ptr*,traverse_ptr*,bool,PIRP);
#include "tree-search.h"
