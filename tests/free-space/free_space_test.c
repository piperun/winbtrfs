/* Check production space-list merging against an interval oracle and visit bounds. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "host.h"

#ifndef MERGE_HOOKS
#define MERGE_HOOKS 0
#endif

/* The runner adds these to every list-cursor step in the extracted functions. */
static unsigned long long address_visits, size_visits;
static unsigned long long allocations, fail_allocation, pool_live;

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK(pool == PagedPool && tag == ALLOC_TAG && size == sizeof(space));
    if (++allocations == fail_allocation) return NULL;
    void* p = malloc(size);
    CHECK(p != NULL);
    memset(p, 0xa5, size);
    pool_live++;
    return p;
}

#ifdef __SANITIZE_ADDRESS__
/* AddressSanitizer quarantines and checks freed memory itself. */
static void ExFreePool(void* p) {
    CHECK(p != NULL && pool_live > 0);
    pool_live--;
    free(p);
}

static void release_quarantine(void) {}
#else
/* A freed entry reads as an empty range at address 0 with poisoned links, and its
   memory is not reused until release_quarantine. */
static space** quarantine;
static size_t quarantined, quarantine_size;

static void ExFreePool(void* p) {
    CHECK(p != NULL && pool_live > 0);
    pool_live--;
    *(space*)p = (space){ .list_entry = { POISON, POISON }, .list_entry_size = { POISON, POISON } };
    if (quarantined == quarantine_size) {
        quarantine_size = quarantine_size ? 2 * quarantine_size : 1024;
        quarantine = realloc(quarantine, quarantine_size * sizeof(*quarantine));
        CHECK(quarantine != NULL);
    }
    quarantine[quarantined++] = p;
}

/* Frees the quarantined entries, failing if one was written after it was freed. */
static void release_quarantine(void) {
    for (size_t i = 0; i < quarantined; i++) {
        space* s = quarantine[i];
        CHECK(s->address == 0 && s->size == 0);
        CHECK(s->list_entry.Flink == POISON && s->list_entry.Blink == POISON);
        CHECK(s->list_entry_size.Flink == POISON && s->list_entry_size.Blink == POISON);
        free(s);
    }
    quarantined = 0;
}
#endif

static NTSTATUS add_rollback_space(LIST_ENTRY* rollback, bool add, LIST_ENTRY* list,
    LIST_ENTRY* list_size, uint64_t address, uint64_t length, chunk* c) {
    (void)rollback; (void)add; (void)list; (void)list_size; (void)address; (void)length; (void)c;
    CHECK(false); /* no call in these tests passes a rollback list */
    return STATUS_INSUFFICIENT_RESOURCES;
}

#if MERGE_HOOKS
/* Hooks the runner adds to the extracted merge and insertion helper. */
static unsigned long long mid_starts, restarts, touched_resized, touched_freed, holder_freed,
                          failures_with_touched;
static space* previous_holder;
static bool expect_first;

static bool on_list(LIST_ENTRY* head, LIST_ENTRY* e) {
    for (LIST_ENTRY* le = head->Flink; le != head; le = le->Flink) {
        if (le == e) return true;
    }
    return false;
}

/* An insert starts at the entry that took the previous range, or at the first entry
   for the first range of a call and after a restart from the head. */
static void note_start(LIST_ENTRY* list, LIST_ENTRY* start) {
    if (start != list->Flink) mid_starts++;
    if (expect_first) CHECK(start == list->Flink);
    else CHECK(previous_holder != NULL && start == &previous_holder->list_entry);
    expect_first = false;
}

static space* note_holder(space* s) {
    previous_holder = s;
    return s;
}

static void note_resize(LIST_ENTRY* touched, space* s) {
    if (touched && on_list(touched, &s->list_entry_size)) touched_resized++;
}

static void note_free(LIST_ENTRY* touched, space* s) {
    if (touched && on_list(touched, &s->list_entry_size)) touched_freed++;
    if (s == previous_holder) holder_freed++;
}

static void note_restart(LIST_ENTRY* start, LIST_ENTRY* first) {
    if (start != first) restarts++;
    expect_first = true;
}

static void note_failure(LIST_ENTRY* touched) {
    if (!IsListEmpty(touched)) failures_with_touched++;
}

static void expect_count(unsigned long long count, bool some, const char* what) {
    if ((count > 0) != some) {
        fprintf(stderr, "%s: %llu, expected %s\n", what, count, some ? "some" : "none");
        CHECK(false);
    }
}
#endif

#include "space.inc"

#define MAX_RANGES 40

