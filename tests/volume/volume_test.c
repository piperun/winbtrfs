/* Use production handlers with ordinary writable/protected/missing devices. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int32_t NTSTATUS, BOOL;
typedef uint32_t ULONG;
typedef uint16_t USHORT, WCHAR;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { USHORT NameLength; WCHAR Name[1]; } MOUNTDEV_NAME;
#include "types.inc"
typedef struct { NTSTATUS writable; } DEVICE_OBJECT;
typedef struct { LIST_ENTRY list_entry; DEVICE_OBJECT* devobj; } volume_child;
typedef struct { int child_lock; LIST_ENTRY children; } pdo_device_extension;
typedef struct { pdo_device_extension* pdode; } volume_device_extension;
typedef struct { LIST_ENTRY list_entry; DEVICE_OBJECT* devobj; struct {uint8_t uuid[16];} devitem; } device;
typedef struct {
    LIST_ENTRY list_entry, devices;
    int tree_lock;
    struct {uint8_t fsid[16]; uint64_t num_devices;} superblock;
} device_extension;
#define STATUS_SUCCESS 0
#define STATUS_MEDIA_WRITE_PROTECTED ((NTSTATUS)0xc00000a2)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define NT_SUCCESS(s) ((s)>=0)
#define IOCTL_DISK_IS_WRITABLE 1
#define IOCTL_MOUNTDEV_QUERY_DEVICE_NAME 2
#define BTRFS_UUID_SIZE 16
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory memcpy
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
static LIST_ENTRY VcbList;
static int global_loading_lock;
static unsigned locks, calls, cases, failures;
static void InitializeListHead(LIST_ENTRY* h) {h->Flink=h->Blink=h;}
static bool IsListEmpty(LIST_ENTRY* h) {return h->Flink==h;}
static void InsertTailList(LIST_ENTRY* h,LIST_ENTRY* e) {e->Blink=h->Blink;e->Flink=h;h->Blink->Flink=e;h->Blink=e;}
static void ExAcquireResourceSharedLite(int* resource,bool wait) {(void)resource;(void)wait;locks++;}
static void ExReleaseResourceLite(int* resource) {(void)resource;locks--;}
static NTSTATUS dev_ioctl(DEVICE_OBJECT* device,ULONG code,void* in,ULONG inlen,void* out,ULONG outlen,bool override,void* retlen) {
    (void)in;(void)inlen;(void)override;(void)retlen;calls++;
    if(code==IOCTL_DISK_IS_WRITABLE)return device->writable;
    MOUNTDEV_NAME* name=out;
    name->NameLength=2;
    if(outlen>=offsetof(MOUNTDEV_NAME,Name)+2){name->Name[0]='D';return STATUS_SUCCESS;}
    return STATUS_BUFFER_OVERFLOW;
}
#include "queries.inc"
static void check(bool ok,const char* name) {cases++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static void writability(void) {
    const NTSTATUS inputs[][2]={{STATUS_SUCCESS,STATUS_SUCCESS},{STATUS_MEDIA_WRITE_PROTECTED,STATUS_MEDIA_WRITE_PROTECTED},{STATUS_MEDIA_WRITE_PROTECTED,STATUS_SUCCESS},{STATUS_IO_DEVICE_ERROR,STATUS_SUCCESS}};
    const NTSTATUS expected[]={STATUS_SUCCESS,STATUS_MEDIA_WRITE_PROTECTED,STATUS_SUCCESS,STATUS_IO_DEVICE_ERROR};
    for(unsigned n=0;n<4;n++){
        pdo_device_extension p={0};InitializeListHead(&p.children);
        DEVICE_OBJECT devices[2]={{inputs[n][0]},{inputs[n][1]}};
        volume_child children[2]={{.devobj=&devices[0]},{.devobj=&devices[1]}};
        for(unsigned i=0;i<2;i++)InsertTailList(&p.children,&children[i].list_entry);
        volume_device_extension volume={.pdode=&p};calls=0;
        NTSTATUS status=vol_is_writable(&volume);
        printf("writable case%u actual=%08x expected=%08x calls=%u\n",n,(unsigned)status,(unsigned)expected[n],calls);
        check(status==expected[n]&&!locks,"volume writable result");
    }
}
static void missing_devices(void) {
    for(unsigned missing=0;missing<2;missing++)for(unsigned fill=0;fill<2;fill++){
        device_extension volume={.superblock.num_devices=1};DEVICE_OBJECT disk={0};device d={.devobj=missing?NULL:&disk};
        InitializeListHead(&VcbList);InitializeListHead(&volume.devices);
        InsertTailList(&VcbList,&volume.list_entry);InsertTailList(&volume.devices,&d.list_entry);
        memset(volume.superblock.fsid,0x17,16);memset(d.devitem.uuid,0x31,16);
        union{uint64_t align;unsigned char bytes[256];} output;
        memset(output.bytes,fill?0xff:0,sizeof(output.bytes));
        NTSTATUS status=query_filesystems(output.bytes,sizeof(output));
        btrfs_filesystem* result=(void*)output.bytes;
        bool ok=status==STATUS_SUCCESS&&!locks&&result->num_devices==1&&result->next_entry==0&&result->device.missing==(BOOL)missing&&
            !memcmp(result->uuid,volume.superblock.fsid,16)&&!memcmp(result->device.uuid,d.devitem.uuid,16)&&
            result->device.name_length==(missing?0:2);
        printf("device missing=%u initial=%u actual=%d\n",missing,fill,result->device.missing);
        check(ok,"overwrite connected-device missing flag");
    }
}
int main(void) {
    writability();missing_devices();
    printf("%u/%u volume scenarios passed\n",cases-failures,cases);
    return failures!=0;
}
