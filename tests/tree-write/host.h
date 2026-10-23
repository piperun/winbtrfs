#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef int32_t NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_INSUFFICIENT_RESOURCES (-1)
#define NonPagedPool 0
#define PagedPool 1
#define ALLOC_TAG 0
#define BTRFS_BLOCK_GROUP_RAID5 0x80
#define BTRFS_BLOCK_GROUP_RAID6 0x100
#define ERR(...) ((void)0)

typedef struct list { struct list *Flink, *Blink; } LIST_ENTRY;
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
static void InitializeListHead(LIST_ENTRY* h) { h->Flink=h->Blink=h; }
static void InsertHeadList(LIST_ENTRY* h, LIST_ENTRY* p) { p->Flink=h->Flink; p->Blink=h; h->Flink->Blink=p; h->Flink=p; }
static void InsertTailList(LIST_ENTRY* h, LIST_ENTRY* p) { InsertHeadList(h->Blink,p); }
static void RemoveEntryList(LIST_ENTRY* p) { p->Flink->Blink=p->Blink; p->Blink->Flink=p->Flink; }
static bool IsListEmpty(LIST_ENTRY* p) { return p->Flink==p; }
typedef struct { uint64_t length, type; } chunk_item;
typedef struct { uint64_t offset; chunk_item* chunk_item; } chunk;
typedef struct { int unused; } device_extension;
typedef struct { int unused; } write_data_context;
static chunk_item items[2];
static chunk chunks[2];
static size_t chunk_count;
static bool test_raid56;
static chunk* get_chunk_from_address(device_extension* v, uint64_t a) {
    (void)v;
    for(size_t i=0;i<chunk_count;i++)
        if(a>=chunks[i].offset && a-chunks[i].offset<items[i].length) return &chunks[i];
    assert(!"address outside test chunks"); return NULL;
}
static void set_chunks(uint64_t offset, uint64_t length, uint64_t type) {
    items[0]=(chunk_item){length,type}; chunks[0]=(chunk){offset,&items[0]}; chunk_count=1; test_raid56=false;
}
typedef struct { uint64_t magic; size_t size; } allocation_header;
static uint64_t allocations, copied_bytes, live_allocations;
static size_t fail_at, last_request;
static void* ExAllocatePoolWithTag(int pool, size_t n, int tag) {
    (void)pool; (void)tag;
    allocations++; last_request=n;
    if(fail_at && allocations==fail_at) return NULL;
    assert(n<=16*1024*1024); /* Boundary tests must fail before a giant copy. */
    allocation_header* h=malloc(sizeof(*h)+n); assert(h);
    h->magic=0x514c504f4f4cULL; h->size=n; live_allocations++;
    memset(h+1,0xa5,n); return h+1;
}
static void ExFreePool(void* p) {
    allocation_header* h=(allocation_header*)p-1;
    assert(h->magic==0x514c504f4f4cULL);
    h->magic=0; live_allocations--; free(h);
}
static void copy_memory(void* d, const void* s, size_t n) { copied_bytes+=n; memcpy(d,s,n); }
#define RtlCopyMemory copy_memory
static uint64_t address_comparisons;
static bool address_after(uint64_t a,uint64_t b) { address_comparisons++; return a>b; }
static bool address_before_equal(uint64_t a,uint64_t b) { address_comparisons++; return a<=b; }
static unsigned char byte_for(uint64_t address) { return (unsigned char)(address*31+(address>>8)*7+19); }
static void reset_counts(void) { allocations=copied_bytes=address_comparisons=0; fail_at=last_request=0; }