typedef struct { uint64_t start, end; } range; /* half-open */

static const uint64_t base = 0x100000000ULL; /* above 4 GiB, so truncated addresses show */
static uint64_t random_state;

static uint64_t next_random(void) { /* splitmix64 */
    uint64_t z = (random_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint64_t below(uint64_t limit) { return next_random() % limit; }

static space* make_space(range r) {
    space* s = malloc(sizeof(space));
    CHECK(s != NULL && r.start < r.end);
    memset(s, 0xa5, sizeof(space));
    s->address = r.start;
    s->size = r.end - r.start;
    pool_live++;
    return s;
}

/* A list head inside a space record that covers every address: code that takes the
   head for an entry absorbs the range into it, and the oracle check fails. */
typedef struct { space record; } list_head;

static LIST_ENTRY* address_head(list_head* h) {
    h->record = (space){ .size = UINT64_MAX, .list_entry_size = { POISON, POISON } };
    InitializeListHead(&h->record.list_entry);
    return &h->record.list_entry;
}

static LIST_ENTRY* size_head(list_head* h) {
    h->record = (space){ .size = UINT64_MAX, .list_entry = { POISON, POISON } };
    InitializeListHead(&h->record.list_entry_size);
    return &h->record.list_entry_size;
}

static int by_size_descending(const void* a, const void* b) {
    const space* x = *(space* const*)a;
    const space* y = *(space* const*)b;
    return (x->size < y->size) - (x->size > y->size);
}

static int by_pointer(const void* a, const void* b) {
    uintptr_t x = (uintptr_t)*(space* const*)a, y = (uintptr_t)*(space* const*)b;
    return (x > y) - (x < y);
}

static int by_start(const void* a, const void* b) {
    const range* x = a;
    const range* y = b;
    return (x->start > y->start) - (x->start < y->start);
}

/* Links the ranges in order. The size list is ordered by the test's own sort. */
static void build_list(LIST_ENTRY* list, LIST_ENTRY* list_size, const range* r, size_t count) {
    space** nodes = malloc((count + 1) * sizeof(*nodes));
    CHECK(nodes != NULL);
    InitializeListHead(list);
    for (size_t i = 0; i < count; i++) {
        nodes[i] = make_space(r[i]);
        InsertTailList(list, &nodes[i]->list_entry);
    }
    if (list_size) {
        InitializeListHead(list_size);
        qsort(nodes, count, sizeof(*nodes), by_size_descending);
        for (size_t i = 0; i < count; i++) InsertTailList(list_size, &nodes[i]->list_entry_size);
    }
    free(nodes);
}

static void free_list(LIST_ENTRY* list) {
    while (!IsListEmpty(list)) ExFreePool(CONTAINING_RECORD(RemoveHeadList(list), space, list_entry));
}

/* Returns the number of entries. Fails on broken links or more than limit entries. */
static size_t list_length(LIST_ENTRY* head, size_t limit) {
    size_t count = 0;
    for (LIST_ENTRY* le = head->Flink; le != head; le = le->Flink) {
        CHECK(count++ < limit);
        CHECK(le->Flink->Blink == le && le->Blink->Flink == le);
    }
    CHECK(head->Flink->Blink == head && head->Blink->Flink == head);
    return count;
}

/* Copies the ranges of an address list in link order. */
static size_t snapshot(LIST_ENTRY* list, range* r, size_t limit) {
    size_t count = list_length(list, limit);
    size_t i = 0;
    for (LIST_ENTRY* le = list->Flink; le != list; le = le->Flink, i++) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        r[i] = (range){ s->address, s->address + s->size };
    }
    return count;
}

/* The size list must hold exactly the given address-list entries, in non-increasing
   size order. Reorders nodes. */
static void check_size_list(LIST_ENTRY* list_size, space** nodes, size_t count) {
    space** sized = malloc((count + 1) * sizeof(*sized));
    uint64_t previous = UINT64_MAX;
    size_t i = 0;
    CHECK(sized != NULL);
    CHECK(list_length(list_size, count) == count);
    for (LIST_ENTRY* le = list_size->Flink; le != list_size; le = le->Flink, i++) {
        sized[i] = CONTAINING_RECORD(le, space, list_entry_size);
        CHECK(sized[i]->size <= previous);
        previous = sized[i]->size;
    }
    qsort(nodes, count, sizeof(*nodes), by_pointer);
    qsort(sized, count, sizeof(*sized), by_pointer);
    for (i = 0; i < count; i++) CHECK(nodes[i] == sized[i]);
    free(sized);
}

/* The address list must hold exactly the expected ranges, with gaps between them. The
   size list must hold the same entries, in non-increasing size order. */
static void check_lists(LIST_ENTRY* list, LIST_ENTRY* list_size, const range* expected, size_t count) {
    space** nodes = malloc((count + 1) * sizeof(*nodes));
    CHECK(nodes != NULL);
    CHECK(list_length(list, count) == count);
    size_t i = 0;
    for (LIST_ENTRY* le = list->Flink; le != list; le = le->Flink, i++) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        if (s->address != expected[i].start || s->address + s->size != expected[i].end) {
            fprintf(stderr, "entry %zu is %llx-%llx, expected %llx-%llx\n", i,
                    (unsigned long long)s->address, (unsigned long long)(s->address + s->size),
                    (unsigned long long)expected[i].start, (unsigned long long)expected[i].end);
            CHECK(false);
        }
        CHECK(i == 0 || nodes[i - 1]->address + nodes[i - 1]->size < s->address);
        nodes[i] = s;
    }
    if (list_size) check_size_list(list_size, nodes, count);
    free(nodes);
}

