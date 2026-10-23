/* Run the production fast write, write-behind and flush lock paths on pthreads. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef bool BOOLEAN;
typedef unsigned char UCHAR;
typedef uint32_t ULONG;
typedef int32_t NTSTATUS;
typedef intptr_t LONG_PTR;
typedef void* PVOID;
typedef void* PIRP;
typedef void* PKTHREAD;
typedef void* PDEVICE_OBJECT;
typedef union {
    struct {
        uint32_t LowPart;
        int32_t HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef struct { NTSTATUS Status; uintptr_t Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

#define __stdcall
#define _Function_class_(x)
#define STATUS_SUCCESS 0
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#define FILE_WRITE_TO_END_OF_FILE 0xffffffff
#define FSRTL_CACHE_TOP_LEVEL_IRP ((LONG_PTR)2)
#define UNUSED(x) ((void)(x))
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)

enum role { ROLE_MAIN, ROLE_WRITER, ROLE_FLUSH, ROLE_LAZY, ROLE_COUNT };
static const char* const role_names[ROLE_COUNT] = {"main", "writer", "flush thread", "lazy writer"};

#define MAX_OWNERS ROLE_COUNT

/* One owner slot per thread; each thread in a scenario has its own role. */
typedef struct {
    const char* name;
    int owners;
    bool exclusive;
    enum role owner_role[MAX_OWNERS];
    int count[MAX_OWNERS];
    int exclusive_waiters;
    int shared_waiters;
} ERESOURCE, *PERESOURCE;

typedef struct {
    ERESOURCE tree_lock;
    bool need_write;
    bool readonly;
    int writes;
} device_extension;

typedef struct {
    PERESOURCE Resource;
    LARGE_INTEGER AllocationSize, FileSize, ValidDataLength;
} FSRTL_COMMON_FCB_HEADER;

typedef struct _fcb {
    FSRTL_COMMON_FCB_HEADER Header;
    device_extension* Vcb;
    struct { uint64_t size; } inode_item;
    PKTHREAD lazy_writer_thread;
} fcb;

typedef struct { void* FsContext; } FILE_OBJECT, *PFILE_OBJECT;

#define FILE_SIZE 65536
#define WRITE_LENGTH 4096
#define PAGE_SIZE 4096
#define DIRTY_PAGE_THRESHOLD 8

static pthread_mutex_t model_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t model_changed = PTHREAD_COND_INITIALIZER; /* lock or throttle state changed */
static pthread_cond_t main_changed = PTHREAD_COND_INITIALIZER;  /* a thread started waiting or finished */
static _Thread_local enum role current_role = ROLE_MAIN;
static _Thread_local int fs_depth;
static _Thread_local PIRP top_level_irp;
static _Thread_local int thread_marker;
static char wait_text[ROLE_COUNT][64];
static const char* waiting_for[ROLE_COUNT];
static PERESOURCE asked[ROLE_COUNT];
static bool started[ROLE_COUNT], finished[ROLE_COUNT];
static char error_text[160];
static bool model_error;

static ERESOURCE fcb_resource = {.name = "fcb->Header.Resource"};
static device_extension volume = {.tree_lock = {.name = "tree_lock"}, .need_write = true};
static fcb file = {
    .Header = {.Resource = &fcb_resource, .AllocationSize.QuadPart = FILE_SIZE,
               .FileSize.QuadPart = FILE_SIZE, .ValidDataLength.QuadPart = FILE_SIZE},
    .Vcb = &volume, .inode_item.size = FILE_SIZE
};
static FILE_OBJECT object = {.FsContext = &file};

static pthread_t writer, flusher, lazy;
static unsigned dirty_pages;
static bool close_throttle_in_copy_write;
static bool writer_in_copy_write, fast_returned, fast_handled;
static uintptr_t fast_information;
static int irp_writes;

static void broadcast_changes(void) {
    pthread_cond_broadcast(&model_changed);
    pthread_cond_broadcast(&main_changed);
}

