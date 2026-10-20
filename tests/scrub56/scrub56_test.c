/* Production RAID56 batch and completion code with deterministic service models. */
#include "host.h"
#include "types.inc"
#include "services.inc"
#include "completion.inc"
#include "batch.inc"

static void exercise(scenario s) {
    cfg = s; count = cfg.raid6 ? 4 : 3;
    volume = (device_extension){ .superblock = { .sectorsize = 4096, .nodesize = 16384 },
        .sector_shift = 12, .csum_size = 4, .extent_root = &volume };
    current_chunk = (chunk){ .offset = 0x123400000ULL, .devices = device_ptrs };
    current_chunk.chunk_item = calloc(1, sizeof(struct btrfs_chunk) + (count - 1) * sizeof(struct btrfs_stripe));
    CHECK(current_chunk.chunk_item);
    current_chunk.chunk_item->num_stripes = count;
    current_chunk.chunk_item->stripe_len = STRIPE_BYTES;
    current_chunk.chunk_item->type = (cfg.metadata ? BTRFS_BLOCK_GROUP_METADATA : BTRFS_BLOCK_GROUP_DATA) |
        (cfg.raid6 ? BTRFS_BLOCK_GROUP_RAID6 : BTRFS_BLOCK_GROUP_RAID5);
    disk_size = (first_row + cfg.rows) * STRIPE_BYTES;
    for (unsigned id = 0; id < count; id++) {
        disk[id] = malloc(disk_size); expected[id] = malloc(disk_size); CHECK(disk[id] && expected[id]);
        for (unsigned p = 0; p < disk_size; p++) disk[id][p] = (uint8_t)(id * 41 + (p >> 12) * 13 + p);
        memcpy(expected[id], disk[id], disk_size);
        objects[id] = (DEVICE_OBJECT){ id, id + 1, cfg.direct ? DO_DIRECT_IO : 0 };
        devices[id] = (device){ .devobj = cfg.missing & (1u << id) ? NULL : &objects[id],
            .fileobj = &objects[id], .devitem = { .devid = 100 + id } };
        device_ptrs[id] = &devices[id];
        current_chunk.chunk_item->stripe[id].offset = 0x800000000ULL + id * 0x10000000ULL;
    }
    NTSTATUS wanted = cfg.fault == PAGE_LOCK ? STATUS_ACCESS_VIOLATION :
        cfg.fault == READ_ERROR || cfg.fault == WRITE_ERROR ? STATUS_IO_DEVICE_ERROR :
        cfg.fault == NO_FAULT ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = scrub_chunk_raid56_stripe_run(&volume, &current_chunk, first_row, first_row + cfg.rows - 1);
    CHECK(status == wanted);
    for (unsigned i = 0; i < 64; i++) if (pools[i].p)
        fprintf(stderr, "retained pool allocation: %zu bytes\n", pools[i].size);
    CHECK(pool_live == 0 && mdl_live == 0 && pending == 0 && range_locks == 0 && reads == completions);
    CHECK(fault_hits == (cfg.fault == NO_FAULT ? 0u : 1u));
    CHECK(read_errors == (cfg.fault == READ_ERROR ? 1u : 0u));
    CHECK(write_errors == (cfg.fault == WRITE_ERROR ? 1u : 0u));
    if (cfg.fault == POOL_ALLOC) CHECK(allocation_calls == cfg.fail_allocation && reads == 0);
    unsigned expected_rows = cfg.fault == NO_FAULT ? cfg.rows : cfg.fault == WRITE_ERROR ?
        min(cfg.rows, (cfg.fail_batch + 1) * BATCH_ROWS) : cfg.fail_batch * BATCH_ROWS;
    CHECK(verified_rows == expected_rows);
    for (unsigned id = 0; id < count; id++) {
        CHECK(!irps[id] && memcmp(disk[id], expected[id], disk_size) == 0);
        for (unsigned batch = 0; batch < MAX_BATCHES; batch++) {
            unsigned attempts = cfg.fault == WRITE_ERROR && batch == cfg.fail_batch && id > cfg.fail_device ?
                0 : wanted_writes[batch][id];
            CHECK(writes[batch][id] == attempts);
        }
        free(disk[id]); free(expected[id]);
    }
    free(current_chunk.chunk_item);
}

