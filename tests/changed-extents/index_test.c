/* Exercise the exact production lookup and allocation-free lifecycle helpers. */
#include "host.h"
#include "counted.inc"
#if HAVE_INDEX
#include "index-check.h"
#endif

static void initialize(chunk* c) {
#if HAVE_INDEX
    memset(c, 0xa5, sizeof(*c));
    initialize_changed_extents(c);
    CHECK(c->changed_extents_root == NULL);
#else
    InitializeListHead(&c->changed_extents);
#endif
}

static void detach(chunk* c, changed_extent* ce) {
#if HAVE_INDEX
    remove_changed_extent(c, ce);
#else
    (void)c;
    RemoveEntryList(&ce->list_entry);
#endif
}

static void drain(chunk* c) {
    while (!IsListEmpty(&c->changed_extents)) {
        changed_extent* ce = CONTAINING_RECORD(c->changed_extents.Flink, changed_extent, list_entry);
        detach(c, ce);
        ExFreePool(ce);
    }
}

#if HAVE_INDEX
enum { CHUNKS = 4, CAPACITY = 96 };
typedef struct {
    chunk chunks[CHUNKS];
    changed_extent* order[CHUNKS][CAPACITY];
    unsigned count[CHUNKS];
    unsigned total;
} model;

static unsigned walk(changed_extent* ce, changed_extent* parent, changed_extent** ordered,
                     unsigned* count, unsigned depth) {
    if (!ce)
        return 0;
    CHECK(depth < 64 && ce->parent == parent);
    unsigned left = walk(ce->left, ce, ordered, count, depth + 1);
    CHECK(*count < CAPACITY);
    ordered[(*count)++] = ce;
    unsigned right = walk(ce->right, ce, ordered, count, depth + 1);
    CHECK(left <= right + 1 && right <= left + 1);
    CHECK(ce->height == 1 + (left > right ? left : right));
    return ce->height;
}

static void verify(model* m) {
    unsigned total = 0;
    for (unsigned c = 0; c < CHUNKS; c++) {
        validate_changed_extents(&m->chunks[c]);
        changed_extent* ordered[CAPACITY];
        unsigned n = 0;
        walk(m->chunks[c].changed_extents_root, NULL, ordered, &n, 0);
        CHECK(n == m->count[c]);
        LIST_ENTRY* le = m->chunks[c].changed_extents.Flink;
        for (unsigned i = 0; i < n; i++) {
            CHECK(le == &m->order[c][i]->list_entry);
            CHECK(le->Flink->Blink == le && le->Blink->Flink == le);
            le = le->Flink;
        }
        CHECK(le == &m->chunks[c].changed_extents);
        for (unsigned i = 0; i < n; i++) {
            unsigned position = n;
            for (unsigned j = 0; j < n; j++)
                if (m->order[c][j] == ordered[i])
                    position = j;
            CHECK(position != n);
            if (i) {
                int key = compare_changed_extent(ordered[i - 1], ordered[i]->address, ordered[i]->size);
                CHECK(key <= 0);
                if (key == 0) {
                    unsigned previous = n;
                    for (unsigned j = 0; j < n; j++)
                        if (m->order[c][j] == ordered[i - 1])
                            previous = j;
                    CHECK(previous < position);
                }
            }
            // Compare the indexed returned pointer with the first list identity.
            changed_extent* first = NULL;
            for (unsigned j = 0; j < n; j++) {
                changed_extent* e = m->order[c][j];
                if (e->address == ordered[i]->address && e->size == ordered[i]->size) {
                    first = e;
                    break;
                }
            }
            CHECK(get_changed_extent_item(&m->chunks[c], ordered[i]->address, ordered[i]->size,
                                          !first->no_csum) == first);
        }
        total += n;
    }
    CHECK(total == m->total);
}

