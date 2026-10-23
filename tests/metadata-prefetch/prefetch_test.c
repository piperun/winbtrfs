/* Removing the prepass or serializing reads must fail the four-read gate. */
#include "host.h"
#include "io-types.inc"
_Static_assert(sizeof(read_data_stripe)<=256,"transient stripe reservation must cover actual read bookkeeping");
#include "search.inc"
#include "loader.inc"
#include "free.inc"
#include "prefetch.inc"

static device_extension vcb;
static root checksum;
static root_nonpaged root_np;
static tree parent;
static tree_nonpaged parent_np;
static tree_data* children;
static fcb file;
static ERESOURCE resource;
static extent changed;
static struct btrfs_chunk ci;
static chunk metadata_chunk;
static unsigned total, reads, worker_reads, active, maximum, first_completed_at, fail_read;
static unsigned leaf_items = 1, bad_header, first_ordinal;
static bool discovery;
static unsigned scenarios;
static bool gate;
static pthread_mutex_t read_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cond=PTHREAD_COND_INITIALIZER;

static chunk* get_chunk_from_address(device_extension* v, uint64_t address) { (void)v; (void)address; return &metadata_chunk; }
NTSTATUS read_data(device_extension* v, uint64_t address, uint32_t bytes, void* csum, bool is_tree,
                   uint8_t* buf, chunk* c, chunk** pc, PIRP irp, uint64_t generation, bool file_read, ULONG priority) {
    (void)c; (void)pc; (void)priority;
    CHECK(is_tree && !csum && !irp && !file_read && generation==11 && bytes==v->superblock.nodesize);
    bool worker=!pthread_equal(owner,pthread_self());
    CHECK(!v->dirty_fcbs_lock.held && !resource.held);
    pthread_mutex_lock(&read_lock);
    reads++; unsigned ordinal=reads;
    if(worker)worker_reads++;
    active++; if(active>maximum)maximum=active;
    pthread_cond_broadcast(&read_cond);
    if(gate && ordinal<=4) {
        struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline); deadline.tv_sec++;
        while(reads<4) if(pthread_cond_timedwait(&read_cond,&read_lock,&deadline))break;
    }
    if(gate && ordinal==1) {
        pthread_mutex_unlock(&read_lock);
        struct timespec delay={0,2000000}; nanosleep(&delay,NULL);
        pthread_mutex_lock(&read_lock);
    }
    if(!first_completed_at) { first_completed_at=reads; first_ordinal=ordinal; }
    active--; pthread_mutex_unlock(&read_lock);
    if(ordinal==fail_read)return STATUS_CRC_ERROR;
    memset(buf,0,bytes);
    struct btrfs_header* h=(void*)buf;
    h->bytenr=address; h->generation=11; h->nritems=leaf_items;
    struct btrfs_item* item=(void*)(buf+sizeof(*h));
    item->key=(struct btrfs_key){BTRFS_EXTENT_CSUM_OBJECTID,BTRFS_EXTENT_CSUM_KEY,((address-0x100000)/bytes)*65536};
    item->offset=bytes-sizeof(*h)-16; item->size=16;
    memset(buf+bytes-16,0x7b,16);
    if(bad_header==1)h->generation++;
    if(bad_header==2)h->bytenr++;
    if(bad_header==3)h->level=1;
    if(bad_header==4)h->nritems=UINT32_MAX;
    if(bad_header==5)item->offset=bytes;
    if(discovery && address==0x80000) {
        CHECK(!worker);
        h->level=1; h->nritems=total;
        struct btrfs_key_ptr* pointers=(void*)(buf+sizeof(*h));
        for(unsigned i=0;i<total;i++) {
            pointers[i].key=children[i].key;
            pointers[i].blockptr=children[i].treeholder.address;
            pointers[i].generation=11;
        }
    }
    return STATUS_SUCCESS;
}
static NTSTATUS do_write(device_extension* v, PIRP irp) {
    (void)irp; CHECK(!alive && !active);
    for(unsigned i=0;i<total;i++) if(!children[i].treeholder.tree)
        CHECK(NT_SUCCESS(do_load_tree(v,&children[i].treeholder,&checksum,&parent,&children[i],NULL)));
    return STATUS_SUCCESS;
}
static void free_trees(device_extension* v) {
    while(!IsListEmpty(&v->trees))free_tree(CONTAINING_RECORD(v->trees.Blink,tree,list_entry));
}
#include "flush.inc"