/* Replaces the ranges with their union: sorted, with gaps between them. */
static size_t normalize(range* r, size_t count) {
    if (count == 0) return 0;
    qsort(r, count, sizeof(*r), by_start);
    size_t last = 0;
    for (size_t i = 1; i < count; i++) {
        if (r[i].start <= r[last].end) {
            if (r[i].end > r[last].end) r[last].end = r[i].end;
        } else {
            r[++last] = r[i];
        }
    }
    return last + 1;
}

/* Fills r with sorted ranges separated by at least one byte. */
static void random_ranges(range* r, size_t count, uint64_t max_gap, uint64_t max_length) {
    uint64_t at = base + below(max_gap + 1);
    for (size_t i = 0; i < count; i++) {
        uint64_t length = 1 + below(max_length);
        r[i] = (range){ at, at + length };
        at += length + 1 + below(max_gap);
    }
}

static void random_pair(range* s, size_t n, range* d, size_t m) {
    static const uint64_t scales[] = { 1, 2, 3, 8, 40 };
    uint64_t gap = scales[below(5)], length = scales[below(5)];
    random_ranges(s, n, gap, length);
    gap = scales[below(5)];
    length = scales[below(5)];
    random_ranges(d, m, gap, length);
}

static void shuffle_ranges(range* r, size_t count) {
    for (size_t i = count; i > 1; i--) {
        size_t k = below(i);
        range t = r[i - 1];
        r[i - 1] = r[k];
        r[k] = t;
    }
}

/* Lists, in merge order, the freed ranges that join no earlier range and need a new entry. */
static size_t separate_ranges(const range* s, size_t n, const range* d, size_t m, size_t* index) {
    range u[2 * MAX_RANGES];
    size_t count = n, found = 0;
    memcpy(u, s, n * sizeof(*s));
    for (size_t j = 0; j < m; j++) {
        bool joins = false;
        for (size_t i = 0; i < count && !joins; i++)
            joins = u[i].start <= d[j].end && d[j].start <= u[i].end;
        if (!joins) index[found++] = j;
        u[count++] = d[j];
        count = normalize(u, count);
    }
    return found;
}

enum { MISS_FIRST, MISS_BETWEEN, MISS_LAST, TOUCH_START, TOUCH_END, BRIDGE, OVERLAP_START,
       OVERLAP_END, ENVELOPED, EQUAL, ENVELOPS, SPANS, SAME_ENTRY, NO_SPACE, NO_DELETING, CLASSES };
static const char* const class_names[CLASSES] = {
    "miss before first entry", "miss between entries", "miss after last entry",
    "touch entry start", "touch entry end", "bridge two entries", "overlap entry start",
    "overlap entry end", "inside entry", "equal to entry", "contain entry",
    "overlap several entries", "join previous range's entry", "empty space list",
    "empty deleting list" };
static unsigned long long coverage[CLASSES];

/* Tallies how each freed range relates to the original entries and to the entry that
   the previous freed range merged into. */
