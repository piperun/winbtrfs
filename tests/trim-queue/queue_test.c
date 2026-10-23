/* Deferred lower-device completions exercise production ownership boundaries. */
#include "queue_host.h"
#include "queue-types.inc"
#include "queue_services.inc"
#include "spaces.inc"
#include "reservations.inc"
#include "queue.inc"
#include "mapping.inc"
#include "clean.inc"

/* Boundaries unrelated to discard ownership retain their normal success path. */
#define WARN(...) ((void)0)
static void IoAcquireVpbSpinLock(KIRQL* irql){CHECK(!pending_count);*irql=0;}
static void IoReleaseVpbSpinLock(KIRQL irql){CHECK(!irql);}
static NTSTATUS FsRtlNotifyVolumeEvent(void* object,unsigned event){(void)object;(void)event;return STATUS_SUCCESS;}
static NTSTATUS do_write(device_extension* v,PIRP irp){(void)v;(void)irp;return STATUS_SUCCESS;}
static void free_trees(device_extension* v){(void)v;}
static void flush_fcb_caches(device_extension* v){(void)v;}
static void update_volumes(device_extension* v){(void)v;}
static void CcWaitForCurrentLazyWriterActivity(void){}
static void uninit(device_extension* v){(void)v;CHECK(!pending_count);}
static bool device_still_valid(device* d,uint64_t generation){(void)d;(void)generation;return false;}
#include "balance.inc"
#include "handoff.inc"

