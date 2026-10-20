/* Native I/O services for the production RAID10 scrub path. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG;
typedef void* PVOID;
typedef struct { bool signaled; } KEVENT;
typedef struct { NTSTATUS Status; uintptr_t Information; } IO_STATUS_BLOCK;
typedef struct { unsigned id, StackSize, Flags; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct IRP IRP, *PIRP;
typedef NTSTATUS (*COMPLETION)(PDEVICE_OBJECT, PIRP, PVOID);
typedef struct {
    unsigned MajorFunction;
    void* FileObject;
    struct { struct { ULONG Length; LARGE_INTEGER ByteOffset; } Read; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
struct IRP {
    IO_STACK_LOCATION stack;
    struct { void* SystemBuffer; } AssociatedIrp;
    unsigned Flags;
    void* UserBuffer;
    void* MdlAddress;
    IO_STATUS_BLOCK IoStatus, *UserIosb;
    COMPLETION completion;
    void* context;
    unsigned id;
    bool submitted, completed;
};
typedef struct {
    PDEVICE_OBJECT devobj;
    void* fileobj;
    bool readonly;
    struct { uint64_t devid; } devitem;
} device;
typedef struct {
    struct btrfs_chunk* chunk_item;
    device** devices;
    uint64_t offset;
} chunk;
typedef struct {
    struct { uint32_t sectorsize, nodesize; } superblock;
    unsigned sector_shift, csum_size;
    struct { uint64_t data_scrubbed; } scrub;
} device_extension;

#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xc0000016)
#define NonPagedPool 0
#define PagedPool 1
#define ALLOC_TAG 0
#define IRP_MJ_READ 3
#define DO_BUFFERED_IO 4
#define DO_DIRECT_IO 16
#define IRP_BUFFERED_IO 16
#define IRP_DEALLOCATE_BUFFER 32
#define IRP_INPUT_OPERATION 64
#define KernelMode 0
#define IoWriteAccess 1
#define Executive 0
#define NotificationEvent 0
#define BTRFS_DEV_STAT_WRITE_ERRS 0
#define BTRFS_DEV_STAT_READ_ERRS 1
#define BTRFS_DEV_STAT_CORRUPTION_ERRS 3
#define __stdcall
#define _Function_class_(x)
#define _In_
#define _Out_
#define UNUSED(x) ((void)(x))
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
/* Native cases use neither buffered nor direct I/O and never raise SEH. */
#define try if (true)
#define except(x) else if (false)
#define GetExceptionCode() STATUS_INTERNAL_ERROR
#define IoGetNextIrpStackLocation(irp) (&(irp)->stack)
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)