static void classify(const range* s, size_t n, const range* d, size_t m,
                     const range* merged, size_t merged_count) {
    if (n == 0 && m > 0) coverage[NO_SPACE]++;
    if (m == 0) coverage[NO_DELETING]++;
    for (size_t j = 0; j < m && n > 0; j++) {
        unsigned overlaps = 0;
        bool at_start = false, at_end = false;
        for (size_t i = 0; i < n; i++) {
            if (d[j].start == s[i].start && d[j].end == s[i].end) coverage[EQUAL]++;
            else if (s[i].start <= d[j].start && d[j].end <= s[i].end) coverage[ENVELOPED]++;
            else if (d[j].start <= s[i].start && s[i].end <= d[j].end) coverage[ENVELOPS]++;
            else if (d[j].start < s[i].start && s[i].start < d[j].end) coverage[OVERLAP_START]++;
            else if (s[i].start < d[j].start && d[j].start < s[i].end) coverage[OVERLAP_END]++;
            else {
                at_start |= d[j].end == s[i].start;
                at_end |= d[j].start == s[i].end;
                continue;
            }
            overlaps++;
        }
        if (overlaps > 1) coverage[SPANS]++;
        if (overlaps == 0) {
            if (at_start && at_end) coverage[BRIDGE]++;
            else if (at_start) coverage[TOUCH_START]++;
            else if (at_end) coverage[TOUCH_END]++;
            else if (d[j].end < s[0].start) coverage[MISS_FIRST]++;
            else if (d[j].start > s[n - 1].end) coverage[MISS_LAST]++;
            else coverage[MISS_BETWEEN]++;
        }
    }
    for (size_t j = 1; j < m; j++) {
        for (size_t i = 0; i < merged_count; i++) {
            if (merged[i].start <= d[j - 1].start && d[j - 1].start < merged[i].end) {
                if (d[j].start < merged[i].end) coverage[SAME_ENTRY]++;
                break;
            }
        }
    }
}

static void check_coverage(void) {
    for (unsigned i = 0; i < CLASSES; i++) {
        if (!coverage[i]) {
            fprintf(stderr, "no trial covered: %s\n", class_names[i]);
            CHECK(false);
        }
    }
}

static NTSTATUS merge(LIST_ENTRY* list, LIST_ENTRY* list_size, LIST_ENTRY* deleting) {
#if MERGE_HOOKS
    expect_first = true;
    previous_holder = NULL;
#endif
    return space_list_merge(list, list_size, deleting);
}

static NTSTATUS add2(LIST_ENTRY* list, LIST_ENTRY* list_size, uint64_t address, uint64_t length) {
#if MERGE_HOOKS
    expect_first = true;
    previous_holder = NULL;
#endif
    return space_list_add2(list, list_size, address, length, NULL, NULL);
}

static void merge_trial(bool with_size, bool shuffle) {
    range s[MAX_RANGES], d[MAX_RANGES], after[MAX_RANGES], expected[2 * MAX_RANGES];
    size_t n = below(MAX_RANGES + 1), m = below(MAX_RANGES + 1), index[MAX_RANGES];
    list_head lh, sh, dh;
    LIST_ENTRY* list = address_head(&lh);
    LIST_ENTRY* sizes = with_size ? size_head(&sh) : NULL;
    LIST_ENTRY* deleting = address_head(&dh);
    random_pair(s, n, d, m);
    memcpy(expected, s, n * sizeof(*s));
    memcpy(expected + n, d, m * sizeof(*d));
    size_t count = normalize(expected, n + m);
    classify(s, n, d, m, expected, count);
    if (shuffle) shuffle_ranges(d, m);
    size_t needed = separate_ranges(s, n, d, m, index);
    build_list(list, sizes, s, n);
    build_list(deleting, NULL, d, m);
    allocations = 0;
    CHECK(merge(list, sizes, deleting) == STATUS_SUCCESS);
    CHECK(allocations == needed);
    check_lists(list, sizes, expected, count);
    CHECK(snapshot(deleting, after, MAX_RANGES) == m && memcmp(after, d, m * sizeof(*d)) == 0);
    free_list(list);
    free_list(deleting);
    release_quarantine();
    CHECK(pool_live == 0);
}

