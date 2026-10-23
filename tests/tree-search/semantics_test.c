/* Search results, lazy loading and optional-cache failures use production bodies. */
#include "host.h"
#include "search.inc"

static traverse_ptr lookup(device_extension* vcb, tree* t, struct btrfs_key query,
                           bool ignore, uint8_t level, NTSTATUS expected) {
    traverse_ptr result = {0};
    CHECK(find_item_in_tree(vcb, t, &result, &query, ignore, level, NULL) == expected);
    return result;
}

static void duplicates(bool leaf) {
    tree node;
    tree_data items[32];
    device_extension vcb = {0};
    prepare_tree(&node, items, 32, leaf ? 0 : 1);
    for (unsigned i = 0; i < 32; i++) items[i].key.offset = (i / 3) * 16;
    items[3].ignore = items[4].ignore = true;
    build_index(&node);
    struct btrfs_key query = {42,108,16};
    traverse_ptr result = lookup(&vcb, &node, query, false, node.header.level, STATUS_SUCCESS);
    CHECK(result.item == &items[leaf ? 5 : 3]);
    result = lookup(&vcb, &node, query, true, node.header.level, STATUS_SUCCESS);
    CHECK(result.item == &items[3]);
    items[5].ignore = true;
    result = lookup(&vcb, &node, query, false, node.header.level, STATUS_SUCCESS);
    CHECK(result.item == &items[leaf ? 2 : 3]);
    query.offset = 17;
    result = lookup(&vcb, &node, query, false, node.header.level, STATUS_SUCCESS);
    CHECK(result.item == &items[leaf ? 2 : 5]);
    release_index(&node);
}

static void optional_failure(unsigned mode) {
    tree node;
    tree_data items[32];
    device_extension vcb = {0};
    prepare_tree(&node, items, 32, 1);
    if (mode == 0) fail_allocation = true;
    else if (mode == 1) {
        items[7].key.offset = 8 * 16;
        items[8].key.offset = 7 * 16;
    } else node.header.nritems += mode == 2 ? -1 : 1;
    build_index(&node);
    fail_allocation = false;
    struct btrfs_key query = {42,108, mode == 1 ? 7 * 16 : 31 * 16};
    traverse_ptr result = lookup(&vcb, &node, query, false, 1, STATUS_SUCCESS);
    CHECK(result.item == &items[mode == 1 ? 6 : 31]);
    CHECK(allocations == releases);
    release_index(&node);
}

static void child_state_and_requested_level(void) {
    tree node, empty, child;
    tree_data items[32], leaf[1];
    device_extension vcb = {0};
    prepare_tree(&node, items, 32, 1);
    prepare_tree(&empty, NULL, 0, 0);
    prepare_tree(&child, leaf, 1, 0);
    build_index(&node);
    struct btrfs_key query = {42,108,16 * 16};
    items[16].ignore = true;
    CHECK(lookup(&vcb, &node, query, false, 1, STATUS_SUCCESS).item == &items[16]);
    items[16].treeholder.tree = &empty;
    CHECK(lookup(&vcb, &node, query, false, 1, STATUS_SUCCESS).item == &items[15]);
    for (unsigned i = 0; i <= 16; i++) items[i].treeholder.tree = &empty;
    lookup(&vcb, &node, query, false, 1, STATUS_NOT_FOUND);
    for (unsigned i = 0; i <= 16; i++) items[i].treeholder.tree = NULL;
    load_calls = 0;
    load_status = STATUS_INTERNAL_ERROR;
    lookup(&vcb, &node, query, false, 0, STATUS_INTERNAL_ERROR);
    CHECK(load_calls == 1);
    loaded_child = &child; load_status = STATUS_SUCCESS;
    leaf[0].key = query;
    traverse_ptr result = lookup(&vcb, &node, query, false, 0, STATUS_SUCCESS);
    CHECK(result.tree == &child && result.item == &leaf[0] && load_calls == 2);
    result = lookup(&vcb, &node, query, false, 0, STATUS_SUCCESS);
    CHECK(result.tree == &child && load_calls == 2);
    result = lookup(&vcb, &node, query, true, 2, STATUS_SUCCESS);
    CHECK(result.tree == &node && result.item == &items[16]);
    release_index(&node);
}

static void cross_leaf(bool previous) {
    tree parent, left, right;
    tree_data separators[2], a[3], b[3];
    device_extension vcb = {0};
    root r = {0};
    prepare_tree(&parent, separators, 2, 1);
    prepare_tree(&left, a, 3, 0);
    prepare_tree(&right, b, 3, 0);
    for (unsigned i = 0; i < 3; i++) b[i].key.offset += 3 * 16;
    separators[0].key = a[0].key; separators[1].key = b[0].key;
    separators[0].treeholder.tree = &left; separators[1].treeholder.tree = &right;
    left.parent = right.parent = &parent;
    left.paritem = &separators[0]; right.paritem = &separators[1];
    parent.root = left.root = right.root = &r; r.treeholder.tree = &parent;
    for (unsigned i = 0; i < 3; i++) (previous ? b : a)[i].ignore = true;
    struct btrfs_key query = previous ? b[0].key : a[0].key;
    traverse_ptr result = lookup(&vcb, &parent, query, false, 0, STATUS_SUCCESS);
    CHECK(result.tree == (previous ? &left : &right));
    CHECK(result.item == (previous ? &a[2] : &b[0]));
    for (unsigned i = 0; i < 3; i++) a[i].ignore = b[i].ignore = true;
    lookup(&vcb, &parent, query, false, 0, STATUS_NOT_FOUND);
    result = lookup(&vcb, &parent, query, true, 0, STATUS_SUCCESS);
    CHECK(result.tree == (previous ? &right : &left));
}

static void invalidation_stays_linear(void) {
    tree node;
    tree_data items[489];
    device_extension vcb = {0};
    prepare_tree(&node, items, 489, 1);
    build_index(&node); release_index(&node);
    node.write = false;
    comparisons = 0;
    struct btrfs_key query = {42,108,244 * 16};
    CHECK(lookup(&vcb, &node, query, false, 1, STATUS_SUCCESS).item == &items[244]);
    CHECK(comparisons == 245 && allocations == releases);
}

int main(void) {
    duplicates(false); duplicates(true);
    for (unsigned mode = 0; mode < 4; mode++) optional_failure(mode);
    child_state_and_requested_level();
    cross_leaf(false); cross_leaf(true);
    invalidation_stays_linear();
    CHECK(allocations == releases);
    puts("duplicate, failure, lazy-load and cross-leaf controls passed");
}
