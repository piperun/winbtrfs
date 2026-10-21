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
/* Uncompressed inline extents whose ram_bytes may claim more than the item stores, as a damaged or
 * crafted image can. The bytes after the stored data hold 0xc3, so copying past the item shows up in
 * the output. Each item stores the ten bytes '0' to '9'. */
static const uint8_t inline_stored[10]={'0','1','2','3','4','5','6','7','8','9'};
static extent* make_inline(uint64_t offset,uint64_t ram_bytes) {
    const size_t inline_start=offsetof(struct btrfs_file_extent_item,disk_bytenr),marker=9000;
    extent* e=malloc(offsetof(extent,extent_data)+sizeof(struct btrfs_file_extent_item)+marker);if(!e)abort();
    memset(e,0,offsetof(extent,extent_data));
    memset(&e->extent_data,0xc3,sizeof(struct btrfs_file_extent_item)+marker);
    e->offset=offset;
    e->extent_data.generation=1;e->extent_data.ram_bytes=ram_bytes;
    e->extent_data.compression=BTRFS_COMPRESS_NONE;e->extent_data.encryption=0;e->extent_data.other_encoding=0;
    e->extent_data.type=BTRFS_FILE_EXTENT_INLINE;
    memcpy((uint8_t*)&e->extent_data+inline_start,inline_stored,sizeof(inline_stored));
    e->datalen=(unsigned)(inline_start+sizeof(inline_stored));
    return e;
}
static void inline_reads(void) {
    static const struct { const char* name; unsigned ram, size, start, length, got; } reads[]={
        {"inline extent read",10,10,0,10,10},
        {"inline extent read",10,10,3,4,4},
        {"inline extent read",10,10,9,1,1},
        {"inline extent shorter than ram_bytes, file ends at stored data",30,10,0,10,10},
        {"inline extent shorter than ram_bytes",30,30,0,30,30},
        {"inline extent shorter than ram_bytes",30,30,5,20,20},
        {"inline extent shorter than ram_bytes, read past stored data",30,30,15,5,5},
        {"inline extent shorter than ram_bytes",40,40,9,31,31},
        {"inline extent shorter than ram_bytes, read past stored data",40,40,10,1,1},
        {"inline extent shorter than ram_bytes, read past stored data",100,100,50,20,20},
    };
    for(unsigned i=0;i<sizeof(reads)/sizeof(reads[0]);i++) {
        fcb f;device_extension v;init(&f,&v,reads[i].size,reads[i].size);
        extent* e=make_inline(0,reads[i].ram);InsertTailList(&f.extents,&e->list_entry);
        uint8_t data[64];memset(data,0xa5,sizeof(data));ULONG got=123;
        NTSTATUS status=read_file(&f,data,reads[i].start,reads[i].length,&got,NULL);
        bool ok=status==STATUS_SUCCESS&&got==reads[i].got&&allocation_count==0;
        /* Stored bytes are copied; the rest of ram_bytes and of the file reads as zeros. */
        for(unsigned n=0;n<sizeof(data);n++) {
            unsigned pos=reads[i].start+n;
            ok=ok&&data[n]==(n>=reads[i].got?0xa5:pos<sizeof(inline_stored)?inline_stored[pos]:0);
        }
        record(ok,reads[i].name,reads[i].start,reads[i].length,got,reads[i].got);
        if(!ok)printf("  ram_bytes=%u size=%u status=%08x first bytes %02x %02x %02x\n",reads[i].ram,reads[i].size,
            (unsigned)status,data[0],data[reads[i].length>10?10:0],data[reads[i].length?reads[i].length-1:0]);
        reclaim_allocations();free(e);
    }
    /* The next extent starts where ram_bytes ends, not where the stored data ends. */
    {
        fcb f;device_extension v;init(&f,&v,40,40);
        extent* e=make_inline(0,30);
        extent reg={.offset=30,.extent_data={.type=BTRFS_FILE_EXTENT_REG,.compression=BTRFS_COMPRESS_NONE,.num_bytes=10,.ram_bytes=10,
            .disk_bytenr=1048576,.disk_num_bytes=4096}};
        InsertTailList(&f.extents,&e->list_entry);InsertTailList(&f.extents,&reg.list_entry);
        uint8_t data[64];memset(data,0xa5,sizeof(data));ULONG got=0;read_calls=0;last_read_address=0;
        NTSTATUS status=read_file(&f,data,0,40,&got,NULL);
        bool ok=status==STATUS_SUCCESS&&got==40&&allocation_count==0&&read_calls==1&&last_read_address==1048576;
        for(unsigned n=0;n<sizeof(data);n++)
            ok=ok&&data[n]==(n<10?inline_stored[n]:n<30?0:n<40?0x22:0xa5);
        record(ok,"regular extent after a short inline extent",0,40,got,40);
        if(!ok)printf("  status=%08x reads=%u address=%llx bytes 10 and 30: %02x %02x\n",(unsigned)status,read_calls,
            (unsigned long long)last_read_address,data[10],data[30]);
        reclaim_allocations();free(e);
    }
    /* An earlier extent that runs past the inline extent's end leaves nothing of it to read. */
    {
        fcb f;device_extension v;init(&f,&v,8292,8292);
        extent reg={.offset=0,.extent_data={.type=BTRFS_FILE_EXTENT_REG,.compression=BTRFS_COMPRESS_NONE,.num_bytes=8192,.ram_bytes=8192,
            .disk_bytenr=1048576,.disk_num_bytes=8192}};
        extent* e=make_inline(100,50);
        InsertTailList(&f.extents,&reg.list_entry);InsertTailList(&f.extents,&e->list_entry);
        static uint8_t data[8448];memset(data,0xa5,sizeof(data));ULONG got=123;read_calls=0;
        NTSTATUS status=read_file(&f,data,0,8292,&got,NULL);
        bool ok=status==STATUS_INTERNAL_ERROR&&got==0&&allocation_count==0&&read_calls==0;
        for(unsigned n=0;n<sizeof(data);n++)ok=ok&&data[n]==0xa5;
        record(ok,"inline extent overlapped by an earlier extent",0,8292,got,0);
        if(!ok)printf("  status=%08x reads=%u byte 8192: %02x\n",(unsigned)status,read_calls,data[8192]);
        reclaim_allocations();free(e);
    }
}
int main(void) {
    eof_reads();compressed_jobs();pending_inputs();inline_reads();
    printf("%u/%u file read scenarios passed\n",cases-failures,cases);
    return failures!=0;
}
