/* Exact symlink reparse setter; file writes and metadata services are modeled. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uint16_t USHORT, WCHAR;
typedef struct { int unused; } LIST_ENTRY;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef void* PIRP;
struct btrfs_timespec { uint64_t sec; };
typedef struct { USHORT Length, MaximumLength; char* Buffer; } ANSI_STRING;
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING;
typedef struct {
    ULONG ReparseTag;
    USHORT ReparseDataLength, Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct { uint8_t DataBuffer[1]; } GenericReparseBuffer;
    };
} REPARSE_DATA_BUFFER;
typedef struct { struct { uint64_t generation; } superblock; } device_extension;
typedef struct { struct { uint64_t ctransid; struct btrfs_timespec ctime; } root_item; } root;
typedef struct {
    uint8_t type; device_extension* Vcb; root* subvol; bool inode_item_changed;
    struct { uint64_t generation, transid, sequence; ULONG mode; struct btrfs_timespec ctime, mtime; } inode_item;
} fcb;
typedef struct { uint8_t type; } dir_child;
typedef struct { dir_child* dc; } file_ref;
typedef struct { bool user_set_change_time, user_set_write_time; } ccb;
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_SYMLINK 7
#define IO_REPARSE_TAG_SYMLINK 0xa000000cU
#define IO_REPARSE_TAG_LX_SYMLINK 0xa000001dU
#define __S_IFMT 0170000
#define __S_IFLNK 0120000
#define __S_IFREG 0100000
#define PagedPool 1
#define ALLOC_TAG 0
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define NT_SUCCESS(s) ((s) >= 0)
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)

static unsigned truncates, writes, dirty_fcbs, dirty_filerefs, allocations;
static ULONG written_length;
static char written[64];

static void* ExAllocatePoolWithTag(int pool, size_t size, unsigned tag) {
    (void)pool; (void)tag; void* p = malloc(size ? size : 1); if (p) allocations++; return p;
}
static void ExFreePool(void* p) { allocations--; free(p); }
static NTSTATUS utf16_to_utf8(char* dest, ULONG dest_max, ULONG* dest_len, WCHAR* src, ULONG src_len) {
    *dest_len = src_len / sizeof(WCHAR);
    for (ULONG i = 0; dest && i < *dest_len && i < dest_max; i++) dest[i] = (char)src[i];
    return STATUS_SUCCESS;
}
static NTSTATUS truncate_file(fcb* f, uint64_t end, PIRP irp, LIST_ENTRY* rollback) {
    (void)f; (void)irp; (void)rollback; truncates++; return end == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
/* Records the request; copies only lengths that fit, so an oversized request is visible without reading past it. */
static NTSTATUS write_file2(device_extension* v, PIRP irp, LARGE_INTEGER offset, void* buf, ULONG* length, bool paging_io,
                            bool no_cache, bool wait, bool deferred_write, bool write_irp, LIST_ENTRY* rollback) {
    (void)v; (void)irp; (void)paging_io; (void)no_cache; (void)wait; (void)deferred_write; (void)write_irp; (void)rollback;
    writes++; written_length = *length;
    if (offset.QuadPart != 0 || *length > sizeof(written)) return STATUS_UNSUCCESSFUL;
    memcpy(written, buf, *length); return STATUS_SUCCESS;
}
static void KeQuerySystemTime(LARGE_INTEGER* t) { t->QuadPart = 42; }
static void win_time_to_unix(LARGE_INTEGER t, struct btrfs_timespec* n) { n->sec = (uint64_t)t.QuadPart; }
static void mark_fcb_dirty(fcb* f) { (void)f; dirty_fcbs++; }
static void mark_fileref_dirty(file_ref* r) { (void)r; dirty_filerefs++; }
#include "symlink.inc"

typedef struct {
    const char* name;
    USHORT data_length;   /* ReparseDataLength: the 4-byte LX header plus the target */
    ULONG buflen;         /* request buffer length, header included */
    NTSTATUS expected;
    const char* target;
} scenario;

static bool run(const scenario* s) {
    /* The request buffer is allocated at its stated length, as the I/O manager's system buffer is. */
    size_t size = s->buflen > 8u + s->data_length ? s->buflen : 8u + s->data_length;
    REPARSE_DATA_BUFFER* rdb = calloc(1, size);
    if (!rdb) return false;
    rdb->ReparseTag = IO_REPARSE_TAG_LX_SYMLINK; rdb->ReparseDataLength = s->data_length;
    if (s->data_length > 4) memcpy(rdb->GenericReparseBuffer.DataBuffer + 4, s->target, s->data_length - 4u);
    device_extension v = { .superblock.generation = 12 }; root sub = { 0 };
    fcb f = { .type = BTRFS_FT_REG_FILE, .Vcb = &v, .subvol = &sub,
              .inode_item = { .generation = 4, .transid = 5, .sequence = 6, .mode = __S_IFREG | 0644 } };
    dir_child dc = { .type = BTRFS_FT_REG_FILE }; file_ref ref = { .dc = &dc }; ccb cb = { 0 };
    truncates = writes = dirty_fcbs = dirty_filerefs = allocations = 0; written_length = 0; memset(written, 0, sizeof(written));
    NTSTATUS status = set_symlink(NULL, &ref, &f, &cb, rdb, s->buflen, NULL);
    bool ok = status == s->expected && allocations == 0;
    if (s->expected == STATUS_SUCCESS) {
        size_t n = strlen(s->target);
        ok = ok && f.type == BTRFS_FT_SYMLINK && dc.type == BTRFS_FT_SYMLINK && (f.inode_item.mode & __S_IFMT) == __S_IFLNK;
        ok = ok && truncates == 1 && writes == 1 && written_length == n && memcmp(written, s->target, n) == 0;
        ok = ok && f.inode_item_changed && dirty_fcbs == 1 && dirty_filerefs == 1;
    } else {
        ok = ok && f.type == BTRFS_FT_REG_FILE && dc.type == BTRFS_FT_REG_FILE && f.inode_item.mode == (__S_IFREG | 0644);
        ok = ok && truncates == 0 && writes == 0 && !f.inode_item_changed && dirty_fcbs == 0 && dirty_filerefs == 0;
    }
    if (!ok)
        printf("FAIL %s: status=%08x expected=%08x truncates=%u writes=%u write length=%u\n", s->name, (unsigned)status,
               (unsigned)s->expected, truncates, writes, (unsigned)written_length);
    free(rdb);
    return ok;
}

int main(void) {
    static const scenario scenarios[] = {
        { "LX symlink with no data", 0, 8, STATUS_INVALID_PARAMETER, "" },
        { "LX symlink with 1 byte of data", 1, 9, STATUS_INVALID_PARAMETER, "" },
        { "LX symlink with 2 bytes of data", 2, 10, STATUS_INVALID_PARAMETER, "" },
        { "LX symlink with 3 bytes of data", 3, 11, STATUS_INVALID_PARAMETER, "" },
        { "LX symlink with an empty target", 4, 12, STATUS_SUCCESS, "" },
        { "LX symlink with a target", 13, 21, STATUS_SUCCESS, "/tmp/link" },
        { "LX symlink longer than its buffer", 13, 20, STATUS_INVALID_PARAMETER, "/tmp/link" },
    };
    unsigned passed = 0, total = sizeof(scenarios) / sizeof(scenarios[0]);
    for (unsigned i = 0; i < total; i++) passed += run(&scenarios[i]);
    printf("%u/%u symlink scenarios passed\n", passed, total);
    return passed != total;
}
