/* Execute real scrub geometry, completion, verification and repair code. */
#include "host.h"
#include "types.inc"
#include "mapping.inc"
#include "services.inc"
#include "completion.inc"
#include "raid10.inc"
#include "caller.inc"

enum repair_mode { HEALTHY, WHOLE_MIRROR, RECONSTRUCT };
static const uint64_t stripe_size = 0x10000;

static void fill_image(uint8_t* data, unsigned group, unsigned groups) {
    unsigned unit = metadata ? volume.superblock.nodesize : volume.superblock.sectorsize;
    for (unsigned p = 0; p < DISK_BYTES; p += unit) {
        uint64_t logical = ((p / stripe_size) * groups + group) * stripe_size + p % stripe_size;
        for (unsigned b = 0; b < unit; b++) data[p + b] = (uint8_t)((logical >> 12) * 37 + b * 13 + (b >> 8));
        if (metadata) {
            struct btrfs_header* header = (struct btrfs_header*)(data + p);
            memset(header->csum, 0, sizeof(header->csum));
            header->bytenr = current_chunk.offset + logical;
            get_tree_checksum(&volume, header, header->csum);
        }
    }
}

static void exercise(unsigned groups, unsigned mirrors, uint64_t relative, uint32_t size,
                     bool is_metadata, enum repair_mode mode, uint32_t readonly_mask, int missing) {
    metadata = is_metadata;
    unsigned count = groups * mirrors;
    CHECK(count <= MAX_DEVICES);
    volume = (device_extension){ .superblock = { .sectorsize = 4096, .nodesize = 16384 },
                                 .sector_shift = 12, .csum_size = 4 };
    current_chunk = (chunk){ .offset = 0x123400000ULL, .devices = device_ptrs };
    current_chunk.chunk_item = calloc(1, sizeof(struct btrfs_chunk) + (count - 1) * sizeof(struct btrfs_stripe));
    CHECK(current_chunk.chunk_item);
    current_chunk.chunk_item->num_stripes = count;
    current_chunk.chunk_item->sub_stripes = mirrors;
    current_chunk.chunk_item->stripe_len = stripe_size;
    current_chunk.chunk_item->length = groups * DISK_BYTES;
    request_offset = current_chunk.offset + relative; request_size = size;

    /* Enumerate logical sectors independently of the production geometry. */
    uint32_t group_lengths[MAX_DEVICES] = {0};
    uint64_t group_starts[MAX_DEVICES];
    for (unsigned g = 0; g < groups; g++) group_starts[g] = UINT64_MAX;
    for (uint64_t off = relative; off < relative + size; off += 4096) {
        unsigned group = (off / stripe_size) % groups;
        uint64_t physical = (off / (stripe_size * groups)) * stripe_size + off % stripe_size;
        if (!group_lengths[group]) group_starts[group] = physical;
        CHECK(physical == group_starts[group] + group_lengths[group]);
        group_lengths[group] += 4096;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned group = i / mirrors;
        disk[i] = malloc(DISK_BYTES); expected[i] = malloc(DISK_BYTES); CHECK(disk[i] && expected[i]);
        if (i % mirrors == 0) fill_image(disk[i], group, groups);
        else memcpy(disk[i], disk[i - 1], DISK_BYTES);
        memcpy(expected[i], disk[i], DISK_BYTES);
        starts[i] = group_lengths[group] ? group_starts[group] : 0;
        lengths[i] = group_lengths[group];
        bases[i] = 0x800000000ULL + i * 0x10000000ULL;
        current_chunk.chunk_item->stripe[i].offset = bases[i];
        objects[i] = (DEVICE_OBJECT){ .id = i, .StackSize = i + 1 };
        devices[i] = (device){ .devobj = (int)i == missing ? NULL : &objects[i],
            .fileobj = &objects[i], .readonly = (readonly_mask & (1u << i)) != 0, .devitem = { .devid = 100 + i } };
        device_ptrs[i] = &devices[i];
    }

    void* csums = NULL;
    if (!metadata) {
        csums = malloc((size / 4096) * volume.csum_size); CHECK(csums);
        for (unsigned pos = 0; pos < size; pos += 4096) {
            uint64_t off = relative + pos;
            unsigned group = (off / stripe_size) % groups;
            unsigned physical = (off / (stripe_size * groups)) * stripe_size + off % stripe_size;
            uint32_t value = checksum(expected[group * mirrors] + physical, 4096);
            memcpy((uint8_t*)csums + (pos / 4096) * volume.csum_size, &value, sizeof(value));
        }
    }

    unsigned damaged = 0;
    unsigned unit = metadata ? volume.superblock.nodesize : volume.superblock.sectorsize;
    for (unsigned i = 0; i < count; i++) {
        if (!lengths[i] || !devices[i].devobj) continue;
        bool corrupt = mode == WHOLE_MIRROR ? i % mirrors == (relative / stripe_size) % mirrors :
                       mode == RECONSTRUCT && lengths[i] >= 2 * unit;
        if (!corrupt) continue;
        unsigned position = starts[i] + (mode == RECONSTRUCT && i % 2 ? lengths[i] - unit : 0) + unit / 2;
        CHECK(position < DISK_BYTES); disk[i][position] ^= 0x5a; damaged++;
        if (devices[i].readonly) expected[i][position] ^= 0x5a;
    }
    if (fail_write || fail_read >= 0 || fail_scratch || fail_context)
        for (unsigned i = 0; i < count; i++) memcpy(expected[i], disk[i], DISK_BYTES);
    NTSTATUS wanted = fail_write || fail_read >= 0 ? STATUS_IO_DEVICE_ERROR :
                      fail_scratch || fail_context ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
    NTSTATUS status = scrub_extent(&volume, &current_chunk, BTRFS_BLOCK_GROUP_RAID10, request_offset, size, csums);
    CHECK(status == wanted && pool_live == 0 && irp_live == 0 && pending == 0 && completed == read_count);
    CHECK(allocation_failed == (fail_context || fail_scratch));
    uint64_t read_bytes = 0; unsigned expected_reads = 0;
    for (unsigned i = 0; i < count; i++) {
        if (devices[i].devobj && lengths[i] && !fail_context) { read_bytes += lengths[i]; expected_reads++; }
        if (memcmp(disk[i], expected[i], DISK_BYTES) != 0) {
            bool outside_changed = memcmp(disk[i], expected[i], starts[i]) != 0 ||
                memcmp(disk[i] + starts[i] + lengths[i], expected[i] + starts[i] + lengths[i], DISK_BYTES - starts[i] - lengths[i]) != 0;
            fprintf(stderr, "device %u: repair=%llx, read=%llx, size=%x; changed outside read range=%u\n", i,
                (unsigned long long)write_addresses[i], (unsigned long long)(bases[i] + starts[i]), lengths[i], outside_changed);
            CHECK(false);
        }
        if (written[i]) CHECK(write_addresses[i] == bases[i] + starts[i]);
        free(disk[i]); free(expected[i]);
    }
    CHECK(read_count == expected_reads && volume.scrub.data_scrubbed == read_bytes);
    if (!damaged || fail_read >= 0 || fail_scratch || fail_context) CHECK(writes == 0);
    if (damaged && wanted == STATUS_SUCCESS) CHECK(recovery_logs > 0);
    if (fail_write) CHECK(writes == 1 && device_errors[BTRFS_DEV_STAT_WRITE_ERRS] == 1);
    if (fail_read >= 0) CHECK(device_errors[BTRFS_DEV_STAT_READ_ERRS] == 1);
    free(csums); free(current_chunk.chunk_item);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    bool list = strcmp(argv[1], "--list") == 0;
    const unsigned layouts[][2] = { {1,2}, {2,2}, {3,2}, {4,2}, {2,3} };
    const uint64_t offsets[] = { 0, 0x10000, 0x30000, 0x44000 };
    const uint32_t sizes[] = { 0x8000, 0x28000 };
    for (unsigned layout = 0; layout < 5; layout++) for (unsigned pos = 0; pos < 4; pos++)
        for (unsigned length = 0; length < 2; length++) for (unsigned meta = 0; meta < 2; meta++)
            for (unsigned reconstruct = 0; reconstruct < 2; reconstruct++) {
                char name[96]; snprintf(name, sizeof(name), "raid10-%ux%u-off%llx-size%x-%s-%s", layouts[layout][0], layouts[layout][1],
                    (unsigned long long)offsets[pos], sizes[length], meta ? "metadata" : "data", reconstruct ? "reconstruct" : "mirror");
                if (list) { puts(name); continue; }
                if (strcmp(argv[1], name) == 0) {
                    timing = pos % 2;
                    exercise(layouts[layout][0], layouts[layout][1], offsets[pos], sizes[length], meta,
                        reconstruct ? RECONSTRUCT : WHOLE_MIRROR, 0, -1); return 0;
                }
            }
    const char* controls[] = { "healthy", "readonly-mirror", "all-readonly", "missing-mirror", "missing-reconstruct",
        "write-failure", "read-failure", "scratch-failure", "healthy-twelve", "reconstruct-twelve", "last-mirror", "context-failure" };
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (list) { puts(controls[i]); continue; }
        if (strcmp(argv[1], controls[i]) != 0) continue;
        timing = 1;
        if (i == 5) fail_write = true;
        if (i == 6) fail_read = 2;
        if (i == 7) fail_scratch = true;
        if (i == 11) fail_context = true;
        if (i == 4) exercise(2, 3, 0x20000, 0x8000, false, RECONSTRUCT, 0, 2);
        else if (i == 8 || i == 9) exercise(4, 3, 0x34000, 0x28000, true, i == 8 ? HEALTHY : RECONSTRUCT, 0, -1);
        else if (i == 10) exercise(2, 3, 0x20000, 0x28000, false, WHOLE_MIRROR, 0, -1);
        else exercise(2, 2, 0x30000, 0x8000, false, i == 0 || i == 3 ? HEALTHY : i == 2 ? RECONSTRUCT : WHOLE_MIRROR,
            i == 1 ? 1u << 3 : i == 2 ? 15 : 0, i == 3 ? 3 : -1);
        return 0;
    }
    CHECK(list); return 0;
}