static bool select_case(const char* wanted, const char* name, scenario s) {
    if (strcmp(wanted, "--list") == 0) { puts(name); return false; }
    if (strcmp(wanted, name) != 0) return false;
    exercise(s); return true;
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const unsigned lengths[] = { 2, 18, 35, 1, 15, 16, 17, 31, 32, 33 };
    for (unsigned raid6 = 0; raid6 < 2; raid6++) {
        unsigned n = raid6 ? 4 : 3;
        for (unsigned direct = 0; direct < 2; direct++) for (unsigned timing = 0; timing < 2; timing++) {
            scenario base = { .raid6 = raid6, .direct = direct, .deferred = timing };
            char name[120];
            for (unsigned len = 0; len < sizeof(lengths) / sizeof(lengths[0]); len++) {
                base.rows = lengths[len];
                snprintf(name, sizeof(name), "raid%u-%s-timing%u-healthy-%u", raid6 ? 6 : 5, direct ? "direct" : "neither", timing, base.rows);
                if (select_case(argv[1], name, base)) return 0;
            }
            for (unsigned rows = 17; rows <= 33; rows += BATCH_ROWS) {
                scenario s = base;
                s.rows = rows; s.repair_batches = 1u << ((rows - 1) / BATCH_ROWS);
                s.repair_devices = 1u | (1u << (n - 1));
                snprintf(name, sizeof(name), "raid%u-%s-timing%u-final-row-repair-%u", raid6 ? 6 : 5,
                    direct ? "direct" : "neither", timing, rows);
                if (select_case(argv[1], name, s)) return 0;
            }
            base.rows = 35;
            const unsigned repairs[] = { 1, 2, 5, 7 };
            for (unsigned r = 0; r < 4; r++) {
                base.repair_batches = repairs[r]; base.repair_devices = 1u | (1u << (n - 1));
                snprintf(name, sizeof(name), "raid%u-%s-timing%u-repair-batches%x", raid6 ? 6 : 5, direct ? "direct" : "neither", timing, repairs[r]);
                if (select_case(argv[1], name, base)) return 0;
            }
            base.missing = 1u << 1; base.repair_batches = 1;
            snprintf(name, sizeof(name), "raid%u-%s-timing%u-missing", raid6 ? 6 : 5, direct ? "direct" : "neither", timing);
            if (select_case(argv[1], name, base)) return 0;
            for (unsigned batch = 0; batch < 2; batch++) for (unsigned id = 0; id < n; id++) {
                scenario s = { .raid6 = raid6, .direct = direct, .deferred = timing, .rows = 18,
                    .fault = WRITE_ERROR, .fail_batch = batch, .fail_device = id,
                    .repair_batches = (1u << (batch + 1)) - 1, .repair_devices = (1u << n) - 1 };
                snprintf(name, sizeof(name), "raid%u-%s-timing%u-write-failure-batch%u-device%u", raid6 ? 6 : 5,
                    direct ? "direct" : "neither", timing, batch, id);
                if (select_case(argv[1], name, s)) return 0;
            }
        }
        for (unsigned metadata = 0; metadata < 2; metadata++) {
            unsigned allocations = 4 + 2 * n + (raid6 ? 1 : 0) + (metadata ? 0 : 2);
            for (unsigned allocation = 1; allocation <= allocations; allocation++) {
                scenario s = { .raid6 = raid6, .direct = true, .metadata = metadata, .rows = 18,
                    .fault = POOL_ALLOC, .fail_allocation = allocation };
                char name[120];
                snprintf(name, sizeof(name), "raid%u-%s-pool-failure-%u", raid6 ? 6 : 5,
                    metadata ? "metadata" : "data", allocation);
                if (select_case(argv[1], name, s)) return 0;
            }
        }
        for (enum fault fault = PAGE_LOCK; fault <= READ_ERROR; fault++) {
            for (unsigned batch = 0; batch < 2; batch++) for (unsigned id = 0; id < n; id++) {
                scenario s = { .raid6 = raid6, .direct = true, .deferred = id % 2, .rows = 18,
                    .fault = fault, .fail_batch = batch, .fail_device = id,
                    .repair_batches = batch ? 1 : 0, .repair_devices = 1u | (1u << (n - 1)) };
                char name[120];
                snprintf(name, sizeof(name), "raid%u-fault%u-batch%u-device%u", raid6 ? 6 : 5, fault, batch, id);
                if (select_case(argv[1], name, s)) return 0;
            }
        }
    }
    CHECK(strcmp(argv[1], "--list") == 0); return 0;
}
