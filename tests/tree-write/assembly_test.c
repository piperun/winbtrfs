/* These tests include actual production regions; no assembly algorithm model. */
static device_extension volume;
static unsigned char* borrowed[1024];
static size_t borrowed_count;

static tree_write* make_write(LIST_ENTRY* list, uint64_t address, uint32_t length, bool own, bool allocated) {
    tree_write* tw=ExAllocatePoolWithTag(PagedPool,sizeof(*tw),0);
    tw->address=address; tw->length=length; tw->allocated=allocated; tw->c=NULL;
    if(own) tw->data=ExAllocatePoolWithTag(NonPagedPool,length,0);
    else { assert(borrowed_count<1024); tw->data=malloc(length); assert(tw->data); borrowed[borrowed_count++]=tw->data; }
    for(uint32_t j=0;j<length;j++) tw->data[j]=byte_for(address+j);
    InsertTailList(list,&tw->list_entry); return tw;
}
static size_t check_links(LIST_ENTRY* list) {
    size_t n=0; LIST_ENTRY* previous=list;
    for(LIST_ENTRY* le=list->Flink;le!=list;le=le->Flink) { assert(le->Blink==previous); previous=le; assert(++n<2048); }
    assert(list->Blink==previous && previous->Flink==list);
    size_t reverse=0;
    for(LIST_ENTRY* le=list->Blink;le!=list;le=le->Blink) { assert(le->Blink->Flink==le); reverse++; }
    assert(n==reverse); return n;
}
static void clean(LIST_ENTRY* list, bool no_free, bool payload) {
    check_links(list);
    while(!IsListEmpty(list)) {
        tree_write* tw=CONTAINING_RECORD(list->Flink,tree_write,list_entry);
        if(payload) for(uint32_t j=0;j<tw->length;j++) assert(tw->data[j]==byte_for(tw->address+j));
        RemoveEntryList(&tw->list_entry);
        if(!no_free||tw->allocated) ExFreePool(tw->data);
        ExFreePool(tw);
    }
    for(size_t i=0;i<borrowed_count;i++) free(borrowed[i]);
    borrowed_count=0; assert(live_allocations==0);
}
static bool coalescing_cost(bool no_free, bool baseline) {
    LIST_ENTRY list; InitializeListHead(&list); set_chunks(0,64*1024*1024,0);
    for(size_t i=0;i<512;i++) make_write(&list,i*16384,16384,!no_free,false);
    reset_counts(); assert(do_tree_writes(&volume,&list,no_free)==STATUS_SUCCESS);
    assert(check_links(&list)==1);
    tree_write* head=CONTAINING_RECORD(list.Flink,tree_write,list_entry);
    assert(head->length==8388608 && head->allocated && head->c==&chunks[0]);
    bool linear=allocations==1 && copied_bytes==8388608;
    if(baseline) assert(allocations==511 && copied_bytes==2151661568ULL);
    printf("{\"kind\":\"coalescing\",\"borrowed\":%s,\"input_bytes\":8388608,\"allocations\":%llu,\"copied_bytes\":%llu,\"linear\":%s},\n",
           no_free?"true":"false",(unsigned long long)allocations,(unsigned long long)copied_bytes,linear?"true":"false");
    clean(&list,no_free,true); return linear||baseline;
}
static void coalescing_controls(void) {
    LIST_ENTRY list; InitializeListHead(&list); set_chunks(0,4096,0); reset_counts();
    assert(do_tree_writes(&volume,&list,true)==STATUS_SUCCESS && IsListEmpty(&list) && !allocations);
    tree_write* single=make_write(&list,16,16,false,false); unsigned char* original=single->data;
    reset_counts(); assert(do_tree_writes(&volume,&list,true)==STATUS_SUCCESS);
    assert(!allocations && single->data==original && !single->allocated); clean(&list,true,true);
    /* Mixed ownership and an earlier assembled run must survive later OOM. */
    for(unsigned failed=0;failed<=2;failed++) {
        InitializeListHead(&list); set_chunks(0,4096,BTRFS_BLOCK_GROUP_RAID5);
        make_write(&list,0,16,false,false); make_write(&list,16,16,true,true);
        make_write(&list,64,16,true,true); make_write(&list,80,16,false,false);
        reset_counts(); fail_at=failed;
        NTSTATUS status=do_tree_writes(&volume,&list,true);
        assert(status==(failed?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS));
        assert(check_links(&list)==(failed==1?4:failed==2?3:2));
        if(!failed) assert(test_raid56);
        fail_at=0; clean(&list,true,true);
    }
    /* Adjacent chunk boundaries, gaps, overlapping and equal starts cannot merge. */
    InitializeListHead(&list); set_chunks(0,64,0);
    items[1]=(chunk_item){128,BTRFS_BLOCK_GROUP_RAID6}; chunks[1]=(chunk){64,&items[1]}; chunk_count=2;
    make_write(&list,48,16,true,false); make_write(&list,64,16,true,false);
    make_write(&list,64,16,true,false); make_write(&list,72,16,true,false); make_write(&list,104,16,true,false);
    reset_counts(); assert(do_tree_writes(&volume,&list,false)==STATUS_SUCCESS);
    assert(check_links(&list)==5 && !allocations && test_raid56);
    assert(CONTAINING_RECORD(list.Flink,tree_write,list_entry)->c==&chunks[0]);
    assert(CONTAINING_RECORD(list.Blink,tree_write,list_entry)->c==&chunks[1]); clean(&list,false,true);
}
static void boundary_controls(void) {
    LIST_ENTRY list; InitializeListHead(&list); set_chunks(0,UINT64_MAX,0);
    tree_write* a=make_write(&list,0,1,true,false); a->length=UINT32_MAX-1;
    tree_write* b=make_write(&list,UINT32_MAX-1,1,true,false);
    reset_counts(); fail_at=1;
    assert(do_tree_writes(&volume,&list,false)==STATUS_INSUFFICIENT_RESOURCES);
    assert(last_request==UINT32_MAX && check_links(&list)==2 && a->length==UINT32_MAX-1 && b->length==1);
    fail_at=0; clean(&list,false,false);
    InitializeListHead(&list); a=make_write(&list,0,1,true,false); a->length=UINT32_MAX;
    make_write(&list,UINT32_MAX,1,true,false);
    reset_counts(); assert(do_tree_writes(&volume,&list,false)==STATUS_SUCCESS);
    assert(!allocations && check_links(&list)==2); clean(&list,false,false);
    InitializeListHead(&list); set_chunks(UINT64_MAX-63,64,0);
    make_write(&list,UINT64_MAX-31,16,true,false); make_write(&list,UINT64_MAX-15,16,true,false);
    reset_counts(); assert(do_tree_writes(&volume,&list,false)==STATUS_SUCCESS);
    assert(allocations==1 && check_links(&list)==1); clean(&list,false,true);
    InitializeListHead(&list); set_chunks(0,64,0);
    make_write(&list,32,16,true,false); make_write(&list,48,32,true,false);
    reset_counts(); assert(do_tree_writes(&volume,&list,false)==STATUS_SUCCESS);
    assert(!allocations && check_links(&list)==2); clean(&list,false,true);
}
static bool ordering_case(size_t n, unsigned pattern, bool baseline) {
    LIST_ENTRY list; InitializeListHead(&list); reset_counts();
    for(size_t i=0;i<n;i++) {
        tree_write* tw=ExAllocatePoolWithTag(PagedPool,sizeof(*tw),0);
        uint64_t key=pattern==0?i:pattern==1?n-i:pattern==2?(i*317)%n:(i*317)%17;
        tw->address=key; tw->length=(uint32_t)i; tw->data=NULL;
        insert_actual(&list,tw);
    }
    uint64_t before_allocations=allocations; finish_order(&list);
    assert(allocations==before_allocations && check_links(&list)==n);
    uint64_t previous=0; uint32_t last_id=0; bool first=true;
    while(!IsListEmpty(&list)) {
        tree_write* tw=CONTAINING_RECORD(list.Flink,tree_write,list_entry);
        if(!first) { assert(tw->address>=previous); if(tw->address==previous) assert(tw->length>last_id); }
        previous=tw->address; last_id=tw->length; first=false;
        RemoveEntryList(&tw->list_entry); ExFreePool(tw);
    }
    assert(live_allocations==0);
    bool bounded=address_comparisons<=n*10;
    if(n==512) printf("{\"kind\":\"ordering\",\"pattern\":%u,\"items\":%zu,\"comparisons\":%llu,\"bounded\":%s},\n",pattern,n,(unsigned long long)address_comparisons,bounded?"true":"false");
    return bounded||baseline;
}
int main(int argc, char** argv) {
    assert(argc==3); bool baseline=!strcmp(argv[2],"baseline"), okay=true;
    bool coalescing=strcmp(argv[1],"ordering"), ordering=strcmp(argv[1],"coalescing");
    puts("{\"cases\":[");
    if(coalescing) {
        okay&=coalescing_cost(false,baseline); okay&=coalescing_cost(true,baseline);
        coalescing_controls(); if(!baseline) boundary_controls();
    }
    if(ordering) for(unsigned p=0;p<4;p++) {
        const size_t sizes[]={0,1,2,3,7,31,512};
        for(size_t i=0;i<sizeof(sizes)/sizeof(*sizes);i++) okay&=ordering_case(sizes[i],p,baseline);
    }
    printf("{\"controls_passed\":true}],\"work_bounds_passed\":%s,\"leaks\":%llu}\n",okay?"true":"false",(unsigned long long)live_allocations);
    return okay?0:1;
}