/* Fails each allocation of a merge in turn. */
static unsigned long long failure_trial(bool with_size, bool shuffle) {
    range s[MAX_RANGES], d[MAX_RANGES], after[MAX_RANGES], expected[2 * MAX_RANGES];
    size_t n = below(21), m = below(21), index[MAX_RANGES];
    random_pair(s, n, d, m);
    if (shuffle) shuffle_ranges(d, m);
    size_t needed = separate_ranges(s, n, d, m, index);
    for (size_t k = 0; k < needed; k++) {
        list_head lh, sh, dh;
        LIST_ENTRY* list = address_head(&lh);
        LIST_ENTRY* sizes = with_size ? size_head(&sh) : NULL;
        LIST_ENTRY* deleting = address_head(&dh);
        build_list(list, sizes, s, n);
        build_list(deleting, NULL, d, m);
        allocations = 0;
        fail_allocation = k + 1;
        NTSTATUS status = merge(list, sizes, deleting);
        fail_allocation = 0;
        CHECK(status == STATUS_INSUFFICIENT_RESOURCES);
        CHECK(allocations == k + 1);
        /* Exactly the ranges before the one that needed the failed entry are merged. */
        memcpy(expected, s, n * sizeof(*s));
        memcpy(expected + n, d, index[k] * sizeof(*d));
        check_lists(list, sizes, expected, normalize(expected, n + index[k]));
        CHECK(snapshot(deleting, after, MAX_RANGES) == m && memcmp(after, d, m * sizeof(*d)) == 0);
        free_list(list);
        free_list(deleting);
        release_quarantine();
        CHECK(pool_live == 0);
    }
    return needed;
}

/* Adds ranges one at a time through space_list_add2. */
static void add2_trial(bool with_size) {
    range s[MAX_RANGES], expected[2 * MAX_RANGES];
    size_t n = below(MAX_RANGES + 1), count = n;
    list_head lh, sh;
    LIST_ENTRY* list = address_head(&lh);
    LIST_ENTRY* sizes = with_size ? size_head(&sh) : NULL;
    uint64_t gap = 1 + below(8), length = 1 + below(8);
    random_ranges(s, n, gap, length);
    memcpy(expected, s, n * sizeof(*s));
    build_list(list, sizes, s, n);
    uint64_t span = (n ? s[n - 1].end - base : 0) + 8;
    for (unsigned k = 0; k < 20; k++) {
        uint64_t start = base - 4 + below(span + 8);
        range r = { start, start + 1 + below(1 + span / 4) };
        bool joins = false;
        for (size_t i = 0; i < count && !joins; i++)
            joins = expected[i].start <= r.end && r.start <= expected[i].end;
        allocations = 0;
        CHECK(add2(list, sizes, r.start, r.end - r.start) == STATUS_SUCCESS);
        CHECK(allocations == !joins);
        expected[count++] = r;
        count = normalize(expected, count);
        check_lists(list, sizes, expected, count);
    }
    free_list(list);
    release_quarantine();
    CHECK(pool_live == 0);
}

static int by_start_then_end(const void* a, const void* b) {
    const range* x = a;
    const range* y = b;
    if (x->start != y->start) return (x->start > y->start) - (x->start < y->start);
    return (x->end > y->end) - (x->end < y->end);
}

/* The address list must hold exactly the added ranges, in non-decreasing address order;
   entries with the same address may be in any order. The size list must hold the same
   entries, in non-increasing size order. */
static void check_added(LIST_ENTRY* list, LIST_ENTRY* list_size, const range* added, size_t count) {
    range* want = malloc((count + 1) * sizeof(*want));
    range* got = malloc((count + 1) * sizeof(*got));
    space** nodes = malloc((count + 1) * sizeof(*nodes));
    CHECK(want != NULL && got != NULL && nodes != NULL);
    size_t length = list_length(list, count + 1), i = 0;
    if (length != count) {
        fprintf(stderr, "address list holds %zu entries, expected %zu\n", length, count);
        CHECK(false);
    }
    for (LIST_ENTRY* le = list->Flink; le != list; le = le->Flink, i++) {
        nodes[i] = CONTAINING_RECORD(le, space, list_entry);
        got[i] = (range){ nodes[i]->address, nodes[i]->address + nodes[i]->size };
        CHECK(i == 0 || got[i - 1].start <= got[i].start);
    }
    memcpy(want, added, count * sizeof(*want));
    qsort(want, count, sizeof(*want), by_start_then_end);
    qsort(got, count, sizeof(*got), by_start_then_end);
    CHECK(memcmp(want, got, count * sizeof(*got)) == 0);
    if (list_size) check_size_list(list_size, nodes, count);
    free(nodes);
    free(got);
    free(want);
}

/* Fails if a list still holds part of a range that has been taken. */
static void check_not_offered(LIST_ENTRY* head, size_t link, const char* name, range taken) {
    for (LIST_ENTRY* le = head->Flink; le != head; le = le->Flink) {
        space* s = (space*)((char*)le - link);
        if (s->address < taken.end && taken.start < s->address + s->size) {
            fprintf(stderr, "%s list still offers %llx-%llx after %llx-%llx was taken\n", name,
                    (unsigned long long)s->address, (unsigned long long)(s->address + s->size),
                    (unsigned long long)taken.start, (unsigned long long)taken.end);
            CHECK(false);
        }
    }
}