static void fail_locked(const char* text) {
    if (!model_error)
        snprintf(error_text, sizeof(error_text), "%s: %s", role_names[current_role], text);
    model_error = true;
}

static void fail_model(const char* text) {
    pthread_mutex_lock(&model_lock);
    fail_locked(text);
    pthread_mutex_unlock(&model_lock);
}

static void wait_model(const char* what) {
    waiting_for[current_role] = what;
    pthread_cond_broadcast(&main_changed);
    pthread_cond_wait(&model_changed, &model_lock);
    waiting_for[current_role] = NULL;
}

static void wait_resource(PERESOURCE r, const char* mode) {
    snprintf(wait_text[current_role], sizeof(wait_text[current_role]), "%s %s", r->name, mode);
    wait_model(wait_text[current_role]);
}

static void FsRtlEnterFileSystem(void) { fs_depth++; }
static void FsRtlExitFileSystem(void) { fs_depth--; }
static PKTHREAD KeGetCurrentThread(void) { return &thread_marker; }
static PIRP IoGetTopLevelIrp(void) { return top_level_irp; }
static void IoSetTopLevelIrp(PIRP irp) { top_level_irp = irp; }

static int owner_slot(PERESOURCE r) {
    for (int i = 0; i < r->owners; i++) {
        if (r->owner_role[i] == current_role)
            return i;
    }
    return -1;
}

static void add_owner(PERESOURCE r, bool exclusive) {
    r->owner_role[r->owners] = current_role;
    r->count[r->owners] = 1;
    r->owners++;
    r->exclusive = exclusive;
}

static void note_asked(PERESOURCE r) {
    asked[current_role] = r;
    broadcast_changes();
}

/* ExAcquireResourceSharedLite: a queued exclusive request blocks threads that do not
 * own the resource yet. Releasing the last owner hands the resource to that request,
 * so it also wins over a shared request for an unowned resource.
 * ExAcquireSharedStarveExclusive joins existing shared owners regardless. */
static bool shared_must_wait(PERESOURCE r, bool starve_exclusive) {
    if (r->exclusive)
        return true;
    if (r->exclusive_waiters == 0)
        return false;
    return !starve_exclusive || r->owners == 0;
}

static BOOLEAN acquire_shared(PERESOURCE r, BOOLEAN wait, bool starve_exclusive) {
    BOOLEAN ret = true;
    int slot;

    pthread_mutex_lock(&model_lock);
    note_asked(r);
    slot = owner_slot(r);

    if (slot >= 0)
        r->count[slot]++;
    else {
        while (shared_must_wait(r, starve_exclusive)) {
            if (!wait) {
                ret = false;
                break;
            }
            r->shared_waiters++;
            wait_resource(r, "shared");
            r->shared_waiters--;
        }

        if (ret)
            add_owner(r, false);
    }

    broadcast_changes();
    pthread_mutex_unlock(&model_lock);
    return ret;
}

static inline BOOLEAN ExAcquireResourceSharedLite(PERESOURCE r, BOOLEAN wait) {
    return acquire_shared(r, wait, false);
}

static inline BOOLEAN ExAcquireSharedStarveExclusive(PERESOURCE r, BOOLEAN wait) {
    return acquire_shared(r, wait, true);
}

static inline BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE r, BOOLEAN wait) {
    BOOLEAN ret = true;
    int slot;

    pthread_mutex_lock(&model_lock);
    note_asked(r);
    slot = owner_slot(r);

    if (slot >= 0 && r->exclusive)
        r->count[slot]++;
    else if (slot >= 0) {
        /* Converting shared ownership to exclusive waits for itself. */
        if (wait)
            fail_locked("asked for exclusive access to a resource it holds shared");
        ret = false;
    } else if (r->owners > 0 && !wait)
        ret = false;
    else {
        if (r->owners > 0) {
            r->exclusive_waiters++;
            while (r->owners > 0)
                wait_resource(r, "exclusive");
            r->exclusive_waiters--;
        }

        add_owner(r, true);
    }

    broadcast_changes();
    pthread_mutex_unlock(&model_lock);
    return ret;
}

