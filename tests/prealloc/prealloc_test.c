/* Preallocated-extent conversion runs the production function; disk writes and extent bookkeeping are modeled. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef void* PIRP;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned sector_shift, csum_size; } device_extension;
typedef struct { bool changed; } chunk;
typedef struct { uint64_t id; } root;
typedef struct {
    LIST_ENTRY list_entry;
    uint64_t offset;
    uint16_t datalen;
    bool unique, ignore, inserted;
    void* csum;
    struct btrfs_file_extent_item extent_data;
} extent;
typedef struct {
    device_extension* Vcb;
    root* subvol;
    uint64_t inode;
    struct { uint64_t flags; } inode_item;
    LIST_ENTRY extents;
} fcb;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define NT_SUCCESS(s) ((s) >= 0)
#define PagedPool 1
#define ALLOC_TAG 0
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define RtlCopyMemory memcpy
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %u: %s\n", __LINE__, #c); exit(1); } } while (0)

static void* allocations[16];
static unsigned outstanding, writes, fail_write, removals, rollback_inserts, ref_updates;
static chunk data_chunk;

static void* ExAllocatePoolWithTag(int pool, size_t size, unsigned tag) {
    (void)pool; (void)tag;
    void* p = malloc(size); CHECK(p);
    for (unsigned i = 0; i < 16; i++) if (!allocations[i]) { allocations[i] = p; outstanding++; return p; }
    CHECK(false); return NULL;
}
static void ExFreePool(void* p) {
    for (unsigned i = 0; i < 16; i++) if (allocations[i] == p) { allocations[i] = NULL; outstanding--; free(p); return; }
    CHECK(false);
}
static void InsertHeadList(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Flink = head->Flink; entry->Blink = head; head->Flink->Blink = entry; head->Flink = entry;
}
static NTSTATUS write_data_complete(device_extension* v, uint64_t address, void* data, uint32_t length, PIRP irp,
                                    chunk* c, bool file_write, uint64_t irp_offset, ULONG priority) {
    (void)v; (void)address; (void)data; (void)length; (void)irp; (void)c; (void)file_write; (void)irp_offset; (void)priority;
    return ++writes == fail_write ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
static void do_calc_job(device_extension* v, uint8_t* data, uint32_t sectors, void* csum) {
    (void)data; memset(csum, 0, sectors * v->csum_size);
}
static NTSTATUS add_insert_extent_rollback(LIST_ENTRY* rollback, fcb* f, extent* ext) {
    (void)rollback; (void)f; (void)ext; rollback_inserts++; return STATUS_SUCCESS;
}
static NTSTATUS remove_fcb_extent(fcb* f, extent* ext, LIST_ENTRY* rollback) {
    (void)f; (void)rollback; CHECK(!ext->ignore); ext->ignore = true; removals++; return STATUS_SUCCESS;
}
static chunk* get_chunk_from_address(device_extension* v, uint64_t address) { (void)v; (void)address; return &data_chunk; }
static NTSTATUS update_changed_extent_ref(device_extension* v, chunk* c, uint64_t address, uint64_t size, uint64_t root_id,
                                          uint64_t objid, uint64_t offset, int32_t count, bool no_csum, bool superseded,
                                          PIRP irp, LIST_ENTRY* rollback) {
    (void)v; (void)c; (void)address; (void)size; (void)root_id; (void)objid; (void)offset; (void)count;
    (void)no_csum; (void)superseded; (void)irp; (void)rollback; ref_updates++; return STATUS_SUCCESS;
}
/* Same placement as the driver's add_extent: before the first later extent at or after the new offset. */
static void add_extent(fcb* f, LIST_ENTRY* prevextle, extent* newext) {
    for (LIST_ENTRY* le = prevextle->Flink; le != &f->extents; le = le->Flink) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);
        if (ext->offset >= newext->offset) { InsertHeadList(ext->list_entry.Blink, &newext->list_entry); return; }
    }
    InsertHeadList(f->extents.Blink, &newext->list_entry);
}
#include "prealloc.inc"

enum { REPLACE_ALL, REPLACE_START, REPLACE_END, REPLACE_MIDDLE };

