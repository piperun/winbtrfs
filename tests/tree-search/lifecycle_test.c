/* Real load_tree/free_tree own optional arrays across success and fallback. */
#include "host.h"
#include "search.inc"
#include "lifecycle.inc"

static void loaded_node(unsigned count, uint8_t level, bool fail_index, bool unsorted) {
    device_extension vcb = {0};
    root r = {0};
    InitializeListHead(&vcb.trees); InitializeListHead(&vcb.trees_hash);
    size_t item_bytes = level ? sizeof(struct btrfs_key_ptr) : sizeof(struct btrfs_item);
    vcb.superblock.nodesize = (uint32_t)(sizeof(struct btrfs_header) + count * item_bytes);
    uint8_t* buffer = ExAllocatePoolWithTag(PagedPool, vcb.superblock.nodesize, ALLOC_TAG);
    memset(buffer, 0, vcb.superblock.nodesize);
    struct btrfs_header* header = (struct btrfs_header*)buffer;
    header->level = level; header->nritems = count;
    for (unsigned i = 0; i < count; i++) {
        struct btrfs_key key = {42,108,i * 16};
        if (level) {
            struct btrfs_key_ptr* entries = (struct btrfs_key_ptr*)(buffer + sizeof(*header));
            entries[i].key = key;
            if (unsorted && i == 7) entries[i].key.offset = 9 * 16;
        } else {
            struct btrfs_item* entries = (struct btrfs_item*)(buffer + sizeof(*header));
            entries[i].key = key;
        }
    }
    fail_bytes = fail_index ? count * sizeof(tree_data*) : 0;
    tree* loaded = NULL;
    CHECK(load_tree(&vcb, 0x10000, buffer, &r, &loaded) == STATUS_SUCCESS);
    fail_bytes = 0;
    CHECK(loaded && !vcb.trees_list_mutex.held);
    CHECK(vcb.trees.Flink == &loaded->list_entry && vcb.trees_hash.Flink == &loaded->list_entry_hash);
#if HAVE_INDEX
    CHECK((loaded->search_items != NULL) == (level && count >= 16 && !fail_index && !unsorted));
#endif
    r.treeholder.tree = loaded;
    struct btrfs_key query = {42,108, (count - 1) * 16};
    traverse_ptr result = {0};
    CHECK(find_item_in_tree(&vcb, loaded, &result, &query, false, level, NULL) == STATUS_SUCCESS);
    CHECK(result.tree == loaded && result.item->key.offset == query.offset);
    free_tree(loaded);
    if (level) ExFreePool(buffer); /* The existing load caller owns an internal block buffer. */
    CHECK(!r.treeholder.tree && IsListEmpty(&vcb.trees) && IsListEmpty(&vcb.trees_hash));
    CHECK(allocations == releases);
}

int main(void) {
    loaded_node(489, 1, false, false);
    loaded_node(489, 1, true, false);
    loaded_node(32, 1, false, true);
    loaded_node(4, 2, false, false);
    loaded_node(32, 0, false, false);
    puts("production load/free ownership controls passed");
}
