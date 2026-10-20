/* Pure byte-range clipping shared by the allocated-range implementations. */
#pragma once

#include <stdint.h>

#if defined(_MSC_VER)
#define BTRFS_RANGE_CDECL __cdecl
#elif defined(__i386__) && (defined(__GNUC__) || defined(__clang__))
#define BTRFS_RANGE_CDECL __attribute__((cdecl))
#else
#define BTRFS_RANGE_CDECL
#endif

typedef struct {
    uint64_t start;
    uint64_t end;
} btrfs_byte_range;

/* Intersect half-open byte intervals without forming offset + length.
 * Empty or reversed queries return 0 and zero both endpoints; a nonempty
 * intersection returns 1. out must be non-null, aligned and writable. */
uint32_t BTRFS_RANGE_CDECL btrfs_clip_byte_range(uint64_t query_start, uint64_t query_end,
                                               uint64_t extent_offset, uint64_t extent_length,
                                               btrfs_byte_range* out);
