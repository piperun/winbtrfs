/* Native Windows services used by the production TRIM submission path. */
#ifndef TRIM_TEST_HOST_H
#define TRIM_TEST_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG;
typedef void* PVOID;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned held; } ERESOURCE;
typedef struct { bool signaled; } KEVENT;
typedef struct { NTSTATUS Status; uintptr_t Information; } IO_STATUS_BLOCK;
typedef struct { int64_t StartingOffset; uint64_t LengthInBytes; } DEVICE_DATA_SET_RANGE;
typedef struct {
    ULONG Size, Action, Flags, ParameterBlockOffset, ParameterBlockLength;
    ULONG DataSetRangesOffset, DataSetRangesLength;
} DEVICE_MANAGE_DATA_SET_ATTRIBUTES;
_Static_assert(sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES) == 28, "Windows DSM header layout");
_Static_assert(sizeof(DEVICE_DATA_SET_RANGE) == 16, "Windows DSM range layout");
typedef struct { unsigned id, StackSize; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct IRP IRP, *PIRP;
typedef NTSTATUS (*COMPLETION)(PDEVICE_OBJECT, PIRP, PVOID);
typedef struct {
    ULONG MajorFunction;
    void* FileObject;
    struct { struct { ULONG IoControlCode, InputBufferLength, OutputBufferLength; } DeviceIoControl; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
struct IRP {
    IO_STACK_LOCATION stack;
    struct { void* SystemBuffer; } AssociatedIrp;
    ULONG Flags;
    void* UserBuffer;
    IO_STATUS_BLOCK* UserIosb;
    COMPLETION completion;
    void* context;
    unsigned device_id;
    bool submitted, completed;
};
typedef struct { LIST_ENTRY list_entry; uint64_t address, size; } space;
typedef struct {
    LIST_ENTRY list_entry, trim_list;
    PDEVICE_OBJECT devobj;
    void* fileobj;
    bool readonly, trim;
    ULONG num_trim_entries;
} device;
typedef struct {
    LIST_ENTRY list_entry, space, space_size, deleting;
    bool space_changed;
    unsigned held;
} chunk;
typedef struct {
    ERESOURCE chunk_lock;
    LIST_ENTRY chunks, devices;
    bool trim, readonly;
    struct { bool no_trim; } options;
} device_extension;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xc0000016)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define STATUS_CANCELLED ((NTSTATUS)0xc0000120)
#define NT_SUCCESS(s) ((s) >= 0)
#define PagedPool 1
#define NonPagedPool 0
#define ALLOC_TAG 0
#define NotificationEvent 0
#define Executive 0
#define KernelMode 0
#define IRP_MJ_DEVICE_CONTROL 14
#define IRP_BUFFERED_IO 16
#define IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES 0x2d9404
#define DeviceDsmAction_Trim 1
#define DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED 0x80000000
#define __stdcall
#define _Function_class_(x)
#define UNUSED(x) ((void)(x))
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define sector_align(n,a) (((n) + (a) - 1) & ~((a) - 1))
#define ExAllocatePoolWithTag(pool,n,tag) allocate(pool,n)
#define IoGetNextIrpStackLocation(irp) (&(irp)->stack)
static void InitializeListHead(LIST_ENTRY* head) { head->Flink = head->Blink = head; }
static bool IsListEmpty(const LIST_ENTRY* head) { return head->Flink == head; }
static void InsertTailList(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Flink = head; entry->Blink = head->Blink;
    head->Blink->Flink = entry; head->Blink = entry;
}
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* head) {
    LIST_ENTRY* entry = head->Flink;
    entry->Flink->Blink = head;
    head->Flink = entry->Flink;
    return entry;
}
#endif
