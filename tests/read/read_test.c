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
int main(void) {
    eof_reads();compressed_jobs();
    printf("%u/%u file read scenarios passed\n",cases-failures,cases);
    return failures!=0;
}