/* Adds an entry at the address of the last entry, then takes its range as an
   allocation does. Neither list may offer any of that range afterwards. */
static void add_at_last_address(bool with_size) {
    const range added[] = { { base, base + 0x1000 }, { base + 0x3000, base + 0x4000 },
                            { base + 0x3000, base + 0x7000 } };
    list_head lh, sh;
    LIST_ENTRY* list = address_head(&lh);
    LIST_ENTRY* sizes = with_size ? size_head(&sh) : NULL;
    for (size_t i = 0; i < 3; i++)
        CHECK(add_space_entry(list, sizes, added[i].start, added[i].end - added[i].start) == STATUS_SUCCESS);
    CHECK(space_list_subtract2(list, sizes, added[2].start, added[2].end - added[2].start, NULL, NULL)
          == STATUS_SUCCESS);
    check_not_offered(list, offsetof(space, list_entry), "address", added[2]);
    if (sizes) check_not_offered(sizes, offsetof(space, list_entry_size), "size", added[2]);
    check_added(list, sizes, added, 1);
    free_list(list);
    if (pool_live != 0) {
        fprintf(stderr, "entries on neither list: %llu\n", pool_live);
        CHECK(false);
    }
}

enum { ADD_EMPTY, ADD_AFTER_LAST, ADD_AT_LAST, ADD_AT_OTHER, ADD_BELOW_LAST, ADD_CLASSES };
static const char* const add_class_names[ADD_CLASSES] = {
    "empty list", "after the last entry", "at the last entry's address",
    "at another entry's address", "below the last entry's address" };
static unsigned long long add_coverage[ADD_CLASSES];

/* Adds entries at random, often equal, addresses and checks both lists after each. */
static void add_entry_trial(bool with_size) {
    range added[MAX_RANGES];
    size_t n = 1 + below(MAX_RANGES);
    list_head lh, sh;
    LIST_ENTRY* list = address_head(&lh);
    LIST_ENTRY* sizes = with_size ? size_head(&sh) : NULL;
    for (size_t i = 0; i < n; i++) {
        uint64_t start = base + below(12) * 0x1000, last = 0;
        bool equal = false;
        for (size_t j = 0; j < i; j++) {
            last = max(last, added[j].start);
            equal |= added[j].start == start;
        }
        if (i == 0) add_coverage[ADD_EMPTY]++;
        else if (start > last) add_coverage[ADD_AFTER_LAST]++;
        else if (start == last) add_coverage[ADD_AT_LAST]++;
        else if (equal) add_coverage[ADD_AT_OTHER]++;
        else add_coverage[ADD_BELOW_LAST]++;
        added[i] = (range){ start, start + (1 + below(8)) * 0x1000 };
        allocations = 0;
        CHECK(add_space_entry(list, sizes, start, added[i].end - start) == STATUS_SUCCESS);
        CHECK(allocations == 1);
        check_added(list, sizes, added, i + 1);
    }
    free_list(list);
    release_quarantine();
    CHECK(pool_live == 0);
}

static void check_add_coverage(void) {
    for (unsigned i = 0; i < ADD_CLASSES; i++) {
        if (!add_coverage[i]) {
            fprintf(stderr, "no trial added an entry: %s\n", add_class_names[i]);
            CHECK(false);
        }
    }
}

/* n separate entries, with m separate freed ranges between them. */
static void build_interleaved(LIST_ENTRY* list, LIST_ENTRY* list_size, LIST_ENTRY* deleting,
                              range* all, size_t n, size_t m) {
    const uint64_t page = 4096, stride = 64 * page;
    range* r = malloc((n + m + 1) * sizeof(*r));
    CHECK(r != NULL);
    for (size_t i = 0; i < n; i++) {
        uint64_t start = base + i * stride;
        r[i] = (range){ start, start + (1 + below(16)) * page };
    }
    for (size_t i = 0; i < m; i++) {
        uint64_t start = base + i * stride + 32 * page;
        r[n + i] = (range){ start, start + (1 + below(16)) * page };
    }
    build_list(list, list_size, r, n);
    build_list(deleting, NULL, r + n, m);
    if (all) memcpy(all, r, (n + m) * sizeof(*r));
    free(r);
}

static int by_slot(const void* a, const void* b) {
    size_t x = *(const size_t*)a, y = *(const size_t*)b;
    return (x > y) - (x < y);
}

