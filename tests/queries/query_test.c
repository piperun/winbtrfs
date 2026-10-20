/* Native contract regressions for the actual production query handlers. */
#include "host.h"
#include "query.inc"

static int checksum_case(const char *name) {
    device_extension v = {{4096, BTRFS_CSUM_TYPE_CRC32}, 12, 4};
    fcb node = {.type = 1, .inode_item = {.size = 8192}};
    ccb handle = {.access = FILE_READ_DATA};
    FILE_OBJECT file = {&node, &handle};
    uint32_t sums[] = {0x11223344, 0x55667788};
    extent ext[2] = {
        {.offset = 0, .extent_data = {.type = 1, .num_bytes = 4096}, .csum = &sums[0]},
        {.offset = 4096, .extent_data = {.type = 1, .num_bytes = 4096}, .csum = &sums[1]}
    };
    NTSTATUS expected = STATUS_SUCCESS;
    uint64_t sectors = 2;
    uint32_t expected_slots[] = {sums[0], sums[1]};
    bool payload = true;
    unsigned count = 2;
    if (!strcmp(name, "checksum-mixed")) {
        ext[0].extent_data.compression = 1;
        expected_slots[0] = 0;
    } else if (!strcmp(name, "checksum-inline")) {
        ext[0].extent_data.type = BTRFS_FILE_EXTENT_INLINE;
        node.inode_item.size = 10;
        sectors = 0;
        payload = false;
    } else if (!strcmp(name, "checksum-directory")) {
        node.type = BTRFS_FT_DIR;
        expected = STATUS_FILE_IS_A_DIRECTORY;
        payload = false;
    } else if (!strcmp(name, "checksum-ads")) {
        node.ads = true;
        expected = STATUS_INVALID_DEVICE_REQUEST;
        payload = false;
    } else if (!strcmp(name, "checksum-nodatasum")) {
        node.inode_item.flags = BTRFS_INODE_NODATASUM;
        expected = STATUS_INVALID_DEVICE_REQUEST;
        payload = false;
    } else if (!strcmp(name, "checksum-sparse") || !strcmp(name, "checksum-pool-failure")) {
        ext[0].offset = 4096;
        count = 1;
        expected_slots[0] = 0x6b6b6b6b;
        expected_slots[1] = sums[0];
        if (!strcmp(name, "checksum-pool-failure")) {
            fail_pool = true;
            expected = STATUS_INSUFFICIENT_RESOURCES;
            payload = false;
        }
    } else if (!strcmp(name, "checksum-prealloc-eof")) {
        ext[0].extent_data.type = BTRFS_FILE_EXTENT_PREALLOC;
        ext[0].extent_data.num_bytes = 8192;
        node.inode_item.size = 4096;
        count = 1;
        sectors = 1;
    } else if (!strcmp(name, "checksum-extent-after-eof")) {
        node.inode_item.size = 4096;
        sectors = 1;
    } else if (!strcmp(name, "checksum-trailing-hole") || !strcmp(name, "checksum-trailing-pool-failure")) {
        count = 1;
        expected_slots[1] = 0x6b6b6b6b;
        if (!strcmp(name, "checksum-trailing-pool-failure")) {
            fail_pool = true;
            expected = STATUS_INSUFFICIENT_RESOURCES;
            payload = false;
        }
    } else if (!strcmp(name, "checksum-sparse-only")) {
        count = 0;
        expected_slots[0] = expected_slots[1] = 0x6b6b6b6b;
    }
    link_extents(&node, ext, count);
    /* All baseline queries have room for both full checksum slots. */
    union { uint64_t alignment; unsigned char bytes[64]; } storage;
    memset(storage.bytes, 0xa5, sizeof(storage.bytes));
    btrfs_csum_info *out = (void *)storage.bytes;
    ULONG length = offsetof(btrfs_csum_info, data) + sizeof(sums);
    ULONG_PTR returned = 0;
    NTSTATUS status = get_csum_info(&v, &file, out, length, &returned, UserMode);
    CHECK(acquired == 1 && released == 1);
    CHECK(allocations == frees);
    CHECK(status == expected);
    if (expected == STATUS_SUCCESS) {
        CHECK(out->num_sectors == sectors);
        CHECK(returned == offsetof(btrfs_csum_info, data) + sectors * 4);
        if (payload) {
            uint32_t slots[2];
            memcpy(slots, out->data, sizeof(slots));
            CHECK(slots[0] == expected_slots[0]);
            if (sectors > 1) CHECK(slots[1] == expected_slots[1]);
        }
        for (unsigned i = returned; i < length; i++) CHECK(storage.bytes[i] == 0xa5);
    } else CHECK(returned == 0);
    for (unsigned i = length; i < sizeof(storage.bytes); i++) CHECK(storage.bytes[i] == 0xa5);
    return 0;
}

