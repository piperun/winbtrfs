/* Exercise the production callback with modeled file contexts and locks. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef bool BOOLEAN;
typedef uint32_t ULONG;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { int64_t sec; } btrfs_timespec;
typedef struct btrfs_inode_item {
    btrfs_timespec otime, atime, mtime, ctime;
    uint64_t size;
    unsigned mode;
} inode_item;
typedef struct _fcb fcb;
typedef struct { fcb* dummy_fcb; fcb* volume_fcb; } device_extension;
struct _fcb {
    bool ads;
    ULONG atts;
    struct { int* Resource; } Header;
    device_extension* Vcb;
    inode_item inode_item;
    struct { ULONG Length; } adsdata;
};
typedef struct _file_ref { struct _file_ref* parent; fcb* fcb; } file_ref;
typedef struct { ULONG access; file_ref* fileref; } ccb;
typedef struct { fcb* FsContext; ccb* FsContext2; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER AllocationSize, EndOfFile;
    ULONG FileAttributes;
} FILE_NETWORK_OPEN_INFORMATION;
typedef struct { int32_t Status; uintptr_t Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
typedef void* PDEVICE_OBJECT;

#define __stdcall
#define _Function_class_(x)
#define FAST_IO_QUERY_NETWORK_OPEN_INFO 0
#define FILE_READ_DATA 0x1
#define FILE_READ_ATTRIBUTES 0x80
#define FILE_WRITE_ATTRIBUTES 0x100
#define FILE_ATTRIBUTE_NORMAL 0x80
#define STATUS_SUCCESS 0
#define S_ISDIR(mode) (((mode) & 0170000) == 0040000)
#define UNUSED(x) ((void)(x))
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define RtlZeroMemory(p, n) memset((p), 0, (n))

static int fs_depth, lock_attempts, lock_acquires, lock_releases;
static unsigned blocked_attempt;
static BOOLEAN expected_wait;
static bool wait_mismatch;

static void FsRtlEnterFileSystem(void) { fs_depth++; }
static void FsRtlExitFileSystem(void) { fs_depth--; }

static inline BOOLEAN ExAcquireResourceSharedLite(int* lock, BOOLEAN wait) {
    (void)lock;
    lock_attempts++;
    if (wait != expected_wait)
        wait_mismatch = true;
    if ((unsigned)lock_attempts == blocked_attempt)
        return false;
    lock_acquires++;
    return true;
}

static inline void ExReleaseResourceLite(int* lock) {
    (void)lock;
    lock_releases++;
}

static void KeQuerySystemTime(LARGE_INTEGER* time) { time->QuadPart = 77; }
static int64_t unix_time_to_win(const btrfs_timespec* time) { return time->sec + 1000; }
static uint64_t fcb_alloc_size(fcb* file) { return file->inode_item.size + 4096; }

#include "network_open.inc"

static int run_case(const char* name, bool ads, ULONG access, BOOLEAN wait, unsigned blocked) {
    device_extension volume = {0};
    int stream_lock = 0, parent_lock = 0;
    fcb parent = {
        .atts = 0x20, .Header.Resource = &parent_lock, .Vcb = &volume,
        .inode_item = {.otime.sec = 10, .atime.sec = 20, .mtime.sec = 30,
                       .ctime.sec = 40, .size = 80}
    };
    fcb stream = {
        .ads = ads, .atts = 0x40, .Header.Resource = &stream_lock, .Vcb = &volume,
        .inode_item = {.otime.sec = 1, .atime.sec = 2, .mtime.sec = 3,
                       .ctime.sec = 4, .size = 50}, .adsdata.Length = 12
    };
    file_ref parent_ref = {.fcb = &parent};
    file_ref stream_ref = {.parent = &parent_ref, .fcb = &stream};
    ccb context = {.access = access, .fileref = ads ? &stream_ref : &parent_ref};
    FILE_OBJECT object = {.FsContext = ads ? &stream : &parent, .FsContext2 = &context};
    FILE_NETWORK_OPEN_INFORMATION info;
    IO_STATUS_BLOCK status = {.Status = -123, .Information = 999};
    bool expected, handled, ok;

    memset(&info, 0x5a, sizeof(info));
    blocked_attempt = blocked;
    expected_wait = wait;
    wait_mismatch = false;
    fs_depth = lock_attempts = lock_acquires = lock_releases = 0;

    handled = fast_io_query_network_open_info(&object, wait, &info, &status, NULL);
    expected = (access & (FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES)) != 0 && blocked == 0;
    ok = handled == expected && fs_depth == 0 && !wait_mismatch &&
         lock_acquires == lock_releases;

    if (expected) {
        ok = ok && status.Status == STATUS_SUCCESS && status.Information == sizeof(info);
        ok = ok && info.CreationTime.QuadPart == 1010 && info.LastWriteTime.QuadPart == 1030;
        ok = ok && info.EndOfFile.QuadPart == (ads ? 12 : 80) &&
             info.AllocationSize.QuadPart == (ads ? 12 : 4176);
        ok = ok && info.FileAttributes == 0x20 && lock_attempts == (ads ? 2 : 1);
    } else {
        ok = ok && status.Status == -123 && status.Information == 999;
        ok = ok && lock_attempts == (blocked ? (int)blocked : 0);
        ok = ok && info.CreationTime.QuadPart == 0x5a5a5a5a5a5a5a5aLL;
    }

    printf("%s %-32s handled=%d attempts=%d releases=%d info=%zu\n",
           ok ? "PASS" : "FAIL", name, handled, lock_attempts, lock_releases,
           (size_t)status.Information);
    return ok;
}

int main(void) {
    unsigned failed = 0, total = 0;
#define CHECK(name, ads, access, wait, blocked) \
    do { failed += !run_case(name, ads, access, wait, blocked); total++; } while (0)
    CHECK("regular authorized", false, FILE_READ_ATTRIBUTES, true, 0);
    CHECK("write attributes only", false, FILE_WRITE_ATTRIBUTES, true, 0);
    CHECK("stream authorized", true, FILE_READ_ATTRIBUTES, true, 0);
    CHECK("nonwait lock succeeds", false, FILE_READ_ATTRIBUTES, false, 0);
    CHECK("unrelated read-data right", false, FILE_READ_DATA, true, 0);
    CHECK("no attribute rights", false, 0, true, 0);
    CHECK("nonwait lock unavailable", false, FILE_READ_ATTRIBUTES, false, 1);
    CHECK("stream parent lock unavailable", true, FILE_READ_ATTRIBUTES, false, 2);
#undef CHECK
    printf("%u/%u fast-info scenarios passed\n", total - failed, total);
    return failed != 0;
}
