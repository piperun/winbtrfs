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
#define _In_opt_
#define _When_(...)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset(p,0,n)
#define min(a,b) ((a)<(b)?(a):(b))
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %u: %s\n",__LINE__,#c); exit(1); } } while(0)
static bool observe_single_insertion;
static unsigned observed_insertions;
static void observe_insertion(void) {
    /* Stop a duplicate link before changing any list pointers. */
    if(observe_single_insertion)CHECK(observed_insertions++==0);
}
static void InitializeListHead(LIST_ENTRY* h) { h->Flink=h->Blink=h; }
static bool IsListEmpty(LIST_ENTRY* h) { CHECK(h->Flink && h->Blink); return h->Flink==h; }
static void InsertTailList(LIST_ENTRY* h,LIST_ENTRY* e) { observe_insertion();e->Flink=h; e->Blink=h->Blink; h->Blink->Flink=e; h->Blink=e; }
static void InsertHeadList(LIST_ENTRY* h,LIST_ENTRY* e) { observe_insertion();e->Blink=h; e->Flink=h->Flink; h->Flink->Blink=e; h->Flink=e; }
void RemoveEntryList(LIST_ENTRY* e) { e->Flink->Blink=e->Blink; e->Blink->Flink=e->Flink; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) { CHECK(!IsListEmpty(h)); LIST_ENTRY* e=h->Flink; e->Flink->Blink=h; h->Flink=e->Flink; return e; }
static int keycmp(struct btrfs_key a,struct btrfs_key b) {
    if(a.objectid!=b.objectid)return a.objectid<b.objectid?-1:1;
    if(a.type!=b.type)return a.type<b.type?-1:1;
    return a.offset==b.offset?0:a.offset<b.offset?-1:1;
}
static size_t RtlCompareMemory(const void* a,const void* b,size_t n) { size_t i=0; while(i<n && ((const uint8_t*)a)[i]==((const uint8_t*)b)[i])i++; return i; }
static uint32_t calc_crc32c(uint32_t seed,const uint8_t* p,uint32_t n) { while(n--)seed=seed*33+*p++; return seed; }
static struct { void* p; LOOKASIDE* owner; } owned[4096];
static unsigned live, allocation_calls, fail_allocation, find_calls, fail_find;
static bool committing, failure_hit;
static void* allocate(size_t size,LOOKASIDE* owner) {
    if(committing && ++allocation_calls==fail_allocation) { failure_hit=true; return NULL; }
    void* p=calloc(1,size?size:1); CHECK(p);
    for(unsigned i=0;i<4096;i++)if(!owned[i].p){owned[i].p=p;owned[i].owner=owner;live++;if(owner)owner->outstanding++;return p;}
    CHECK(false);return NULL;
}
static void release(void* p,LOOKASIDE* owner) {
    CHECK(p);for(unsigned i=0;i<4096;i++)if(owned[i].p==p){CHECK(owned[i].owner==owner);owned[i].p=NULL;live--;if(owner){CHECK(owner->outstanding);owner->outstanding--;}free(p);return;}CHECK(false);
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
#include "batch_producer.inc"

static device_extension volume;
static root roots[3];
static LIST_ENTRY batches;
static void make_root(unsigned i) {
    roots[i].id=i+1;InitializeListHead(&roots[i].leaf.itemlist);
    tree_data* td=allocate(sizeof(*td),&volume.tree_data_lookaside);
    td->key.objectid=10;td->key.type=BTRFS_INODE_REF_KEY;td->key.offset=1;
    td->inserted=true;td->size=sizeof(struct btrfs_inode_ref);td->data=allocate(td->size,NULL);
    ((struct btrfs_inode_ref*)td->data)->name_len=0;
    InsertTailList(&roots[i].leaf.itemlist,&td->list_entry);roots[i].leaf.header.nritems=1;
}
static batch_item* add_item(batch_root* br,unsigned item,enum batch_operation op,bool collision) {
    batch_item_ind* bii=allocate(sizeof(*bii),NULL);InitializeListHead(&bii->items);InsertTailList(&br->items_ind,&bii->list_entry);
    batch_item* bi=allocate(sizeof(*bi),&volume.batch_item_lookaside);
    bi->operation=op;bi->key.objectid=collision?10:20+item;bi->key.type=BTRFS_INODE_REF_KEY;bi->key.offset=1;
    if(op==Batch_Insert || op==Batch_InodeRef || op==Batch_DeleteInodeRef || op==Batch_DirItem){
        bi->datalen=sizeof(struct btrfs_inode_ref);bi->data=allocate(bi->datalen,NULL);
        ((struct btrfs_inode_ref*)bi->data)->name_len=0;
    }
    InsertTailList(&bii->items,&bi->list_entry);return bi;
}
static void exercise(unsigned root_count,unsigned items,enum batch_operation operation,bool collision,bool overflow,bool clear_only) {
    volume.batch_item_lookaside.size=sizeof(batch_item);volume.tree_data_lookaside.size=sizeof(tree_data);
    volume.superblock.nodesize=overflow?sizeof(struct btrfs_header)+sizeof(struct btrfs_item)+sizeof(struct btrfs_inode_ref):4096;
    volume.superblock.incompat_flags=BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF;volume.superblock.generation=9;
    InitializeListHead(&batches);
    for(unsigned r=0;r<root_count;r++){
        make_root(r);batch_root* br=allocate(sizeof(*br),NULL);br->r=&roots[r];InitializeListHead(&br->items_ind);InsertTailList(&batches,&br->list_entry);
        for(unsigned i=0;i<items;i++)add_item(br,i,operation,collision);
    }
    NTSTATUS status=STATUS_SUCCESS;committing=true;
    if(clear_only)clear_batch_list(&volume,&batches);else status=commit_batch_list(&volume,&batches,NULL);
    committing=false;
    CHECK(status==(failure_hit?(fail_find?STATUS_INTERNAL_ERROR:STATUS_INSUFFICIENT_RESOURCES):STATUS_SUCCESS));
    CHECK(IsListEmpty(&batches) && !volume.batch_item_lookaside.outstanding);
    if(operation==Batch_Insert && !collision && !clear_only) {
        unsigned applied=root_count*items;
        if(fail_find && fail_find<=root_count)applied=(fail_find-1)*items;
        else if(fail_allocation && fail_allocation<=applied)applied=fail_allocation-1;
        CHECK(volume.tree_data_lookaside.outstanding==root_count+applied);
    }
    /* Applied tree items and their payload remain valid until their tree is destroyed. */
    for(unsigned r=0;r<root_count;r++)while(!IsListEmpty(&roots[r].leaf.itemlist)){
        tree_data* td=CONTAINING_RECORD(RemoveHeadList(&roots[r].leaf.itemlist),tree_data,list_entry);
        if(td->data){volatile uint8_t byte=td->data[0];(void)byte;ExFreePool(td->data);}
        ExFreeToPagedLookasideList(&volume.tree_data_lookaside,td);
    }
    CHECK(!live && !volume.tree_data_lookaside.outstanding);
}
static void overflow_order(unsigned layout,unsigned failure) {
    device_extension v={0};v.batch_item_lookaside.size=sizeof(batch_item);
    v.tree_data_lookaside.size=sizeof(tree_data);
    v.superblock.nodesize=sizeof(struct btrfs_header)+sizeof(struct btrfs_item)+sizeof(struct btrfs_inode_ref);
    v.superblock.incompat_flags=layout==6?0:BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF;
    LIST_ENTRY items;InitializeListHead(&items);
    batch_item original={0},pending[3]={0};
    original.key=(struct btrfs_key){10,BTRFS_INODE_REF_KEY,77};
    original.operation=Batch_InodeRef;
    original.datalen=sizeof(struct btrfs_inode_ref)+3;
    original.data=allocate(original.datalen,NULL);
    struct btrfs_inode_ref* ir=(struct btrfs_inode_ref*)original.data;
    ir->index=45;ir->name_len=3;memcpy(ir+1,"abc",3);
    struct btrfs_key generated_key={10,BTRFS_INODE_EXTREF_KEY,calc_crc32c(77,(uint8_t*)"abc",3)};
    InsertTailList(&items,&original.list_entry);
    unsigned count=layout==0||layout==6?0:layout==2?3:layout==5?1:2;
    for(unsigned i=0;i<count;i++) {
        pending[i].key=(struct btrfs_key){20+i*10,BTRFS_INODE_ITEM_KEY,0};
        if((layout==2&&i==0)||layout==3)
            pending[i].key=(struct btrfs_key){10,BTRFS_INODE_REF_KEY,78+i};
        if(layout==4)pending[i].key=generated_key;
        InsertTailList(&items,&pending[i].list_entry);
    }
    tree t={0};tree_data old={.size=sizeof(struct btrfs_inode_ref)},newtd={0};
    bool ignore=false;fail_allocation=failure;committing=true;observe_single_insertion=true;
    NTSTATUS status=handle_batch_collision(&v,&original,&t,&old,&newtd,&items,&ignore);
    observe_single_insertion=false;committing=false;
    bool generated=layout!=6&&!failure;
    CHECK(status==(layout==6?STATUS_INTERNAL_ERROR:failure?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS));
    CHECK(ignore==generated&&observed_insertions==(generated?1u:0u));
    unsigned visited=0,next_pending=0,generated_count=0;
    struct btrfs_key previous=original.key;
    while(!IsListEmpty(&items)) {
        batch_item* item=CONTAINING_RECORD(items.Flink,batch_item,list_entry);
        CHECK(visited++<count+2);
        CHECK(item->list_entry.Flink->Blink==&item->list_entry&&item->list_entry.Blink->Flink==&item->list_entry);
        CHECK(keycmp(previous,item->key)<=0);previous=item->key;
        RemoveHeadList(&items);
        if(item==&original)CHECK(visited==1);
        else if(next_pending<count&&item==&pending[next_pending])next_pending++;
        else {
            CHECK(generated&&++generated_count==1&&keycmp(item->key,generated_key)==0);
            struct btrfs_inode_extref* ier=(struct btrfs_inode_extref*)item->data;
            CHECK(item->operation==Batch_InodeExtRef&&ier->parent_objectid==77&&ier->index==45&&ier->name_len==3);
            CHECK(!memcmp(ier->name,"abc",3));
            ExFreePool(item->data);ExFreeToPagedLookasideList(&v.batch_item_lookaside,item);
        }
    }
    CHECK(next_pending==count&&generated_count==(generated?1u:0u));
    ExFreePool(original.data);CHECK(!live&&!v.batch_item_lookaside.outstanding);
}
int main(int argc,char** argv){
    CHECK(argc==2);
    for(unsigned layout=0;layout<7;layout++)for(unsigned failure=0;failure<(layout==6?1u:3u);failure++) {
        char name[80];snprintf(name,sizeof(name),"overflow-order-layout%u-allocation%u",layout,failure);
        if(!strcmp(argv[1],"--list")){puts(name);continue;}if(strcmp(argv[1],name))continue;
        overflow_order(layout,failure);return 0;
    }
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
    for(unsigned existing=0;existing<2;existing++)for(unsigned failure=0;failure<(existing?3u:4u);failure++) {
        char name[80];snprintf(name,sizeof(name),"producer-existing%u-allocation%u",existing,failure);
        if(!strcmp(argv[1],"--list")){puts(name);continue;}if(strcmp(argv[1],name))continue;
        device_extension volume={0};volume.batch_item_lookaside.size=sizeof(batch_item);
        volume.tree_data_lookaside.size=sizeof(tree_data);root r={0};LIST_ENTRY list;InitializeListHead(&list);
        unsigned initial=existing?BATCH_ITEM_LIMIT:0;
        for(unsigned i=0;i<initial;i++)CHECK(insert_tree_item_batch(&list,&volume,&r,i,BTRFS_INODE_ITEM_KEY,0,NULL,0,Batch_Insert)==STATUS_SUCCESS);
        uint8_t* data=allocate(8,NULL);data[0]=0x5a;
        fail_allocation=failure;committing=true;
        NTSTATUS status=insert_tree_item_batch(&list,&volume,&r,initial+1,BTRFS_INODE_ITEM_KEY,0,data,8,Batch_Insert);
        committing=false;CHECK(status==(failure?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS));
        unsigned queued=0;
        for(LIST_ENTRY* le=list.Flink;le!=&list;le=le->Flink){
            batch_root* br=CONTAINING_RECORD(le,batch_root,list_entry);
            for(LIST_ENTRY* li=br->items_ind.Flink;li!=&br->items_ind;li=li->Flink){
                batch_item_ind* bii=CONTAINING_RECORD(li,batch_item_ind,list_entry);unsigned actual=0;
                for(LIST_ENTRY* item=bii->items.Flink;item!=&bii->items;item=item->Flink){queued++;actual++;}
                CHECK(actual==bii->num_items);
            }
        }
        CHECK(queued==initial+(failure?0u:1u));CHECK(data[0]==0x5a);
        if(failure)ExFreePool(data);
        /* Release queued payloads explicitly so this producer regression is independent of clear's policy. */
        for(LIST_ENTRY* le=list.Flink;le!=&list;le=le->Flink){
            batch_root* br=CONTAINING_RECORD(le,batch_root,list_entry);
            for(LIST_ENTRY* li=br->items_ind.Flink;li!=&br->items_ind;li=li->Flink){
                batch_item_ind* bii=CONTAINING_RECORD(li,batch_item_ind,list_entry);
                for(LIST_ENTRY* item=bii->items.Flink;item!=&bii->items;item=item->Flink){
                    batch_item* bi=CONTAINING_RECORD(item,batch_item,list_entry);if(bi->data){ExFreePool(bi->data);bi->data=NULL;}
                }
            }
        }
        clear_batch_list(&volume,&list);CHECK(!live && !volume.batch_item_lookaside.outstanding);return 0;
    }
    static const struct {const char* name;enum batch_operation op;bool collision,overflow,clear;} modes[]={
        {"delete",Batch_DeleteInode,false,false,false}, {"insert",Batch_Insert,false,false,false},
        {"collision",Batch_InodeRef,true,false,false}, {"overflow",Batch_InodeRef,true,true,false},
        {"clear-payload",Batch_Insert,false,false,true}, {"delete-ref",Batch_DeleteInodeRef,true,false,false}};
    for(unsigned mode=0;mode<sizeof(modes)/sizeof(modes[0]);mode++)for(unsigned failure=0;failure<8;failure++){
        char name[100];snprintf(name,sizeof(name),"%s-failure%u",modes[mode].name,failure);
        if(!strcmp(argv[1],"--list")){puts(name);continue;}if(strcmp(argv[1],name))continue;
        if(failure==1)fail_find=1;else if(failure==2)fail_find=2;else if(failure>=3)fail_allocation=failure-2;
        exercise(3,modes[mode].overflow?1:2,modes[mode].op,modes[mode].collision,modes[mode].overflow,modes[mode].clear);return 0;
    }
    return strcmp(argv[1],"--list")?2:0;
}
