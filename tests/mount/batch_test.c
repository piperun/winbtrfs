/* Execute production batch commit/collision code with tracked ownership. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"
typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG;
typedef void* PIRP;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct tree tree;
typedef struct tree_data {
    struct btrfs_key key; LIST_ENTRY list_entry; bool ignore, inserted;
    uint16_t size; uint8_t* data; struct { tree* tree; } treeholder;
} tree_data;
struct tree { struct btrfs_header header; tree* parent; tree_data* paritem; LIST_ENTRY itemlist; uint32_t size; bool write; };
typedef struct { uint64_t id; tree leaf; } root;
typedef struct { tree* tree; tree_data* item; } traverse_ptr;
typedef struct { size_t size; unsigned outstanding; } LOOKASIDE;
typedef struct { struct btrfs_super_block superblock; LOOKASIDE batch_item_lookaside, tree_data_lookaside; } device_extension;
#include "batch_types.inc"
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define NT_SUCCESS(s) ((s)>=0)
#define PagedPool 1
#define ALLOC_TAG 0
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define _Requires_exclusive_lock_held_(...)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset(p,0,n)
#define min(a,b) ((a)<(b)?(a):(b))
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %u: %s\n",__LINE__,#c); exit(1); } } while(0)
static void InitializeListHead(LIST_ENTRY* h) { h->Flink=h->Blink=h; }
static bool IsListEmpty(LIST_ENTRY* h) { CHECK(h->Flink && h->Blink); return h->Flink==h; }
static void InsertTailList(LIST_ENTRY* h,LIST_ENTRY* e) { e->Flink=h; e->Blink=h->Blink; h->Blink->Flink=e; h->Blink=e; }
static void InsertHeadList(LIST_ENTRY* h,LIST_ENTRY* e) { e->Blink=h; e->Flink=h->Flink; h->Flink->Blink=e; h->Flink=e; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) { CHECK(!IsListEmpty(h)); LIST_ENTRY* e=h->Flink; e->Flink->Blink=h; h->Flink=e->Flink; return e; }
static int keycmp(struct btrfs_key a,struct btrfs_key b) {
    if(a.objectid!=b.objectid)return a.objectid<b.objectid?-1:1;
    if(a.type!=b.type)return a.type<b.type?-1:1;
    return a.offset==b.offset?0:a.offset<b.offset?-1:1;
}
static size_t RtlCompareMemory(const void* a,const void* b,size_t n) { size_t i=0; while(i<n && ((const uint8_t*)a)[i]==((const uint8_t*)b)[i])i++; return i; }
static uint32_t calc_crc32c(uint32_t seed,const uint8_t* p,uint32_t n) { while(n--)seed=seed*33+*p++; return seed; }
static struct { void* p; LOOKASIDE* owner; } owned[256];
static unsigned live, allocation_calls, fail_allocation, find_calls, fail_find;
static bool committing, failure_hit;
static void* allocate(size_t size,LOOKASIDE* owner) {
    if(committing && ++allocation_calls==fail_allocation) { failure_hit=true; return NULL; }
    void* p=calloc(1,size?size:1); CHECK(p);
    for(unsigned i=0;i<256;i++)if(!owned[i].p){owned[i].p=p;owned[i].owner=owner;live++;if(owner)owner->outstanding++;return p;}
    CHECK(false);return NULL;
}
static void release(void* p,LOOKASIDE* owner) {
    CHECK(p);for(unsigned i=0;i<256;i++)if(owned[i].p==p){CHECK(owned[i].owner==owner);owned[i].p=NULL;live--;if(owner){CHECK(owner->outstanding);owner->outstanding--;}free(p);return;}CHECK(false);
}
static void* ExAllocatePoolWithTag(int pool,size_t n,unsigned tag) { (void)pool;(void)tag;return allocate(n,NULL); }
static void ExFreePool(void* p) { release(p,NULL); }
static void* ExAllocateFromPagedLookasideList(LOOKASIDE* l) { return allocate(l->size,l); }
static void ExFreeToPagedLookasideList(LOOKASIDE* l,void* p) { release(p,l); }
static NTSTATUS find_item(device_extension* v,root* r,traverse_ptr* tp,const struct btrfs_key* k,bool ignore,PIRP irp) {
    (void)v;(void)ignore;(void)irp;
    if(++find_calls==fail_find){failure_hit=true;return STATUS_INTERNAL_ERROR;}
    tp->tree=&r->leaf;tp->item=CONTAINING_RECORD(r->leaf.itemlist.Flink,tree_data,list_entry);
    for(LIST_ENTRY* le=r->leaf.itemlist.Flink;le!=&r->leaf.itemlist;le=le->Flink){tree_data* td=CONTAINING_RECORD(le,tree_data,list_entry);if(keycmp(td->key,*k)>0)break;tp->item=td;}
    return STATUS_SUCCESS;
}
static bool find_next_item(device_extension* v,traverse_ptr* tp,traverse_ptr* next,bool ignore,PIRP irp) { (void)v;(void)tp;(void)next;(void)ignore;(void)irp;return false; }
#include "batch_collision.inc"
#include "batch_commit.inc"


int main(int argc,char** argv) {
    CHECK(argc==2);
    for(unsigned failure=0;failure<3;failure++) {
        char name[80];snprintf(name,sizeof(name),"extref-allocation%u",failure);
        if(!strcmp(argv[1],"--list")){puts(name);continue;}if(strcmp(argv[1],name))continue;
        device_extension volume={0};volume.batch_item_lookaside.size=sizeof(batch_item);
        volume.tree_data_lookaside.size=sizeof(tree_data);
        LIST_ENTRY items;InitializeListHead(&items);
        batch_item bi={0};bi.key.objectid=20;bi.key.offset=1;
        bi.data=allocate(sizeof(struct btrfs_inode_ref),NULL);
        InsertTailList(&items,&bi.list_entry);
        fail_allocation=failure;committing=true;
        NTSTATUS status=add_delete_inode_extref(&volume,&bi,&items);
        committing=false;CHECK(status==(failure?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS));
        CHECK(RemoveHeadList(&items)==&bi.list_entry);
        if(!failure) {
            batch_item* generated=CONTAINING_RECORD(RemoveHeadList(&items),batch_item,list_entry);
            CHECK(generated->operation==Batch_DeleteInodeExtRef);
            ExFreePool(generated->data);ExFreeToPagedLookasideList(&volume.batch_item_lookaside,generated);
        }
        ExFreePool(bi.data);
        CHECK(IsListEmpty(&items) && !live && !volume.batch_item_lookaside.outstanding);
        return 0;
    }
    return strcmp(argv[1],"--list")?2:0;
}