static inline void ExReleaseResourceLite(PERESOURCE r) {
    int slot;

    pthread_mutex_lock(&model_lock);
    slot = owner_slot(r);

    if (slot < 0)
        fail_locked("released a resource it does not own");
    else if (--r->count[slot] == 0) {
        r->owners--;
        r->owner_role[slot] = r->owner_role[r->owners];
        r->count[slot] = r->count[r->owners];

        if (r->owners == 0)
            r->exclusive = false;
    }

    broadcast_changes();
    pthread_mutex_unlock(&model_lock);
}

static inline BOOLEAN ExIsResourceAcquiredExclusiveLite(PERESOURCE r) {
    BOOLEAN ret;

    pthread_mutex_lock(&model_lock);
    ret = owner_slot(r) >= 0 && r->exclusive;
    pthread_mutex_unlock(&model_lock);
    return ret;
}

static inline ULONG ExIsResourceAcquiredSharedLite(PERESOURCE r) {
    ULONG ret;
    int slot;

    pthread_mutex_lock(&model_lock);
    slot = owner_slot(r);
    ret = slot >= 0 ? (ULONG)r->count[slot] : 0;
    pthread_mutex_unlock(&model_lock);
    return ret;
}

static unsigned pages_for(ULONG length) {
    return (length + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* Holds writers while dirty pages are over the threshold; only the lazy writer
 * lowers the count. */
static BOOLEAN CcCanIWrite(PFILE_OBJECT FileObject, ULONG BytesToWrite, BOOLEAN Wait, UCHAR Retrying) {
    BOOLEAN ret = true;

    (void)FileObject;
    (void)Retrying;

    pthread_mutex_lock(&model_lock);

    while (dirty_pages + pages_for(BytesToWrite) > DIRTY_PAGE_THRESHOLD) {
        if (!Wait) {
            ret = false;
            break;
        }
        wait_model("the CcCanIWrite throttle");
    }

    pthread_mutex_unlock(&model_lock);
    return ret;
}

/* The flush timer fires while the writer is inside FsRtlCopyWrite. */
static void copy_write_hook(void) {
    pthread_mutex_lock(&model_lock);

    if (!writer_in_copy_write) {
        writer_in_copy_write = true;

        if (close_throttle_in_copy_write)
            dirty_pages = DIRTY_PAGE_THRESHOLD;

        broadcast_changes();

        while (asked[ROLE_FLUSH] != &volume.tree_lock && !finished[ROLE_FLUSH])
            pthread_cond_wait(&model_changed, &model_lock);
    }

    pthread_mutex_unlock(&model_lock);
}

/* Documented FsRtlCopyWrite contract, in the order ReactOS ntoskrnl/fsrtl/fastio.c uses:
 * CcCanIWrite with the caller's Wait first, then the main resource, shared for a write
 * that ends within ValidDataLength and exclusive otherwise. */
static BOOLEAN FsRtlCopyWrite(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
                              ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject) {
    fcb* target = FileObject->FsContext;
    int64_t end = FileOffset->QuadPart + Length;
    bool exclusive;

    (void)LockKey;
    (void)Buffer;
    (void)DeviceObject;

    copy_write_hook();

    if (!CcCanIWrite(FileObject, Length, Wait, false))
        return false;

    exclusive = end > target->Header.ValidDataLength.QuadPart;

    if (exclusive ? !ExAcquireResourceExclusiveLite(target->Header.Resource, Wait) :
                    !ExAcquireResourceSharedLite(target->Header.Resource, Wait))
        return false;

    pthread_mutex_lock(&model_lock);
    dirty_pages += pages_for(Length);

    if (end > target->Header.FileSize.QuadPart)
        target->Header.FileSize.QuadPart = end;

    if (end > target->Header.ValidDataLength.QuadPart)
        target->Header.ValidDataLength.QuadPart = end;

    broadcast_changes();
    pthread_mutex_unlock(&model_lock);

    ExReleaseResourceLite(target->Header.Resource);

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = Length;
    return true;
}

static NTSTATUS do_write(device_extension* Vcb, PIRP Irp) {
    (void)Irp;
    Vcb->writes++;
    return STATUS_SUCCESS;
}

static void free_trees(device_extension* Vcb) {
    (void)Vcb;
}

/* The prepass runs under the existing exclusive volume lock. Its reader and
 * publication behavior is exercised separately by metadata-prefetch tests. */
static void preload_checksum_trees(device_extension* Vcb) { (void)Vcb; }
#include "throttle.inc"

/* After fast I/O declines, the I/O manager sends an IRP: write_file2 calls CcCanIWrite
 * before taking any lock, then takes tree_lock shared and the file's resource exclusive. */
static void irp_write(ULONG length) {
    CcCanIWrite(&object, length, true, false);
    ExAcquireResourceSharedLite(&volume.tree_lock, true);
    ExAcquireResourceExclusiveLite(file.Header.Resource, true);

    pthread_mutex_lock(&model_lock);
    dirty_pages += pages_for(length);
    irp_writes++;
    broadcast_changes();
    pthread_mutex_unlock(&model_lock);

    ExReleaseResourceLite(file.Header.Resource);
    ExReleaseResourceLite(&volume.tree_lock);
}

static void finish(void) {
    pthread_mutex_lock(&model_lock);
    finished[current_role] = true;
    broadcast_changes();
    pthread_mutex_unlock(&model_lock);
}

static void* writer_main(void* arg) {
    static char buffer[WRITE_LENGTH];
    LARGE_INTEGER offset = {.QuadPart = 0};
    IO_STATUS_BLOCK iosb = {.Status = -1};
    BOOLEAN handled;

    (void)arg;
    current_role = ROLE_WRITER;

    handled = fast_io_write(&object, &offset, WRITE_LENGTH, true, 0, buffer, &iosb, NULL);

    pthread_mutex_lock(&model_lock);
    fast_returned = true;
    fast_handled = handled;
    fast_information = iosb.Information;
    if (fs_depth != 0)
        fail_locked("fast_io_write left FsRtlEnterFileSystem unbalanced");
    broadcast_changes();
    pthread_mutex_unlock(&model_lock);

    if (!handled)
        irp_write(WRITE_LENGTH);

    finish();
    return NULL;
}

static void* flush_main(void* arg) {
    (void)arg;
    current_role = ROLE_FLUSH;
    do_flush(&volume);
    finish();
    return NULL;
}

static void* lazy_writer_main(void* arg) {
    (void)arg;
    current_role = ROLE_LAZY;

    if (!acquire_for_lazy_write(&object, true))
        fail_model("acquire_for_lazy_write failed with Wait TRUE");
    else {
        pthread_mutex_lock(&model_lock);
        if (file.lazy_writer_thread != KeGetCurrentThread())
            fail_locked("acquire_for_lazy_write did not record the lazy writer thread");
        dirty_pages = 0; /* CcFlushCache writes the file's dirty pages */
        broadcast_changes();
        pthread_mutex_unlock(&model_lock);

        release_from_lazy_write(&object);

        if (file.lazy_writer_thread || IoGetTopLevelIrp())
            fail_model("release_from_lazy_write left the lazy writer state set");
    }

    finish();
    return NULL;
}

static bool writer_entered(void) { return writer_in_copy_write || fast_returned; }
static bool flush_asked(void) { return asked[ROLE_FLUSH] == &volume.tree_lock || finished[ROLE_FLUSH]; }
static bool writer_settled(void) { return waiting_for[ROLE_WRITER] || finished[ROLE_WRITER]; }
static bool all_finished(void) { return finished[ROLE_WRITER] && finished[ROLE_FLUSH] && finished[ROLE_LAZY]; }

static bool wait_main(bool (*condition)(void)) {
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;

    while (!condition()) {
        if (pthread_cond_timedwait(&main_changed, &model_lock, &deadline) == ETIMEDOUT)
            return condition();
    }

    return true;
}

static void start(pthread_t* thread, enum role role, void* (*entry)(void*)) {
    started[role] = true;
    pthread_create(thread, NULL, entry, NULL);
}

static void print_resource(const PERESOURCE r) {
    printf("  %s: ", r->name);

    if (r->owners == 0)
        printf("unowned");
    else {
        printf("owned %s by", r->exclusive ? "exclusive" : "shared");
        for (int i = 0; i < r->owners; i++)
            printf("%s %s", i ? "," : "", role_names[r->owner_role[i]]);
    }

    printf("; %d exclusive waiter(s), %d shared waiter(s)\n", r->exclusive_waiters, r->shared_waiters);
}

static void report_stall(const char* name) {
    printf("FAIL %s: no progress for 2 s\n", name);

    for (int r = ROLE_WRITER; r < ROLE_COUNT; r++) {
        if (finished[r])
            printf("  %s: finished\n", role_names[r]);
        else if (!started[r])
            printf("  %s: not started\n", role_names[r]);
        else if (waiting_for[r])
            printf("  %s: waiting for %s\n", role_names[r], waiting_for[r]);
        else
            printf("  %s: running\n", role_names[r]);
    }

    print_resource(&volume.tree_lock);
    print_resource(&fcb_resource);
}

static bool resource_idle(const PERESOURCE r) {
    return r->owners == 0 && r->exclusive_waiters == 0 && r->shared_waiters == 0;
}

int main(int argc, char** argv) {
    const char* name = argc > 1 ? argv[1] : "";
    bool ok;

    if (!strcmp(name, "throttled"))
        dirty_pages = DIRTY_PAGE_THRESHOLD;
    else if (!strcmp(name, "throttle-closes"))
        close_throttle_in_copy_write = true;
    else if (strcmp(name, "unthrottled")) {
        fprintf(stderr, "unknown scenario '%s'\n", name);
        return 2;
    }

    pthread_mutex_lock(&model_lock);
    start(&writer, ROLE_WRITER, writer_main);
    ok = wait_main(writer_entered);

    if (ok) {
        start(&flusher, ROLE_FLUSH, flush_main);
        ok = wait_main(flush_asked) && wait_main(writer_settled);
    }

    if (ok) {
        start(&lazy, ROLE_LAZY, lazy_writer_main);
        ok = wait_main(all_finished);
    }

    if (!ok) {
        report_stall(name);
        fflush(stdout);
        _exit(1);
    }

    pthread_mutex_unlock(&model_lock);
    pthread_join(writer, NULL);
    pthread_join(flusher, NULL);
    pthread_join(lazy, NULL);

    if (model_error)
        printf("FAIL %s: %s\n", name, error_text);
    else if (!resource_idle(&volume.tree_lock) || !resource_idle(&fcb_resource))
        printf("FAIL %s: a lock is still owned or waited on\n", name);
    else if (volume.writes != 1)
        printf("FAIL %s: do_write ran %d times\n", name, volume.writes);
    else if (fast_handled ? fast_information != WRITE_LENGTH || irp_writes != 0 : irp_writes != 1)
        printf("FAIL %s: fast=%d information=%zu irp=%d\n", name, fast_handled, (size_t)fast_information, irp_writes);
    else if (file.inode_item.size != FILE_SIZE)
        printf("FAIL %s: inode size changed to %llu\n", name, (unsigned long long)file.inode_item.size);
    else {
        printf("PASS %-16s fast=%d irp=%d\n", name, fast_handled, irp_writes);
        return 0;
    }

    return 1;
}