static void exercise(unsigned branch, bool fail, bool datasum) {
    const uint64_t sector = 4096;
    /* Written range per branch, in sectors of the 4-sector preallocated extent. */
    static const unsigned ranges[4][2] = { {0, 4}, {0, 2}, {2, 4}, {1, 3} };
    /* Resulting live extents per branch: offset, end, preallocated. */
    static const unsigned results[4][3][3] = {
        { {0, 4, 0} },
        { {0, 2, 0}, {2, 4, 1} },
        { {0, 2, 1}, {2, 4, 0} },
        { {0, 1, 1}, {1, 3, 0}, {3, 4, 1} },
    };
    static const unsigned result_counts[4] = { 1, 2, 2, 3 };
    device_extension volume = { 12, 4 };
    root subvol = { 256 };
    fcb file = { .Vcb = &volume, .subvol = &subvol, .inode = 257,
                 .inode_item = { datasum ? 0 : BTRFS_INODE_NODATASUM } };
    file.extents.Flink = file.extents.Blink = &file.extents;
    extent original = { .offset = 0, .datalen = sizeof(struct btrfs_file_extent_item), .unique = true };
    original.extent_data.type = BTRFS_FILE_EXTENT_PREALLOC;
    original.extent_data.disk_bytenr = 64 * sector;
    original.extent_data.disk_num_bytes = original.extent_data.num_bytes = original.extent_data.ram_bytes = 4 * sector;
    InsertHeadList(&file.extents, &original.list_entry);
    uint8_t payload[4 * 4096] = { 0 };
    uint64_t written = 12345;
    fail_write = fail ? 1 : 0;
    NTSTATUS status = do_write_file_prealloc(&file, &original, ranges[branch][0] * sector, ranges[branch][1] * sector,
                                             payload, &written, NULL, false, 0, 0, NULL);
    if (fail) {
        CHECK(status == STATUS_IO_DEVICE_ERROR && writes == 1);
        CHECK(outstanding == 0);
        CHECK(written == 12345 && removals == 0 && rollback_inserts == 0 && ref_updates == 0 && !data_chunk.changed);
        CHECK(!original.ignore && file.extents.Flink == &original.list_entry && original.list_entry.Flink == &file.extents);
        return;
    }
    CHECK(status == STATUS_SUCCESS && writes == 1 && removals == 1 && data_chunk.changed);
    CHECK(written == (ranges[branch][1] - ranges[branch][0]) * sector);
    unsigned live = 0, csums = 0;
    for (LIST_ENTRY* le = file.extents.Flink; le != &file.extents; le = le->Flink) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);
        if (ext->ignore) { CHECK(ext == &original); continue; }
        CHECK(live < result_counts[branch]);
        const unsigned* want = results[branch][live++];
        CHECK(ext->offset == want[0] * sector && ext->extent_data.num_bytes == (want[1] - want[0]) * sector);
        CHECK(ext->extent_data.type == (want[2] ? BTRFS_FILE_EXTENT_PREALLOC : BTRFS_FILE_EXTENT_REG));
        CHECK(ext->extent_data.offset == want[0] * sector);
        CHECK(!ext->csum == (want[2] || !datasum));
        csums += ext->csum != NULL;
    }
    CHECK(live == result_counts[branch] && outstanding == live + csums);
    for (LIST_ENTRY* le = file.extents.Flink; le != &file.extents;) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);
        le = le->Flink;
        if (ext == &original) continue;
        if (ext->csum) ExFreePool(ext->csum);
        ExFreePool(ext);
    }
    CHECK(outstanding == 0);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    static const char* branches[] = { "replace-all", "replace-start", "replace-end", "replace-middle" };
    for (unsigned branch = 0; branch < 4; branch++) for (unsigned fail = 0; fail < 2; fail++)
        for (unsigned datasum = 0; datasum < 2; datasum++) {
            char name[64];
            snprintf(name, sizeof(name), "%s-%s-%s", branches[branch], fail ? "write-fails" : "write-succeeds",
                     datasum ? "datasum" : "nodatasum");
            if (strcmp(argv[1], "--list") == 0) puts(name);
            else if (strcmp(argv[1], name) == 0) { exercise(branch, fail, datasum); return 0; }
        }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