static void setup(unsigned leaves, unsigned nodesize) {
    scenarios++;
    owner=pthread_self(); memset(&vcb,0,sizeof(vcb)); memset(&checksum,0,sizeof(checksum));
    memset(&parent,0,sizeof(parent)); memset(&file,0,sizeof(file)); memset(&changed,0,sizeof(changed));
    memset(&resource,0,sizeof(resource)); memset(&root_np,0,sizeof(root_np));
    total=leaves; reads=worker_reads=active=maximum=first_completed_at=fail_read=0;
    creates=joins=closes=fail_create=alive=0; allocations=releases=allocation_attempts=fail_at=0; peak_bytes=0; CHECK(!live_bytes);
    gate=discovery=false; leaf_items=1; bad_header=first_ordinal=0;
    vcb.superblock.nodesize=nodesize; vcb.need_write=true; vcb.log_to_phys_loaded=true; vcb.sector_shift=12;
    InitializeListHead(&vcb.trees); InitializeListHead(&vcb.trees_hash); InitializeListHead(&vcb.dirty_fcbs);
    ExInitializeFastMutex(&vcb.trees_list_mutex); ExInitializeFastMutex(&parent_np.mutex);
    parent.nonpaged=&parent_np; parent.header.level=1; parent.header.nritems=leaves; parent.root=&checksum;
    InitializeListHead(&parent.itemlist); checksum.nonpaged=&root_np; checksum.root_item.level=1;
    checksum.treeholder.tree=&parent; vcb.checksum_root=&checksum;
    children=calloc(leaves,sizeof(*children)); CHECK(children);
    for(unsigned i=0;i<leaves;i++) {
        children[i].key=(struct btrfs_key){BTRFS_EXTENT_CSUM_OBJECTID,BTRFS_EXTENT_CSUM_KEY,(uint64_t)i*65536};
        children[i].treeholder.address=0x100000+(uint64_t)i*nodesize; children[i].treeholder.generation=11;
        InsertTailList(&parent.itemlist,&children[i].list_entry);
    }
    file.Header.Resource=&resource; file.extents_changed=true; InitializeListHead(&file.extents);
    InsertTailList(&vcb.dirty_fcbs,&file.list_entry_dirty); InsertTailList(&file.extents,&changed.list_entry);
    changed.inserted=true; changed.csum=&changed; changed.extent_data.type=BTRFS_FILE_EXTENT_REG;
    changed.extent_data.disk_num_bytes=changed.extent_data.num_bytes=(uint64_t)leaves*65536;
    ci=(struct btrfs_chunk){BTRFS_BLOCK_GROUP_METADATA|BTRFS_BLOCK_GROUP_DUP,UINT64_MAX,2};
    metadata_chunk=(chunk){0,&ci,NULL};
}
static void finish(void) {
    CHECK(!alive && !active && joins==closes);
    free_trees(&vcb); CHECK(!live_bytes && allocations==releases);
    pthread_mutex_destroy(&vcb.trees_list_mutex.lock); pthread_mutex_destroy(&parent_np.mutex.lock);
    free(children);
}
#if HAVE_PREFETCH
static void run_preload(void) {
    ExAcquireResourceExclusiveLite(&vcb.tree_lock,true);
    preload_checksum_trees(&vcb);
    CHECK(!alive && !active && joins==closes);
    ExReleaseResourceLite(&vcb.tree_lock);
}
static unsigned loaded(void) {
    unsigned result=0;
    for(unsigned i=0;i<total;i++)result+=children[i].treeholder.tree!=NULL;
    return result;
}
static void failure_controls(void) {
    for(unsigned failure=1;failure<=4;failure++) {
        setup(64,16384); fail_create=failure; run_preload();
        CHECK(!worker_reads && !loaded() && joins==failure-1); finish();
    }
    for(unsigned failure=1;failure<=30;failure++) {
        setup(64,16384); fail_at=failure; run_preload(); fail_at=0;
        CHECK(!alive && maximum<=4); finish();
    }
    for(unsigned failure=1;failure<=5;failure++) {
        setup(64,16384); bad_header=failure; run_preload();
        CHECK(!loaded() && worker_reads==64); finish();
    }
    setup(64,16384); fail_read=3; run_preload();
    CHECK(loaded()==63 && !children[2].treeholder.tree); fail_read=0;
    CHECK(NT_SUCCESS(do_load_tree(&vcb,&children[2].treeholder,&checksum,&parent,&children[2],NULL)));
    CHECK(loaded()==64); finish();
}
static void selection_controls(void) {
    setup(128,16384);
    changed.extent_data.disk_bytenr=32*65536; changed.extent_data.num_bytes=32*65536;
    run_preload(); CHECK(worker_reads==34);
    for(unsigned i=0;i<128;i++)CHECK((children[i].treeholder.tree!=NULL)==(i>=32 && i<=65));
    finish();
    for(unsigned count=0;count<32;count++) {
        setup(count,16384); run_preload(); CHECK(!creates && !worker_reads); finish();
    }
    for(unsigned mode=0;mode<8;mode++) {
        setup(64,16384);
        if(mode==0)file.deleted=true;
        if(mode==1)file.ads=true;
        if(mode==2)file.extents_changed=false;
        if(mode==3)changed.ignore=true;
        if(mode==4)changed.inserted=false;
        if(mode==5)changed.csum=NULL;
        if(mode==6)changed.extent_data.disk_num_bytes=0;
        if(mode==7)changed.extent_data.type=2;
        run_preload(); CHECK(!creates && !worker_reads); finish();
    }
    setup(64,16384); changed.extent_data.compression=1; changed.extent_data.offset=UINT64_MAX;
    changed.extent_data.num_bytes=1; run_preload(); CHECK(loaded()==64); finish();
    setup(64,16384); changed.extent_data.disk_bytenr=UINT64_MAX-2; changed.extent_data.offset=3;
    run_preload(); CHECK(!creates); finish();
    setup(64,16384); children[20].ignore=true; run_preload(); CHECK(!creates); finish();
    setup(64,16384); children[20].key=children[19].key; run_preload(); CHECK(!creates); finish();
    setup(64,16384); parent.header.level=0; run_preload(); CHECK(!creates); finish();
    setup(64,16384);
    CHECK(NT_SUCCESS(do_load_tree(&vcb,&children[5].treeholder,&checksum,&parent,&children[5],NULL)));
    reads=0; run_preload(); CHECK(worker_reads==63 && loaded()==64); finish();
}
static void profile_controls(void) {
    const uint64_t accepted[]={0,32,16,512,1024}, rejected[]={8,64,128,256,2048,48};
    for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);i++) {
        setup(64,65536); ci.type=4|accepted[i]; ci.num_stripes=4;
        run_preload(); CHECK(loaded()==64 && maximum<=4); CHECK(peak_bytes<=PREFETCH_TREE_BUDGET+PREFETCH_PLAN_BUDGET); finish();
    }
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);i++) {
        setup(64,16384); ci.type=4|rejected[i]; run_preload(); CHECK(!creates && !reads); finish();
    }
    setup(64,16384); ci.num_stripes=5; run_preload(); CHECK(!creates); finish();
    setup(64,16384); ci.num_stripes=0; run_preload(); CHECK(!creates); finish();
}
static void range_controls(void) {
    setup(64,16384);
    extent extra[4]={0};
    const uint64_t starts[]={20000,0,12000,20000}, lengths[]={4096,8192,8192,4096};
    RemoveEntryList(&changed.list_entry);
    for(unsigned i=0;i<4;i++) {
        extra[i]=changed; extra[i].extent_data.disk_bytenr=starts[i]; extra[i].extent_data.offset=0;
        extra[i].extent_data.num_bytes=lengths[i];
        InsertTailList(&file.extents,&extra[i].list_entry);
    }
    prefetch_range ranges[16]; prefetch_context ctx={0}; ctx.Vcb=&vcb; ctx.ranges=ranges;
    prefetch_collect_ranges(&ctx);
    CHECK(ctx.range_count==2 && ranges[0].start==0 && ranges[0].end==8192);
    CHECK(ranges[1].start==12000 && ranges[1].end==24096);
    finish();
}
static void budget_control(void) {
    setup(4096,65536); leaf_items=1000;
    run_preload(); CHECK(loaded()>32 && loaded()<1000);
    CHECK(worker_reads<1000); /* Stop optional I/O when the tree budget fills. */
    CHECK(peak_bytes<=PREFETCH_TREE_BUDGET+PREFETCH_PLAN_BUDGET); finish();
}
static void publication_controls(void) {
    setup(64,16384);
    uint8_t* buffer=ExAllocatePoolWithTag(PagedPool,16384,ALLOC_TAG);
    CHECK(NT_SUCCESS(read_data(&vcb,children[0].treeholder.address,16384,NULL,true,buffer,NULL,NULL,NULL,11,false,0)));
    uint64_t address=children[0].treeholder.address;
    children[0].treeholder.address++;
    CHECK(!publish_prefetched_tree(&vcb,&children[0].treeholder,&checksum,&parent,&children[0],address,11,buffer));
    children[0].treeholder.address=address; children[0].treeholder.generation++;
    CHECK(!publish_prefetched_tree(&vcb,&children[0].treeholder,&checksum,&parent,&children[0],address,11,buffer));
    children[0].treeholder.generation=11;
    CHECK(publish_prefetched_tree(&vcb,&children[0].treeholder,&checksum,&parent,&children[0],address,11,buffer));
    buffer=ExAllocatePoolWithTag(PagedPool,16384,ALLOC_TAG);
    CHECK(NT_SUCCESS(read_data(&vcb,address,16384,NULL,true,buffer,NULL,NULL,NULL,11,false,0)));
    CHECK(!publish_prefetched_tree(&vcb,&children[0].treeholder,&checksum,&parent,&children[0],address,11,buffer));
    ExFreePool(buffer); finish();
}
static void discovery_controls(void) {
    setup(64,16384); discovery=true;
    tree_data branch={0}; branch.key=children[0].key;
    branch.treeholder=(tree_holder){0x80000,11,NULL};
    InitializeListHead(&parent.itemlist); InsertTailList(&parent.itemlist,&branch.list_entry);
    parent.header.level=2; parent.header.nritems=1;
    run_preload(); CHECK(reads==65 && worker_reads==64 && branch.treeholder.tree);
    unsigned count=0;
    tree* internal=branch.treeholder.tree;
    for(LIST_ENTRY* e=internal->itemlist.Flink;e!=&internal->itemlist;e=e->Flink)
        count+=CONTAINING_RECORD(e,tree_data,list_entry)->treeholder.tree!=NULL;
    CHECK(count==64); finish();
}
static void cap_controls(void) {
    setup(5000,16384);
    prefetch_range range={0,UINT64_MAX};
    prefetch_job* jobs=calloc(4096,sizeof(*jobs)); CHECK(jobs);
    prefetch_context ctx={0}; ctx.Vcb=&vcb; ctx.ranges=&range; ctx.range_count=1; ctx.jobs=jobs;
    prefetch_plan_trees(&ctx); CHECK(ctx.job_count==4096 && !reads); free(jobs); finish();
    setup(64,16384);
    extent* extents=calloc(32769,sizeof(*extents)); CHECK(extents);
    prefetch_range* ranges=calloc(32768,sizeof(*ranges)); CHECK(ranges);
    InitializeListHead(&file.extents);
    for(unsigned i=0;i<32769;i++) {
        extents[i]=changed; extents[i].extent_data.disk_bytenr=(uint64_t)i*8192;
        extents[i].extent_data.num_bytes=4096;
        InsertTailList(&file.extents,&extents[i].list_entry);
    }
    memset(&ctx,0,sizeof(ctx)); ctx.Vcb=&vcb; ctx.ranges=ranges;
    prefetch_collect_ranges(&ctx); CHECK(ctx.range_count==32768 && ranges[32767].start==UINT64_C(268427264));
    free(ranges); free(extents); finish();
}
static void demand_controls(void) {
    for(unsigned preload=0;preload<2;preload++) {
        setup(64,16384); if(preload)run_preload();
        for(unsigned i=0;i<64;i++) {
            struct btrfs_key key={BTRFS_EXTENT_CSUM_OBJECTID,BTRFS_EXTENT_CSUM_KEY,(uint64_t)i*65536};
            traverse_ptr result={0};
            CHECK(find_item(&vcb,&checksum,&result,&key,false,NULL)==STATUS_SUCCESS);
            CHECK(result.item->key.offset==(uint64_t)i*65536 && result.item->size==16);
            for(unsigned b=0;b<16;b++)CHECK(result.item->data[b]==0x7b);
        }
        CHECK(reads==64); finish();
    }
}
#endif
int main(int argc, char** argv) {
    (void)argv;
    setup(64,16384); gate=true; do_flush(&vcb);
    printf("{\"leaves\":64,\"reads_before_first_completion\":%u,\"maximum_reads\":%u,\"readers_created\":%u,\"joined\":%u,\"peak_requested_pool\":%zu}\n",first_completed_at,maximum,creates,joins,peak_bytes);
    fflush(stdout);
    CHECK(first_completed_at>=4 && first_ordinal!=1 && maximum==4 && creates==4 && joins==4); finish();
    if(argc>1)return 0;
#if HAVE_PREFETCH
    failure_controls(); selection_controls(); profile_controls(); range_controls(); budget_control();
    publication_controls(); discovery_controls(); cap_controls();
    demand_controls();
#endif
    printf("%u production preload concurrency/ownership scenarios passed\n",scenarios);
}
