/* Regression fixtures exercise extracted production serializers and scan state. */
#include "host.h"
#include "services.inc"
#include "queries.inc"
static unsigned failures,checks;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#c);failures++;}}while(0)
typedef struct {
    root subvol;
    device_extension volume;
    fcb parent,nodes[6];
    file_ref parent_ref,refs[6];
    fcb_nonpaged nonpaged;
    dir_child children[6];
    WCHAR names[6][64],uppercase_names[6][64];
    ccb handle;
    FILE_OBJECT file;
    unsigned count;
} fixture;
static UNICODE_STRING string(WCHAR* text){unsigned n=0;while(text[n])n++;return (UNICODE_STRING){.Buffer=text,.Length=n*2,.MaximumLength=n*2};}
static void init(fixture* f,const char* const* names,unsigned count){
    memset(f,0,sizeof(*f));f->count=count;f->subvol.id=5;
    init_list(&f->volume.roots);f->volume.superblock.sectorsize=4096;
    f->parent=(fcb){.inode=256,.subvol=&f->subvol,.Vcb=&f->volume,.nonpaged=&f->nonpaged};
    f->nodes[5]=(fcb){.inode=9999,.subvol=&f->subvol,.Vcb=&f->volume};
    f->volume.dummy_fcb=&f->nodes[5];
    init_list(&f->parent.dir_children_index);init_list(&f->parent.dir_children_hash);init_list(&f->parent.dir_children_hash_uc);
    f->parent_ref.fcb=&f->parent;f->handle.fileref=&f->parent_ref;f->handle.access=FILE_LIST_DIRECTORY;
    f->file=(FILE_OBJECT){.FsContext=&f->parent,.FsContext2=&f->handle};
    for(unsigned n=0;n<count;n++){
        for(unsigned i=0;names[n][i];i++){f->names[n][i]=(unsigned char)names[n][i];f->uppercase_names[n][i]=uppercase(f->names[n][i]);}
        f->nodes[n]=(fcb){.inode=1001+n,.subvol=&f->subvol,.Vcb=&f->volume,.atts=FILE_ATTRIBUTE_NORMAL,.inode_item={.size=1,.nbytes=4096}};
        f->refs[n].fcb=&f->nodes[n];
        f->children[n]=(dir_child){.fileref=&f->refs[n],.index=2+n,.key={.objectid=1001+n,.type=BTRFS_INODE_ITEM_KEY},.name=string(f->names[n]),.name_uc=string(f->uppercase_names[n]),.type=1};
        append(&f->parent.dir_children_index,&f->children[n].list_entry_index);
        dir_child* child=&f->children[n];
        child->hash=calc_crc32c(0xffffffff,(const uint8_t*)child->name.Buffer,child->name.Length);
        child->hash_uc=calc_crc32c(0xffffffff,(const uint8_t*)child->name_uc.Buffer,child->name_uc.Length);
        append(&f->parent.dir_children_hash,&child->list_entry_hash);
        append(&f->parent.dir_children_hash_uc,&child->list_entry_hash_uc);
        f->parent.hash_ptrs[child->hash>>24]=&child->list_entry_hash;
        f->parent.hash_ptrs_uc[child->hash_uc>>24]=&child->list_entry_hash_uc;
    }
}
static void finish(fixture* f){if(f->handle.query_string.Buffer)RtlFreeUnicodeString(&f->handle.query_string);}
typedef union {uint64_t align;unsigned char bytes[2048];} output;
static NTSTATUS query(fixture* f,output* o,ULONG capacity,enum _FILE_INFORMATION_CLASS cls,ULONG flags,WCHAR* mask,uintptr_t* bytes){
    memset(o,0x55,sizeof(*o));
    UNICODE_STRING expression=mask?string(mask):(UNICODE_STRING){0};
    IRP irp={.stack={.Parameters.QueryDirectory={.FileInformationClass=cls,.Length=capacity,.FileName=mask?&expression:NULL},.FileObject=&f->file,.Flags=flags},.RequestorMode=UserMode,.buffer=o->bytes};
    NTSTATUS status=query_directory(&irp);*bytes=irp.IoStatus.Information;return status;
}
static ULONG read32(const void* p){ULONG n;memcpy(&n,p,sizeof(n));return n;}
static void expect_name(output* o,ULONG offset,const WCHAR* name){
    unsigned n=0;while(name[n])n++;
    CHECK(read32(o->bytes+offset+8)==n*2);
    CHECK(!memcmp(o->bytes+offset+12,name,n*2));
}
static void ids(void){
    const char* names[]={"a","b"};
    for(unsigned both=0;both<2;both++){
        fixture f;output o;uintptr_t bytes;init(&f,names,2);
        CHECK(query(&f,&o,sizeof(o),both?FileIdExtdBothDirectoryInformation:FileIdExtdDirectoryInformation,0,NULL,&bytes)==STATUS_SUCCESS);
        unsigned offset=0;
        for(unsigned n=0;n<2;n++){
            FILE_ID_128 id;uint64_t identity[2];memcpy(&id,o.bytes+offset+72,sizeof(id));memcpy(identity,id.Identifier,sizeof(identity));
            CHECK(identity[0]==f.nodes[n].inode);CHECK(identity[1]==f.subvol.id);
            if(n==0){ULONG next=read32(o.bytes+offset);CHECK(next>0);offset+=next;}
        }
        finish(&f);
    }
}
static void id_special_entries(void){
    const char* names[]={"child"};
    for(unsigned both=0;both<2;both++)for(unsigned kind=0;kind<3;kind++){
        fixture f;output o;uintptr_t bytes;init(&f,names,1);
        root other={.id=55,.parent=f.subvol.id};
        fcb grandparent={.inode=777,.subvol=&other,.Vcb=&f.volume};file_ref grandparent_ref={.fcb=&grandparent};
        uint64_t want_inode,want_subvol;
        if(kind==0){
            f.parent_ref.parent=&grandparent_ref;
            want_inode=f.parent.inode;want_subvol=f.subvol.id;
        }else{
            f.children[0].key.type=BTRFS_ROOT_ITEM_KEY;f.children[0].key.objectid=other.id;
            if(kind==1){append(&f.volume.roots,&other.list_entry);f.nodes[0].inode=BTRFS_FIRST_FREE_OBJECTID;f.nodes[0].subvol=&other;want_inode=BTRFS_FIRST_FREE_OBJECTID;want_subvol=other.id;}
            else{want_inode=f.volume.dummy_fcb->inode;want_subvol=f.volume.dummy_fcb->subvol->id;}
        }
        enum _FILE_INFORMATION_CLASS cls=both?FileIdExtdBothDirectoryInformation:FileIdExtdDirectoryInformation;
        CHECK(query(&f,&o,sizeof(o),cls,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);
        uint64_t got[2];memcpy(got,o.bytes+72,sizeof(got));CHECK(got[0]==want_inode);CHECK(got[1]==want_subvol);
        if(kind==0){
            CHECK(query(&f,&o,sizeof(o),cls,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);
            memcpy(got,o.bytes+72,sizeof(got));CHECK(got[0]==grandparent.inode);CHECK(got[1]==other.id);
        }
        finish(&f);
    }
}
static void alignment(void){
    const char* names[]={"a","bb","ccc"};
    enum _FILE_INFORMATION_CLASS classes[]={FileNamesInformation,FileDirectoryInformation,FileFullDirectoryInformation,FileBothDirectoryInformation,FileIdBothDirectoryInformation,FileIdFullDirectoryInformation,FileIdExtdDirectoryInformation,FileIdExtdBothDirectoryInformation};
    const unsigned headers[]={12,64,68,94,104,80,88,114};
    for(unsigned cls=0;cls<8;cls++)for(unsigned residue=0;residue<(cls?8:4);residue++){
        fixture f;output o;uintptr_t bytes;init(&f,names,3);
        CHECK(query(&f,&o,512+residue,classes[cls],0,NULL,&bytes)==STATUS_SUCCESS);
        unsigned offset=0;
        for(unsigned n=0;n<3;n++){
            CHECK(offset%(cls?8:4)==0);
            CHECK(offset+headers[cls]+f.children[n].name.Length<=bytes);
            CHECK(!memcmp(o.bytes+offset+headers[cls],f.names[n],f.children[n].name.Length));
            ULONG next=read32(o.bytes+offset);
            if(n<2){CHECK(next>=headers[cls]+f.children[n].name.Length);offset+=next;}else{CHECK(next==0);CHECK(bytes==offset+headers[cls]+f.children[n].name.Length);}
        }
        finish(&f);
    }
}
static void partial_initial(void){
    const char* names[]={"long-first-name","tail"};
    for(unsigned which=0;which<8;which++){
        enum _FILE_INFORMATION_CLASS classes[]={FileNamesInformation,FileDirectoryInformation,FileFullDirectoryInformation,FileBothDirectoryInformation,FileIdBothDirectoryInformation,FileIdFullDirectoryInformation,FileIdExtdDirectoryInformation,FileIdExtdBothDirectoryInformation};
        unsigned headers[]={12,64,68,94,104,80,88,114};
        fixture f;output o;uintptr_t bytes;init(&f,names,2);unsigned header=headers[which];
        CHECK(query(&f,&o,header+2,classes[which],SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_BUFFER_OVERFLOW);
        CHECK(bytes==header+2);CHECK(read32(o.bytes)==0);CHECK(read32(o.bytes+(which?60:8))==f.children[0].name.Length);
        CHECK(!memcmp(o.bytes+header,f.names[0],2));finish(&f);
    }
}
static void retry(void){
    const char* names[]={"a","long-pending-name","tail"};fixture f;output o;uintptr_t bytes;init(&f,names,3);
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a");
    CHECK(query(&f,&o,14,FileNamesInformation,0,NULL,&bytes)==STATUS_SUCCESS);CHECK(bytes==0);
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"long-pending-name");
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"tail");
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,0,NULL,&bytes)==STATUS_NO_MORE_FILES);CHECK(bytes==0);finish(&f);
}
static void retry_filtered(void){
    const char* names[]={"skip.bin","a.txt","skip-again.bin","long-pending-name.txt","tail.txt"};
    fixture f;output o;uintptr_t bytes;init(&f,names,5);
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,L"*.txt",&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");
    CHECK(query(&f,&o,14,FileNamesInformation,0,NULL,&bytes)==STATUS_SUCCESS);CHECK(bytes==0);
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"long-pending-name.txt");finish(&f);
}
static void failed_second_record(void){
    const char* names[]={"a","long-pending-name"};fixture f;output o;uintptr_t bytes;init(&f,names,2);
    CHECK(query(&f,&o,30,FileNamesInformation,0,NULL,&bytes)==STATUS_SUCCESS);CHECK(bytes==14);expect_name(&o,0,L"a");CHECK(read32(o.bytes)==0);
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,0,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"long-pending-name");finish(&f);
}
static void exact_expression(void){
    const char* names[]={"a.txt","b.bin"};
    for(unsigned sensitive=0;sensitive<2;sensitive++){
        fixture f;output o;uintptr_t bytes;init(&f,names,2);f.handle.case_sensitive=sensitive;
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,0,L"a.txt",&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,0,L"b.bin",&bytes)==STATUS_NO_MORE_FILES);CHECK(bytes==0);
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RESTART_SCAN,L"b.bin",&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RESTART_SCAN,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");finish(&f);
    }
    fixture f;output o;uintptr_t bytes;init(&f,names,2);
    CHECK(query(&f,&o,14,FileNamesInformation,0,L"a.txt",&bytes)==STATUS_BUFFER_OVERFLOW);CHECK(bytes==14);
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,0,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");finish(&f);
}
static void wildcard_case(void){
    const char* names[]={"alpha","Alpha","alpine"};
    for(unsigned sensitive=0;sensitive<2;sensitive++){
        fixture f;output o;uintptr_t bytes;init(&f,names,3);f.handle.case_sensitive=sensitive;
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,L"a*",&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"alpha");
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,sensitive?L"alpine":L"Alpha");finish(&f);
    }
}
static void captured_expression(void){
    const char* names[]={"a.txt","b.bin","c.txt"};
    for(unsigned changed=0;changed<2;changed++){
        fixture f;output o;uintptr_t bytes;init(&f,names,3);
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,L"*.txt",&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,changed?L"*.bin":NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"c.txt");
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RESTART_SCAN|SL_RETURN_SINGLE_ENTRY,changed?L"*.bin":NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");
        CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"c.txt");finish(&f);
    }
    fixture f;output o;uintptr_t bytes;init(&f,names,3);
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,NULL,&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"a.txt");
    CHECK(query(&f,&o,sizeof(o),FileNamesInformation,SL_RETURN_SINGLE_ENTRY,L"*.txt",&bytes)==STATUS_SUCCESS);expect_name(&o,0,L"b.bin");finish(&f);
}
static void run(const char* name,void (*test)(void)){
    unsigned old_checks=checks,old_failures=failures;test();
    printf("%s: %u/%u checks passed\n",name,checks-old_checks-(failures-old_failures),checks-old_checks);
}
int main(void){
    run("extended IDs",ids);run("special entry IDs",id_special_entries);run("record alignment",alignment);
    run("partial initial output",partial_initial);run("larger buffer retry",retry);
    run("filtered retry",retry_filtered);run("failed second record",failed_second_record);run("exact expression",exact_expression);
    run("wildcard case",wildcard_case);run("captured expression",captured_expression);
    printf("%u/%u directory checks passed\n",checks-failures,checks);return failures?1:0;
}
