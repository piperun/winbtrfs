/* Modeled cache, decompression and device services; read logic is production. */
static struct { void* pointer; size_t size; } allocations[32];
static unsigned allocation_count, allocation_attempt, fail_allocation;
static NTSTATUS job_status;
static bool diskacc;
enum { JOB_SYNCHRONOUS, JOB_QUEUED, JOB_RUNNING };
static unsigned job_execution, read_calls, fail_read;
static unsigned premature_input_frees, missing_job_inputs, blocked_waits, completed_jobs;
static struct { calc_job* job; uintptr_t input; unsigned length; } pending_jobs[16];

static bool owns_input(uintptr_t input, unsigned length) {
    for (unsigned i = 0; i < 32; i++) {
        uintptr_t begin = (uintptr_t)allocations[i].pointer;
        if (allocations[i].pointer && input >= begin && input - begin <= allocations[i].size &&
            length <= allocations[i].size - (input - begin))
            return true;
    }
    return false;
}

static void complete_job(calc_job* job) {
    for (unsigned i = 0; i < 16; i++) if (pending_jobs[i].job == job) {
        /* Validate ownership without reading the borrowed input, even on failure. */
        if (!owns_input(pending_jobs[i].input, pending_jobs[i].length))
            missing_job_inputs++;
        else
            memset(job->out, 0x7e, job->outlen);
        job->Status = job_status;
        job->event.completed = true;
        pending_jobs[i].job = NULL;
        completed_jobs++;
        return;
    }
    abort();
}

static void* ExAllocatePoolWithTag(POOL_TYPE pool, size_t n, unsigned tag) {
    (void)pool; (void)tag;
    if (++allocation_attempt == fail_allocation) return NULL;
    void* p=malloc(n); if (!p) abort();
    for(unsigned i=0;i<32;i++) if(!allocations[i].pointer) {
        allocations[i].pointer=p; allocations[i].size=n; allocation_count++; return p;
    }
    abort();
}
static void ExFreePool(void* p) {
    for(unsigned i=0;i<32;i++) if(allocations[i].pointer==p) {
        uintptr_t begin = (uintptr_t)p;
        for (unsigned j = 0; j < 16; j++) if (pending_jobs[j].job &&
            pending_jobs[j].input >= begin && pending_jobs[j].input - begin < allocations[i].size)
            premature_input_frees++;
        allocations[i].pointer=NULL; allocation_count--; free(p); return;
    }
    abort();
}
static void reclaim_allocations(void) {
    for(unsigned i=0;i<32;i++) if(allocations[i].pointer) ExFreePool(allocations[i].pointer);
}
static chunk* get_chunk_from_address(device_extension* v, uint64_t address) { static chunk c; (void)v; (void)address; return &c; }
static NTSTATUS read_data(device_extension* v,uint64_t address,uint32_t length,void* csum,bool tree,uint8_t* out,chunk* c,void* addr,PIRP irp,unsigned flags,bool mdl,int priority) {
    (void)v;(void)address;(void)csum;(void)tree;(void)c;(void)addr;(void)irp;(void)flags;(void)mdl;(void)priority;
    if (++read_calls == fail_read) return STATUS_DATA_ERROR;
    memset(out,0x22,length); return STATUS_SUCCESS;
}
static NTSTATUS zlib_decompress(void* in,unsigned inlen,void* out,unsigned outlen) { (void)in;(void)inlen;memset(out,0x7e,outlen);return job_status; }
static NTSTATUS zstd_decompress(void* in,unsigned inlen,void* out,unsigned outlen) { return zlib_decompress(in,inlen,out,outlen); }
static NTSTATUS lzo_decompress(void* in,unsigned inlen,void* out,unsigned outlen,unsigned off) { (void)off;return zlib_decompress(in,inlen,out,outlen); }
static NTSTATUS add_calc_job_decomp(device_extension* v,uint8_t compression,void* in,unsigned inlen,void* out,unsigned outlen,unsigned off,calc_job** job) {
    (void)v;(void)compression;(void)off;
    *job=ExAllocatePoolWithTag(NonPagedPool,sizeof(**job),ALLOC_TAG);
    if(!*job)return STATUS_INSUFFICIENT_RESOURCES;
    (*job)->event.completed=false; (*job)->out=out; (*job)->outlen=outlen;
    for (unsigned i = 0; i < 16; i++) if (!pending_jobs[i].job) {
        pending_jobs[i].job=*job; pending_jobs[i].input=(uintptr_t)in; pending_jobs[i].length=inlen;
        if (job_execution == JOB_SYNCHRONOUS) complete_job(*job);
        return STATUS_SUCCESS;
    }
    abort();
}
static void calc_thread_main(device_extension* v,calc_job* job) {
    (void)v;
    if (!job->event.completed && job_execution != JOB_RUNNING) complete_job(job);
}
static void KeWaitForSingleObject(KEVENT* e,int reason,int mode,bool alert,void* timeout) {
    (void)reason;(void)mode;(void)alert;(void)timeout;
    if (!e->completed) {
        if (job_execution != JOB_RUNNING) blocked_waits++;
        complete_job(CONTAINING_RECORD(e,calc_job,event));
    }
}
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return &irp->stack; }
static bool FsRtlCheckLockForReadAccess(int* lock,PIRP irp) { (void)lock;(void)irp;return true; }
static uint8_t* map_user_buffer(PIRP irp,int priority) { (void)priority;return irp->buffer; }
static void init_file_cache(PFILE_OBJECT file,CC_FILE_SIZES* sizes) { (void)sizes;file->PrivateCacheMap=(void*)1; }
static void CcMdlRead(PFILE_OBJECT file,LARGE_INTEGER* start,ULONG length,void** mdl,IO_STATUS_BLOCK* iosb) {
    (void)file;(void)start;(void)length;(void)mdl;(void)iosb;abort();
}
static bool CcCopyRead(PFILE_OBJECT file,LARGE_INTEGER* start,ULONG length,bool wait,void* data,IO_STATUS_BLOCK* iosb) {
    (void)file;(void)start;(void)wait;memset(data,0,length);iosb->Status=STATUS_SUCCESS;iosb->Information=length;return true;
}
static bool (*fCcCopyReadEx)(PFILE_OBJECT,LARGE_INTEGER*,ULONG,bool,void*,IO_STATUS_BLOCK*,PETHREAD);
static NTSTATUS GetExceptionCode(void) { return STATUS_INTERNAL_ERROR; }
static void IoMarkIrpPending(PIRP irp) { (void)irp; }
static bool IoIsSystemThread(PETHREAD thread) { (void)thread;return false; }
static PETHREAD PsGetCurrentThread(void) { return NULL; }
static PIRP IoGetTopLevelIrp(void) { return NULL; }
static void* PsGetThreadProcess(PETHREAD thread) { return thread; }
static void (*fPsUpdateDiskCounters)(void*,uint64_t,uint64_t,unsigned,unsigned,unsigned);
