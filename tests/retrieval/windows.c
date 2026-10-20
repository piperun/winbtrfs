/* Bounded real-driver checks on disposable WB_RAID5/WB_RAID6 test volumes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>
#include "btrfs.h"
#include "btrfsioctl.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %u: %s (error %lu)\n",__LINE__,#c,GetLastError()); return 1; } } while (0)
static unsigned char* data;
static unsigned char* readback;
static DWORD sector;

static HANDLE create_fixture(const wchar_t* path) {
    return CreateFileW(path,GENERIC_READ|GENERIC_WRITE|FILE_WRITE_ATTRIBUTES,0,NULL,CREATE_NEW,
                       FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,NULL);
}
static BOOL write_at(HANDLE file, LONGLONG offset, const void* bytes, DWORD length) {
    LARGE_INTEGER pos; pos.QuadPart=offset; DWORD written=0;
    return SetFilePointerEx(file,pos,NULL,FILE_BEGIN) && WriteFile(file,bytes,length,&written,NULL) &&
           written==length && FlushFileBuffers(file);
}
static BOOL nocow(HANDLE file) {
    btrfs_set_inode_info info={0}; IO_STATUS_BLOCK iosb;
    info.flags=BTRFS_INODE_NODATACOW|BTRFS_INODE_NODATASUM; info.flags_changed=TRUE;
    NTSTATUS status=NtFsControlFile(file,NULL,NULL,NULL,&iosb,FSCTL_BTRFS_SET_INODE_INFO,&info,sizeof(info),NULL,0);
    if(status) SetLastError(RtlNtStatusToDosError(status));
    return status==0;
}
static int query(HANDLE file, const LONGLONG* ends, const BOOL* holes, unsigned count) {
    union { uint64_t align; unsigned char bytes[256]; } storage;
    RETRIEVAL_POINTERS_BUFFER* out=(void*)storage.bytes;
    STARTING_VCN_INPUT_BUFFER input; input.StartingVcn.QuadPart=0;
    DWORD bytes=0;
    CHECK(DeviceIoControl(file,FSCTL_GET_RETRIEVAL_POINTERS,&input,sizeof(input),out,sizeof(storage),&bytes,NULL));
    CHECK(out->ExtentCount==count && out->StartingVcn.QuadPart==0);
    CHECK(bytes==offsetof(RETRIEVAL_POINTERS_BUFFER,Extents)+count*sizeof(out->Extents[0]));
    for(unsigned i=0;i<count;i++) {
        CHECK(out->Extents[i].NextVcn.QuadPart==ends[i]);
        CHECK(holes[i] ? out->Extents[i].Lcn.QuadPart==-1 : out->Extents[i].Lcn.QuadPart>=0);
    }
    for(unsigned i=0;i<count;i++) {
        input.StartingVcn.QuadPart=i ? ends[i-1] : 0;
        bytes=0; SetLastError(ERROR_SUCCESS);
        BOOL success=DeviceIoControl(file,FSCTL_GET_RETRIEVAL_POINTERS,&input,sizeof(input),out,
                                    sizeof(RETRIEVAL_POINTERS_BUFFER),&bytes,NULL);
        CHECK(i+1==count ? success : !success && GetLastError()==ERROR_MORE_DATA);
        CHECK(bytes==sizeof(RETRIEVAL_POINTERS_BUFFER) && out->ExtentCount==1);
        CHECK(out->StartingVcn.QuadPart==(i ? ends[i-1] : 0));
        CHECK(out->Extents[0].NextVcn.QuadPart==ends[i]);
    }
    return 0;
}
static int queries(const wchar_t* path, BOOL sliced) {
    HANDLE file=create_fixture(path); CHECK(file!=INVALID_HANDLE_VALUE); CHECK(nocow(file));
    DWORD bytes; CHECK(DeviceIoControl(file,FSCTL_SET_SPARSE,NULL,0,NULL,0,&bytes,NULL));
    memset(data,0x17,sector*8);
    if(sliced) {
        CHECK(write_at(file,0,data,sector*4));
        FILE_ZERO_DATA_INFORMATION range;
        range.FileOffset.QuadPart=sector; range.BeyondFinalZero.QuadPart=sector*3;
        CHECK(DeviceIoControl(file,FSCTL_SET_ZERO_DATA,&range,sizeof(range),NULL,0,&bytes,NULL));
    } else {
        CHECK(write_at(file,sector*2,data,sector*2));
        CHECK(write_at(file,sector*6,data,sector*2));
    }
    CHECK(FlushFileBuffers(file)); CHECK(CloseHandle(file));
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE);
    const LONGLONG sparse_ends[]={2,4,6,8}, sliced_ends[]={1,3,4};
    const BOOL sparse_holes[]={TRUE,FALSE,TRUE,FALSE}, sliced_holes[]={FALSE,TRUE,FALSE};
    CHECK(query(file,sliced?sliced_ends:sparse_ends,sliced?sliced_holes:sparse_holes,sliced?3:4)==0);
    CHECK(CloseHandle(file)); CHECK(DeleteFileW(path));
    printf("PASS %s retrieval pointers, full buffer and pagination\n",sliced?"sliced":"sparse");
    return 0;
}
static int writes(const wchar_t* target, const wchar_t* victim) {
    HANDLE file=create_fixture(target); CHECK(file!=INVALID_HANDLE_VALUE); CHECK(nocow(file));
    memset(data,0x11,sector*2); CHECK(write_at(file,0,data,sector*2)); CHECK(CloseHandle(file));
    HANDLE other=create_fixture(victim); CHECK(other!=INVALID_HANDLE_VALUE); CHECK(nocow(other));
    memset(data,0x22,sector); CHECK(write_at(other,0,data,sector)); CHECK(CloseHandle(other));
    file=CreateFileW(target,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,
                     FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE);
    memset(data,0x33,sector*2); CHECK(write_at(file,sector,data,sector*2)); CHECK(CloseHandle(file));
    file=CreateFileW(target,GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE);
    DWORD got=0; CHECK(ReadFile(file,readback,sector*3,&got,NULL) && got==sector*3);
    for(DWORD i=0;i<got;i++) CHECK(readback[i]==(i<sector?0x11:0x33));
    CHECK(CloseHandle(file));
    other=CreateFileW(victim,GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING,NULL);
    CHECK(other!=INVALID_HANDLE_VALUE); CHECK(ReadFile(other,readback,sector,&got,NULL) && got==sector);
    for(DWORD i=0;i<got;i++) CHECK(readback[i]==0x22);
    CHECK(CloseHandle(other)); CHECK(DeleteFileW(target)); CHECK(DeleteFileW(victim));
    puts("PASS NOCOW boundary write, reopened data and neighboring file");
    return 0;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 || wcslen(argv[1])!=2 || argv[1][1]!=L':' || argv[1][0]<L'D' || argv[1][0]>L'Z')return 2;
    wchar_t root[]=L"D:\\",label[64],fs[64]; root[0]=argv[1][0];
    DWORD sectors_per_cluster,bytes_per_sector,free_clusters,clusters;
    CHECK(GetVolumeInformationW(root,label,64,NULL,NULL,NULL,fs,64));
    CHECK((!wcscmp(label,L"WB_RAID5") || !wcscmp(label,L"WB_RAID6")) && !_wcsicmp(fs,L"Btrfs"));
    CHECK(GetDiskFreeSpaceW(root,&sectors_per_cluster,&bytes_per_sector,&free_clusters,&clusters));
    sector=sectors_per_cluster*bytes_per_sector;
    CHECK(sector>=4096 && sector<=65536);
    data=VirtualAlloc(NULL,sector*8,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    readback=VirtualAlloc(NULL,sector*8,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(data && readback);
    wchar_t sparse[128],sliced[128],target[128],victim[128];
    swprintf(sparse,128,L"%lsregression-%lu-sparse.bin",root,GetCurrentProcessId());
    swprintf(sliced,128,L"%lsregression-%lu-sliced.bin",root,GetCurrentProcessId());
    swprintf(target,128,L"%lsregression-%lu-nocow.bin",root,GetCurrentProcessId());
    swprintf(victim,128,L"%lsregression-%lu-neighbor.bin",root,GetCurrentProcessId());
    CHECK(queries(sparse,FALSE)==0); CHECK(queries(sliced,TRUE)==0); CHECK(writes(target,victim)==0);
    VirtualFree(data,0,MEM_RELEASE); VirtualFree(readback,0,MEM_RELEASE);
    return 0;
}