static device_extension volume;
static chunk chunks[3];
static device devices[3];
static DEVICE_OBJECT objects[3];
static FILE_OBJECT files[3];
static struct btrfs_chunk items[3];
static void check_handoff_ownership(void) {
    if(enforce_handoff_ownership&&pending_count){
        CHECK(active->vde&&active->vde->mounted_device==active->devobj);
        CHECK(active->Vpb->DeviceObject==active->devobj);
        CHECK(!(active->Vpb->Flags&VPB_DIRECT_WRITES_ALLOWED));
    }
}
static void range(LIST_ENTRY* head,uint64_t start,uint64_t length) {
    space* s=ExAllocatePoolWithTag(PagedPool,sizeof(*s),ALLOC_TAG);CHECK(s);
    s->address=start;s->size=length;InsertTailList(head,&s->list_entry);
}
static bool contains(LIST_ENTRY* head,uint64_t address) {
    for(LIST_ENTRY* le=head->Flink;le!=head;le=le->Flink){space* s=CONTAINING_RECORD(le,space,list_entry);if(address>=s->address&&address-s->address<s->size)return true;}
    return false;
}
static void balance_allocation_during_preparation(void) {
    inject_balance_allocation=false;
    balance_allocation_blocked=volume.chunk_lock.held;
    if (!balance_allocation_blocked) {
        ExAcquireResourceExclusiveLite(&volume.chunk_lock,true);
        CHECK(NT_SUCCESS(space_list_subtract2(&devices[0].space,NULL,0x104000,0x1000,NULL,NULL)));
        ExReleaseResourceLite(&volume.chunk_lock);
    }
}
static void physical_preparation_excludes_allocation(void);
static void setup(unsigned count) {
    InitializeListHead(&volume.devices);InitializeListHead(&volume.chunks);
    volume.trim=true;active=&volume;
    for(unsigned i=0;i<count;i++) {
        objects[i].StackSize=4;files[i].id=i;objects[i].id=i;
        devices[i].devobj=&objects[i];devices[i].fileobj=&files[i];devices[i].trim=true;
        devices[i].devitem.devid=i+1;devices[i].devitem.total_bytes=0x1000000;
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
static void physical_preparation_excludes_allocation(void) {
    setup(1);free_spaces(&chunks[0].deleting);
    range(&devices[0].space,0x100000,0x10000);range(&devices[0].trim_list,0x102000,0x1000);
    inject_balance_allocation=true;commit();
    CHECK(!inject_balance_allocation&&balance_allocation_blocked);
    trim_process(&volume);finish_all(false);trim_process(&volume);cleanup();
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
    CHECK(NT_SUCCESS(trim_subtract_device_pending(&volume,&devices[0],&devices[0].space)));CHECK(!contains(&devices[0].space,0x102000)&&contains(&devices[0].space,0x101000));
    finish_all(false);trim_process(&volume);cleanup();
}
static void queued_shutdown(void) {setup(1);commit();CHECK(!submitted);cleanup();CHECK(!submitted);}
static void regenerated_map_failure_preserves_reservations(void) {
    setup(1);free_spaces(&chunks[0].deleting);
    range(&devices[0].space,0x100000,0x10000);range(&devices[0].trim_list,0x102000,0x1000);
    commit();trim_process(&volume);fail_at=allocations+2;
    CHECK(regenerate_space_list(&volume,&devices[0])==STATUS_INSUFFICIENT_RESOURCES);
    fail_at=0;CHECK(!contains(&devices[0].space,0x102000));
    CHECK(contains(&devices[0].space,0x101000)&&contains(&devices[0].space,0x10ffff));
    finish_all(false);trim_process(&volume);cleanup();
}
static void physical_reclaim_after_shrink(bool crossing) {
    setup(1);free_spaces(&chunks[0].deleting);
    range(&devices[0].space,0x100000,0x10000);range(&devices[0].trim_list,0x102000,0x1000);
    commit();trim_process(&volume);
    uint64_t limit=crossing?0x102800:0x101800;
    devices[0].devitem.total_bytes=limit;
    CHECK(NT_SUCCESS(space_list_subtract2(&devices[0].space,NULL,limit,0x110000-limit,NULL,NULL)));
    finish_all(false);trim_process(&volume);
    CHECK(!contains(&devices[0].space,limit)&&!contains(&devices[0].space,0x102fff));
    if(crossing)CHECK(contains(&devices[0].space,0x1027ff));
    for(LIST_ENTRY* le=devices[0].space.Flink;le!=&devices[0].space;le=le->Flink){space* s=CONTAINING_RECORD(le,space,list_entry);CHECK(s->address+s->size<=limit);}
    cleanup();
}
static void reclaim_many_sorted_ranges(void) {
    const unsigned count=4000;
    setup(1);free_spaces(&chunks[0].space);InitializeListHead(&chunks[0].space_size);free_spaces(&chunks[0].deleting);
    for(unsigned i=0;i<count;i++){
        CHECK(NT_SUCCESS(space_list_add2(&chunks[0].space,&chunks[0].space_size,(uint64_t)i*8192,4096,NULL,NULL)));
        range(&chunks[0].deleting,(uint64_t)i*8192+4096,2048);
    }
    list_visits=0;
    CHECK(trim_reclaim_ranges(&chunks[0].space,&chunks[0].space_size,&chunks[0].deleting,UINT64_MAX));
    size_t visits=list_visits;
    printf("reclaim %u sorted ranges: %zu list visits\n",count,visits);
    CHECK(visits<32*count);
    CHECK(IsListEmpty(&chunks[0].deleting));cleanup();
}
static void reclaim_unsorted_clipped_ranges(bool oom) {
    setup(1);free_spaces(&chunks[0].space);InitializeListHead(&chunks[0].space_size);free_spaces(&chunks[0].deleting);
    CHECK(NT_SUCCESS(space_list_add2(&chunks[0].space,&chunks[0].space_size,0x10000,0x2000,NULL,NULL)));
    CHECK(NT_SUCCESS(space_list_add2(&chunks[0].space,&chunks[0].space_size,0x15000,0x1000,NULL,NULL)));
    CHECK(NT_SUCCESS(space_list_add2(&chunks[0].space,&chunks[0].space_size,0x19000,0x1000,NULL,NULL)));
    range(&chunks[0].deleting,0x17000,0x1000);range(&chunks[0].deleting,0x11000,0x5000);
    range(&chunks[0].deleting,0x18000,0x1000);range(&chunks[0].deleting,0x1c000,0x2000);range(&chunks[0].deleting,0x20000,0x1000);
    if(oom)fail_at=allocations+2;
    bool done=trim_reclaim_ranges(&chunks[0].space,&chunks[0].space_size,&chunks[0].deleting,0x1d000);fail_at=0;
    CHECK(done==!oom);
    if(oom){
        CHECK(contains(&chunks[0].space,0x17000)&&!contains(&chunks[0].deleting,0x17000));
        CHECK(contains(&chunks[0].deleting,0x1c000));
        CHECK(NT_SUCCESS(space_list_subtract2(&chunks[0].space,&chunks[0].space_size,0x17000,0x1000,NULL,NULL)));
        CHECK(trim_reclaim_ranges(&chunks[0].space,&chunks[0].space_size,&chunks[0].deleting,0x1d000));
        CHECK(!contains(&chunks[0].space,0x17000));
    }
    CHECK(IsListEmpty(&chunks[0].deleting)&&contains(&chunks[0].space,0x15fff));
    CHECK(contains(&chunks[0].space,0x19fff)&&contains(&chunks[0].space,0x1cfff)&&!contains(&chunks[0].space,0x1d000));
    uint64_t previous=UINT64_MAX;unsigned by_size=0,by_address=0;
    for(LIST_ENTRY* le=chunks[0].space_size.Flink;le!=&chunks[0].space_size;le=le->Flink){space* s=CONTAINING_RECORD(le,space,list_entry_size);CHECK(s->size<=previous);previous=s->size;by_size++;}
    for(LIST_ENTRY* le=chunks[0].space.Flink;le!=&chunks[0].space;le=le->Flink)by_address++;
    CHECK(by_size==by_address&&by_address==3);cleanup();
}
static void inflight_shutdown(void) {setup(1);commit();trim_process(&volume);cleanup();CHECK(waits==1);}
static void drain_poller_consumes_work_event(void) {setup(1);commit();trim_process(&volume);preempt_drain_wait=true;cleanup();CHECK(waits==1&&!preempt_drain_wait);}
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
static void volume_lock_quiesces_discard(void) {
    setup(1);commit();trim_process(&volume);range(&chunks[0].deleting,0x14000,0x1000);commit();
    CHECK(submitted==1&&pending_count==1);
    ExAcquireResourceExclusiveLite(&volume.tree_lock,true);trim_pause_locked(&volume);ExReleaseResourceLite(&volume.tree_lock);
    trim_wait_idle(&volume);CHECK(!pending_count&&waits==1);trim_process(&volume);trim_process(&volume);CHECK(submitted==1);
    range(&chunks[0].deleting,0x16000,0x1000);commit();CHECK(!IsListEmpty(&chunks[0].deleting));
    /* Handle cleanup resumes while holding the tree resource shared. */
    ExAcquireResourceSharedLite(&volume.tree_lock,true);trim_resume(&volume);ExReleaseResourceLite(&volume.tree_lock);
    trim_process(&volume);CHECK(submitted==1);cleanup();
}
static void initialization_oom(void) {
    InitializeListHead(&volume.chunks);InitializeListHead(&volume.devices);active=&volume;
    fail_at=1;trim_initialize(&volume);CHECK(!volume.trim_state);trim_commit(&volume);trim_process(&volume);trim_drain(&volume);trim_destroy(&volume);CHECK(!pool_live);
}
static void balance_quiesces_before_selection(bool allocation_failure) {
    setup(1);volume.balance.opts[0].flags=BTRFS_BALANCE_OPTS_ENABLED;
    commit();trim_process(&volume);range(&chunks[0].deleting,0x14000,0x1000);commit();
    if(allocation_failure)fail_at=allocations+1;
    NTSTATUS status=balance_start(&volume);fail_at=0;
    CHECK(!pending_count&&submitted==1&&waits==1);
    CHECK(volume.trim_state->pauses==1);
    if(allocation_failure){CHECK(status==STATUS_INSUFFICIENT_RESOURCES&&trim_chunk_pending(&chunks[0]));}
    else {CHECK(NT_SUCCESS(status)&&!trim_chunk_pending(&chunks[0]));CHECK(should_balance_chunk(&volume,0,&chunks[0]));CHECK(IsListEmpty(&volume.trim_state->batches));}
    trim_resume(&volume);cleanup();
}
static void removal_rejects_live_stripes(void) {
    setup(1);items[0].stripe[0].devid=devices[0].devitem.devid;
    CHECK(removal_preamble(&volume,&devices[0])==STATUS_DEVICE_NOT_READY);
    commit();trim_process(&volume);
    CHECK(removal_preamble(&volume,&devices[0])==STATUS_DEVICE_NOT_READY);
    finish_all(false);trim_process(&volume);
    CHECK(removal_preamble(&volume,&devices[0])==STATUS_DEVICE_NOT_READY);
    items[0].stripe[0].devid=devices[0].devitem.devid+1;
    CHECK(NT_SUCCESS(removal_preamble(&volume,&devices[0])));cleanup();
}
static void ownership_handoff_drains(unsigned path) {
    setup(1);VPB vpb={.Flags=VPB_MOUNTED,.DeviceObject=&objects[0]};
    DEVICE_OBJECT real={.Vpb=&vpb};vpb.RealDevice=&real;
    volume_device_extension vde={.mounted_device=&objects[0]};
    volume.Vpb=&vpb;volume.devobj=&objects[0];volume.vde=&vde;volume.open_files=1;
    InitializeListHead(&volume.list_entry);
    objects[0].Vpb=&vpb;objects[0].DeviceExtension=&volume;
    commit();trim_process(&volume);CHECK(pending_count==1);enforce_handoff_ownership=true;
    switch(path){
    case 0:CHECK(NT_SUCCESS(pnp_remove_device(&objects[0])));break;
    case 1:CHECK(NT_SUCCESS(pnp_surprise_removal(&objects[0],NULL)));break;
    case 2:CHECK(NT_SUCCESS(dismount_volume(&volume,false,NULL)));break;
    case 3:check_after_wakeup(&objects[0],&volume);break;
    case 4:uninit_preamble(&volume);break;
    case 5:CHECK(NT_SUCCESS(invalidate_handoff(&volume,&real,NULL)));break;
    }
    CHECK(!pending_count&&waits==1);enforce_handoff_ownership=false;
    if(real.Vpb!=&vpb)ExFreePool(real.Vpb);
    cleanup();
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"reclaim-unsorted-clipped-ranges")){reclaim_unsorted_clipped_ranges(false);return 0;}
    if(!strcmp(argv[1],"reclaim-unsorted-clipped-ranges-oom")){reclaim_unsorted_clipped_ranges(true);return 0;}
    if(!strcmp(argv[1],"reclaim-many-sorted-ranges")){reclaim_many_sorted_ranges();return 0;}
    if(!strcmp(argv[1],"drain-poller-consumes-work-event")){drain_poller_consumes_work_event();return 0;}
    if(!strcmp(argv[1],"physical-preparation-excludes-allocation")){physical_preparation_excludes_allocation();return 0;}
    if(!strcmp(argv[1],"regenerated-map-oom-preserves-reservations")){regenerated_map_failure_preserves_reservations();return 0;}
    if(!strcmp(argv[1],"physical-reclaim-crossing-shrink")){physical_reclaim_after_shrink(true);return 0;}
    if(!strcmp(argv[1],"physical-reclaim-above-shrink")){physical_reclaim_after_shrink(false);return 0;}
    if(!strcmp(argv[1],"balance-quiesces-before-selection")){balance_quiesces_before_selection(false);return 0;}
    if(!strcmp(argv[1],"balance-quiesce-reclaim-oom")){balance_quiesces_before_selection(true);return 0;}
    if(!strcmp(argv[1],"removal-rejects-live-stripes")){removal_rejects_live_stripes();return 0;}
    unsigned handoff_path;if(sscanf(argv[1],"ownership-handoff-%u",&handoff_path)==1){CHECK(handoff_path<6);ownership_handoff_drains(handoff_path);return 0;}
    const char* names[]={"deferred-unrelated-open-and-allocation","multiple-commits-one-inflight","pending-cache-view","reclaim-oom-no-resubmit","physical-intersection-excludes-reused-range","queue-memory-ceiling","partial-reclaim-never-serializes-reused-space","removed-device-not-replacement","removal-before-submission","regenerated-device-map-excludes-reservations","queued-shutdown-no-discard","inflight-shutdown-drains-outside-lock","initialization-oom","completion-poll-before-wakeup","clean-queues-without-wait","merge-failure-first-chunk","merge-failure-second-chunk","balance-excludes-pending","relocating-chunk-not-queued","volume-lock-quiesces-discard"};
    if(!strcmp(argv[1],"--list")) {puts("reclaim-unsorted-clipped-ranges");puts("reclaim-unsorted-clipped-ranges-oom");puts("reclaim-many-sorted-ranges");puts("drain-poller-consumes-work-event");for(unsigned i=0;i<6;i++)printf("ownership-handoff-%u\n",i);puts("balance-quiesces-before-selection");puts("balance-quiesce-reclaim-oom");puts("removal-rejects-live-stripes");puts("physical-reclaim-crossing-shrink");puts("physical-reclaim-above-shrink");puts("regenerated-map-oom-preserves-reservations");puts("physical-preparation-excludes-allocation");for(unsigned i=0;i<20;i++)puts(names[i]);for(unsigned i=1;i<=40;i++)printf("preparation-oom-%u\n",i);for(unsigned i=1;i<=20;i++)printf("physical-oom-%u\n",i);for(unsigned m=0;m<3;m++)for(unsigned s=0;s<3;s++)printf("completion-%u-status-%u\n",m,s);return 0;}
    for(unsigned i=0;i<20;i++)if(!strcmp(argv[1],names[i])){switch(i){case 0:deferred();break;case 1:repeated();break;case 2:cache_view();break;case 3:reclaim_failure();break;case 4:physical();break;case 5:ceiling();break;case 6:partial_reclaim();break;case 7:removed_device();break;case 8:removal_before_submission();break;case 9:regenerated_map();break;case 10:queued_shutdown();break;case 11:inflight_shutdown();break;case 12:initialization_oom();break;case 13:completion_poll_race();break;case 14:clean_queues_without_wait();break;case 15:clean_merge_failure(1);break;case 16:clean_merge_failure(2);break;case 17:balance_excludes_pending();break;case 18:relocating_chunk_not_queued();break;case 19:volume_lock_quiesces_discard();break;}return 0;}
    unsigned a,b;if(sscanf(argv[1],"preparation-oom-%u",&a)==1){preparation_failure(a);return 0;}
    if(sscanf(argv[1],"physical-oom-%u",&a)==1){physical_oom(a);return 0;}
    if(sscanf(argv[1],"completion-%u-status-%u",&a,&b)==2){NTSTATUS status[]={STATUS_SUCCESS,STATUS_IO_DEVICE_ERROR,STATUS_CANCELLED};modes(a,status[b]);return 0;}return 2;
}
