/* Returning to a linear interior scan must fail the measured complexity bound. */
#include "host.h"
#include "search.inc"

static void interior_cost(bool enforce_bound) {
    tree node;
    tree_data items[489];
    device_extension vcb = {0};
    prepare_tree(&node, items, 489, 1);
    build_index(&node);
    struct btrfs_key query = {42, 108, 244 * 16};
    traverse_ptr result = {0};
    comparisons = 0;
    CHECK(find_item_in_tree(&vcb, &node, &result, &query, false, 1, NULL) == STATUS_SUCCESS);
    CHECK(result.tree == &node && result.item == &items[244]);
    printf("{\"items\":489,\"query\":\"interior\",\"comparisons\":%llu}\n", (unsigned long long)comparisons);
    fflush(stdout);
    if (enforce_bound) CHECK(comparisons <= 11);
    release_index(&node);
}

static void boundaries(void) {
    tree node;
    tree_data items[32];
    device_extension vcb = {0};
    prepare_tree(&node, items, 32, 1);
    build_index(&node);
    const struct { uint64_t objectid, offset; unsigned index; } cases[] = {
        {41, 0, 0}, {42, 0, 0}, {42, 7, 0}, {42, 16, 1},
        {42, 17, 1}, {42, 31 * 16, 31}, {42, 32 * 16, 31}, {43, 0, 31},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct btrfs_key query = {cases[i].objectid, 108, cases[i].offset};
        traverse_ptr result = {0};
        CHECK(find_item_in_tree(&vcb, &node, &result, &query, false, 1, NULL) == STATUS_SUCCESS);
        CHECK(result.tree == &node && result.item == &items[cases[i].index]);
    }
    release_index(&node);
}

int main(int argc, char** argv) {
    bool enforce_bound = argc == 1 || strcmp(argv[1], "baseline");
    interior_cost(enforce_bound);
    boundaries();
    CHECK(allocations == releases);
    puts("ordered search controls passed");
    return 0;
}
