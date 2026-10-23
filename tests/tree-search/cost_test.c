/* Deterministic operation counts include one array build and its release. */
#include "host.h"
#include "search.inc"

int main(void) {
    const unsigned sizes[] = {16,32,128,489};
    device_extension vcb = {0};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        unsigned count = sizes[s];
        tree node;
        tree_data* items = calloc(count, sizeof(*items));
        CHECK(items);
        prepare_tree(&node, items, count, 1);
        comparisons = 0;
        uint64_t old_bytes = allocation_bytes;
        unsigned old_allocations = allocations, old_releases = releases;
        build_index(&node);
        uint64_t construction = comparisons;
        comparisons = 0;
        for (unsigned q = 0; q < 10000; q++) {
            unsigned index = (q * 97u + 13u) % count;
            struct btrfs_key key = {42,108,index * 16 + (q & 1)};
            traverse_ptr result = {0};
            CHECK(find_item_in_tree(&vcb, &node, &result, &key, false, 1, NULL) == STATUS_SUCCESS);
            CHECK(result.item == &items[index] && result.tree == &node);
        }
        uint64_t queries = comparisons;
        release_index(&node);
        printf("{\"items\":%u,\"queries\":10000,\"query_comparisons\":%llu,\"construction_comparisons\":%llu,\"allocated_bytes\":%llu,\"allocations\":%u,\"releases\":%u}\n",
               count, (unsigned long long)queries, (unsigned long long)construction,
               (unsigned long long)(allocation_bytes - old_bytes), allocations - old_allocations, releases - old_releases);
        CHECK(allocations == releases);
        free(items);
    }
}
