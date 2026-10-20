/* Pure interval arithmetic; no filesystem state, allocation or Windows APIs. */
#include "range-math.h"

uint32_t BTRFS_RANGE_CDECL btrfs_clip_byte_range(uint64_t query_start, uint64_t query_end,
                                               uint64_t extent_offset, uint64_t extent_length,
                                               btrfs_byte_range* out) {
    btrfs_byte_range result = {0, 0};
    uint64_t start, end, available;

    if (query_start >= query_end || extent_offset >= query_end) {
        *out = result;
        return 0;
    }

    available = query_end - extent_offset;
    start = extent_offset < query_start ? query_start : extent_offset;
    end = extent_offset + (extent_length < available ? extent_length : available);
    if (start >= end) {
        *out = result;
        return 0;
    }

    result.start = start;
    result.end = end;
    *out = result;
    return 1;
}
