/* NOCOW traversal uses real write logic; lower disk I/O is a checked service. */
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
typedef struct {
    LIST_ENTRY list_entry;
    bool ignore, unique, inserted;
    uint64_t offset;
    void* csum;
    struct btrfs_file_extent_item extent_data;
} extent;
typedef struct {
    device_extension* Vcb;
    struct { unsigned Flags2; } Header;
    struct { uint64_t flags; } inode_item;
    LIST_ENTRY extents;
    bool extents_changed;
} fcb;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define NT_SUCCESS(s) ((s) >= 0)
#define FSRTL_FLAG2_IS_PAGING_FILE 1
#define HighPagePriority 2
#define NormalPagePriority 1
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %u: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint8_t disk[16 * 4096], expected[sizeof(disk)], payload[4 * 4096];
static unsigned sector, writes, inserts, excisions, dirty, fail_write;
static uint64_t write_addresses[4], write_sizes[4], insert_start, insert_size;
static unsigned char* insert_data;
static chunk data_chunk;

static NTSTATUS write_data_complete(device_extension* v, uint64_t address, void* data, uint32_t length,
                                    PIRP irp, void* csum, bool file_write, uint64_t irp_offset, ULONG priority) {
    (void)v; (void)irp; (void)csum; (void)file_write; (void)irp_offset; (void)priority;
    CHECK(writes < 4 && address + length <= sizeof(disk));
    write_addresses[writes] = address; write_sizes[writes++] = length;
    if (writes == fail_write) return STATUS_IO_DEVICE_ERROR;
    memcpy(disk + address, data, length); return STATUS_SUCCESS;
}
static chunk* get_chunk_from_address(device_extension* v, uint64_t address) { (void)v; (void)address; return &data_chunk; }
static void do_calc_job(device_extension* v, void* data, uint32_t sectors, void* csum) {
    (void)v; (void)data; (void)sectors; (void)csum; CHECK(false);
}
static NTSTATUS excise_extents(device_extension* v, fcb* f, uint64_t start, uint64_t end, PIRP irp, LIST_ENTRY* rollback) {
    (void)v; (void)f; (void)irp; (void)rollback; CHECK(start < end); excisions++; return STATUS_SUCCESS;
}
static NTSTATUS insert_extent(device_extension* v, fcb* f, uint64_t start, uint64_t length, void* data,
                               PIRP irp, bool file_write, uint64_t irp_offset, LIST_ENTRY* rollback) {
    (void)v; (void)f; (void)irp; (void)file_write; (void)irp_offset; (void)rollback;
    inserts++; insert_start = start; insert_size = length; insert_data = data; return STATUS_SUCCESS;
}
static NTSTATUS do_write_file_prealloc(fcb* f, extent* ext, uint64_t start, uint64_t end, void* data, uint64_t* written,
                                       PIRP irp, bool file_write, uint64_t irp_offset, ULONG priority, LIST_ENTRY* rollback) {
    (void)f; (void)ext; (void)start; (void)end; (void)data; (void)written;
    (void)irp; (void)file_write; (void)irp_offset; (void)priority; (void)rollback; CHECK(false); return STATUS_SUCCESS;
}
static void mark_fcb_dirty(fcb* f) { (void)f; dirty++; }
#include "write.inc"

static void append(fcb* file, extent* ext, uint64_t start, uint64_t length, uint64_t physical) {
    memset(ext, 0, sizeof(*ext)); ext->unique = true; ext->offset = start * sector;
    ext->extent_data.type = BTRFS_FILE_EXTENT_REG;
    ext->extent_data.num_bytes = ext->extent_data.ram_bytes = length * sector;
    ext->extent_data.disk_bytenr = physical * sector;
    ext->list_entry.Blink = file->extents.Blink; ext->list_entry.Flink = &file->extents;
    file->extents.Blink->Flink = &ext->list_entry; file->extents.Blink = &ext->list_entry;
}
static void exercise(unsigned shift, unsigned mode) {
    sector = 1u << shift; writes = inserts = excisions = dirty = fail_write = 0;
    memset(disk, 0x5a, sizeof(disk)); memcpy(expected,disk,sizeof(disk));
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 17 + i / sector);
    device_extension volume = {shift,4};
    fcb file = {.Vcb=&volume, .inode_item={BTRFS_INODE_NODATACOW|BTRFS_INODE_NODATASUM}};
    file.extents.Flink = file.extents.Blink = &file.extents;
    extent first, second;
    append(&file,&first,0,2,2);
    if (mode != 1) append(&file,&second,2,2,10);
    unsigned start = mode == 2 ? 0 : 1;
    unsigned end = mode == 3 ? 2 : 3;
    if (mode == 4) fail_write = 1;
    if (mode == 5) fail_write = 2;
    NTSTATUS status = do_write_file(&file,start*sector,end*sector,payload,NULL,false,0,NULL);
    CHECK(status == (fail_write ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS));
    unsigned first_size = min(2-start,end-start)*sector;
    CHECK(write_addresses[0] == (2+start)*sector && write_sizes[0] == first_size);
    if (fail_write != 1) memcpy(expected+(2+start)*sector,payload,first_size);
    bool remaining = end > 2 && fail_write != 1;
    if (remaining && mode != 1) {
        CHECK(writes == 2 && write_addresses[1] == 10*sector && write_sizes[1] == sector);
        if (fail_write != 2) memcpy(expected+10*sector,payload+first_size,sector);
    } else CHECK(writes == 1);
    if (remaining && mode == 1) {
        CHECK(inserts == 1 && excisions == 1 && insert_start == 2*sector && insert_size == sector);
        CHECK(insert_data == payload+first_size && dirty == 1 && file.extents_changed);
    } else CHECK(inserts == 0 && excisions == 0 && dirty == 0);
    CHECK(memcmp(disk,expected,sizeof(disk)) == 0);
}
int main(int argc, char** argv) {
    CHECK(argc == 2); bool list = strcmp(argv[1],"--list") == 0;
    for (unsigned shift = 9; shift <= 12; shift += 3) {
        for (unsigned mode = 0; mode < 6; mode++) {
            char name[64]; snprintf(name,sizeof(name),"sector%u-mode%u",shift,mode);
            if (list) puts(name);
            else if (strcmp(argv[1],name) == 0) { exercise(shift,mode); return 0; }
        }
    }
    return list ? 0 : 2;
}
