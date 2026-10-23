/* Execute production mutation statements; stale arrays must not return moved items. */
#include "host.h"
#include "search.inc"
#include "mutations.inc"

static void expect_item(tree* t, uint64_t offset, tree_data* expected) {
    device_extension vcb = {0};
    struct btrfs_key query = {42, 108, offset};
    traverse_ptr result = {0};
    CHECK(find_item_in_tree(&vcb, t, &result, &query, false, 1, NULL) == STATUS_SUCCESS);
    CHECK(result.tree == t && result.item == expected);
}

static void split_drops_moved_suffix(void) {
    tree left, right;
    tree_data items[32];
    prepare_tree(&left, items, 32, 1);
    prepare_tree(&right, NULL, 0, 1);
    build_index(&left);
    split_items(&left, &right, &items[16]);
    expect_item(&left, 31 * 16, &items[15]);
    expect_item(&right, 0, &items[16]);
    release_index(&left); release_index(&right);
}

static void merge_and_redistribute(bool merge) {
    tree left, right;
    tree_data a[16], b[16];
    prepare_tree(&left, a, 16, 1);
    prepare_tree(&right, b, 16, 1);
    for (unsigned i = 0; i < 16; i++) b[i].key.offset += 16 * 16;
    build_index(&left); build_index(&right);
    if (merge) {
        merge_items(&left, &right);
        expect_item(&left, 31 * 16, &b[15]);
        device_extension vcb = {0}; traverse_ptr result = {0};
        struct btrfs_key query = {42,108,0};
        CHECK(find_item_in_tree(&vcb, &right, &result, &query, false, 1, NULL) == STATUS_NOT_FOUND);
    } else {
        move_item(&left, &right, &b[0]);
        expect_item(&left, 16 * 16, &b[0]);
        expect_item(&right, 0, &b[1]);
    }
    release_index(&left); release_index(&right);
}

static void parent_removal(bool sibling) {
    tree parent, child;
    tree_data items[32];
    prepare_tree(&parent, items, 32, 1);
    prepare_tree(&child, NULL, 0, 0);
    child.parent = &parent; child.paritem = &items[16];
    build_index(&parent);
    if (sibling) remove_sibling(&child, child.paritem);
    else remove_child(&child);
    expect_item(&parent, 16 * 16, &items[15]);
    release_index(&parent);
}

static void ancestor_rekey(bool batch) {
    tree parent, child;
    tree_data items[32], leaf[1];
    prepare_tree(&parent, items, 32, 1);
    prepare_tree(&child, leaf, 1, 0);
    child.parent = &parent; child.paritem = &items[0];
    items[0].treeholder.tree = &child;
    build_index(&parent);
    struct btrfs_key replacement = {41,108,0};
    traverse_ptr tp = {&child, &leaf[0]};
    if (batch) batch_rekey(tp, replacement);
    else insert_rekey(tp, replacement);
    CHECK(items[0].key.objectid == 41 && !parent.write);
    /* The cache owns memory only while the node is unchanged, independent of write. */
    CHECK(allocations == releases);
    release_index(&parent);
}

static void root_conversion_releases_array(void) {
    tree node;
    tree_data items[32];
    prepare_tree(&node, items, 32, 1);
    build_index(&node);
    for (unsigned i = 0; i < 32; i++) items[i].ignore = true;
    node.header.nritems = 0;
    convert_empty_root(&node);
    CHECK(node.header.level == 0 && allocations == releases);
    release_index(&node);
}

static void separator_insertion_and_recycling(void) {
    tree parent, child;
    tree_data items[32], inserted = {0};
    prepare_tree(&parent, items, 32, 1);
    prepare_tree(&child, NULL, 0, 0);
    child.parent = &parent; child.paritem = &items[15];
    inserted.key = (struct btrfs_key){42,108,15 * 16 + 8};
    build_index(&parent);
    insert_separator(&child, &inserted);
    expect_item(&parent, inserted.key.offset, &inserted);
    release_index(&parent);
    RemoveEntryList(&inserted.list_entry);

    tree_data* recycled = malloc(sizeof(*recycled));
    CHECK(recycled);
    *recycled = (tree_data){0};
    recycled->key = inserted.key;
    InsertHeadList(&items[15].list_entry, &recycled->list_entry);
    parent.header.nritems = 33;
    child.paritem = recycled;
    build_index(&parent);
    remove_child(&child);
    free(recycled);
    expect_item(&parent, 15 * 16 + 8, &items[15]);
    release_index(&parent);
}

static void serialization_rekey_releases_array(void) {
    tree parent, child;
    tree_data items[32];
    prepare_tree(&parent, items, 32, 1);
    prepare_tree(&child, NULL, 0, 0);
    child.parent = &parent; child.paritem = &items[0];
    child.new_address = 0x1234000;
    build_index(&parent);
    struct btrfs_key replacement = {41,108,0};
    serialize_separator(&child, replacement);
    CHECK(items[0].key.objectid == 41 && items[0].treeholder.address == 0x1234000);
    CHECK(items[0].treeholder.generation == 77 && allocations == releases);
}

static void constructors_clear_old_pool_bytes(void) {
    tree root_node, sibling, parent, original = {0};
    memset(&root_node, 0xa5, sizeof(root_node));
    memset(&sibling, 0xa5, sizeof(sibling));
    memset(&parent, 0xa5, sizeof(parent));
    initialize_root_prefix(&root_node);
    CHECK(initialize_sibling_prefix(&original, &sibling) == STATUS_SUCCESS);
    CHECK(initialize_parent_prefix(&parent) == STATUS_SUCCESS);
    release_index(&root_node); release_index(&sibling); release_index(&parent);
    ExFreePool(parent.nonpaged);
    CHECK(allocations == releases);
}

int main(void) {
    split_drops_moved_suffix();
    merge_and_redistribute(true);
    merge_and_redistribute(false);
    parent_removal(true); parent_removal(false);
    ancestor_rekey(false); ancestor_rekey(true);
    root_conversion_releases_array();
    separator_insertion_and_recycling();
    serialization_rekey_releases_array();
    constructors_clear_old_pool_bytes();
    CHECK(allocations == releases);
    puts("production tree mutation controls passed");
}
