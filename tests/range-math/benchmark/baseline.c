#include <stdint.h>

typedef struct {
    uint64_t start;
    uint64_t end;
} btrfs_byte_range;

/* Same C ABI and writable output as the helpers; no interval arithmetic. */
uint32_t btrfs_clip_byte_range_baseline(uint64_t query_start, uint64_t query_end,
                                        uint64_t extent_offset, uint64_t extent_length,
                                        btrfs_byte_range* out) {
    (void)extent_offset;
    (void)extent_length;
    out->start = query_start;
    out->end = query_end;
    return 1;
}
