/* Exercise production read counts and completed-job allocation ownership. */
#include "host.h"
#include "read_types.inc"
#include "services.inc"
#include "read_functions.inc"
static unsigned cases, failures;
static void record(bool pass,const char* name,uint64_t start,unsigned length,ULONG actual,ULONG expected) {
    cases++; if(!pass) { failures++; printf("FAIL %s start=%llu length=%u actual=%u expected=%u allocations=%u\n",name,(unsigned long long)start,length,actual,expected,allocation_count); }
}
static void init(fcb* f,device_extension* v,ULONG size,ULONG valid) {
    memset(f,0,sizeof(*f));memset(v,0,sizeof(*v));
    v->superblock.sectorsize=4096;v->sector_shift=12;v->csum_size=4;
    f->Vcb=v;f->subvol=v;f->type=BTRFS_FT_REG_FILE;
    f->Header.FileSize.QuadPart=f->inode_item.size=size;
    f->Header.ValidDataLength.QuadPart=valid;
    InitializeListHead(&f->extents);
}
static void eof_reads(void) {
    const unsigned starts[]={0,20,49,50,60,80,99,100,101};
    const unsigned lengths[]={0,1,4,20,40,100};
    for(unsigned uncached=0;uncached<2;uncached++)for(unsigned a=0;a<9;a++)for(unsigned b=0;b<6;b++) {
        fcb f;device_extension v;init(&f,&v,100,50);
        FILE_OBJECT file={.FsContext=&f};unsigned char data[256];memset(data,0xa5,sizeof(data));
        IRP irp={.stack={.FileObject=&file,.Parameters.Read={.Length=lengths[b],.ByteOffset.QuadPart=starts[a]}},.Flags=uncached?IRP_NOCACHE:0,.buffer=data};
        ULONG got=123,expected=starts[a]<100?min(lengths[b],100-starts[a]):0;
        NTSTATUS status=do_read(&irp,true,&got);
        bool ok=status==(!lengths[b]||starts[a]<100?STATUS_SUCCESS:STATUS_END_OF_FILE)&&got==expected&&irp.IoStatus.Information==expected;
        for(unsigned n=0;n<sizeof(data);n++)ok=ok&&data[n]==(n<expected?0:0xa5);
        record(ok,"EOF zero fill",starts[a],lengths[b],got,expected);
    }
}
static void compressed_jobs(void) {
    const unsigned compressions[]={BTRFS_COMPRESS_ZLIB,BTRFS_COMPRESS_ZSTD};
    for(unsigned type=0;type<2;type++)for(unsigned failed_job=0;failed_job<2;failed_job++)for(unsigned fail=0;fail<=5;fail++) {
        fcb f;device_extension v;init(&f,&v,8192,8192);
        extent e={.offset=0,.extent_data={.type=BTRFS_FILE_EXTENT_REG,.compression=compressions[type],.num_bytes=8192,.ram_bytes=8192,.disk_bytenr=1048576,.disk_num_bytes=4096}};
        InsertTailList(&f.extents,&e.list_entry);
        unsigned char data[8192];memset(data,0xa5,sizeof(data));ULONG got=0;
        job_status=failed_job?STATUS_DATA_ERROR:STATUS_SUCCESS;allocation_attempt=0;fail_allocation=fail;
        NTSTATUS status=read_file(&f,data,0,4096,&got,NULL);
        bool ok=allocation_count==0;
        if(fail)ok=ok&&status==STATUS_INSUFFICIENT_RESOURCES;
        else ok=ok&&status==job_status&&got==4096;
        if(!fail&&!failed_job)for(unsigned i=0;i<4096;i++)ok=ok&&data[i]==0x7e;
        for(unsigned i=4096;i<sizeof(data);i++)ok=ok&&data[i]==0xa5;
        record(ok,"completed compressed job",type,fail,got,fail?0:4096);
        reclaim_allocations();
    }
    fail_allocation=0;
}
static void pending_inputs(void) {
    const unsigned compressions[]={BTRFS_COMPRESS_ZLIB,BTRFS_COMPRESS_ZSTD};
    for(unsigned type=0;type<2;type++)for(unsigned execution=JOB_QUEUED;execution<=JOB_RUNNING;execution++)
    for(unsigned failed_job=0;failed_job<2;failed_job++)for(unsigned fail=0;fail<5;fail++) {
        fcb f;device_extension v;init(&f,&v,8192,8192);
        extent extents[2]={
            {.offset=0,.extent_data={.type=BTRFS_FILE_EXTENT_REG,.compression=compressions[type],.num_bytes=4096,.ram_bytes=4096,.disk_bytenr=1048576,.disk_num_bytes=4096}},
            {.offset=4096,.extent_data={.type=BTRFS_FILE_EXTENT_REG,.compression=compressions[type],.num_bytes=4096,.ram_bytes=4096,.disk_bytenr=2097152,.disk_num_bytes=4096}}
        };
        for(unsigned i=0;i<2;i++)InsertTailList(&f.extents,&extents[i].list_entry);
        unsigned char data[12288];memset(data,0xa5,sizeof(data));ULONG got=0;
        job_execution=execution;job_status=failed_job?STATUS_INTERNAL_ERROR:STATUS_SUCCESS;
        allocation_attempt=read_calls=0;fail_read=fail==1?2:0;
        /* Two read-part/buffer pairs and one complete job use seven allocations. */
        fail_allocation=fail>=2?fail+6:0;
        premature_input_frees=missing_job_inputs=blocked_waits=completed_jobs=0;
        NTSTATUS status=read_file(&f,data,0,8192,&got,NULL);
        NTSTATUS expected_status=fail==1?STATUS_DATA_ERROR:fail>=2?STATUS_INSUFFICIENT_RESOURCES:job_status;
        bool ok=status==expected_status&&got==(fail?0:8192)&&allocation_count==0&&
            premature_input_frees==0&&missing_job_inputs==0&&blocked_waits==0&&completed_jobs==(fail?1:2);
        if(!fail&&!failed_job)for(unsigned i=0;i<8192;i++)ok=ok&&data[i]==0x7e;
        if(fail)for(unsigned i=0;i<8192;i++)ok=ok&&data[i]==0xa5;
        for(unsigned i=8192;i<sizeof(data);i++)ok=ok&&data[i]==0xa5;
        record(ok,execution==JOB_QUEUED?"queued input lifetime":"running input lifetime",type,fail,got,fail?0:8192);
        if(!ok)printf("  premature frees=%u missing inputs=%u blocked waits=%u completed jobs=%u status=%08x expected=%08x\n",
            premature_input_frees,missing_job_inputs,blocked_waits,completed_jobs,(unsigned)status,(unsigned)expected_status);
        reclaim_allocations();
    }
    job_execution=JOB_SYNCHRONOUS;fail_allocation=fail_read=0;
}
int main(void) {
    eof_reads();compressed_jobs();pending_inputs();
    printf("%u/%u file read scenarios passed\n",cases-failures,cases);
    return failures!=0;
}