static int ranges_case(const char *name) {
    fcb node = {.type = 1, .atts = FILE_ATTRIBUTE_SPARSE_FILE, .inode_item = {.size = 32768}};
    FILE_OBJECT file = {&node, NULL};
    extent ext[4] = {
        {.offset = 0, .extent_data = {.type = 1, .num_bytes = 4096, .disk_bytenr = 4096}},
        {.offset = 16384, .extent_data = {.type = 1, .num_bytes = 4096, .disk_bytenr = 8192}}
    };
    unsigned count = 2;
    FILE_ALLOCATED_RANGE_BUFFER in = {{0}, {32768}};
    FILE_ALLOCATED_RANGE_BUFFER expected[2] = {{{0}, {4096}}, {{16384}, {4096}}};
    unsigned expected_count = 2;
    NTSTATUS expected_status = STATUS_SUCCESS;
    ULONG capacity = sizeof(expected);
    if (!strcmp(name, "ranges-hole")) {
        in = (FILE_ALLOCATED_RANGE_BUFFER){{8192}, {4096}};
        expected_count = 0;
    } else if (!strcmp(name, "ranges-intersection")) {
        in = (FILE_ALLOCATED_RANGE_BUFFER){{2048}, {16384}};
        expected[0] = (FILE_ALLOCATED_RANGE_BUFFER){{2048}, {2048}};
        expected[1] = (FILE_ALLOCATED_RANGE_BUFFER){{16384}, {2048}};
    } else if (!strcmp(name, "ranges-ordinary")) {
        node.atts = 0;
        in = (FILE_ALLOCATED_RANGE_BUFFER){{4096}, {4096}};
        expected[0] = in;
        expected_count = 1;
    } else if (!strcmp(name, "ranges-eof")) {
        in = (FILE_ALLOCATED_RANGE_BUFFER){{32768}, {4096}};
        expected_count = 0;
    } else if (!strcmp(name, "ranges-zero")) {
        in.Length.QuadPart = 0;
        expected_count = 0;
    } else if (!strcmp(name, "ranges-negative")) {
        in.FileOffset.QuadPart = -1;
        expected_status = STATUS_INVALID_PARAMETER;
        expected_count = 0;
    } else if (!strcmp(name, "ranges-overflow")) {
        in.FileOffset.QuadPart = INT64_MAX;
        in.Length.QuadPart = 1;
        expected_status = STATUS_INVALID_PARAMETER;
        expected_count = 0;
    } else if (!strcmp(name, "ranges-negative-length")) {
        in.Length.QuadPart = -1;
        expected_status = STATUS_INVALID_PARAMETER;
        expected_count = 0;
    } else if (!strcmp(name, "ranges-page")) {
        capacity = sizeof(expected[0]);
        expected_status = STATUS_BUFFER_OVERFLOW;
        expected_count = 1;
    } else if (!strcmp(name, "ranges-next-page")) {
        in = (FILE_ALLOCATED_RANGE_BUFFER){{4096}, {28672}};
        expected[0] = expected[1];
        expected_count = 1;
    } else if (!strcmp(name, "ranges-explicit-hole")) {
        ext[2] = ext[1];
        ext[1] = (extent){.offset = 4096, .extent_data = {.type = 1, .num_bytes = 12288}};
        count = 3;
    } else if (!strcmp(name, "ranges-prealloc")) {
        ext[1].extent_data.type = BTRFS_FILE_EXTENT_PREALLOC;
    } else if (!strcmp(name, "ranges-ignored")) {
        ext[0].ignore = true;
        expected[0] = expected[1];
        expected_count = 1;
    } else if (!strcmp(name, "ranges-clip-eof")) {
        node.inode_item.size = 18432;
        expected[1].Length.QuadPart = 2048;
    } else if (!strcmp(name, "ranges-inline")) {
        ext[0].extent_data.type = BTRFS_FILE_EXTENT_INLINE;
        ext[0].extent_data.ram_bytes = 10;
        ext[0].extent_data.disk_bytenr = 0;
        count = 1;
        node.inode_item.size = 10;
        expected_count = 1;
        expected[0].Length.QuadPart = 10;
    } else if (!strcmp(name, "ranges-adjacent")) {
        ext[1].offset = 4096;
        expected_count = 1;
        expected[0].Length.QuadPart = 8192;
    } else if (!strcmp(name, "ranges-prealloc-after-eof")) {
        ext[1].offset = 36864;
        ext[1].extent_data.type = BTRFS_FILE_EXTENT_PREALLOC;
        expected_count = 1;
    } else if (!strcmp(name, "ranges-beyond-eof")) {
        in.FileOffset.QuadPart = 36864;
        expected_count = 0;
    } else if (!strcmp(name, "ranges-large-extent-length")) {
        ext[0].offset = 4096;
        ext[0].extent_data.num_bytes = UINT64_MAX;
        count = 1;
        in = (FILE_ALLOCATED_RANGE_BUFFER){{8192}, {8192}};
        expected[0] = in;
        expected_count = 1;
    } else if (!strcmp(name, "ranges-empty-extent")) {
        ext[0].extent_data.num_bytes = 0;
        count = 1;
        expected_count = 0;
    } else if (!strcmp(name, "ranges-zero-capacity") || !strcmp(name, "ranges-short-capacity")) {
        capacity = !strcmp(name, "ranges-zero-capacity") ? 0 : sizeof(expected[0]) - 1;
        expected_status = STATUS_BUFFER_TOO_SMALL;
        expected_count = 0;
    } else if (!strcmp(name, "ranges-before-request")) {
        in = (FILE_ALLOCATED_RANGE_BUFFER){{4096}, {4096}};
        expected_count = 0;
    } else if (!strcmp(name, "ranges-at-request-end")) {
        in.Length.QuadPart = 16384;
        expected_count = 1;
    }
    link_extents(&node, ext, count);
    struct { FILE_ALLOCATED_RANGE_BUFFER ranges[2]; unsigned char guard[16]; } out;
    memset(&out, 0xa5, sizeof(out));
    ULONG_PTR returned = 0;
    NTSTATUS status = query_ranges(&file, &in, sizeof(in), out.ranges, capacity, &returned);
    CHECK(acquired == released);
    CHECK(status == expected_status);
    CHECK(returned == expected_count * sizeof(expected[0]));
    for (unsigned i = 0; i < expected_count; i++) {
        CHECK(out.ranges[i].FileOffset.QuadPart == expected[i].FileOffset.QuadPart);
        CHECK(out.ranges[i].Length.QuadPart == expected[i].Length.QuadPart);
    }
    for (unsigned i = capacity; i < sizeof(out); i++) CHECK(((unsigned char *)&out)[i] == 0xa5);
    return 0;
}

