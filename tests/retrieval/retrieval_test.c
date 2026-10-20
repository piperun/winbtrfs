/* Retrieval-pointer contract fixtures for the extracted production query. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { LARGE_INTEGER StartingVcn; } STARTING_VCN_INPUT_BUFFER;
typedef struct { LARGE_INTEGER NextVcn, Lcn; } EXTENT_RUN;
typedef struct { ULONG ExtentCount; LARGE_INTEGER StartingVcn; EXTENT_RUN Extents[1]; } RETRIEVAL_POINTERS_BUFFER;
typedef struct extent {
    LIST_ENTRY list_entry;
    bool ignore;
    uint64_t offset;
    struct btrfs_file_extent_item extent_data;
} extent;
typedef struct {
    struct { void* Resource; LARGE_INTEGER AllocationSize; } Header;
    LIST_ENTRY extents;
    struct { uint64_t size; } inode_item;
} fcb;
typedef struct { void* FsContext; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct { struct { uint32_t sectorsize; } superblock; unsigned sector_shift; } device_extension;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_END_OF_FILE ((NTSTATUS)0xc0000011)
#define TRACE(...) ((void)0)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %u: %s\n", __LINE__, #c); exit(1); } } while (0)
static unsigned locks;
static void ExAcquireResourceSharedLite(void* p, bool wait) { (void)p; CHECK(wait); CHECK(locks++ == 0); }
static void ExReleaseResourceLite(void* p) { (void)p; CHECK(locks-- == 1); }
_Static_assert(sizeof(EXTENT_RUN) == 16, "Windows retrieval run size");
_Static_assert(offsetof(RETRIEVAL_POINTERS_BUFFER, Extents) == 16, "Windows retrieval header size");
#include "query.inc"

static fcb file;
static extent extents[4];
static EXTENT_RUN expected[8];
static unsigned expected_count;
static unsigned sector_shift;

static void setup(unsigned shift, uint64_t sectors) {
    memset(&file, 0, sizeof(file)); memset(extents, 0, sizeof(extents));
    file.extents.Flink = file.extents.Blink = &file.extents;
    sector_shift = shift;
    file.inode_item.size = sectors << shift;
    file.Header.AllocationSize.QuadPart = sectors << shift;
    expected_count = locks = 0;
}
static void extent_at(unsigned i, uint64_t start, uint64_t length, uint64_t lcn) {
    extent* ext = &extents[i];
    ext->offset = start << sector_shift;
    ext->extent_data.type = BTRFS_FILE_EXTENT_REG;
    ext->extent_data.ram_bytes = ext->extent_data.num_bytes = length << sector_shift;
    ext->extent_data.disk_bytenr = lcn << sector_shift;
    ext->list_entry.Blink = file.extents.Blink; ext->list_entry.Flink = &file.extents;
    file.extents.Blink->Flink = &ext->list_entry; file.extents.Blink = &ext->list_entry;
}
static void run_to(uint64_t next, int64_t lcn) {
    expected[expected_count++] = (EXTENT_RUN){{(int64_t)next}, {lcn}};
}

static void fixture(unsigned kind, unsigned shift) {
    setup(shift, 4);
    switch (kind) {
        case 0: extent_at(0, 0, 2, 100); extent_at(1, 2, 2, 200); run_to(2,100); run_to(4,200); break;
        case 1: extent_at(0, 2, 2, 100); run_to(2,-1); run_to(4,100); break;
        case 2: extent_at(0, 0, 1, 100); extent_at(1, 3, 1, 200); run_to(1,100); run_to(3,-1); run_to(4,200); break;
        case 3: extent_at(0, 0, 2, 100); run_to(2,100); run_to(4,-1); break;
        case 4:
            extent_at(0, 0, 2, 100); extent_at(1, 2, 2, 200);
            extents[0].extent_data.ram_bytes = extents[1].extent_data.ram_bytes = 8ULL << shift;
            extents[0].extent_data.offset = 3ULL << shift;
            run_to(2,103); run_to(4,200); break;
        case 5: extent_at(0, 0, 4, 0); run_to(4,-1); break;
        case 6: extent_at(0, 0, 4, 100); extents[0].extent_data.compression = BTRFS_COMPRESS_ZLIB; run_to(4,-1); break;
        case 7: extent_at(0, 0, 2, 100); extents[0].ignore = true; extent_at(1, 2, 2, 200); run_to(2,-1); run_to(4,200); break;
        case 8: run_to(4,-1); break;
        case 9: extent_at(0, 0, 4, 100); extents[0].extent_data.type = BTRFS_FILE_EXTENT_INLINE; break;
        case 10: file.inode_item.size = 0; extent_at(0, 0, 4, 100); extents[0].extent_data.type = BTRFS_FILE_EXTENT_PREALLOC; run_to(4,100); break;
        case 11: extent_at(0, 0, 4, 100); file.inode_item.size--; run_to(4,100); break;
        default: CHECK(false);
    }
}

static void exercise(unsigned kind, unsigned shift, unsigned capacity, uint64_t requested) {
    fixture(kind, shift);
    union { uint64_t align; unsigned char bytes[160]; } storage;
    memset(&storage, 0xa5, sizeof(storage));
    RETRIEVAL_POINTERS_BUFFER* out = (void*)storage.bytes;
    device_extension volume = {{1u << shift}, shift};
    FILE_OBJECT object = {&file}; STARTING_VCN_INPUT_BUFFER input = {{(int64_t)requested}};
    ULONG size = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents) + capacity * sizeof(EXTENT_RUN);
    ULONG_PTR returned = 0;
    NTSTATUS status = get_retrieval_pointers(&volume, &object, &input, sizeof(input), out, size, &returned);
    CHECK(locks == 0);
    for (unsigned i = size; i < sizeof(storage.bytes); i++) CHECK(storage.bytes[i] == 0xa5);
    if (!capacity) { CHECK(status == STATUS_BUFFER_TOO_SMALL && returned == 0); return; }
    unsigned first = 0;
    while (first < expected_count && (uint64_t)expected[first].NextVcn.QuadPart <= requested) first++;
    if (first == expected_count) { CHECK(status == STATUS_END_OF_FILE && returned == 0); return; }
    unsigned count = min(capacity, expected_count - first);
    CHECK(status == (count == expected_count - first ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW));
    CHECK(out->ExtentCount == count);
    CHECK(returned == offsetof(RETRIEVAL_POINTERS_BUFFER, Extents) + count * sizeof(EXTENT_RUN));
    CHECK(out->StartingVcn.QuadPart == (first ? expected[first-1].NextVcn.QuadPart : 0));
    for (unsigned i = 0; i < count; i++) {
        CHECK(out->Extents[i].NextVcn.QuadPart == expected[first+i].NextVcn.QuadPart);
        CHECK(out->Extents[i].Lcn.QuadPart == expected[first+i].Lcn.QuadPart);
        CHECK(out->Extents[i].NextVcn.QuadPart > (i ? out->Extents[i-1].NextVcn.QuadPart : out->StartingVcn.QuadPart));
    }
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    bool list = strcmp(argv[1], "--list") == 0;
    for (unsigned kind = 0; kind < 12; kind++) {
        for (unsigned shift = 9; shift <= 12; shift += 3) {
            for (unsigned capacity = 0; capacity <= 3; capacity++) {
                for (unsigned start = 0; start <= 4; start++) {
                    char name[80]; snprintf(name, sizeof(name), "layout%u-sector%u-cap%u-vcn%u", kind, shift, capacity, start);
                    if (list) puts(name);
                    else if (strcmp(argv[1], name) == 0) { exercise(kind,shift,capacity,start); return 0; }
                }
            }
        }
    }
    return list ? 0 : 2;
}
