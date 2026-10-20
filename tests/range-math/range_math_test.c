/* Portable range contract checked against independent widened arithmetic. */
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "range-math.h"

_Static_assert(sizeof(btrfs_byte_range) == 16, "range ABI size");
_Static_assert(offsetof(btrfs_byte_range, start) == 0, "range ABI start");
_Static_assert(offsetof(btrfs_byte_range, end) == 8, "range ABI end");
_Static_assert(_Alignof(btrfs_byte_range) == _Alignof(uint64_t), "range ABI alignment");

static unsigned cases, properties, failures;

static btrfs_byte_range oracle(uint64_t query_start, uint64_t query_end,
                               uint64_t extent_offset, uint64_t extent_length) {
    btrfs_byte_range result = {0, 0};
    __uint128_t start = query_start > extent_offset ? query_start : extent_offset;
    __uint128_t end = (__uint128_t)extent_offset + extent_length;

    if (end > query_end)
        end = query_end;
    if (query_start < query_end && start < end) {
        result.start = (uint64_t)start;
        result.end = (uint64_t)end;
    }
    return result;
}

static void report(bool ok, const char* kind, uint64_t query_start, uint64_t query_end,
                    uint64_t extent_offset, uint64_t extent_length) {
    if (ok)
        return;
    failures++;
    if (failures <= 10)
        printf("FAIL %s query=%" PRIu64 ":%" PRIu64 " extent=%" PRIu64 ":%" PRIu64 "\n",
               kind, query_start, query_end, extent_offset, extent_length);
}

static void check_case(uint64_t query_start, uint64_t query_end,
                        uint64_t extent_offset, uint64_t extent_length) {
    struct {
        uint64_t before;
        btrfs_byte_range range;
        uint64_t after;
    } output = {UINT64_C(0x1122334455667788), {UINT64_MAX, UINT64_MAX}, UINT64_C(0x8877665544332211)};
    btrfs_byte_range expected = oracle(query_start, query_end, extent_offset, extent_length);
    uint32_t present = btrfs_clip_byte_range(query_start, query_end, extent_offset, extent_length, &output.range);
    cases++;
    report(present == (uint32_t)(expected.start < expected.end) &&
           output.range.start == expected.start && output.range.end == expected.end &&
           output.before == UINT64_C(0x1122334455667788) && output.after == UINT64_C(0x8877665544332211),
           "oracle and output guards", query_start, query_end, extent_offset, extent_length);

    if (present == 1 && output.range.start < output.range.end) {
        btrfs_byte_range repeated;
        uint32_t repeated_present = btrfs_clip_byte_range(query_start, query_end, output.range.start,
                                                          output.range.end - output.range.start, &repeated);
        properties++;
        report(repeated_present == present && repeated.start == output.range.start && repeated.end == output.range.end,
               "idempotence", query_start, query_end, extent_offset, extent_length);
    }

    if (query_start < query_end) {
        uint64_t inset = (query_end - query_start) / 3;
        uint64_t nested_start = query_start + inset, nested_end = query_end - inset;
        btrfs_byte_range direct, nested;
        uint32_t direct_present = btrfs_clip_byte_range(nested_start, nested_end, extent_offset, extent_length, &direct);
        uint32_t nested_present = btrfs_clip_byte_range(nested_start, nested_end, expected.start,
                                                        expected.end - expected.start, &nested);
        properties++;
        report(direct_present == nested_present && direct.start == nested.start && direct.end == nested.end,
               "nested clipping", query_start, query_end, extent_offset, extent_length);
    }
}

int main(void) {
    static const struct {
        uint64_t length;
        uint64_t start;
        uint64_t end;
    } near_limit[] = {
        {49, 0, 0}, {50, 0, 0},
        {51, UINT64_MAX - 50, UINT64_MAX - 49},
        {99, UINT64_MAX - 50, UINT64_MAX - 1},
        {100, UINT64_MAX - 50, UINT64_MAX},
        {101, UINT64_MAX - 50, UINT64_MAX},
        {UINT64_MAX, UINT64_MAX - 50, UINT64_MAX},
    };
    for (unsigned i = 0; i < sizeof(near_limit) / sizeof(near_limit[0]); i++) {
        btrfs_byte_range expected = oracle(UINT64_MAX - 50, UINT64_MAX,
                                           UINT64_MAX - 100, near_limit[i].length);
        report(expected.start == near_limit[i].start && expected.end == near_limit[i].end,
               "literal near-limit threshold", UINT64_MAX - 50, UINT64_MAX,
               UINT64_MAX - 100, near_limit[i].length);
        check_case(UINT64_MAX - 50, UINT64_MAX, UINT64_MAX - 100, near_limit[i].length);
    }
    static const uint64_t edges[] = {
        0, 1, 2, 3, UINT32_MAX - UINT64_C(1), UINT32_MAX, UINT32_MAX + UINT64_C(1),
        INT64_MAX - UINT64_C(1), INT64_MAX, INT64_MAX + UINT64_C(1), UINT64_MAX - 2, UINT64_MAX - 1, UINT64_MAX
    };
    for (uint64_t query_start = 0; query_start < 16; query_start++)
        for (uint64_t query_end = 0; query_end < 16; query_end++)
            for (uint64_t offset = 0; offset < 16; offset++)
                for (uint64_t length = 0; length < 16; length++)
                    check_case(query_start, query_end, offset, length);

    const unsigned edge_count = sizeof(edges) / sizeof(edges[0]);
    for (unsigned query_start = 0; query_start < edge_count; query_start++)
        for (unsigned query_end = 0; query_end < edge_count; query_end++)
            for (unsigned offset = 0; offset < edge_count; offset++)
                for (unsigned length = 0; length < edge_count; length++)
                    check_case(edges[query_start], edges[query_end], edges[offset], edges[length]);

    printf("%u range cases and %u property checks, %u failures\n", cases, properties, failures);
    return failures != 0;
}
