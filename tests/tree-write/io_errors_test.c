/* Full production orchestration and cleanup; Windows I/O boundaries are fake. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef int32_t LONG;
#define STATUS_SUCCESS 0
#define STATUS_INSUFFICIENT_RESOURCES (-1)
#define STATUS_IO_DEVICE_ERROR (-2)
#define STATUS_DEVICE_NOT_READY (-3)
#define STATUS_UNEXPECTED_IO_ERROR (-4)
#define NT_SUCCESS(s) ((s) >= 0)
#define NonPagedPool 0
#define ALLOC_TAG 0
#define BTRFS_BLOCK_GROUP_RAID5 0x80
#define BTRFS_BLOCK_GROUP_RAID6 0x100
#define BTRFS_DEV_STAT_WRITE_ERRS 0
#define NotificationEvent 0
#define Executive 0
#define KernelMode 0
#define HighPagePriority 0
#define MDL_PAGES_LOCKED 1
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define RtlCopyMemory memcpy

typedef struct list { struct list *Flink, *Blink; } LIST_ENTRY;
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
static void InitializeListHead(LIST_ENTRY* h) { h->Flink=h->Blink=h; }
static void InsertTailList(LIST_ENTRY* h, LIST_ENTRY* p) {
    p->Flink=h; p->Blink=h->Blink; h->Blink->Flink=p; h->Blink=p;
}
static void RemoveEntryList(LIST_ENTRY* p) { p->Flink->Blink=p->Blink; p->Blink->Flink=p->Flink; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) { LIST_ENTRY* p=h->Flink; RemoveEntryList(p); return p; }
static bool IsListEmpty(LIST_ENTRY* h) { return h->Flink==h; }

typedef struct { unsigned MdlFlags; } *PMDL;
typedef struct { unsigned context; } KEVENT;
typedef struct { NTSTATUS Status; } IO_STATUS_BLOCK;
typedef struct { void* stripe; bool launched; } *PIRP;
typedef struct { void* devobj; unsigned id; } device;
typedef struct { uint64_t length, type; } chunk_item;
typedef struct {
    uint64_t offset;
    chunk_item* chunk_item;
    LIST_ENTRY partial_stripes;
    unsigned partial_stripes_lock;
} chunk;
typedef struct { int unused; } device_extension;
typedef struct { LIST_ENTRY list_entry; void* bmparr; unsigned id; } partial_stripe;
#include "io_types.inc"

static chunk_item item;
static chunk physical_chunk;
static device_extension volume;
static device devices[4];
static tree_write writes[4];
static unsigned char payload[4][16];
static unsigned prepared, submitted, waits, flushes, logged, locked;
static unsigned fail_prepare, fail_completion, second_completion, fail_flush;
static bool prepare_empty, ignore_failure, fail_context_allocation;
static NTSTATUS first_completion_status = STATUS_IO_DEVICE_ERROR;
static void* live[256];
static size_t live_count, releases, mdl_unlocks, mdl_frees, irp_frees;

static void* ExAllocatePoolWithTag(int pool, size_t n, int tag) {
    (void)pool; (void)tag;
    if (fail_context_allocation && n == sizeof(write_data_context)*4) return NULL;
    assert(n && live_count < 256);
    void* p=malloc(n); assert(p);
    memset(p,POOL_PATTERN ? 0xa5 : 0,n);
    live[live_count++]=p; return p;
}
static void ExFreePool(void* p) {
    size_t i;
    for(i=0;i<live_count && live[i]!=p;i++);
    assert(i<live_count && "invalid or duplicate pool release");
    live[i]=live[--live_count]; releases++; free(p);
}
static PMDL make_mdl(void) {
    PMDL mdl=ExAllocatePoolWithTag(0,sizeof(*mdl),0);
    mdl->MdlFlags=MDL_PAGES_LOCKED; return mdl;
}
static void MmUnlockPages(PMDL mdl) {
    assert(mdl->MdlFlags & MDL_PAGES_LOCKED); mdl->MdlFlags=0; mdl_unlocks++;
}
static void IoFreeMdl(PMDL mdl) { assert(!mdl->MdlFlags); mdl_frees++; ExFreePool(mdl); }
static void IoFreeIrp(PIRP irp) {
    assert(!irp->launched || waits==4); irp_frees++; ExFreePool(irp);
}
static chunk* get_chunk_from_address(device_extension* v, uint64_t address) {
    assert(v==&volume && address<item.length); return &physical_chunk;
}
static void KeInitializeEvent(KEVENT* event, int type, bool signaled) {
    assert(type==NotificationEvent && !signaled); event->context=prepared;
}
static void KeWaitForSingleObject(KEVENT* event, int reason, int mode, bool alertable, void* timeout) {
    assert(event->context==waits && reason==Executive && mode==KernelMode && !alertable && !timeout);
    assert(submitted==8); waits++;
}
static void IoCallDriver(void* devobj, PIRP irp) {
    write_data_stripe* stripe=irp->stripe;
    assert(devobj==stripe->device->devobj && !irp->launched);
    irp->launched=true; submitted++;
    stripe->status=WriteDataStatus_Success;
    stripe->iosb.Status=STATUS_SUCCESS;
    unsigned context=stripe->device->id;
    if(context==fail_completion || context==second_completion) {
        stripe->iosb.Status=context==fail_completion ? first_completion_status : STATUS_DEVICE_NOT_READY;
        stripe->status=WriteDataStatus_Error;
    }
}
static void log_device_error(device_extension* v, device* dev, int type) {
    assert(v==&volume && (dev->id==fail_completion || dev->id==second_completion));
    assert(type==BTRFS_DEV_STAT_WRITE_ERRS); logged++;
}
static void ExAcquireResourceExclusiveLite(unsigned* lock, bool wait) {
    assert(lock==&physical_chunk.partial_stripes_lock && wait && !locked); locked++;
}
static void ExReleaseResourceLite(unsigned* lock) {
    assert(lock==&physical_chunk.partial_stripes_lock && locked==1); locked--;
}
static NTSTATUS flush_partial_stripe(device_extension* v, chunk* c, partial_stripe* ps) {
    assert(v==&volume && c==&physical_chunk && locked==1 && waits==4);
    assert(ps->id==++flushes); return flushes==fail_flush ? STATUS_UNEXPECTED_IO_ERROR : STATUS_SUCCESS;
}
#include "free_write_data_stripes.inc"
static NTSTATUS write_data(device_extension* v, uint64_t address, void* data, uint32_t length,
                           write_data_context* context, void* a, void* b, bool c, int d, int priority) {
    write_data_context* wtc=context;
    void* stripes=NULL;
    NTSTATUS Status;
    assert(v==&volume && address==prepared*64 && data==payload[prepared] && length==16);
    assert(!a && !b && !c && !d && priority==HighPagePriority);
    assert(context->Event.context==prepared && IsListEmpty(&context->stripes));
    assert(!context->need_wait && !context->stripes_left);
    assert(!context->parity1 && !context->parity2 && !context->scratch);
    assert(!context->mdl && !context->parity1_mdl && !context->parity2_mdl);
    unsigned id=++prepared;
    if(prepare_empty && id==fail_prepare) return STATUS_DEVICE_NOT_READY;
    context->parity1=ExAllocatePoolWithTag(0,8,0);
    context->parity2=ExAllocatePoolWithTag(0,8,0);
    context->scratch=ExAllocatePoolWithTag(0,8,0);
    context->mdl=make_mdl(); context->parity1_mdl=make_mdl(); context->parity2_mdl=make_mdl();
    PMDL shared=make_mdl();
    for(unsigned i=0;i<3;i++) {
        write_data_stripe* stripe=ExAllocatePoolWithTag(0,sizeof(*stripe),0);
        stripe->context=context; stripe->buf=data; stripe->device=&devices[id-1];
        stripe->mdl=i<2 ? shared : make_mdl();
        stripe->Irp=ExAllocatePoolWithTag(0,sizeof(*stripe->Irp),0);
        stripe->Irp->stripe=stripe; stripe->Irp->launched=false;
        stripe->status=i==2 ? WriteDataStatus_Ignore : WriteDataStatus_Pending;
        stripe->iosb.Status=ignore_failure && i==2 ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
        InsertTailList(&context->stripes,&stripe->list_entry);
    }
    if(id==fail_prepare) {
        Status=STATUS_DEVICE_NOT_READY;
        goto end;
    }
    Status=STATUS_SUCCESS;
    /* Use the real callee's failure epilogue, including its cleanup ownership. */
