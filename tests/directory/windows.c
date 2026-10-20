/* Bounded ordinary API checks on disposable WB_RAID5/WB_RAID6 volumes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006)
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %u: %s (error %lu)\n",__LINE__,#c,GetLastError());return 1;}}while(0)
typedef NTSTATUS (NTAPI *query_directory_fn)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS,BOOLEAN,PUNICODE_STRING,BOOLEAN);
typedef NTSTATUS (NTAPI *query_file_fn)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
typedef NTSTATUS (NTAPI *open_file_fn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,ULONG,ULONG);
static query_directory_fn query_directory;
static query_file_fn query_file;
static open_file_fn open_file;
static wchar_t directory[256];
static unsigned calls;
static const wchar_t* const names[]={L"a.txt",L"b.bin",L"c.txt",L"Alpha.txt",L"alpine.txt",L"long-pending-name.txt"};
typedef struct {ULONG NextEntryOffset,FileIndex,FileNameLength;WCHAR FileName[1];} names_information;
typedef struct {
    ULONG NextEntryOffset,FileIndex;
    LARGE_INTEGER CreationTime,LastAccessTime,LastWriteTime,ChangeTime,EndOfFile,AllocationSize;
    ULONG FileAttributes,FileNameLength,EaSize,ReparsePointTag;
    unsigned char FileId[16];
    WCHAR FileName[1];
} extended_information;
typedef struct {
    ULONG NextEntryOffset,FileIndex;
    LARGE_INTEGER CreationTime,LastAccessTime,LastWriteTime,ChangeTime,EndOfFile,AllocationSize;
    ULONG FileAttributes,FileNameLength,EaSize,ReparsePointTag;
    unsigned char FileId[16];
    CCHAR ShortNameLength;
    WCHAR ShortName[12],FileName[1];
} extended_both_information;
typedef struct {uint64_t serial;unsigned char id[16];} file_identity;
typedef union {uint64_t align;unsigned char bytes[2048];} output;
_Static_assert(offsetof(extended_information,FileName)==88,"Windows extended directory layout");
_Static_assert(offsetof(extended_both_information,FileName)==114,"Windows extended both directory layout");
static ULONG read32(const void* p){ULONG n;memcpy(&n,p,sizeof(n));return n;}
static UNICODE_STRING string(const wchar_t* s){UNICODE_STRING value;value.Buffer=(WCHAR*)s;value.Length=(USHORT)(wcslen(s)*sizeof(WCHAR));value.MaximumLength=value.Length;return value;}
static HANDLE open_directory(BOOL sensitive){
    if(!sensitive)return CreateFileW(directory,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    wchar_t native[280];swprintf(native,280,L"\\??\\%ls",directory);
    UNICODE_STRING name=string(native);
    OBJECT_ATTRIBUTES attributes={.Length=sizeof(attributes),.ObjectName=&name};
    IO_STATUS_BLOCK iosb;HANDLE handle=INVALID_HANDLE_VALUE;
    NTSTATUS status=open_file(&handle,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|SYNCHRONIZE,&attributes,&iosb,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT);
    if(status!=STATUS_SUCCESS){SetLastError(ERROR_OPEN_FAILED);return INVALID_HANDLE_VALUE;}
    return handle;
}
static NTSTATUS scan(HANDLE handle,output* out,ULONG capacity,unsigned cls,BOOL single,const wchar_t* pattern,BOOL restart,ULONG_PTR* bytes){
    if(calls++>=64){fprintf(stderr,"directory request limit reached\n");ExitProcess(1);}
    memset(out,0x55,sizeof(*out));IO_STATUS_BLOCK iosb={0};
    UNICODE_STRING mask=pattern?string(pattern):(UNICODE_STRING){0};
    NTSTATUS status=query_directory(handle,NULL,NULL,NULL,&iosb,out->bytes,capacity,(FILE_INFORMATION_CLASS)cls,single,pattern?&mask:NULL,restart);
    *bytes=iosb.Information;return status;
}
static int read_name(const output* out,ULONG_PTR bytes,unsigned offset,unsigned header,unsigned length_offset,wchar_t* name){
    CHECK(offset<=bytes && bytes-offset>=header);
    ULONG length=read32(out->bytes+offset+length_offset);
    CHECK(length%sizeof(WCHAR)==0 && length<128*sizeof(WCHAR) && length<=bytes-offset-header);
    memcpy(name,out->bytes+offset+header,length);name[length/sizeof(WCHAR)]=0;return 0;
}
static int identities(void){
    for(unsigned both=0;both<2;both++){
        HANDLE handle=open_directory(FALSE);CHECK(handle!=INVALID_HANDLE_VALUE);
        output out;ULONG_PTR bytes;
        CHECK(scan(handle,&out,sizeof(out),both?63:60,FALSE,L"*.txt",FALSE,&bytes)==STATUS_SUCCESS);
        CHECK(bytes<=sizeof(out));unsigned offset=0,count=0;unsigned char seen[5][16];
        for(unsigned record=0;record<6;record++){
            wchar_t name[128],path[512];CHECK(read_name(&out,bytes,offset,both?114:88,60,name)==0);
            CHECK(wcschr(name,L'\\')==NULL && wcschr(name,L'/')==NULL);
            swprintf(path,512,L"%ls\\%ls",directory,name);
            HANDLE file=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
            CHECK(file!=INVALID_HANDLE_VALUE);IO_STATUS_BLOCK iosb;file_identity id;
            CHECK(query_file(file,&iosb,&id,sizeof(id),(FILE_INFORMATION_CLASS)59)==STATUS_SUCCESS);
            CHECK(iosb.Information==sizeof(id));CHECK(!memcmp(id.id,out.bytes+offset+72,sizeof(id.id)));
            CHECK(count<5);for(unsigned n=0;n<count;n++)CHECK(memcmp(seen[n],id.id,16)!=0);
            memcpy(seen[count++],id.id,16);CHECK(CloseHandle(file));
            ULONG next=read32(out.bytes+offset);if(!next)break;
            CHECK(next%8==0 && next<=bytes-offset);offset+=next;
        }
        CHECK(count==5);CHECK(CloseHandle(handle));
    }
    puts("PASS extended file IDs");return 0;
}
static int alignment(void){
    for(unsigned cls=0;cls<3;cls++)for(unsigned residue=0;residue<(cls?8:4);residue++){
        unsigned information_class=cls==0?12:cls==1?60:63,header=cls==0?12:cls==1?88:114;
        HANDLE handle=open_directory(FALSE);CHECK(handle!=INVALID_HANDLE_VALUE);output out;ULONG_PTR bytes;
        CHECK(scan(handle,&out,1536+residue,information_class,FALSE,NULL,FALSE,&bytes)==STATUS_SUCCESS);
        CHECK(bytes<=1536+residue);unsigned offset=0,count=0;
        for(unsigned record=0;record<10;record++){
            CHECK(offset%(cls?8:4)==0);wchar_t name[128];CHECK(read_name(&out,bytes,offset,header,cls?60:8,name)==0);count++;
            ULONG next=read32(out.bytes+offset);
            if(!next){CHECK(bytes==offset+header+wcslen(name)*sizeof(WCHAR));break;}
            CHECK(next>=header+wcslen(name)*sizeof(WCHAR) && next<=bytes-offset);offset+=next;
        }
        CHECK(count==8);CHECK(CloseHandle(handle));
    }
    puts("PASS directory record alignment");return 0;
}
static int short_buffers(void){
    HANDLE handle=open_directory(FALSE);CHECK(handle!=INVALID_HANDLE_VALUE);output out;ULONG_PTR bytes;wchar_t name[128];
    CHECK(scan(handle,&out,14,12,TRUE,L"long*",FALSE,&bytes)==STATUS_BUFFER_OVERFLOW);
    CHECK(bytes==14 && read32(out.bytes)==0 && read32(out.bytes+8)==wcslen(names[5])*sizeof(WCHAR));
    CHECK(!memcmp(out.bytes+12,L"l",sizeof(WCHAR)));CHECK(CloseHandle(handle));
    handle=open_directory(FALSE);CHECK(handle!=INVALID_HANDLE_VALUE);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,NULL,FALSE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,name)==0 && !wcscmp(name,L"."));
    CHECK(scan(handle,&out,14,12,TRUE,NULL,FALSE,&bytes)==STATUS_SUCCESS && bytes==0);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,NULL,FALSE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,name)==0 && !wcscmp(name,L".."));CHECK(CloseHandle(handle));
    puts("PASS short directory buffers and retry");return 0;
}
static int captured_patterns(void){
    output out;ULONG_PTR bytes;wchar_t first[128],name[128];HANDLE handle=open_directory(FALSE);CHECK(handle!=INVALID_HANDLE_VALUE);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,L"a*.txt",FALSE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,first)==0);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,NULL,TRUE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,name)==0 && !wcscmp(first,name));
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,L"*.bin",FALSE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,name)==0 && (name[0]==L'a'||name[0]==L'A') && wcscmp(name,first));
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,L"*.bin",TRUE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,name)==0 && !wcscmp(first,name));CHECK(CloseHandle(handle));
    handle=open_directory(FALSE);CHECK(handle!=INVALID_HANDLE_VALUE);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,L"a.txt",FALSE,&bytes)==STATUS_SUCCESS);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,L"b.bin",FALSE,&bytes)==STATUS_NO_MORE_FILES && bytes==0);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,L"b.bin",TRUE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,name)==0 && !wcscmp(name,L"a.txt"));CHECK(CloseHandle(handle));
    handle=open_directory(TRUE);CHECK(handle!=INVALID_HANDLE_VALUE);
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,L"al*",FALSE,&bytes)==STATUS_SUCCESS);
    CHECK(read_name(&out,bytes,0,12,8,name)==0 && !wcscmp(name,L"alpine.txt"));
    CHECK(scan(handle,&out,sizeof(out),12,TRUE,NULL,FALSE,&bytes)==STATUS_NO_MORE_FILES);CHECK(CloseHandle(handle));
    puts("PASS captured patterns and case-sensitive wildcard");return 0;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2||wcslen(argv[1])!=2||argv[1][1]!=L':'||argv[1][0]<L'D'||argv[1][0]>L'Z')return 2;
    wchar_t root[]=L"D:\\",label[64],fs[64];root[0]=argv[1][0];
    CHECK(GetVolumeInformationW(root,label,64,NULL,NULL,NULL,fs,64));
    CHECK((!wcscmp(label,L"WB_RAID5")||!wcscmp(label,L"WB_RAID6"))&&!_wcsicmp(fs,L"Btrfs"));
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");CHECK(ntdll);
    FARPROC symbol=GetProcAddress(ntdll,"NtQueryDirectoryFile");memcpy(&query_directory,&symbol,sizeof(symbol));
    symbol=GetProcAddress(ntdll,"NtQueryInformationFile");memcpy(&query_file,&symbol,sizeof(symbol));
    symbol=GetProcAddress(ntdll,"NtOpenFile");memcpy(&open_file,&symbol,sizeof(symbol));CHECK(query_directory&&query_file&&open_file);
    FILETIME time;GetSystemTimeAsFileTime(&time);
    swprintf(directory,256,L"%lsdirectory-%lu-%08lx%08lx",root,GetCurrentProcessId(),time.dwHighDateTime,time.dwLowDateTime);
    CHECK(CreateDirectoryW(directory,NULL));printf("Directory: %ls\n",directory);
    for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);n++){
        wchar_t path[512];swprintf(path,512,L"%ls\\%ls",directory,names[n]);
        HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(file!=INVALID_HANDLE_VALUE);CHECK(CloseHandle(file));
    }
    CHECK(identities()==0);CHECK(alignment()==0);CHECK(short_buffers()==0);CHECK(captured_patterns()==0);
    for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);n++){wchar_t path[512];swprintf(path,512,L"%ls\\%ls",directory,names[n]);CHECK(DeleteFileW(path));}
    CHECK(RemoveDirectoryW(directory));printf("PASS directory API checks (%u requests)\n",calls);return 0;
}