int main(int argc, char **argv) {
    const char *cases[] = {
        "checksum-plain", "checksum-mixed", "checksum-inline", "checksum-directory",
        "checksum-ads", "checksum-nodatasum", "ranges-all", "ranges-hole",
        "checksum-sparse", "checksum-pool-failure", "checksum-prealloc-eof", "checksum-extent-after-eof",
        "checksum-trailing-hole", "checksum-trailing-pool-failure", "checksum-sparse-only",
        "ranges-intersection", "ranges-ordinary", "ranges-eof", "ranges-zero",
        "ranges-negative", "ranges-overflow", "ranges-page", "ranges-next-page",
        "ranges-explicit-hole", "ranges-prealloc", "ranges-ignored", "ranges-clip-eof",
        "ranges-negative-length", "ranges-inline", "ranges-adjacent", "ranges-prealloc-after-eof", "ranges-beyond-eof",
        "ranges-large-extent-length", "ranges-empty-extent", "ranges-zero-capacity", "ranges-short-capacity",
        "ranges-before-request", "ranges-at-request-end"
    };
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) puts(cases[i]);
        return 0;
    }
    if (argc != 2) return 2;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(argv[1], cases[i])) continue;
        return !strncmp(argv[1], "checksum-", 9) ? checksum_case(argv[1]) : ranges_case(argv[1]);
    }
    return 2;
}
