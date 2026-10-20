/* Compare both production implementations on every call made by the fixture. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "range-math.h"

uint32_t BTRFS_RANGE_CDECL btrfs_clip_byte_range_c(uint64_t query_start, uint64_t query_end,
                                                 uint64_t extent_offset, uint64_t extent_length,
                                                 btrfs_byte_range* out);

uint32_t BTRFS_RANGE_CDECL btrfs_compare_clip_byte_range(uint64_t query_start, uint64_t query_end,
                                                       uint64_t extent_offset, uint64_t extent_length,
                                                       btrfs_byte_range* out) {
    struct guarded_range {
        uint64_t before;
        btrfs_byte_range range;
        uint64_t after;
    } c = {UINT64_C(0x1122334455667788), {UINT64_MAX, UINT64_MAX}, UINT64_C(0x8877665544332211)},
      rust = {UINT64_C(0x1122334455667788), {UINT64_MAX, UINT64_MAX}, UINT64_C(0x8877665544332211)};
    uint32_t c_present = btrfs_clip_byte_range_c(query_start, query_end, extent_offset, extent_length, &c.range);
    uint32_t rust_present = btrfs_clip_byte_range(query_start, query_end, extent_offset, extent_length, &rust.range);

    if (c_present != rust_present || c.range.start != rust.range.start || c.range.end != rust.range.end ||
        c.before != UINT64_C(0x1122334455667788) || c.after != UINT64_C(0x8877665544332211) ||
        rust.before != UINT64_C(0x1122334455667788) || rust.after != UINT64_C(0x8877665544332211)) {
        fprintf(stderr, "C/Rust mismatch query=%" PRIu64 ":%" PRIu64 " extent=%" PRIu64 ":%" PRIu64 "\n",
                query_start, query_end, extent_offset, extent_length);
        exit(1);
    }
    *out = rust.range;
    return rust_present;
}
