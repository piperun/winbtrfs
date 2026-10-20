/* Windows-width types for production file-read regression fixtures. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"
typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef int POOL_TYPE;
typedef void* PETHREAD;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { NTSTATUS Status; uintptr_t Information; } IO_STATUS_BLOCK;
typedef struct { LARGE_INTEGER AllocationSize, FileSize, ValidDataLength; } CC_FILE_SIZES;
typedef struct { bool completed; } KEVENT;
typedef struct { NTSTATUS Status; KEVENT event; } calc_job;
typedef struct { int unused; } chunk;
typedef struct {
    struct { uint32_t sectorsize; } superblock;
    uint32_t sector_shift, csum_size;
} device_extension;
typedef struct {
    LIST_ENTRY list_entry;
    uint64_t offset;
    bool ignore;
    unsigned datalen;
    void* csum;
    struct btrfs_file_extent_item extent_data;
} extent;
typedef struct {
    device_extension* Vcb;
    void* subvol;
    bool ads;
    uint8_t type;
    int lock;
    struct { LARGE_INTEGER AllocationSize, FileSize, ValidDataLength; uint8_t Flags2; } Header;
    struct { uint64_t size; } inode_item;
    struct { ULONG Length; uint8_t* Buffer; } adsdata;
    LIST_ENTRY extents;
} fcb;
typedef struct { void* FsContext; void* PrivateCacheMap; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct {
    PFILE_OBJECT FileObject;
    uint8_t MinorFunction;
    struct { struct { ULONG Length; LARGE_INTEGER ByteOffset; } Read; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct {
    IO_STACK_LOCATION stack;
    ULONG Flags;
    void* MdlAddress;
    IO_STATUS_BLOCK IoStatus;
    struct { struct { PETHREAD Thread; } Overlay; } Tail;
    uint8_t* buffer;
} IRP, *PIRP;
#define STATUS_SUCCESS 0
#define STATUS_END_OF_FILE ((NTSTATUS)0xc0000011)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xc0000010)
#define STATUS_FILE_LOCK_CONFLICT ((NTSTATUS)0xc0000054)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xc0000002)
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003e)
#define NT_SUCCESS(s) ((s) >= 0)
#define EXCEPTION_EXECUTE_HANDLER 1
#define IRP_PAGING_IO 1
#define IRP_NOCACHE 2
#define IRP_MN_MDL 1
#define FSRTL_FLAG2_IS_PAGING_FILE 1
#define HighPagePriority 1
#define NormalPagePriority 0
#define PagedPool 1
#define NonPagedPool 0
#define ALLOC_TAG 0
#define Executive 0
#define KernelMode 0
#define LZO_PAGE_SIZE 4096
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset(p,0,n)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
static void InitializeListHead(LIST_ENTRY* h) { h->Flink = h->Blink = h; }
static bool IsListEmpty(LIST_ENTRY* h) { return h->Flink == h; }
static void InsertTailList(LIST_ENTRY* h, LIST_ENTRY* e) {
    e->Blink = h->Blink; e->Flink = h; h->Blink->Flink = e; h->Blink = e;
}
static void InsertHeadList(LIST_ENTRY* h, LIST_ENTRY* e) {
    e->Flink = h->Flink; e->Blink = h; h->Flink->Blink = e; h->Flink = e;
}
static void RemoveEntryList(LIST_ENTRY* e) { e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) { LIST_ENTRY* e=h->Flink; RemoveEntryList(e); return e; }
static LIST_ENTRY* RemoveTailList(LIST_ENTRY* h) { LIST_ENTRY* e=h->Blink; RemoveEntryList(e); return e; }
static uint64_t sector_align(uint64_t n, uint32_t sector) { return (n+sector-1)&~(uint64_t)(sector-1); }