static uint64_t random_value(uint64_t* state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static unsigned shape(changed_extent* ce, changed_extent* parent, unsigned* count, unsigned depth) {
    if (!ce) return 0;
    CHECK(depth < 64 && ce->parent == parent);
    unsigned left = shape(ce->left, ce, count, depth + 1);
    (*count)++;
    unsigned right = shape(ce->right, ce, count, depth + 1);
    CHECK(left <= right + 1 && right <= left + 1);
    CHECK(ce->height == 1 + (left > right ? left : right));
    return ce->height;
}

static void duplicate_roots(void) {
    chunk c;
    initialize(&c);
    for (unsigned i = 0; i < 256; i++) {
        changed_extent* ce = calloc(1, sizeof(*ce));
        CHECK(ce != NULL);
        ce->address = UINT64_MAX;
        ce->size = ce->old_size = UINT64_MAX;
        InitializeListHead(&ce->refs);
        InitializeListHead(&ce->old_refs);
        insert_changed_extent(&c, ce);
    }
    for (unsigned remaining = 256; remaining; remaining--) {
        unsigned visited = 0;
        shape(c.changed_extents_root, NULL, &visited, 0);
        CHECK(visited == remaining);
        changed_extent* first = CONTAINING_RECORD(c.changed_extents.Flink, changed_extent, list_entry);
        loop_steps = 0;
        CHECK(get_changed_extent_item(&c, UINT64_MAX, UINT64_MAX, true) == first);
        CHECK(loop_steps <= 14);
        changed_extent* root = c.changed_extents_root;
        remove_changed_extent(&c, root);
        ExFreePool(root);
    }
    CHECK(c.changed_extents_root == NULL && IsListEmpty(&c.changed_extents));
    puts("256 equal-key root removals preserve first-list identity");
}

static void erase(model* m, unsigned c, unsigned position) {
    CHECK(position < m->count[c]);
    m->count[c]--;
    memmove(&m->order[c][position], &m->order[c][position + 1],
            (m->count[c] - position) * sizeof(changed_extent*));
    m->total--;
}

static void random_lifecycle(void) {
    const uint64_t sizes[] = {0, 4096, 8192, UINT64_MAX};
    for (unsigned seed = 1; seed <= 24; seed++) {
        model m = {0};
        uint64_t state = seed;
        for (unsigned c = 0; c < CHUNKS; c++)
            initialize(&m.chunks[c]);
        for (unsigned step = 0; step < 2000; step++) {
            unsigned c = (unsigned)(random_value(&state) % CHUNKS);
            unsigned op = (unsigned)(random_value(&state) % 7);
            uint64_t address = random_value(&state) % 16;
            if (address == 15)
                address = UINT64_MAX;
            uint64_t size = sizes[random_value(&state) % 4];
            if (m.total == CAPACITY)
                op = 6;
            if (op < 3) {
                changed_extent* first = NULL;
                for (unsigned j = 0; j < m.count[c]; j++)
                    if (m.order[c][j]->address == address && m.order[c][j]->size == size) {
                        first = m.order[c][j];
                        break;
                    }
                changed_extent* ce = get_changed_extent_item(&m.chunks[c], address, size, seed & 1);
                CHECK(ce != NULL);
                if (first)
                    CHECK(ce == first);
                else {
                    m.order[c][m.count[c]++] = ce;
                    m.total++;
                }
            } else if (op == 3) {
                changed_extent* ce = calloc(1, sizeof(*ce));
                CHECK(ce != NULL);
                ce->address = address;
                ce->size = ce->old_size = size;
                InitializeListHead(&ce->refs);
                InitializeListHead(&ce->old_refs);
                insert_changed_extent(&m.chunks[c], ce);
                m.order[c][m.count[c]++] = ce;
                m.total++;
            } else if (m.count[c]) {
                unsigned position = (unsigned)(random_value(&state) % m.count[c]);
                changed_extent* ce = m.order[c][position];
                erase(&m, c, position);
                if (op == 4) {
                    unsigned target = (unsigned)(random_value(&state) % CHUNKS);
                    move_changed_extent(&m.chunks[c], &m.chunks[target], ce, address);
                    m.order[target][m.count[target]++] = ce;
                    m.total++;
                } else {
                    detach(&m.chunks[c], ce);
                    ExFreePool(ce);
                }
            }
            verify(&m);
        }
        for (unsigned c = 0; c < CHUNKS; c++) {
            drain(&m.chunks[c]);
            CHECK(m.chunks[c].changed_extents_root == NULL);
        }
    }
    puts("48000 randomized lifecycle operations passed");
}
#endif

int main(void) {
    chunk c;
    initialize(&c);
    changed_extent* last = NULL;
    unsigned n = 4096;
    for (unsigned i = 0; i < n; i++) {
        last = get_changed_extent_item(&c, i, 4096, false);
        CHECK(last != NULL);
    }
    loop_steps = 0;
    CHECK(get_changed_extent_item(&c, n - 1, 4096, true) == last);
    CHECK(!last->no_csum);
    printf("lookup n=%u: %llu comparisons/advances, limit 28\n", n, loop_steps);
    CHECK(loop_steps <= 28);
    drain(&c);
    puts("exact lookup complexity passed");
#if HAVE_INDEX
    CHECK(c.changed_extents_root == NULL);
    initialize(&c);
    for (unsigned i = n; i; i--)
        last = get_changed_extent_item(&c, i - 1, 8192, false);
    loop_steps = 0;
    CHECK(get_changed_extent_item(&c, 0, 8192, true) == last);
    CHECK(loop_steps <= 28);
    unsigned visited = 0;
    shape(c.changed_extents_root, NULL, &visited, 0);
    CHECK(visited == n);
    drain(&c);
    duplicate_roots();
    random_lifecycle();
    printf("host changed_extent size: %zu bytes\n", sizeof(changed_extent));
#endif
    return 0;
}
