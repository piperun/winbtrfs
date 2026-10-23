/* Optional private guest fixtures are generated outside the public test source. */
#include "host.h"
#include "search.inc"

static void replay(unsigned slot, const uint64_t keys[][4], size_t count, uint32_t live,
                   struct btrfs_key query, bool ignore, NTSTATUS expected_status,
                   uint64_t expected_index, uint64_t expected_comparisons) {
    device_extension vcb = {0};
    tree leaf;
    tree_data* items = calloc(count ? count : 1, sizeof(*items));
    CHECK(items);
    prepare_tree(&leaf, items, (unsigned)count, 0);
    leaf.header.nritems = live;
    for (size_t i = 0; i < count; i++) {
        items[i].key = (struct btrfs_key){keys[i][0], (uint8_t)keys[i][1], keys[i][2]};
        items[i].ignore = keys[i][3] != 0;
    }
    traverse_ptr result = {0};
    comparisons = 0;
    NTSTATUS status = find_item_in_tree(&vcb, &leaf, &result, &query, ignore, 0, NULL);
    CHECK(status == expected_status);
    if (NT_SUCCESS(status)) CHECK(result.tree == &leaf && result.item == &items[expected_index]);
    CHECK(comparisons == expected_comparisons);
    printf("{\"slot\":%u,\"physical_items\":%zu,\"comparisons\":%llu,\"expected_status_and_identity\":true}\n",
           slot, count, (unsigned long long)comparisons);
    free(items);
}
#include "captured-fixtures.inc"