#include "write_data_end.inc"
}
#include "do_tree_writes.inc"

static const char* cases[]={
    "success", "ignored_completion_error", "context_allocation_failure",
    "completion_first", "completion_middle", "completion_last", "completion_first_failure_wins",
    "raid5_success", "raid6_success", "raid5_completion_then_successful_flush",
    "raid6_completion_then_successful_flush", "completion_then_failed_flush",
    "partial_flush_failure", "prepare_first", "prepare_middle", "prepare_last",
    "prepare_first_empty", "prepare_middle_empty", "prepare_last_empty"
};

int main(int argc, char** argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"--list")) {
        for(size_t i=0;i<sizeof(cases)/sizeof(*cases);i++) puts(cases[i]);
        return 0;
    }
    const char* name=argv[1];
    bool known=false;
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);i++) known|=!strcmp(name,cases[i]);
    assert(known);
    NTSTATUS expected=STATUS_SUCCESS;
    fail_completion=second_completion=99;
    if(strstr(name,"completion") && !strstr(name,"ignored")) {
        fail_completion=strstr(name,"middle") ? 3 : strstr(name,"last") ? 4 : 1;
        expected=STATUS_IO_DEVICE_ERROR;
    }
    if(strstr(name,"first_failure_wins")) second_completion=4;
    ignore_failure=!strcmp(name,"ignored_completion_error");
    if(strstr(name,"prepare_")) {
        fail_prepare=strstr(name,"first") ? 1 : strstr(name,"middle") ? 2 : 4;
        prepare_empty=strstr(name,"empty")!=NULL; expected=STATUS_DEVICE_NOT_READY;
    }
    if(!strcmp(name,"context_allocation_failure")) {
        fail_context_allocation=true; expected=STATUS_INSUFFICIENT_RESOURCES;
    }
    bool raid=strstr(name,"raid") || strstr(name,"flush");
    item=(chunk_item){1024,raid ? (strstr(name,"raid6") ? BTRFS_BLOCK_GROUP_RAID6 : BTRFS_BLOCK_GROUP_RAID5) : 0};
    physical_chunk.offset=0; physical_chunk.chunk_item=&item;
    InitializeListHead(&physical_chunk.partial_stripes);
    unsigned partials=raid ? 3 : 0;
    for(unsigned i=1;i<=partials;i++) {
        partial_stripe* ps=ExAllocatePoolWithTag(0,sizeof(*ps),0);
        ps->id=i; ps->bmparr=i==2 ? NULL : ExAllocatePoolWithTag(0,8,0);
        InsertTailList(&physical_chunk.partial_stripes,&ps->list_entry);
    }
    if(!strcmp(name,"partial_flush_failure") || !strcmp(name,"completion_then_failed_flush")) fail_flush=2;
    if(!strcmp(name,"partial_flush_failure")) expected=STATUS_UNEXPECTED_IO_ERROR;
    LIST_ENTRY list; InitializeListHead(&list);
    for(unsigned i=0;i<4;i++) {
        devices[i]=(device){&devices[i],i+1};
        writes[i]=(tree_write){.address=i*64,.length=16,.data=payload[i],.allocated=false};
        InsertTailList(&list,&writes[i].list_entry);
    }
    NTSTATUS actual=do_tree_writes(&volume,&list,true);
    if(actual!=expected) fprintf(stderr,"expected status %d, got %d\n",expected,actual);
    assert(actual==expected);
    assert(!locked);
    if(fail_prepare || fail_context_allocation) {
        unsigned populated=fail_prepare ? fail_prepare-(prepare_empty ? 1 : 0) : 0;
        assert(prepared==fail_prepare && !submitted && !waits && !flushes && !logged);
        assert(mdl_unlocks==populated*5 && mdl_frees==populated*5 && irp_frees==populated*3);
    } else {
        assert(prepared==4 && submitted==8 && waits==4);
        assert(mdl_unlocks==20 && mdl_frees==20 && irp_frees==12);
        assert(logged==(fail_completion==99 ? 0u : second_completion==99 ? 1u : 2u));
        assert(flushes==(fail_flush ? 2u : partials));
    }
    /* Flush failure leaves later partials queued, as in the existing transaction. */
    unsigned remaining=0;
    while(!IsListEmpty(&physical_chunk.partial_stripes)) {
        partial_stripe* ps=CONTAINING_RECORD(RemoveHeadList(&physical_chunk.partial_stripes),partial_stripe,list_entry);
        assert(fail_flush && ps->id==3); remaining++;
        if(ps->bmparr) ExFreePool(ps->bmparr);
        ExFreePool(ps);
    }
    assert(remaining==(fail_flush ? 1u : 0u));
    assert(!live_count);
    printf("PASS %s: status=%d prepared=%u submitted=%u waits=%u flushes=%u releases=%zu\n",
           name,actual,prepared,submitted,waits,flushes,releases);
    return 0;
}
