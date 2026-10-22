/* Host model for the extent acceptance branch of open_fcb. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); exit(1); } } while (0)
#define ERR(...) ((void)0)
#define STATUS_SUCCESS ((int32_t)0)
#define STATUS_INTERNAL_ERROR ((int32_t)0xc00000e5)

typedef int32_t NTSTATUS;
typedef struct {
    struct btrfs_key key;
    uint32_t size;
    uint8_t data[sizeof(struct btrfs_file_extent_item)];
} tree_item;
typedef struct { tree_item* item; void* tree; } traverse_ptr;

static unsigned reaped;
static uint64_t last_extent_end;
static bool have_last_extent;
static void reap_fcb(void* fcb) { CHECK(fcb); reaped++; }
static bool is_tree_unique(void* vcb, void* tree, void* irp) {
    (void)vcb; (void)tree; (void)irp; return false;
}
static bool is_extent_unique(void* vcb, uint64_t address, uint64_t length, void* irp) {
    (void)vcb; (void)address; (void)length; (void)irp; return false;
}

#include "extent_helper.inc"

static NTSTATUS load_item(uint64_t logical, const struct btrfs_file_extent_item* data) {
    tree_item item = { .key = { .objectid = 257, .type = BTRFS_EXTENT_DATA_KEY, .offset = logical },
                       .size = sizeof(*data) };
    traverse_ptr tp = { .item = &item };
    struct btrfs_file_extent_item* ed = (struct btrfs_file_extent_item*)item.data;
    bool unique = false;
    int dummy;
    void *Vcb = &dummy, *Irp = &dummy, *fcb = &dummy;
    memcpy(item.data, data, sizeof(*data));
    do {
#include "extent_branch.inc"
    } while (false);
    (void)unique;
    (void)last_extent_end;
    (void)have_last_extent;
    return STATUS_SUCCESS;
}

static struct btrfs_file_extent_item regular(void) {
    struct btrfs_file_extent_item item = { 0 };
    item.type = BTRFS_FILE_EXTENT_REG;
    item.disk_bytenr = 0x10000;
    item.disk_num_bytes = 8192;
    item.ram_bytes = 8192;
    item.num_bytes = 4096;
    return item;
}

static void expect_rejected(uint64_t logical, const struct btrfs_file_extent_item* item) {
    CHECK(load_item(logical, item) == STATUS_INTERNAL_ERROR);
    CHECK(reaped == 1);
}

static void expect_loaded(uint64_t logical, const struct btrfs_file_extent_item* item) {
    CHECK(load_item(logical, item) == STATUS_SUCCESS);
    CHECK(reaped == 0);
}

static const char* const cases[] = {
    "regular-valid", "prealloc-valid", "hole-valid", "compressed-valid",
    "uncompressed-slice-valid",
    "adjacent-valid", "gap-valid", "shared-physical-valid",
    "zero-length-rejected", "logical-overflow-rejected", "physical-overflow-rejected",
    "uncompressed-backing-rejected", "compressed-backing-rejected",
    "uncompressed-offset-rejected", "compressed-offset-rejected",
    "logical-overlap-rejected", "prealloc-overlap-rejected",
    "hole-overlap-rejected", "partial-hole-rejected"
};

int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) puts(cases[i]);
        return 0;
    }
    struct btrfs_file_extent_item item = regular();
    if (strcmp(argv[1], "regular-valid") == 0) expect_loaded(0, &item);
    else if (strcmp(argv[1], "prealloc-valid") == 0) { item.type = BTRFS_FILE_EXTENT_PREALLOC; expect_loaded(0, &item); }
    else if (strcmp(argv[1], "hole-valid") == 0) {
        item.disk_bytenr = item.disk_num_bytes = 0; expect_loaded(0, &item);
    } else if (strcmp(argv[1], "compressed-valid") == 0) {
        item.compression = BTRFS_COMPRESS_ZLIB; item.disk_num_bytes = 4096;
        item.offset = 4096; expect_loaded(0, &item);
    } else if (strcmp(argv[1], "uncompressed-slice-valid") == 0) {
        item.offset = 4096; expect_loaded(0, &item);
    } else if (strcmp(argv[1], "adjacent-valid") == 0) {
        expect_loaded(0, &item); item.disk_bytenr += item.disk_num_bytes;
        expect_loaded(4096, &item);
    } else if (strcmp(argv[1], "gap-valid") == 0) {
        expect_loaded(0, &item); expect_loaded(8192, &item);
    } else if (strcmp(argv[1], "shared-physical-valid") == 0) {
        expect_loaded(0, &item); expect_loaded(4096, &item);
    } else if (strcmp(argv[1], "zero-length-rejected") == 0) {
        item.num_bytes = 0; expect_rejected(0, &item);
    } else if (strcmp(argv[1], "logical-overflow-rejected") == 0) {
        expect_rejected(UINT64_MAX - 1023, &item);
    } else if (strcmp(argv[1], "physical-overflow-rejected") == 0) {
        item.disk_bytenr = UINT64_MAX - 4095; expect_rejected(0, &item);
    } else if (strcmp(argv[1], "uncompressed-backing-rejected") == 0) {
        item.offset = 4096; item.num_bytes = 5000; expect_rejected(0, &item);
    } else if (strcmp(argv[1], "compressed-backing-rejected") == 0) {
        item.compression = BTRFS_COMPRESS_ZLIB; item.disk_num_bytes = 4096;
        item.offset = 4096; item.num_bytes = 5000; expect_rejected(0, &item);
    } else if (strcmp(argv[1], "uncompressed-offset-rejected") == 0) {
        item.offset = item.disk_num_bytes + 1; expect_rejected(0, &item);
    } else if (strcmp(argv[1], "compressed-offset-rejected") == 0) {
        item.compression = BTRFS_COMPRESS_ZLIB;
        item.offset = item.ram_bytes + 1; expect_rejected(0, &item);
    } else if (strcmp(argv[1], "logical-overlap-rejected") == 0) {
        expect_loaded(0, &item); expect_rejected(2048, &item);
    } else if (strcmp(argv[1], "prealloc-overlap-rejected") == 0) {
        item.type = BTRFS_FILE_EXTENT_PREALLOC;
        expect_loaded(0, &item); expect_rejected(2048, &item);
    } else if (strcmp(argv[1], "hole-overlap-rejected") == 0) {
        item.disk_bytenr = item.disk_num_bytes = 0;
        expect_loaded(0, &item);
        item = regular();
        expect_rejected(2048, &item);
    } else if (strcmp(argv[1], "partial-hole-rejected") == 0) {
        item.disk_num_bytes = 0; expect_rejected(0, &item);
    } else return 2;
    return 0;
}