/* n separate entries, with m freed ranges in distinct random gaps between them.
   Every second range extends the entry before its gap; the rest join nothing. */
static void build_spread(LIST_ENTRY* list, LIST_ENTRY* list_size, LIST_ENTRY* deleting,
                         range* all, size_t n, size_t m) {
    const uint64_t page = 4096, stride = 64 * page;
    range* r = malloc((n + m + 1) * sizeof(*r));
    size_t* slots = malloc((m + 1) * sizeof(*slots));
    CHECK(r != NULL && slots != NULL && m <= n);
    for (size_t i = 0; i < n; i++) {
        uint64_t start = base + i * stride;
        r[i] = (range){ start, start + (1 + below(16)) * page };
    }
    for (size_t j = 0; j < m; j++) {
        bool taken;
        do {
            slots[j] = below(n);
            taken = false;
            for (size_t k = 0; k < j; k++) taken |= slots[k] == slots[j];
        } while (taken);
    }
    qsort(slots, m, sizeof(*slots), by_slot);
    for (size_t j = 0; j < m; j++) {
        uint64_t start = j % 2 ? r[slots[j]].end : base + slots[j] * stride + 32 * page;
        r[n + j] = (range){ start, start + (1 + below(16)) * page };
    }
    build_list(list, list_size, r, n);
    build_list(deleting, NULL, r + n, m);
    if (all) memcpy(all, r, (n + m) * sizeof(*r));
    free(slots);
    free(r);
}

static unsigned long long ceil_log2(unsigned long long x) {
    unsigned long long bits = 0;
    while ((1ULL << bits) < x) bits++;
    return bits;
}

/* Merges m freed ranges into n entries, checks the result and bounds the list steps. */
static void count_steps(size_t n, size_t m, bool with_size, bool spread) {
    const unsigned long long address_limit = 4ULL * (n + m);
    const unsigned long long size_limit = 2ULL * (n + m * ceil_log2(m));
    range* expected = malloc((n + m) * sizeof(*expected));
    list_head lh, sh, dh;
    LIST_ENTRY* list = address_head(&lh);
    LIST_ENTRY* sizes = with_size ? size_head(&sh) : NULL;
    LIST_ENTRY* deleting = address_head(&dh);
    CHECK(expected != NULL);
    random_state = 7;
    if (spread) build_spread(list, sizes, deleting, expected, n, m);
    else build_interleaved(list, sizes, deleting, expected, n, m);
    address_visits = size_visits = 0;
    CHECK(merge(list, sizes, deleting) == STATUS_SUCCESS);
    printf("%s list: n=%zu m=%zu, address-list steps %llu (limit %llu), "
           "size-list steps %llu (limit %llu)\n", with_size ? "with size" : "no size",
           n, m, address_visits, address_limit, size_visits, size_limit);
    fflush(stdout);
    check_lists(list, sizes, expected, normalize(expected, n + m));
    CHECK(address_visits <= address_limit);
    CHECK(size_visits <= size_limit);
#if MERGE_HOOKS
    expect_count(mid_starts, m > 1, "inserts starting after the first entry");
    expect_count(restarts, false, "restarts from the head");
#endif
    free_list(list);
    free_list(deleting);
    release_quarantine();
    free(expected);
}

