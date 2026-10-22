/* Validate the production index against the existing list after any operation. */
#pragma once

static unsigned check_index_node(const chunk* c, const changed_extent* ce,
        const changed_extent* parent, const changed_extent** previous, size_t* count, unsigned depth) {
    if (!ce) return 0;
    CHECK(depth < 64);
    bool live = false;
    for (const LIST_ENTRY* le = c->changed_extents.Flink; le != &c->changed_extents; le = le->Flink)
        if (CONTAINING_RECORD(le, changed_extent, list_entry) == ce)
            live = true;
    CHECK(live && ce->parent == parent);
    unsigned left = check_index_node(c, ce->left, ce, previous, count, depth + 1);
    if (*previous) {
        int order = compare_changed_extent(*previous, ce->address, ce->size);
        CHECK(order <= 0);
        if (order == 0) {
            bool seen = false;
            for (const LIST_ENTRY* le = c->changed_extents.Flink; le != &c->changed_extents; le = le->Flink) {
                const changed_extent* entry = CONTAINING_RECORD(le, changed_extent, list_entry);
                if (entry == *previous) seen = true;
                if (entry == ce) { CHECK(seen); break; }
            }
        }
    }
    *previous = ce;
    (*count)++;
    unsigned right = check_index_node(c, ce->right, ce, previous, count, depth + 1);
    CHECK(left <= right + 1 && right <= left + 1);
    CHECK(ce->height == 1 + (left > right ? left : right));
    return ce->height;
}

static void validate_changed_extents(const chunk* c) {
    size_t indexed = 0, listed = 0;
    const changed_extent* previous = NULL;
    check_index_node(c, c->changed_extents_root, NULL, &previous, &indexed, 0);
    for (const LIST_ENTRY* le = c->changed_extents.Flink; le != &c->changed_extents; le = le->Flink) {
        CHECK(le->Flink->Blink == le && le->Blink->Flink == le);
        listed++;
    }
    CHECK(indexed == listed);
}
