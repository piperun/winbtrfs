/* Windows notification boundary for native deferred-work ownership tests. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG;
typedef uint16_t USHORT, WCHAR;
typedef void* PVOID;
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct { USHORT Length, MaximumLength; char* Buffer; } STRING, *PSTRING;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned shared, exclusive; } ERESOURCE;
typedef struct { uint64_t id; } root;
typedef struct device_extension device_extension;
typedef struct {
    device_extension* Vcb;
    root* subvol;
    struct { uint32_t nlink; } inode_item;
    LIST_ENTRY hardlinks;
} fcb;
typedef struct file_ref { fcb* fcb; LONG refcount; bool deleted; } file_ref;
typedef struct { uint64_t parent; UNICODE_STRING name; LIST_ENTRY list_entry; } hardlink;
struct device_extension {
    ERESOURCE tree_lock, fileref_lock;
    file_ref* root_fileref;
    void* NotifySync;
    LIST_ENTRY DirNotifyList;
};
typedef struct { unsigned marker; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct work_item* PIO_WORKITEM;
typedef void (*work_callback)(PDEVICE_OBJECT, PVOID);
struct work_item { work_callback callback; void* context; unsigned number; };

#define _In_
#define _Inout_
#define _In_opt_
#define __stdcall
#define _Function_class_(x)
#define TRUE true
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034)
#define PagedPool 1
#define ALLOC_TAG 0
#define DelayedWorkQueue 1
#define UNUSED(x) ((void)(x))
#define ERR(...) host_log(__VA_ARGS__)
#define WARN(...) host_log(__VA_ARGS__)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

static void InitializeListHead(LIST_ENTRY* h) { h->Flink = h->Blink = h; }
static void host_log(const char* format, ...) { (void)format; }
static LONG InterlockedIncrement(LONG* value) { return ++*value; }
static LONG InterlockedDecrement(LONG* value) { return --*value; }
static void ExAcquireResourceSharedLite(ERESOURCE* r, bool wait);
static void ExAcquireResourceExclusiveLite(ERESOURCE* r, bool wait);
static void ExReleaseResourceLite(ERESOURCE* r);
static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag);
static void ExFreePool(void* p);
static PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT object);
static void IoFreeWorkItem(PIO_WORKITEM item);
static void IoQueueWorkItem(PIO_WORKITEM item, work_callback callback, unsigned queue, void* context);
static NTSTATUS fileref_get_filename(file_ref* fr, PUNICODE_STRING name, USHORT* offset, ULONG* length);
static NTSTATUS open_fileref_by_inode(device_extension* v, root* r, uint64_t inode, file_ref** fr, void* irp);
static void FsRtlNotifyFilterReportChange(void* sync, LIST_ENTRY* list, PSTRING name,
    USHORT offset, PSTRING stream, PSTRING parent, ULONG filter, ULONG action,
    void* target, void* context);
