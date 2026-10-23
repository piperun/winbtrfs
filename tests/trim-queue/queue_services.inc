static size_t allocations,fail_at,pool_live,irp_live,references,submitted,waits;
static bool inline_completion, preempt_on_signal, completion_lock_held;
static bool inject_balance_allocation, balance_allocation_blocked;
static bool enforce_handoff_ownership;
static bool preempt_drain_wait;
static ERESOURCE global_loading_lock;
static NTSTATUS completion_status=STATUS_SUCCESS;
static PIRP pending_irps[16];static unsigned pending_count;
static void* ExAllocatePoolWithTag(unsigned pool,size_t n,unsigned tag) {
    (void)pool;CHECK(tag==ALLOC_TAG);if(++allocations==fail_at)return NULL;
    void* p=malloc(n);CHECK(p);memset(p,0xa5,n);pool_live++;return p;
}
static void ExFreePool(void* p) {CHECK(p&&pool_live);for(unsigned i=0;i<pending_count;i++)CHECK(p!=pending_irps[i]->AssociatedIrp.SystemBuffer&&p!=pending_irps[i]->context);pool_live--;free(p);}
static NTSTATUS add_rollback_space(LIST_ENTRY* r,bool a,LIST_ENTRY* l,LIST_ENTRY* z,uint64_t s,uint64_t n,chunk* c){(void)r;(void)a;(void)l;(void)z;(void)s;(void)n;(void)c;CHECK(false);return STATUS_INSUFFICIENT_RESOURCES;}
static void ExAcquireResourceExclusiveLite(ERESOURCE* r,bool wait){check_handoff_ownership();CHECK(wait&&!r->held);r->held=1;}
static void ExAcquireResourceSharedLite(ERESOURCE* r,bool wait){ExAcquireResourceExclusiveLite(r,wait);}
static void ExReleaseResourceLite(ERESOURCE* r){check_handoff_ownership();CHECK(r->held);r->held=0;}
static void acquire_chunk_lock(chunk* c,device_extension* v){CHECK(v->chunk_lock.held&&!c->held);c->held=1;}
static void release_chunk_lock(chunk* c,device_extension* v){CHECK(v->chunk_lock.held&&c->held);c->held=0;}
static void ObReferenceObject(void* p){CHECK(p);((DEVICE_OBJECT*)p)->refs++;references++;}
static void ObDereferenceObject(void* p){CHECK(p&&((DEVICE_OBJECT*)p)->refs&&references);((DEVICE_OBJECT*)p)->refs--;references--;}
static LONG InterlockedIncrement(volatile LONG* p){return ++*p;}
static LONG InterlockedDecrement(volatile LONG* p){CHECK(*p>0);return --*p;}

static void KeInitializeEvent(KEVENT* e,int t,bool b){CHECK(t==NotificationEvent);e->signaled=b;}
static void KeSetEvent(KEVENT* e,int boost,bool wait){CHECK(!boost&&!wait);if(preempt_on_signal&&!completion_lock_held){preempt_on_signal=false;poll_during_signal();}e->signaled=true;}
static void KeClearEvent(KEVENT* e){e->signaled=false;}
static PIRP IoAllocateIrp(unsigned n,bool charge){CHECK(n&&!charge);if(inject_balance_allocation)balance_allocation_during_preparation();if(++allocations==fail_at)return NULL;PIRP p=calloc(1,sizeof(*p));CHECK(p);irp_live++;return p;}
static void IoFreeIrp(PIRP p){CHECK(p&&irp_live&&(!p->submitted||p->completed));irp_live--;free(p);}
static void IoSetCompletionRoutine(PIRP p,COMPLETION c,void* v,bool a,bool b,bool d){CHECK(a&&b&&d);p->completion=c;p->context=v;}
static void complete(PIRP p){CHECK(p->submitted&&!p->completed);p->completed=true;p->UserIosb->Status=completion_status;CHECK(p->completion(NULL,p,p->context)==STATUS_MORE_PROCESSING_REQUIRED);}
static NTSTATUS IoCallDriver(PDEVICE_OBJECT d,PIRP p){
 CHECK(!active->tree_lock.held&&!active->chunk_lock.held&&d->refs&&p->stack.FileObject->refs);for(LIST_ENTRY* l=active->chunks.Flink;l!=&active->chunks;l=l->Flink)CHECK(!CONTAINING_RECORD(l,chunk,list_entry)->held);
 CHECK(p->stack.MajorFunction==IRP_MJ_DEVICE_CONTROL&&p->stack.Parameters.DeviceIoControl.IoControlCode==IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES);
 DEVICE_MANAGE_DATA_SET_ATTRIBUTES* a=p->AssociatedIrp.SystemBuffer;CHECK(a->Size==28&&a->DataSetRangesOffset==32&&a->DataSetRangesLength&&!(a->DataSetRangesLength%16));
 CHECK(p->stack.Parameters.DeviceIoControl.InputBufferLength==32+a->DataSetRangesLength&&p->Flags==IRP_BUFFERED_IO&&!p->UserBuffer);
 p->submitted=true;submitted++;if(inline_completion){complete(p);return completion_status;}
 CHECK(pending_count<16);pending_irps[pending_count++]=p;return STATUS_PENDING;
}
static NTSTATUS KeWaitForSingleObject(KEVENT* e,int reason,int mode,bool alert,void* timeout){CHECK(!active->tree_lock.held&&!active->chunk_lock.held&&reason==Executive&&mode==KernelMode&&!alert&&!timeout);waits++;while(pending_count){PIRP p=pending_irps[--pending_count];complete(p);}if(preempt_drain_wait){preempt_drain_wait=false;trim_process(active);}CHECK(e->signaled);return STATUS_SUCCESS;}
static void free_spaces(LIST_ENTRY* list){while(!IsListEmpty(list))ExFreePool(CONTAINING_RECORD(RemoveHeadList(list),space,list_entry));}

static void KeInitializeSpinLock(KSPIN_LOCK* lock){lock->held=false;}
static void KeAcquireSpinLock(KSPIN_LOCK* lock,KIRQL* irql){CHECK(!lock->held&&!completion_lock_held);lock->held=true;completion_lock_held=true;*irql=0;}
static void KeReleaseSpinLock(KSPIN_LOCK* lock,KIRQL irql){CHECK(lock->held&&completion_lock_held&&!irql);lock->held=false;completion_lock_held=false;if(preempt_on_signal){preempt_on_signal=false;poll_during_signal();}}
