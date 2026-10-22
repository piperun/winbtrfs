/* Actual payload producers, insertion/splitting and queue teardown; kernel APIs are modeled. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef void* PIRP;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { uint16_t Length, MaximumLength; char* Buffer; } ANSI_STRING, *PANSI_STRING;
typedef struct { uint64_t id; } root;
typedef struct { size_t size; unsigned outstanding; } LOOKASIDE;
typedef struct fcb fcb;
typedef struct {
    struct btrfs_super_block superblock;
    LOOKASIDE batch_item_lookaside;
    int dirty_fcbs_lock;
    fcb* dummy_fcb;
} device_extension;
typedef struct {
    LIST_ENTRY list_entry;
    uint64_t offset;
    uint16_t datalen;
    bool inserted;
    struct btrfs_file_extent_item extent_data;
} extent;
struct fcb {
    device_extension* Vcb;
    root* subvol;
    uint64_t inode;
    struct btrfs_inode_item inode_item;
    bool ads, dirty, inode_item_changed;
    uint8_t type;
    unsigned refcount;
    LIST_ENTRY extents, list_entry_dirty;
};
typedef struct { ANSI_STRING utf8; uint64_t index; struct btrfs_key key; } dir_child;
typedef struct file_ref {
    fcb* fcb;
    struct file_ref* parent;
    dir_child* dc;
    ANSI_STRING oldutf8;
    uint64_t oldindex;
    bool created, deleted, dirty;
} file_ref;
#include "batch_types.inc"

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003e)
#define NT_SUCCESS(s) ((s)>=0)
#define PagedPool 1
#define ALLOC_TAG 0
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define _In_opt_
#define _When_(...)
#define RtlCopyMemory memcpy
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"line %u: %s\n",__LINE__,#c); exit(1); } } while(0)
static void InitializeListHead(LIST_ENTRY* h) { h->Flink=h->Blink=h; }
static bool IsListEmpty(LIST_ENTRY* h) { CHECK(h->Flink&&h->Blink); return h->Flink==h; }
static void InsertTailList(LIST_ENTRY* h,LIST_ENTRY* e) { e->Flink=h;e->Blink=h->Blink;h->Blink->Flink=e;h->Blink=e; }
static void InsertHeadList(LIST_ENTRY* h,LIST_ENTRY* e) { e->Blink=h;e->Flink=h->Flink;h->Flink->Blink=e;h->Flink=e; }
static void RemoveEntryList(LIST_ENTRY* e) { e->Blink->Flink=e->Flink;e->Flink->Blink=e->Blink; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) { CHECK(!IsListEmpty(h));LIST_ENTRY* e=h->Flink;RemoveEntryList(e);return e; }
static int keycmp(struct btrfs_key a,struct btrfs_key b) {
    if(a.objectid!=b.objectid)return a.objectid<b.objectid?-1:1;
    if(a.type!=b.type)return a.type<b.type?-1:1;
    return a.offset==b.offset?0:a.offset<b.offset?-1:1;
}
static uint32_t calc_crc32c(uint32_t seed,const uint8_t* p,uint32_t n) { while(n--)seed=seed*33+*p++;return seed; }
static int ExIsResourceAcquiredExclusiveLite(int* lock) { CHECK(*lock==1);return 1; }
static void ExAcquireResourceExclusiveLite(int* lock,bool wait) { (void)wait;CHECK(*lock==0);*lock=1; }
static void ExReleaseResourceLite(int* lock) { CHECK(*lock==1);*lock=0; }

typedef struct { void* p; size_t size; LOOKASIDE* owner; bool queued; } allocation;
static allocation owned[4096];
static unsigned live,allocation_attempts,fail_allocation,queue_attempts,queue_successes,fail_queue;
static bool testing,clearing,failure_hit;
static NTSTATUS queue_error;
static allocation* record(void* p) {
    CHECK(p);
    for(unsigned i=0;i<4096;i++)if(owned[i].p==p)return &owned[i];
    CHECK(false);return NULL;
}
static void* allocate(size_t size,LOOKASIDE* owner) {
    if(testing&&++allocation_attempts==fail_allocation){failure_hit=true;return NULL;}
    void* p=malloc(size?size:1);CHECK(p);memset(p,0xa5,size);
    for(unsigned i=0;i<4096;i++)if(!owned[i].p) {
        owned[i]=(allocation){p,size,owner,false};live++;if(owner)owner->outstanding++;return p;
    }
    CHECK(false);return NULL;
}
static void* ExAllocatePoolWithTag(int pool,size_t size,unsigned tag) { CHECK(pool==PagedPool&&tag==ALLOC_TAG);return allocate(size,NULL); }
static void* ExAllocateFromPagedLookasideList(LOOKASIDE* l) { return allocate(l->size,l); }
static void release(void* p,LOOKASIDE* owner) {
    allocation* a=record(p);CHECK(a->owner==owner&&(!a->queued||clearing));
    if(owner){CHECK(owner->outstanding);owner->outstanding--;}
    a->p=NULL;live--;free(p);
}
static void free_fcb(fcb* f) { CHECK(!f->dirty&&f->refcount==2); f->refcount--; }
static void ExFreePool(void* p) { release(p,NULL); }
static void ExFreeToPagedLookasideList(LOOKASIDE* l,void* p) { release(p,l); }
#define insert_tree_item_batch real_insert_tree_item_batch
#include "queue.inc"
#undef insert_tree_item_batch

typedef struct { uint8_t type; enum batch_operation operation; uint64_t objectid,offset; bool payload; } expected_call;
static expected_call expected[6];
static unsigned expected_count;
static NTSTATUS insert_tree_item_batch(LIST_ENTRY* list,device_extension* v,root* r,uint64_t objectid,
                                       uint8_t type,uint64_t offset,void* data,uint16_t length,enum batch_operation op) {
    CHECK(queue_attempts<expected_count);
    expected_call e=expected[queue_attempts++];
    CHECK(type==e.type&&op==e.operation&&objectid==e.objectid&&offset==e.offset&&!!data==e.payload);
    if(data)CHECK(record(data)->size==length&&!record(data)->queued);
    if(queue_attempts==fail_queue){failure_hit=true;return queue_error;}
    NTSTATUS status=real_insert_tree_item_batch(list,v,r,objectid,type,offset,data,length,op);
    if(NT_SUCCESS(status)) {
        queue_successes++;
        if(data)record(data)->queued=true;
    }
    return status;
}
/* Subvolume mutation is outside these same-subvolume fixtures. */
static NTSTATUS add_root_ref(device_extension* v,uint64_t a,uint64_t b,struct btrfs_root_ref* rr,PIRP irp) {
    (void)v;(void)a;(void)b;(void)rr;(void)irp;CHECK(false);return STATUS_INTERNAL_ERROR;
}
static NTSTATUS delete_root_ref(device_extension* v,uint64_t a,uint64_t b,uint64_t inode,ANSI_STRING* name,PIRP irp) {
    (void)v;(void)a;(void)b;(void)inode;(void)name;(void)irp;CHECK(false);return STATUS_INTERNAL_ERROR;
}
static NTSTATUS update_root_backref(device_extension* v,uint64_t a,uint64_t b,PIRP irp) {
    (void)v;(void)a;(void)b;(void)irp;CHECK(false);return STATUS_INTERNAL_ERROR;
}
#include "sparse.inc"
#include "clear.inc"
#include "fileref.inc"
static NTSTATUS flush_extent_payloads(fcb* fcb,LIST_ENTRY* batchlist) {
    NTSTATUS Status=STATUS_SUCCESS;
    LIST_ENTRY* le;
    bool prealloc=false,extents_inline=false;
    uint64_t last_end;
#include "extents.inc"
    (void)prealloc;(void)extents_inline;(void)last_end;
    goto end;
#include "fcb_end.inc"
static NTSTATUS flush_inode_payload(fcb* fcb,LIST_ENTRY* batchlist) {
    NTSTATUS Status=STATUS_SUCCESS;
    bool cache=false;
    struct btrfs_inode_item* ii;
    uint64_t ii_offset=99;
#include "inode.inc"
    goto end;
#include "fcb_end.inc"

static void expect(uint8_t type,enum batch_operation operation,uint64_t objectid,uint64_t offset,bool payload) {
    expected[expected_count++]=(expected_call){type,operation,objectid,offset,payload};
}
static unsigned inspect_queue(LIST_ENTRY* list) {
    unsigned items=0;
    for(LIST_ENTRY* le=list->Flink;le!=list;le=le->Flink) {
        batch_root* br=CONTAINING_RECORD(le,batch_root,list_entry);
        for(LIST_ENTRY* li=br->items_ind.Flink;li!=&br->items_ind;li=li->Flink) {
            batch_item_ind* bii=CONTAINING_RECORD(li,batch_item_ind,list_entry);unsigned count=0;
            for(LIST_ENTRY* bi=bii->items.Flink;bi!=&bii->items;bi=bi->Flink) {
                CHECK(++items<1100&&bi->Flink->Blink==bi&&bi->Blink->Flink==bi);count++;
                batch_item* item=CONTAINING_RECORD(bi,batch_item,list_entry);
                if(item->data)CHECK(record(item->data)->queued&&record(item->data)->size==item->datalen);
            }
            CHECK(count==bii->num_items);
        }
    }
    return items;
}

static const unsigned max_allocations[]={6,4,8,7,13,3,7};
static void exercise(unsigned scenario,unsigned pool_failure,unsigned queue_failure,NTSTATUS error) {
    device_extension v={0};v.batch_item_lookaside.size=sizeof(batch_item);v.dirty_fcbs_lock=1;
    v.superblock.generation=9;v.superblock.incompat_flags=BTRFS_FEATURE_INCOMPAT_NO_HOLES;
    root subvol={.id=7};LIST_ENTRY batches,dirty;InitializeListHead(&batches);InitializeListHead(&dirty);
    fcb child={.Vcb=&v,.subvol=&subvol,.inode=2000,.refcount=2,.dirty=true,.inode_item_changed=true,.type=BTRFS_FT_REG_FILE};
    fcb parent={.Vcb=&v,.subvol=&subvol,.inode=1000,.type=BTRFS_FT_DIR};
    InitializeListHead(&child.extents);InsertTailList(&dirty,&child.list_entry_dirty);
    dir_child dc={.utf8={3,3,"abc"},.index=8};
    file_ref parent_ref={.fcb=&parent};file_ref ref={.fcb=&child,.parent=&parent_ref,.dc=&dc,.oldindex=6,.dirty=true};
    uint32_t name_hash=calc_crc32c(0xfffffffe,(uint8_t*)"abc",3),old_hash=calc_crc32c(0xfffffffe,(uint8_t*)"old",3);
    unsigned kind=scenario==5?1:scenario==6?2:scenario;
    unsigned initial=scenario>=5?1000:0;
    for(unsigned i=0;i<initial;i++)CHECK(real_insert_tree_item_batch(&batches,&v,&subvol,i,BTRFS_INODE_ITEM_KEY,0,NULL,0,Batch_Insert)==STATUS_SUCCESS);
    extent extents[2]={0};
    if(kind==0) {
        for(unsigned i=0;i<2;i++) {
            extents[i].offset=i*4096;extents[i].datalen=sizeof(struct btrfs_file_extent_item);extents[i].inserted=true;
            extents[i].extent_data=(struct btrfs_file_extent_item){.type=BTRFS_FILE_EXTENT_REG,.ram_bytes=4096,.num_bytes=4096,
                .disk_bytenr=1048576+i*1048576,.disk_num_bytes=4096};
            InsertTailList(&child.extents,&extents[i].list_entry);
            expect(BTRFS_EXTENT_DATA_KEY,Batch_Insert,2000,i*4096,true);
        }
    } else if(kind==1) {
        child.inode_item=(struct btrfs_inode_item){.generation=9,.size=8192,.nlink=1};
        expect(BTRFS_INODE_ITEM_KEY,Batch_Insert,2000,99,true);
    } else if(kind==2) {
        ref.created=true;
        expect(BTRFS_DIR_INDEX_KEY,Batch_Insert,1000,8,true);
        expect(BTRFS_DIR_ITEM_KEY,Batch_DirItem,1000,name_hash,true);
        expect(BTRFS_INODE_REF_KEY,Batch_InodeRef,2000,1000,true);
    } else {
        ref.deleted=kind==3;
        ref.oldutf8=(ANSI_STRING){3,3,allocate(3,NULL)};memcpy(ref.oldutf8.Buffer,"old",3);
        expect(BTRFS_DIR_ITEM_KEY,Batch_DeleteDirItem,1000,old_hash,true);
        if(kind==4)expect(BTRFS_DIR_ITEM_KEY,Batch_DirItem,1000,name_hash,true);
        expect(BTRFS_INODE_REF_KEY,Batch_DeleteInodeRef,2000,1000,true);
        if(kind==4)expect(BTRFS_INODE_REF_KEY,Batch_InodeRef,2000,1000,true);
        expect(BTRFS_DIR_INDEX_KEY,Batch_Delete,1000,kind==3?6:8,false);
        if(kind==4)expect(BTRFS_DIR_INDEX_KEY,Batch_Insert,1000,8,true);
    }
    fail_allocation=pool_failure;fail_queue=queue_failure;queue_error=error;testing=true;
    NTSTATUS status=kind==0?flush_extent_payloads(&child,&batches):kind==1?flush_inode_payload(&child,&batches):flush_fileref(&ref,&batches,NULL);
    testing=false;
    CHECK(status==(queue_failure?error:pool_failure?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS));
    CHECK(failure_hit==!!(pool_failure||queue_failure));
    if(NT_SUCCESS(status))CHECK(allocation_attempts==max_allocations[scenario]);
    if(kind<=1)CHECK(!child.dirty&&IsListEmpty(&dirty)&&child.refcount==1);
    else CHECK(child.refcount==2&&ref.dirty==!NT_SUCCESS(status));
    if(kind==2)CHECK(ref.created==!NT_SUCCESS(status));
    if(kind==1)CHECK(child.inode_item_changed==!NT_SUCCESS(status));
    if(!pool_failure)CHECK(queue_attempts==(queue_failure?queue_failure:expected_count));
    unsigned queued=inspect_queue(&batches);
    CHECK(queued==initial+queue_successes);
    clearing=true;clear_batch_list(&v,&batches);clearing=false;
    if(ref.oldutf8.Buffer)ExFreePool(ref.oldutf8.Buffer);
    CHECK(IsListEmpty(&batches)&&!live&&!v.batch_item_lookaside.outstanding);
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    const char* names[]={"extent","inode","create","delete","rename","inode-split","create-split"};
    const unsigned calls[]={2,1,3,3,6,1,3};
    for(unsigned scenario=0;scenario<7;scenario++) {
        for(unsigned failure=0;failure<=max_allocations[scenario];failure++) {
            char name[80];snprintf(name,sizeof(name),"%s-pool%u",names[scenario],failure);
            if(!strcmp(argv[1],"--list")){puts(name);continue;}if(strcmp(argv[1],name))continue;
            exercise(scenario,failure,0,STATUS_SUCCESS);return 0;
        }
        for(unsigned failure=1;failure<=calls[scenario];failure++)for(unsigned error=0;error<2;error++) {
            char name[80];snprintf(name,sizeof(name),"%s-queue%u-error%u",names[scenario],failure,error);
            if(!strcmp(argv[1],"--list")){puts(name);continue;}if(strcmp(argv[1],name))continue;
            exercise(scenario,0,failure,error?STATUS_DATA_ERROR:STATUS_INTERNAL_ERROR);return 0;
        }
    }
    return strcmp(argv[1],"--list")?2:0;
}
