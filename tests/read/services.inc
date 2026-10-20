/* Modeled cache, decompression and device services; query logic is production. */
static struct { void* pointer; } allocations[32];
static unsigned allocation_count, allocation_attempt, fail_allocation;
static NTSTATUS job_status;
static bool diskacc;
static void* ExAllocatePoolWithTag(POOL_TYPE pool, size_t n, unsigned tag) {
    (void)pool; (void)tag;
    if (++allocation_attempt == fail_allocation) return NULL;
    void* p=malloc(n); if (!p) abort();
    for(unsigned i=0;i<32;i++) if(!allocations[i].pointer) {
        allocations[i].pointer=p; allocation_count++; return p;
    }
    abort();
}
static void ExFreePool(void* p) {
    for(unsigned i=0;i<32;i++) if(allocations[i].pointer==p) {
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
    memset(out,0x22,length); return STATUS_SUCCESS;
}
static NTSTATUS zlib_decompress(void* in,unsigned inlen,void* out,unsigned outlen) { (void)in;(void)inlen;memset(out,0x7e,outlen);return job_status; }
static NTSTATUS zstd_decompress(void* in,unsigned inlen,void* out,unsigned outlen) { return zlib_decompress(in,inlen,out,outlen); }
static NTSTATUS lzo_decompress(void* in,unsigned inlen,void* out,unsigned outlen,unsigned off) { (void)off;return zlib_decompress(in,inlen,out,outlen); }
static NTSTATUS add_calc_job_decomp(device_extension* v,uint8_t compression,void* in,unsigned inlen,void* out,unsigned outlen,unsigned off,calc_job** job) {
    (void)v;(void)compression;(void)off;
    *job=ExAllocatePoolWithTag(NonPagedPool,sizeof(**job),ALLOC_TAG);
    if(!*job)return STATUS_INSUFFICIENT_RESOURCES;
    (*job)->Status=zlib_decompress(in,inlen,out,outlen); (*job)->event.completed=true;
    return STATUS_SUCCESS;
}
static void calc_thread_main(device_extension* v,calc_job* job) { (void)v;(void)job; }
static void KeWaitForSingleObject(KEVENT* e,int reason,int mode,bool alert,void* timeout) {
    (void)reason;(void)mode;(void)alert;(void)timeout; if(!e->completed)abort();
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
