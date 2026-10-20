/* Minimal Windows services for executing the production EA parser natively. */
#ifndef EA_TEST_HOST_H
#define EA_TEST_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "btrfs.h"

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uint8_t UCHAR;
typedef struct { uint16_t Length, MaximumLength; char* Buffer; } STRING, ANSI_STRING;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct device_extension device_extension;
typedef struct { unsigned held; } ERESOURCE;
typedef struct {
    ULONG NextEntryOffset;
    UCHAR Flags, EaNameLength;
    uint16_t EaValueLength;
    char EaName[1];
} FILE_FULL_EA_INFORMATION;
_Static_assert(offsetof(FILE_FULL_EA_INFORMATION, EaName) == 8, "Windows EA layout");
_Static_assert(offsetof(FILE_FULL_EA_INFORMATION, EaValueLength) == 6, "Windows EA layout");
typedef struct {
    struct btrfs_inode_item inode_item;
    ANSI_STRING ea_xattr;
    uint32_t ealen;
    uint8_t type;
    bool sd_dirty, sd_deleted, ea_changed;
    device_extension* Vcb;
    bool ads, inode_item_changed;
    struct { ERESOURCE* Resource; } Header;
} fcb;
typedef struct file_ref { fcb* fcb; struct file_ref* parent; } file_ref;
typedef struct { file_ref* fileref; ULONG access, ea_index; bool user_set_change_time; } ccb;
typedef struct { fcb* FsContext; ccb* FsContext2; } FILE_OBJECT, *PFILE_OBJECT;
struct device_extension {
    unsigned type;
    bool readonly;
    fcb* volume_fcb;
    struct { uint32_t nodesize; uint64_t generation; } superblock;
};
typedef struct { device_extension* DeviceExtension; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct {
    PFILE_OBJECT FileObject;
    struct { struct { ULONG Length; } SetEa; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct {
    unsigned RequestorMode;
    IO_STACK_LOCATION stack;
    void* buffer;
    struct { NTSTATUS Status; uintptr_t Information; } IoStatus;
} IRP, *PIRP;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_EA_TOO_LARGE ((NTSTATUS)0xc0000050)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xc0000010)
#define STATUS_MEDIA_WRITE_PROTECTED ((NTSTATUS)0xc00000a2)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define NT_SUCCESS(s) ((s) >= 0)
#define IN
#define __stdcall
#define _Dispatch_type_(x)
#define _Function_class_(x)
#define VCB_TYPE_VOLUME 1
#define VCB_TYPE_FS 2
#define NormalPagePriority 0
#define UserMode 1
#define KernelMode 0
#define FILE_READ_EA 8
#define FILE_WRITE_EA 16
#define FILE_WRITE_ATTRIBUTES 256
#define FILE_NOTIFY_CHANGE_EA 0x80
#define FILE_ACTION_MODIFIED 3
#define IO_NO_INCREMENT 0
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define IoGetCurrentIrpStackLocation(irp) (&(irp)->stack)
#define PagedPool 1
#define ALLOC_TAG 0
#define EA_EA "user.EA"
#define ERR(...) ((void)0)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
#define RtlCompareMemory(a,b,n) (memcmp(a,b,n) == 0 ? (size_t)(n) : 0)
#define ExAllocatePoolWithTag(pool,size,tag) allocate(size)

static void InitializeListHead(LIST_ENTRY* head) { head->Flink = head->Blink = head; }
static bool IsListEmpty(const LIST_ENTRY* head) { return head->Flink == head; }
static void InsertTailList(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Blink = head->Blink;
    entry->Flink = head;
    head->Blink->Flink = entry;
    head->Blink = entry;
}
static void RemoveEntryList(LIST_ENTRY* entry) {
    entry->Blink->Flink = entry->Flink;
    entry->Flink->Blink = entry->Blink;
}
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* head) {
    LIST_ENTRY* entry = head->Flink;
    RemoveEntryList(entry);
    return entry;
}
static unsigned char uppercase(unsigned char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }
static void RtlUpperString(STRING* dst, const STRING* src) {
    for (uint16_t i = 0; i < src->Length; i++)
        dst->Buffer[i] = (char)uppercase((unsigned char)src->Buffer[i]);
    dst->Length = src->Length;
}
#endif