static double seconds(void) {
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Prints, for each m, the best time of a number of merges with and without a size
   list. An n of zero means as many entries as freed ranges. */
static void time_merges(const size_t* counts, size_t rows, size_t n, bool spread, unsigned runs) {
    printf("%8s %16s %16s\n", "m", "no size list", "size list");
    for (size_t i = 0; i < rows; i++) {
        double best[2] = { 1e30, 1e30 };
        for (unsigned with_size = 0; with_size < 2; with_size++) {
            for (unsigned run = 0; run < runs; run++) {
                list_head lh, sh, dh;
                LIST_ENTRY* list = address_head(&lh);
                LIST_ENTRY* sizes = with_size ? size_head(&sh) : NULL;
                LIST_ENTRY* deleting = address_head(&dh);
                size_t entries = n ? n : counts[i];
                random_state = 7;
                if (spread) build_spread(list, sizes, deleting, NULL, entries, counts[i]);
                else build_interleaved(list, sizes, deleting, NULL, entries, counts[i]);
                double start = seconds();
                CHECK(merge(list, sizes, deleting) == STATUS_SUCCESS);
                double elapsed = seconds() - start;
                if (elapsed < best[with_size]) best[with_size] = elapsed;
                free_list(list);
                free_list(deleting);
                release_quarantine();
            }
        }
        printf("%8zu %13.3f ms %13.3f ms\n", counts[i], best[0] * 1e3, best[1] * 1e3);
        fflush(stdout);
    }
}

static void bench(void) {
    static const size_t many[] = { 5000, 10000, 20000, 40000 }, few[] = { 1, 4, 16, 64 };
    printf("merge of m separate freed ranges between n = m entries, best of 5\n");
    time_merges(many, sizeof(many) / sizeof(*many), 0, false, 5);
    printf("\nmerge of m freed ranges into random gaps between n = 33015 entries, best of 20\n");
    time_merges(few, sizeof(few) / sizeof(*few), 33015, true, 20);
}

enum { MERGE, UNSORTED, FAILURE, ADD2, FAILURE_UNSORTED, ADD_ENTRY, KINDS };
static const char* const kind_names[KINDS] = { "merge", "unsorted", "failure", "add2",
                                               "failure-unsorted", "add-entry" };
static const unsigned seeds[KINDS] = { 4, 2, 2, 2, 2, 2 };
static const unsigned trials[KINDS] = { 2000, 1000, 300, 300, 300, 300 };

static void run_trials(unsigned kind, unsigned seed, bool with_size) {
    bool sorted = kind == MERGE || kind == FAILURE, failing = kind == FAILURE || kind == FAILURE_UNSORTED;
    unsigned long long injected = 0;
    random_state = (uint64_t)seed << 8 | kind;
    for (unsigned t = 0; t < trials[kind]; t++) {
        if (kind == ADD2) add2_trial(with_size);
        else if (kind == ADD_ENTRY) add_entry_trial(with_size);
        else if (failing) injected += failure_trial(with_size, !sorted);
        else merge_trial(with_size, !sorted);
    }
    if (kind == ADD_ENTRY) {
        /* add_space_entry neither merges nor goes through the merge's hooks. */
        check_add_coverage();
        return;
    }
    if (kind == MERGE || kind == UNSORTED) check_coverage();
    if (failing) CHECK(injected > 0);
#if MERGE_HOOKS
    /* The paths through the merge that each kind of trial exists for must have run. */
    if (kind == ADD2) {
        expect_count(mid_starts, false, "inserts from space_list_add2 not starting at the head");
        return;
    }
    expect_count(mid_starts, true, "inserts starting after the first entry");
    expect_count(restarts, !sorted, "restarts from the head");
    if (!sorted) expect_count(holder_freed, true, "entries freed after taking the previous range");
    if (with_size && !failing) expect_count(touched_resized, true, "entries on touched resized again");
    if (with_size && !sorted) expect_count(touched_freed, true, "entries on touched freed");
    if (with_size && failing) expect_count(failures_with_touched, true, "failed allocations with entries on touched");
#endif
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (strcmp(argv[1], "--bench") == 0) {
        bench();
        return 0;
    }
    bool list = strcmp(argv[1], "--list") == 0;
    for (unsigned kind = 0; kind < KINDS; kind++) {
        for (unsigned seed = 1; seed <= seeds[kind]; seed++) {
            for (unsigned with_size = 0; with_size < 2; with_size++) {
                char name[64];
                snprintf(name, sizeof(name), "%s-seed%u-%s", kind_names[kind], seed,
                         with_size ? "size" : "nosize");
                if (list) { puts(name); continue; }
                if (strcmp(argv[1], name) == 0) { run_trials(kind, seed, with_size); goto done; }
            }
        }
    }
    for (unsigned with_size = 0; with_size < 2; with_size++) {
        const char* name = with_size ? "add-at-last-address-size" : "add-at-last-address-nosize";
        if (list) { puts(name); continue; }
        if (strcmp(argv[1], name) == 0) { add_at_last_address(with_size); goto done; }
    }
    for (unsigned with_size = 0; with_size < 2; with_size++) {
        const char* name = with_size ? "complexity-size" : "complexity-nosize";
        if (list) { puts(name); continue; }
        if (strcmp(argv[1], name) == 0) { count_steps(20000, 20000, with_size, false); goto done; }
    }
    static const size_t few[] = { 1, 16, 64 };
    for (size_t i = 0; i < sizeof(few) / sizeof(*few); i++) {
        char name[32];
        snprintf(name, sizeof(name), "small-merge-%zu", few[i]);
        if (list) { puts(name); continue; }
        if (strcmp(argv[1], name) == 0) { count_steps(20000, few[i], true, true); goto done; }
    }
    CHECK(list);
done:
    release_quarantine();
    CHECK(pool_live == 0);
    return 0;
}
