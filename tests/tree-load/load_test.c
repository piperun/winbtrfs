/* Production load_tree must unwind unpublished state without owning input. */
#include "host.h"
#include "search.inc"
#include "loader.inc"

static unsigned cases;
static void attempt(uint8_t level, unsigned fail, int malformed) {
    device_extension vcb = {0};
    root r = {0};
    const unsigned count = 20;
    vcb.superblock.nodesize = 4096;
    InitializeListHead(&vcb.trees);
    InitializeListHead(&vcb.trees_hash);
    device_extension original = vcb;
    uint8_t* buffer = calloc(1, vcb.superblock.nodesize);
    CHECK(buffer);
    struct btrfs_header* header = (struct btrfs_header*)buffer;
    header->level = level;
    header->nritems = malformed == 1 ? 10000 : count;
    for (unsigned i = 0; i < count; i++) {
        if (level) {
            struct btrfs_key_ptr* entries = (void*)(buffer + sizeof(*header));
            entries[i].key = (struct btrfs_key){42, 108, i * 16};
        } else {
            struct btrfs_item* entries = (void*)(buffer + sizeof(*header));
            entries[i].key = (struct btrfs_key){42, 108, i * 16};
            if (malformed == 2 && i == 7) entries[i].size = 4096;
        }
    }
    uint8_t snapshot[4096];
    memcpy(snapshot, buffer, sizeof(snapshot));
    tree sentinel = {0};
    tree* result = &sentinel;
    allocations = releases = allocation_attempts = 0;
    fail_at = fail;
    NTSTATUS status = load_tree(&vcb, 0x10000, buffer, &r, &result);
    fail_at = 0;
    if (NT_SUCCESS(status)) {
        CHECK(!malformed);
        CHECK(level && fail == count + 3); /* Optional search array. */
        CHECK(result != &sentinel && !result->search_items);
        while (!IsListEmpty(&result->itemlist)) {
            tree_data* td = CONTAINING_RECORD(RemoveHeadList(&result->itemlist), tree_data, list_entry);
            ExFreeToPagedLookasideList(&vcb.tree_data_lookaside, td);
        }
        ExFreePool(result->nonpaged);
        ExFreePool(result);
    } else {
        CHECK(status == (malformed == 2 ? STATUS_INTERNAL_ERROR : STATUS_INSUFFICIENT_RESOURCES));
        CHECK(result == &sentinel);
        CHECK(!memcmp(&vcb, &original, sizeof(vcb)));
        CHECK(!r.treeholder.tree);
    }
    CHECK(!memcmp(buffer, snapshot, sizeof(snapshot)));
    if (allocations != releases)
        fprintf(stderr, "unpublished leak: level=%u allocation=%u malformed=%d allocated=%u freed=%u\n", level, fail, malformed, allocations, releases);
    CHECK(allocations == releases);
    free(buffer);
    cases++;
}

int main(void) {
    for (unsigned level = 0; level <= 1; level++) {
        for (unsigned fail = 1; fail <= 21 + 2 * level; fail++) attempt(level, fail, 0);
        attempt(level, 0, 1);
    }
    attempt(0, 0, 2);
    printf("%u actual loader failure/ownership cases passed\n", cases);
}
