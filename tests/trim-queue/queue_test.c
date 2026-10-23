/* Deferred lower-device completions exercise production ownership boundaries. */
#include "queue_host.h"
#include "queue-types.inc"
#include "queue_services.inc"
#include "spaces.inc"
#include "reservations.inc"
#include "queue.inc"
#include "mapping.inc"
#include "clean.inc"
#include "balance.inc"

static device_extension volume;
static chunk chunks[3];
static device devices[3];
static DEVICE_OBJECT objects[3];
static FILE_OBJECT files[3];
static struct btrfs_chunk items[3];
static void range(LIST_ENTRY* head,uint64_t start,uint64_t length) {
    space* s=ExAllocatePoolWithTag(PagedPool,sizeof(*s),ALLOC_TAG);CHECK(s);
    s->address=start;s->size=length;InsertTailList(head,&s->list_entry);
}
static bool contains(LIST_ENTRY* head,uint64_t address) {
    for(LIST_ENTRY* le=head->Flink;le!=head;le=le->Flink){space* s=CONTAINING_RECORD(le,space,list_entry);if(address>=s->address&&address-s->address<s->size)return true;}
    return false;
}
static void setup(unsigned count) {
    InitializeListHead(&volume.devices);InitializeListHead(&volume.chunks);
    volume.trim=true;active=&volume;
    for(unsigned i=0;i<count;i++) {
        objects[i].StackSize=4;files[i].id=i;objects[i].id=i;
        devices[i].devobj=&objects[i];devices[i].fileobj=&files[i];devices[i].trim=true;
        InitializeListHead(&devices[i].space);InitializeListHead(&devices[i].trim_list);
        InsertTailList(&volume.devices,&devices[i].list_entry);
        chunks[i].offset=0x10000*(i+1);chunks[i].chunk_item=&items[i];chunks[i].space_changed=true;
        chunks[i].devices=malloc(sizeof(device*));CHECK(chunks[i].devices);chunks[i].devices[0]=&devices[i];
        items[i].type=BTRFS_BLOCK_GROUP_DATA;items[i].num_stripes=1;items[i].stripe[0].offset=0x100000*(i+1);
        InitializeListHead(&chunks[i].space);InitializeListHead(&chunks[i].space_size);InitializeListHead(&chunks[i].deleting);InitializeListHead(&chunks[i].pending_trims);
        InsertTailList(&volume.chunks,&chunks[i].list_entry);
        CHECK(NT_SUCCESS(space_list_add2(&chunks[i].space,&chunks[i].space_size,chunks[i].offset+0x8000,0x1000,NULL,NULL)));
        range(&chunks[i].deleting,chunks[i].offset+0x2000,0x1000);
    }
    trim_initialize(&volume);CHECK(volume.trim_state);
}
static void commit(void){ExAcquireResourceExclusiveLite(&volume.tree_lock,true);trim_commit(&volume);ExReleaseResourceLite(&volume.tree_lock);}
static void finish_all(bool reverse) {
    while(pending_count){unsigned n=reverse?pending_count-1:0;PIRP irp=pending_irps[n];pending_irps[n]=pending_irps[--pending_count];complete(irp);}
}
static void cleanup(void) {
    volume.removing=true;trim_drain(&volume);trim_destroy(&volume);
    while(!IsListEmpty(&volume.chunks)) {
        chunk* c=CONTAINING_RECORD(RemoveHeadList(&volume.chunks),chunk,list_entry);
        CHECK(IsListEmpty(&c->pending_trims));free_spaces(&c->space);free_spaces(&c->deleting);free(c->devices);
    }
    while(!IsListEmpty(&volume.devices)) {
        device* d=CONTAINING_RECORD(RemoveHeadList(&volume.devices),device,list_entry);free_spaces(&d->space);free_spaces(&d->trim_list);
    }
    CHECK(!pool_live&&!irp_live&&!references&&!pending_count);CHECK(!volume.tree_lock.held&&!volume.chunk_lock.held);
}
static void deferred(void) {
    setup(1);CHECK(trim_event(&volume));commit();CHECK(IsListEmpty(&chunks[0].deleting));CHECK(trim_chunk_pending(&chunks[0]));
    CHECK(!contains(&chunks[0].space,0x12000));CHECK(contains(&chunks[0].space,0x18000));
    trim_process(&volume);CHECK(pending_count==1&&waits==0);
    CHECK(!volume.tree_lock.held&&!chunks[0].held);/* ordinary open can acquire the resource */
    ExAcquireResourceSharedLite(&volume.tree_lock,true);ExReleaseResourceLite(&volume.tree_lock);
    CHECK(!contains(&chunks[0].space,0x12000));finish_all(false);trim_process(&volume);
    CHECK(!trim_chunk_pending(&chunks[0])&&contains(&chunks[0].space,0x12000));CHECK(submitted==1);cleanup();
}
static void repeated(void) {
    setup(1);commit();trim_process(&volume);range(&chunks[0].deleting,0x14000,0x1000);commit();trim_process(&volume);
    CHECK(submitted==1&&!contains(&chunks[0].space,0x14000));finish_all(false);trim_process(&volume);trim_process(&volume);
    CHECK(submitted==2&&pending_count==1);finish_all(false);trim_process(&volume);
    CHECK(contains(&chunks[0].space,0x12000)&&contains(&chunks[0].space,0x14000));cleanup();
}
static void cache_view(void) {
    setup(1);commit();trim_process(&volume);LIST_ENTRY view;InitializeListHead(&view);
    CHECK(NT_SUCCESS(trim_merge_pending(&chunks[0],&view,NULL)));CHECK(contains(&view,0x12000));CHECK(!contains(&chunks[0].space,0x12000));
    free_spaces(&view);finish_all(false);trim_process(&volume);cleanup();
}
static void reclaim_failure(void) {
    setup(1);commit();trim_process(&volume);finish_all(false);fail_at=allocations+1;trim_process(&volume);
    CHECK(trim_chunk_pending(&chunks[0])&&submitted==1);fail_at=0;trim_process(&volume);
    CHECK(!trim_chunk_pending(&chunks[0])&&submitted==1);cleanup();
}
static void preparation_failure(unsigned slot) {
    setup(3);fail_at=allocations+slot;commit();fail_at=0;
    if(trim_chunk_pending(&chunks[0])){trim_process(&volume);finish_all(true);trim_process(&volume);}
    else {for(unsigned i=0;i<3;i++)CHECK(!trim_chunk_pending(&chunks[i])&&!IsListEmpty(&chunks[i].deleting));CHECK(!submitted);}
    cleanup();
}
static void modes(unsigned mode,NTSTATUS status) {
    inline_completion=mode==0;completion_status=status;setup(3);commit();trim_process(&volume);finish_all(mode==2);trim_process(&volume);
    for(unsigned i=0;i<3;i++){CHECK(contains(&chunks[i].space,chunks[i].offset+0x2000));}CHECK(submitted==3&&waits==0);cleanup();
}
static void physical(void) {
    setup(1);free_spaces(&chunks[0].deleting);
    range(&devices[0].space,0x100000,0x2000);range(&devices[0].space,0x104000,0x4000);
    range(&devices[0].trim_list,0x100000,0x8000);devices[0].num_trim_entries=1;
    commit();CHECK(!contains(&devices[0].space,0x100000)&&!contains(&devices[0].space,0x104000));trim_process(&volume);
    CHECK(pending_count==1);DEVICE_MANAGE_DATA_SET_ATTRIBUTES* data=pending_irps[0]->AssociatedIrp.SystemBuffer;
    DEVICE_DATA_SET_RANGE* r=(void*)((char*)data+data->DataSetRangesOffset);CHECK(data->DataSetRangesLength==32);
    CHECK(r[0].StartingOffset==0x100000&&r[0].LengthInBytes==0x2000&&r[1].StartingOffset==0x104000&&r[1].LengthInBytes==0x4000);
    finish_all(false);trim_process(&volume);CHECK(contains(&devices[0].space,0x100000)&&!contains(&devices[0].space,0x102000));cleanup();
}
static void ceiling(void) {
    setup(1);free_spaces(&chunks[0].deleting);
    for(unsigned i=0;i<100000;i++)range(&chunks[0].deleting,0x10000+(uint64_t)i*8192,4096);
    commit();CHECK(!trim_chunk_pending(&chunks[0])&&!IsListEmpty(&chunks[0].deleting)&&!submitted);cleanup();
}
static void partial_reclaim(void) {
    setup(1);range(&chunks[0].deleting,0x14000,0x1000);commit();trim_process(&volume);finish_all(false);
    fail_at=allocations+2;trim_process(&volume);fail_at=0;
    CHECK(contains(&chunks[0].space,0x12000)&&!contains(&chunks[0].space,0x14000));
    CHECK(NT_SUCCESS(space_list_subtract2(&chunks[0].space,&chunks[0].space_size,0x12000,0x1000,NULL,NULL)));
    LIST_ENTRY view;InitializeListHead(&view);CHECK(NT_SUCCESS(trim_merge_pending(&chunks[0],&view,NULL)));
    CHECK(!contains(&view,0x12000)&&contains(&view,0x14000));free_spaces(&view);
    trim_process(&volume);CHECK(!contains(&chunks[0].space,0x12000)&&contains(&chunks[0].space,0x14000));cleanup();
}
static void removed_device(void) {
    setup(1);free_spaces(&chunks[0].deleting);range(&devices[0].space,0x100000,0x10000);range(&devices[0].trim_list,0x102000,0x1000);
    commit();trim_process(&volume);CHECK(pending_count==1);trim_forget_device(&volume,&devices[0]);
    finish_all(false);trim_process(&volume);CHECK(!contains(&devices[0].space,0x102000));cleanup();
}
static void removal_before_submission(void) {
    setup(1);commit();trim_forget_device(&volume,&devices[0]);trim_process(&volume);trim_process(&volume);CHECK(!submitted);cleanup();
}
static void regenerated_map(void) {
    setup(1);free_spaces(&chunks[0].deleting);range(&devices[0].space,0x100000,0x10000);range(&devices[0].trim_list,0x102000,0x1000);
    commit();trim_process(&volume);free_spaces(&devices[0].space);range(&devices[0].space,0x100000,0x10000);
    CHECK(NT_SUCCESS(trim_subtract_device_pending(&volume,&devices[0])));CHECK(!contains(&devices[0].space,0x102000)&&contains(&devices[0].space,0x101000));
    finish_all(false);trim_process(&volume);cleanup();
}
static void queued_shutdown(void) {setup(1);commit();CHECK(!submitted);cleanup();CHECK(!submitted);}
static void inflight_shutdown(void) {setup(1);commit();trim_process(&volume);cleanup();CHECK(waits==1);}
static void physical_oom(unsigned slot) {
    setup(1);free_spaces(&chunks[0].deleting);range(&devices[0].space,0x100000,0x10000);range(&devices[0].trim_list,0x102000,0x1000);
    fail_at=allocations+slot;commit();fail_at=0;
    if(contains(&devices[0].space,0x102000)){CHECK(contains(&devices[0].space,0x100000)&&contains(&devices[0].space,0x10ffff));CHECK(!submitted);}
    else {trim_process(&volume);finish_all(false);trim_process(&volume);CHECK(contains(&devices[0].space,0x102000));}
    cleanup();
}
static void poll_during_signal(void) {trim_process(&volume);volume.removing=true;trim_destroy(&volume);}
static void completion_poll_race(void) {setup(1);commit();trim_process(&volume);preempt_on_signal=true;finish_all(false);cleanup();}
static void clean_merge_failure(unsigned failing) {
    setup(3);volume.options.no_trim=true;fail_at=allocations+failing;
    ExAcquireResourceExclusiveLite(&volume.tree_lock,true);
    CHECK(clean_space_cache(&volume)==STATUS_INSUFFICIENT_RESOURCES);
    ExReleaseResourceLite(&volume.tree_lock);fail_at=0;
    CHECK(!volume.chunk_lock.held&&!submitted);
    for(unsigned i=0;i<3;i++){bool merged=i+1<failing;CHECK(!chunks[i].held&&IsListEmpty(&chunks[i].deleting)==merged&&chunks[i].space_changed==!merged);}
    cleanup();
}
static void clean_queues_without_wait(void) {
    setup(3);ExAcquireResourceExclusiveLite(&volume.tree_lock,true);CHECK(clean_space_cache(&volume)==STATUS_SUCCESS);ExReleaseResourceLite(&volume.tree_lock);
    for(unsigned i=0;i<3;i++){CHECK(!chunks[i].space_changed&&IsListEmpty(&chunks[i].deleting)&&trim_chunk_pending(&chunks[i]));}
    trim_process(&volume);CHECK(pending_count==3&&waits==0);finish_all(false);trim_process(&volume);cleanup();
}
static void balance_excludes_pending(void) {
    setup(1);volume.balance.opts[0].flags=BTRFS_BALANCE_OPTS_ENABLED;
    CHECK(should_balance_chunk(&volume,0,&chunks[0]));commit();CHECK(!should_balance_chunk(&volume,0,&chunks[0]));
    trim_process(&volume);CHECK(!should_balance_chunk(&volume,0,&chunks[0]));finish_all(false);fail_at=allocations+1;trim_process(&volume);fail_at=0;
    CHECK(!should_balance_chunk(&volume,0,&chunks[0]));trim_process(&volume);CHECK(should_balance_chunk(&volume,0,&chunks[0]));cleanup();
}
static void relocating_chunk_not_queued(void) {
    setup(1);chunks[0].list_entry_balance.Flink=(LIST_ENTRY*)1;commit();CHECK(!trim_chunk_pending(&chunks[0])&&!IsListEmpty(&chunks[0].deleting));
    chunks[0].list_entry_balance.Flink=NULL;chunks[0].reloc=true;commit();CHECK(!trim_chunk_pending(&chunks[0]));cleanup();
}
static void initialization_oom(void) {
    InitializeListHead(&volume.chunks);InitializeListHead(&volume.devices);active=&volume;
    fail_at=1;trim_initialize(&volume);CHECK(!volume.trim_state);trim_commit(&volume);trim_process(&volume);trim_drain(&volume);trim_destroy(&volume);CHECK(!pool_live);
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const char* names[]={"deferred-unrelated-open-and-allocation","multiple-commits-one-inflight","pending-cache-view","reclaim-oom-no-resubmit","physical-intersection-excludes-reused-range","queue-memory-ceiling","partial-reclaim-never-serializes-reused-space","removed-device-not-replacement","removal-before-submission","regenerated-device-map-excludes-reservations","queued-shutdown-no-discard","inflight-shutdown-drains-outside-lock","initialization-oom","completion-poll-before-wakeup","clean-queues-without-wait","merge-failure-first-chunk","merge-failure-second-chunk","balance-excludes-pending","relocating-chunk-not-queued"};
    if(!strcmp(argv[1],"--list")) {for(unsigned i=0;i<19;i++)puts(names[i]);for(unsigned i=1;i<=40;i++)printf("preparation-oom-%u\n",i);for(unsigned i=1;i<=20;i++)printf("physical-oom-%u\n",i);for(unsigned m=0;m<3;m++)for(unsigned s=0;s<3;s++)printf("completion-%u-status-%u\n",m,s);return 0;}
    for(unsigned i=0;i<19;i++)if(!strcmp(argv[1],names[i])){switch(i){case 0:deferred();break;case 1:repeated();break;case 2:cache_view();break;case 3:reclaim_failure();break;case 4:physical();break;case 5:ceiling();break;case 6:partial_reclaim();break;case 7:removed_device();break;case 8:removal_before_submission();break;case 9:regenerated_map();break;case 10:queued_shutdown();break;case 11:inflight_shutdown();break;case 12:initialization_oom();break;case 13:completion_poll_race();break;case 14:clean_queues_without_wait();break;case 15:clean_merge_failure(1);break;case 16:clean_merge_failure(2);break;case 17:balance_excludes_pending();break;case 18:relocating_chunk_not_queued();break;}return 0;}
    unsigned a,b;if(sscanf(argv[1],"preparation-oom-%u",&a)==1){preparation_failure(a);return 0;}
    if(sscanf(argv[1],"physical-oom-%u",&a)==1){physical_oom(a);return 0;}
    if(sscanf(argv[1],"completion-%u-status-%u",&a,&b)==2){NTSTATUS status[]={STATUS_SUCCESS,STATUS_IO_DEVICE_ERROR,STATUS_CANCELLED};modes(a,status[b]);return 0;}return 2;
}
