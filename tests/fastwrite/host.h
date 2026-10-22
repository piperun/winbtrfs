/* Kernel types and mocks for the fast write regression.
 *
 * - ERESOURCEs follow the ExAcquireResource*Lite rules, and a release hands
 *   ownership to the waiters before they run, as ExReleaseResourceLite does.
 * - FsRtlCopyWrite and FsRtlPrepareMdlWriteDev follow the ReactOS
 *   implementations, which follow Nagar, Windows NT File System Internals,
 *   p. 544: a write that extends the file takes the FCB resource exclusive,
 *   sets Header.FileSize, copies or maps the data and releases the resource
 *   before it returns.
 * - A page is clean once a paging write of it succeeds, whatever length the
 *   file system wrote, as with the memory manager.
 *
 * A writer can be preempted as soon as FsRtlCopyWrite releases the FCB
 * resource. The harness models that by letting the thread that was waiting
 * for the resource run until it finishes or blocks before the writer goes on. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define __stdcall
#define _In_
#define _Function_class_(x)
#define UNUSED(x) ((void)(x))
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define try if (1)
#define except(x) if (0 && (x))
#define EXCEPTION_EXECUTE_HANDLER 1
#define GetExceptionCode() STATUS_INTERNAL_ERROR
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))
#define RtlZeroMemory(d, n) memset((d), 0, (n))
#define IoGetCurrentIrpStackLocation(irp) (&(irp)->stack)
#define S_ISDIR(mode) (((mode) & 0170000) == 0040000)
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define STATUS_FILE_LOCK_CONFLICT ((NTSTATUS)0xC0000054)
#define STATUS_DISK_FULL ((NTSTATUS)0xC000007F)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_CANT_WAIT ((NTSTATUS)0xC00000D8)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xC00000E5)

#define FILE_WRITE_TO_END_OF_FILE 0xffffffff
#define FSRTL_FLAG2_IS_PAGING_FILE 0x10
#define FSRTL_CACHE_TOP_LEVEL_IRP ((intptr_t)2)
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_SYMLINK 7
#define BTRFS_COMPRESS_NONE 0
#define BTRFS_FILE_EXTENT_INLINE 0
#define COMPRESSED_EXTENT_SIZE 0x20000
#define IRP_NOCACHE 0x1
#define IRP_PAGING_IO 0x2
#define IRP_SYNCHRONOUS_PAGING_IO 0x40
#define IRP_MN_MDL 0x2
#define MDL_PAGES_LOCKED 0x2
#define MDL_PARTIAL 0x10
#define FO_SYNCHRONOUS_IO 0x2
#define FO_WRITE_THROUGH 0x10
#define FO_FILE_MODIFIED 0x1000
#define FO_FILE_SIZE_CHANGED 0x2000
#define FILE_NOTIFY_CHANGE_SIZE 0x8
#define FILE_NOTIFY_CHANGE_LAST_WRITE 0x10
#define FILE_NOTIFY_CHANGE_STREAM_SIZE 0x200
#define FILE_NOTIFY_CHANGE_STREAM_WRITE 0x800
#define FILE_ACTION_MODIFIED 3
#define FILE_ACTION_MODIFIED_STREAM 8
#define KernelMode 0
#define IoReadAccess 0
#define PagedPool 1
#define ALLOC_TAG 0
#define NormalPagePriority 16
#define HighPagePriority 32

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef bool BOOLEAN;
typedef void* PVOID;
typedef struct kthread* PKTHREAD;
typedef struct kthread* PETHREAD;
typedef struct device_object* PDEVICE_OBJECT;
typedef union {
    struct {
        uint32_t LowPart;
        int32_t HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { USHORT Length, MaximumLength; uint16_t* Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct { NTSTATUS Status; uintptr_t Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
typedef struct { LARGE_INTEGER AllocationSize, FileSize, ValidDataLength; } CC_FILE_SIZES;
typedef struct {
    LARGE_INTEGER AllocationSize, EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending, Directory;
} FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;
/* An MDL here describes cached bytes [offset, offset + length). */
typedef struct _MDL { unsigned int MdlFlags; int64_t offset; ULONG length; } MDL, *PMDL;
typedef struct { void *DataSectionObject, *SharedCacheMap; } SECTION_OBJECT_POINTERS;
typedef struct _FILE_OBJECT {
    void *FsContext, *FsContext2;
    SECTION_OBJECT_POINTERS* SectionObjectPointer;
    void* PrivateCacheMap;
    ULONG Flags;
    LARGE_INTEGER CurrentByteOffset;
} FILE_OBJECT, *PFILE_OBJECT;
typedef struct {
    unsigned char MinorFunction;
    PFILE_OBJECT FileObject;
    union { struct { ULONG Length; LARGE_INTEGER ByteOffset; } Write; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct _IRP {
    ULONG Flags;
    IO_STATUS_BLOCK IoStatus;
    MDL* MdlAddress;
    void* UserBuffer;
    union { void* SystemBuffer; } AssociatedIrp;
    struct { struct { PETHREAD Thread; } Overlay; } Tail;
    IO_STACK_LOCATION stack;
} IRP, *PIRP;

enum { NOBODY, MAIN, WRITER, OTHER, THREADS };

typedef struct _ERESOURCE {
    const char* name;
    int exclusive_owner, exclusive_count, shared_owners;
    int shared[THREADS];
    int queue[THREADS], queued;
} ERESOURCE;

enum { FastIoIsNotPossible, FastIoIsPossible, FastIoIsQuestionable };

struct btrfs_timespec { uint64_t seconds; uint32_t nanoseconds; };
struct btrfs_inode_item {
    uint64_t transid, size, nbytes, sequence;
    uint32_t nlink, mode;
    struct btrfs_timespec ctime, mtime;
};
struct btrfs_file_extent_item {
    uint64_t generation, ram_bytes;
    uint8_t compression, encryption;
    uint16_t other_encoding;
    uint8_t type;
    uint64_t disk_bytenr;
};
typedef struct _device_extension {
    ERESOURCE tree_lock;
    struct { uint32_t sectorsize; uint64_t generation; } superblock;
    struct { uint32_t max_inline; } options;
} device_extension;
typedef struct _root { struct { uint64_t ctransid; struct btrfs_timespec ctime; } root_item; } root;
typedef struct _fcb {
    struct {
        ULONG Flags, Flags2;
        int IsFastIoPossible;
        ERESOURCE *Resource, *PagingIoResource;
        LARGE_INTEGER AllocationSize, FileSize, ValidDataLength;
    } Header;
    device_extension* Vcb;
    root* subvol;
    uint64_t inode;
    uint8_t type;
    bool ads, deleted, inode_item_changed, dirty;
    struct btrfs_inode_item inode_item;
    struct { USHORT Length, MaximumLength; char* Buffer; } adsdata;
    ULONG adsmaxlen;
    int lock;
    PKTHREAD lazy_writer_thread;
} fcb;
typedef struct _dir_child { UNICODE_STRING name; } dir_child;
typedef struct _file_ref { fcb* fcb; struct _file_ref* parent; dir_child* dc; bool delete_on_close; } file_ref;
typedef struct { file_ref* fileref; bool user_set_write_time, user_set_change_time; } ccb;

#define PAGE 4096
#define PAGES 4

static const char* scenario;
static pthread_mutex_t kernel = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kernel_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local int self;
static _Thread_local int fs_depth;
static _Thread_local PIRP top_level_irp;
static const char* thread_names[THREADS] = {"nobody", "main", "writer", "other"};
static ERESOURCE* waiting_for[THREADS];
static bool wants_exclusive[THREADS];

static device_extension volume = {.tree_lock.name = "tree_lock"};
static ERESOURCE fcb_resource = {.name = "FCB resource"};
static ERESOURCE paging_resource = {.name = "paging I/O resource"};
static root subvolume;
static fcb file;
static file_ref fileref;
static ccb context;
static SECTION_OBJECT_POINTERS section;
static int private_cache_map;
static FILE_OBJECT file_object, second_object;

static uint8_t cache[PAGES * PAGE], disk[PAGES * PAGE];
static bool dirty[PAGES];
static int64_t cache_file_size;

struct page_write { const char* thread; int64_t offset; NTSTATUS status; uintptr_t written; };
static struct page_write page_writes[16];
static unsigned int page_write_count;

/* The writer announces its copy and waits there until the other thread waits
 * for a resource; after FsRtlCopyWrite releases the FCB resource the writer
 * waits until that thread finishes or blocks again. Or the writer stops when
 * it calls FsRtlCopyWrite until the other thread finishes or blocks. */
static bool hold_copy, preempt_after_release, hold_copy_write;
static bool writer_copying, writer_at_copy_write, writer_done, other_done;

static BOOLEAN (*fCcCopyWriteEx)(PFILE_OBJECT, PLARGE_INTEGER, ULONG, BOOLEAN, PVOID, PETHREAD);
static void (*fPsUpdateDiskCounters)(void*, uint64_t, uint64_t, ULONG, ULONG, ULONG);
static bool diskacc;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", scenario, __LINE__, #c); _Exit(1); } } while (0)

static void report_stall(const char* what) {
    fprintf(stderr, "FAIL %s: %s waited 2 s for %s\n", scenario, thread_names[self], what);
    for (int t = MAIN; t < THREADS; t++) {
        ERESOURCE* r = waiting_for[t];

        if (r)
            fprintf(stderr, "  %s waits for %s %s (exclusive owner %s, %d shared owners)\n", thread_names[t],
                    r->name, wants_exclusive[t] ? "exclusive" : "shared", thread_names[r->exclusive_owner],
                    r->shared_owners);
    }
    _Exit(1);
}

/* Called with the kernel mutex held. */
#define WAIT_UNTIL(condition, what) do { \
    struct timespec until_; \
    clock_gettime(CLOCK_REALTIME, &until_); \
    until_.tv_sec += 2; \
    while (!(condition)) { \
        if (pthread_cond_timedwait(&kernel_changed, &kernel, &until_) == ETIMEDOUT && !(condition)) \
            report_stall(what); \
    } \
} while (0)

static void grant(ERESOURCE* r, int index) {
    int t = r->queue[index];

    if (wants_exclusive[t]) {
        r->exclusive_owner = t;
        r->exclusive_count = 1;
    } else {
        r->shared[t] = 1;
        r->shared_owners++;
    }

    waiting_for[t] = NULL;
    memmove(&r->queue[index], &r->queue[index + 1], (size_t)(r->queued - index - 1) * sizeof(int));
    r->queued--;
}

/* After an exclusive release every shared waiter gets the resource, otherwise
 * the first exclusive waiter does. */
static void grant_waiters(ERESOURCE* r, bool after_exclusive) {
    bool shared_waiting = false;

    for (int i = 0; i < r->queued; i++)
        shared_waiting |= !wants_exclusive[r->queue[i]];

    if (after_exclusive && shared_waiting) {
        for (int i = 0; i < r->queued;) {
            if (!wants_exclusive[r->queue[i]])
                grant(r, i);
            else
                i++;
        }
        return;
    }

    for (int i = 0; i < r->queued; i++) {
        if (wants_exclusive[r->queue[i]]) {
            grant(r, i);
            return;
        }
    }

    while (r->queued > 0)
        grant(r, 0);
}

static BOOLEAN acquire(ERESOURCE* r, bool exclusive, BOOLEAN wait) {
    bool exclusive_waiting = false;

    pthread_mutex_lock(&kernel);

    for (int i = 0; i < r->queued; i++)
        exclusive_waiting |= wants_exclusive[r->queue[i]];

    if (r->exclusive_owner == self)
        r->exclusive_count++;
    else if (exclusive) {
        /* A thread that owns an ERESOURCE shared cannot acquire it exclusive. */
        CHECK(r->shared[self] == 0);

        if (r->exclusive_owner != NOBODY || r->shared_owners > 0)
            goto wait;

        r->exclusive_owner = self;
        r->exclusive_count = 1;
    } else if (r->shared[self] > 0)
        r->shared[self]++;
    else if (r->exclusive_owner == NOBODY && !exclusive_waiting) {
        r->shared[self] = 1;
        r->shared_owners++;
    } else
        goto wait;

    pthread_cond_broadcast(&kernel_changed);
    pthread_mutex_unlock(&kernel);
    return true;

wait:
    if (!wait) {
        pthread_mutex_unlock(&kernel);
        return false;
    }

    wants_exclusive[self] = exclusive;
    waiting_for[self] = r;
    r->queue[r->queued++] = self;
    pthread_cond_broadcast(&kernel_changed);
    WAIT_UNTIL(waiting_for[self] != r, r->name);
    pthread_mutex_unlock(&kernel);
    return true;
}

static inline BOOLEAN ExAcquireResourceSharedLite(ERESOURCE* r, BOOLEAN wait) { return acquire(r, false, wait); }
static inline BOOLEAN ExAcquireResourceExclusiveLite(ERESOURCE* r, BOOLEAN wait) { return acquire(r, true, wait); }

static inline void ExReleaseResourceLite(ERESOURCE* r) {
    pthread_mutex_lock(&kernel);

    if (r->exclusive_owner == self) {
        if (--r->exclusive_count == 0) {
            r->exclusive_owner = NOBODY;
            grant_waiters(r, true);
        }
    } else {
        CHECK(r->shared[self] > 0);

        if (--r->shared[self] == 0 && --r->shared_owners == 0)
            grant_waiters(r, false);
    }

    pthread_cond_broadcast(&kernel_changed);
    pthread_mutex_unlock(&kernel);
}

static inline BOOLEAN ExIsResourceAcquiredExclusiveLite(ERESOURCE* r) {
    bool owned;

    pthread_mutex_lock(&kernel);
    owned = r->exclusive_owner == self;
    pthread_mutex_unlock(&kernel);

    return owned;
}

static bool owns_resource(ERESOURCE* r) {
    return r->exclusive_owner == self || r->shared[self] > 0;
}

static bool other_waiting(void) {
    return waiting_for[OTHER] != NULL;
}

/* Called by CcCopyWrite and CcPrepareMdlWrite with the kernel mutex held. */
static void hold_writer_copy(void) {
    if (self != WRITER || !hold_copy)
        return;

    hold_copy = false;
    writer_copying = true;
    pthread_cond_broadcast(&kernel_changed);
    WAIT_UNTIL(other_waiting(), "the other thread to wait for a resource");
}

static void hold_writer_at_copy_write(void) {
    if (self != WRITER || !hold_copy_write)
        return;

    pthread_mutex_lock(&kernel);
    hold_copy_write = false;
    writer_at_copy_write = true;
    pthread_cond_broadcast(&kernel_changed);
    WAIT_UNTIL(other_done || other_waiting(), "the other thread to finish or wait");
    pthread_mutex_unlock(&kernel);
}

/* The writer can be preempted here, after FsRtlCopyWrite has released the
 * FCB resource and before the driver's code after the call runs. */
static void preempt_writer(void) {
    if (self != WRITER || !preempt_after_release)
        return;

    pthread_mutex_lock(&kernel);
    WAIT_UNTIL(other_done || other_waiting(), "the other thread to finish or wait");
    pthread_mutex_unlock(&kernel);
}

static void mark_dirty(int64_t offset, ULONG length) {
    for (int64_t p = offset / PAGE; p * PAGE < offset + length; p++)
        dirty[p] = true;
}

static inline void FsRtlEnterFileSystem(void) { fs_depth++; }
static inline void FsRtlExitFileSystem(void) { fs_depth--; }
static inline PKTHREAD KeGetCurrentThread(void) { return (PKTHREAD)(uintptr_t)self; }
static inline PETHREAD PsGetCurrentThread(void) { return (PETHREAD)(uintptr_t)self; }
static inline PIRP IoGetTopLevelIrp(void) { return top_level_irp; }
static inline void IoSetTopLevelIrp(PIRP irp) { top_level_irp = irp; }
static inline bool IoIsSystemThread(PETHREAD thread) { UNUSED(thread); return true; }
static inline void* PsGetThreadProcess(PETHREAD thread) { UNUSED(thread); return NULL; }
static inline void InitializeListHead(LIST_ENTRY* list) { list->Flink = list->Blink = list; }
static inline void KeQuerySystemTime(LARGE_INTEGER* time) { time->QuadPart = 0; }

static inline void win_time_to_unix(LARGE_INTEGER time, struct btrfs_timespec* out) {
    out->seconds = (uint64_t)time.QuadPart;
    out->nanoseconds = 0;
}

static inline uint64_t sector_align(uint64_t n, uint64_t a) {
    if (n & (a - 1))
        n = (n + a) & ~(a - 1);

    return n;
}

static inline uint64_t fcb_alloc_size(fcb* f) {
    return sector_align(f->inode_item.size, f->Vcb->superblock.sectorsize);
}

static inline void* ExAllocatePoolWithTag(int type, size_t size, ULONG tag) {
    UNUSED(type);
    UNUSED(tag);
    return malloc(size);
}

static inline void ExFreePool(void* p) { free(p); }

static inline BOOLEAN CcCanIWrite(PFILE_OBJECT object, ULONG length, BOOLEAN wait, BOOLEAN retrying) {
    UNUSED(object);
    UNUSED(length);
    UNUSED(wait);
    UNUSED(retrying);
    return true;
}

static inline BOOLEAN CcCopyWrite(PFILE_OBJECT object, PLARGE_INTEGER offset, ULONG length, BOOLEAN wait, PVOID buffer) {
    UNUSED(object);
    UNUSED(wait);

    pthread_mutex_lock(&kernel);
    CHECK(owns_resource(&fcb_resource));
    CHECK(offset->QuadPart + length <= (int64_t)sizeof(cache));
    hold_writer_copy();
    memcpy(&cache[offset->QuadPart], buffer, length);
    mark_dirty(offset->QuadPart, length);
    pthread_mutex_unlock(&kernel);

    return true;
}

static inline void CcPrepareMdlWrite(PFILE_OBJECT object, PLARGE_INTEGER offset, ULONG length, MDL** chain,
                                     PIO_STATUS_BLOCK iosb) {
    MDL* mdl = malloc(sizeof(MDL));

    UNUSED(object);
    CHECK(mdl != NULL);

    pthread_mutex_lock(&kernel);
    CHECK(owns_resource(&fcb_resource));
    CHECK(offset->QuadPart + length <= (int64_t)sizeof(cache));
    hold_writer_copy();
    pthread_mutex_unlock(&kernel);

    mdl->MdlFlags = MDL_PAGES_LOCKED;
    mdl->offset = offset->QuadPart;
    mdl->length = length;
    *chain = mdl;
    iosb->Status = STATUS_SUCCESS;
    iosb->Information = length;
}

/* Shrinking the cached file discards the data past the new end. */
static inline void CcSetFileSizes(PFILE_OBJECT object, CC_FILE_SIZES* sizes) {
    UNUSED(object);

    pthread_mutex_lock(&kernel);
    if (sizes->FileSize.QuadPart < cache_file_size)
        memset(&cache[sizes->FileSize.QuadPart], 0, (size_t)(cache_file_size - sizes->FileSize.QuadPart));
    cache_file_size = sizes->FileSize.QuadPart;
    pthread_mutex_unlock(&kernel);
}

/* No scenario writes through a non-cached handle, starts caching late,
 * compresses, stores inline data or extends past AllocationSize. */
static inline void CcFlushCache(SECTION_OBJECT_POINTERS* s, PLARGE_INTEGER offset, ULONG length, PIO_STATUS_BLOCK iosb) {
    UNUSED(s); UNUSED(offset); UNUSED(length); UNUSED(iosb);
    CHECK(!"CcFlushCache");
}

static inline BOOLEAN CcPurgeCacheSection(SECTION_OBJECT_POINTERS* s, PLARGE_INTEGER offset, ULONG length, BOOLEAN uninit) {
    UNUSED(s); UNUSED(offset); UNUSED(length); UNUSED(uninit);
    CHECK(!"CcPurgeCacheSection");
    return false;
}

static inline void init_file_cache(PFILE_OBJECT object, CC_FILE_SIZES* sizes) {
    UNUSED(object); UNUSED(sizes);
    CHECK(!"init_file_cache");
}

static inline NTSTATUS extend_file(fcb* f, file_ref* ref, uint64_t end, bool prealloc, PIRP irp, LIST_ENTRY* rollback) {
    UNUSED(f); UNUSED(ref); UNUSED(end); UNUSED(prealloc); UNUSED(irp); UNUSED(rollback);
    CHECK(!"extend_file");
    return STATUS_INTERNAL_ERROR;
}

static inline NTSTATUS compress_inline(fcb* f, uint8_t* buf, unsigned int length, uint8_t* type, unsigned int* inline_len) {
    UNUSED(f); UNUSED(buf); UNUSED(length); UNUSED(type); UNUSED(inline_len);
    CHECK(!"compress_inline");
    return STATUS_INTERNAL_ERROR;
}

static inline NTSTATUS excise_extents(device_extension* vcb, fcb* f, uint64_t start, uint64_t end, PIRP irp,
                                      LIST_ENTRY* rollback) {
    UNUSED(vcb); UNUSED(f); UNUSED(start); UNUSED(end); UNUSED(irp); UNUSED(rollback);
    CHECK(!"excise_extents");
    return STATUS_INTERNAL_ERROR;
}

static inline NTSTATUS add_extent_to_fcb(fcb* f, uint64_t offset, struct btrfs_file_extent_item* ed, uint16_t size,
                                         bool unique, void* csum, LIST_ENTRY* rollback) {
    UNUSED(f); UNUSED(offset); UNUSED(ed); UNUSED(size); UNUSED(unique); UNUSED(csum); UNUSED(rollback);
    CHECK(!"add_extent_to_fcb");
    return STATUS_INTERNAL_ERROR;
}

static inline NTSTATUS write_compressed(fcb* f, uint64_t start, uint64_t end, void* data, PIRP irp, LIST_ENTRY* rollback) {
    UNUSED(f); UNUSED(start); UNUSED(end); UNUSED(data); UNUSED(irp); UNUSED(rollback);
    CHECK(!"write_compressed");
    return STATUS_INTERNAL_ERROR;
}

static inline void MmProbeAndLockPages(MDL* mdl, int mode, int operation) {
    UNUSED(mdl); UNUSED(mode); UNUSED(operation);
    CHECK(!"MmProbeAndLockPages");
}

static inline void MmUnlockPages(MDL* mdl) {
    UNUSED(mdl);
    CHECK(!"MmUnlockPages");
}

/* Only IRPs without an MDL reach write_file here. */
static inline void* map_user_buffer(PIRP irp, ULONG priority) {
    UNUSED(priority);
    CHECK(!irp->MdlAddress);
    return irp->UserBuffer;
}

static inline BOOLEAN FsRtlCheckLockForWriteAccess(int* lock, PIRP irp) {
    UNUSED(lock); UNUSED(irp);
    return true;
}

static inline void queue_notification_fcb(file_ref* ref, ULONG filter, ULONG action, PUNICODE_STRING stream) {
    UNUSED(ref); UNUSED(filter); UNUSED(action); UNUSED(stream);
}

static inline void mark_fcb_dirty(fcb* f) { f->dirty = true; }
static inline void mark_fileref_dirty(file_ref* ref) { UNUSED(ref); }
static inline bool write_fcb_compressed(fcb* f) { UNUSED(f); return false; }
static inline bool fcb_is_inline(fcb* f) { UNUSED(f); return false; }
static inline void clear_rollback(LIST_ENTRY* rollback) { CHECK(rollback->Flink == rollback); }

static inline void do_rollback(device_extension* vcb, LIST_ENTRY* rollback) {
    UNUSED(vcb); UNUSED(rollback);
    CHECK(!"do_rollback");
}

static inline NTSTATUS read_file(fcb* f, uint8_t* data, uint64_t start, uint64_t length, ULONG* read, PIRP irp) {
    UNUSED(irp);
    CHECK(f == &file && start + length <= sizeof(disk));

    pthread_mutex_lock(&kernel);
    memcpy(data, &disk[start], length);
    pthread_mutex_unlock(&kernel);

    if (read)
        *read = (ULONG)length;

    return STATUS_SUCCESS;
}

static inline NTSTATUS do_write_file(fcb* f, uint64_t start, uint64_t end, void* data, PIRP irp, bool file_write,
                                     uint32_t irp_offset, LIST_ENTRY* rollback) {
    UNUSED(irp); UNUSED(file_write); UNUSED(irp_offset); UNUSED(rollback);
    CHECK(f == &file && start < end && end <= sizeof(disk));

    pthread_mutex_lock(&kernel);
    memcpy(&disk[start], data, end - start);
    pthread_mutex_unlock(&kernel);

    return STATUS_SUCCESS;
}

static inline BOOLEAN FsRtlCopyWrite(PFILE_OBJECT object, PLARGE_INTEGER file_offset, ULONG length, BOOLEAN wait,
                                     ULONG key, PVOID buffer, PIO_STATUS_BLOCK iosb, PDEVICE_OBJECT device) {
    fcb* f = object->FsContext;
    bool append = file_offset->LowPart == FILE_WRITE_TO_END_OF_FILE && file_offset->HighPart == -1;
    bool shared, extended = false;
    LARGE_INTEGER offset, old_size = f->Header.FileSize, old_valid = f->Header.ValidDataLength;
    int64_t end;
    BOOLEAN result;

    UNUSED(key);
    UNUSED(device);

    hold_writer_at_copy_write();

    if (!CcCanIWrite(object, length, wait, false) || object->Flags & FO_WRITE_THROUGH)
        return false;

    iosb->Status = STATUS_SUCCESS;
    iosb->Information = length;

    if (length == 0)
        return true;

    FsRtlEnterFileSystem();

    shared = !append && file_offset->QuadPart + length <= f->Header.ValidDataLength.QuadPart;

    if (!(shared ? ExAcquireResourceSharedLite : ExAcquireResourceExclusiveLite)(f->Header.Resource, wait)) {
        FsRtlExitFileSystem();
        return false;
    }

    offset.QuadPart = append ? f->Header.FileSize.QuadPart : file_offset->QuadPart;
    end = offset.QuadPart + length;

    if (!object->PrivateCacheMap || f->Header.IsFastIoPossible != FastIoIsPossible ||
        offset.QuadPart >= f->Header.ValidDataLength.QuadPart + 0x2000 || end > f->Header.AllocationSize.QuadPart) {
        ExReleaseResourceLite(f->Header.Resource);
        FsRtlExitFileSystem();
        return false;
    }

    /* No scenario leaves a gap for CcZeroData to fill. */
    CHECK(offset.QuadPart <= f->Header.ValidDataLength.QuadPart);

    if (end > f->Header.FileSize.QuadPart) {
        CHECK(!shared);
        extended = true;
        f->Header.FileSize.QuadPart = end;
    }

    result = CcCopyWrite(object, &offset, length, wait, buffer);

    if (result) {
        if (end > f->Header.ValidDataLength.QuadPart)
            f->Header.ValidDataLength.QuadPart = end;

        object->Flags |= FO_FILE_MODIFIED;

        if (extended) {
            pthread_mutex_lock(&kernel);
            cache_file_size = end;
            pthread_mutex_unlock(&kernel);
            object->Flags |= FO_FILE_SIZE_CHANGED;
        }

        object->CurrentByteOffset.QuadPart = end;
    } else if (extended) {
        f->Header.FileSize = old_size;
        f->Header.ValidDataLength = old_valid;
    }

    ExReleaseResourceLite(f->Header.Resource);
    FsRtlExitFileSystem();
    preempt_writer();

    return result;
}

static inline BOOLEAN FsRtlPrepareMdlWriteDev(PFILE_OBJECT object, PLARGE_INTEGER file_offset, ULONG length, ULONG key,
                                              MDL** chain, PIO_STATUS_BLOCK iosb, PDEVICE_OBJECT device) {
    fcb* f = object->FsContext;
    bool append = file_offset->LowPart == FILE_WRITE_TO_END_OF_FILE && file_offset->HighPart == -1;
    bool shared, extended = false;
    LARGE_INTEGER offset;
    int64_t end;

    UNUSED(key);
    UNUSED(device);

    if (!CcCanIWrite(object, length, true, false) || object->Flags & FO_WRITE_THROUGH)
        return false;

    iosb->Status = STATUS_SUCCESS;

    if (length == 0)
        return true;

    FsRtlEnterFileSystem();

    shared = !append && file_offset->QuadPart + length <= f->Header.ValidDataLength.QuadPart;
    (shared ? ExAcquireResourceSharedLite : ExAcquireResourceExclusiveLite)(f->Header.Resource, true);

    offset.QuadPart = append ? f->Header.FileSize.QuadPart : file_offset->QuadPart;
    end = offset.QuadPart + length;

    if (!object->PrivateCacheMap || f->Header.IsFastIoPossible != FastIoIsPossible ||
        end > f->Header.AllocationSize.QuadPart) {
        ExReleaseResourceLite(f->Header.Resource);
        FsRtlExitFileSystem();
        return false;
    }

    CHECK(offset.QuadPart <= f->Header.ValidDataLength.QuadPart);

    if (end > f->Header.FileSize.QuadPart) {
        CHECK(!shared);
        extended = true;
        f->Header.FileSize.QuadPart = end;
    }

    CcPrepareMdlWrite(object, &offset, length, chain, iosb);

    if (end > f->Header.ValidDataLength.QuadPart)
        f->Header.ValidDataLength.QuadPart = end;

    object->Flags |= FO_FILE_MODIFIED;

    if (extended) {
        pthread_mutex_lock(&kernel);
        cache_file_size = end;
        pthread_mutex_unlock(&kernel);
        object->Flags |= FO_FILE_SIZE_CHANGED;
    }

    ExReleaseResourceLite(f->Header.Resource);
    FsRtlExitFileSystem();
    preempt_writer();

    return true;
}

/* CcMdlWriteComplete marks the pages dirty and frees the MDL. */
static inline void CcMdlWriteComplete(PFILE_OBJECT object, PLARGE_INTEGER offset, MDL* chain) {
    UNUSED(object);
    CHECK(chain->offset == offset->QuadPart);

    pthread_mutex_lock(&kernel);
    mark_dirty(chain->offset, chain->length);
    pthread_mutex_unlock(&kernel);

    free(chain);
}

static inline BOOLEAN FsRtlMdlWriteCompleteDev(PFILE_OBJECT object, PLARGE_INTEGER offset, MDL* chain,
                                               PDEVICE_OBJECT device) {
    UNUSED(device);

    if (object->Flags & FO_WRITE_THROUGH)
        return false;

    CcMdlWriteComplete(object, offset, chain);
    return true;
}
