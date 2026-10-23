#include "host.h"
#include "types.inc"
static const char* scenario;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)
#include "services.inc"
#include "completion.inc"
#include "submission.inc"

enum kind { NORMAL, MIXED, EMPTY, DISABLED, OPT_OUT, NO_ELIGIBLE, CONTEXT_FAILURE, MANY_RANGES, MERGE };
typedef struct { char name[96]; unsigned count, code, timing; enum kind kind; NTSTATUS status; } test_case;
static test_case cases[180];
static unsigned case_count;
static void add_case(const char* name, unsigned count, unsigned code, unsigned mode, enum kind kind, NTSTATUS status) {
    CHECK(case_count < sizeof(cases) / sizeof(cases[0]));
    test_case* c = &cases[case_count++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->count = count; c->code = code; c->timing = mode; c->kind = kind; c->status = status;
}
static void define_cases(void) {
    for (unsigned count = 1; count <= 3; count += 2) {
        unsigned combinations = count == 1 ? 3 : 27;
        for (unsigned code = 0; code < combinations; code++)
            for (unsigned mode = 0; mode < 4; mode++) {
                char label[96];
                snprintf(label, sizeof(label), "devices-%u-failures-%u-timing-%u", count, code, mode);
                add_case(label, count, code, mode, NORMAL, STATUS_SUCCESS);
            }
    }
    for (unsigned code = 0; code < 9; code++)
        for (unsigned mode = 0; mode < 4; mode++) {
            char label[96];
            snprintf(label, sizeof(label), "mixed-eligibility-failures-%u-timing-%u", code, mode);
            add_case(label, 6, code, mode, MIXED, STATUS_SUCCESS);
        }
    add_case("no-devices", 0, 0, 0, NORMAL, STATUS_SUCCESS);
    add_case("empty-range-list", 1, 0, 0, EMPTY, STATUS_SUCCESS);
    add_case("volume-trim-disabled", 3, 0, 0, DISABLED, STATUS_SUCCESS);
    add_case("mount-no-trim", 3, 0, 0, OPT_OUT, STATUS_SUCCESS);
    add_case("no-eligible-devices", 3, 0, 0, NO_ELIGIBLE, STATUS_SUCCESS);
    add_case("context-allocation-failure", 3, 0, 0, CONTEXT_FAILURE, STATUS_SUCCESS);
    add_case("multiple-ranges", 3, 0, 2, MANY_RANGES, STATUS_SUCCESS);
    add_case("inline-device-errors", 3, 0, 0, NORMAL, STATUS_IO_DEVICE_ERROR);
    add_case("deferred-device-errors-with-allocation-failure", 3, 5, 1, NORMAL, STATUS_IO_DEVICE_ERROR);
    add_case("cancelled-requests-with-allocation-failure", 3, 5, 3, NORMAL, STATUS_CANCELLED);
}
static void run(const test_case* c) {
    device_extension volume = {.trim = c->kind != DISABLED, .options.no_trim = c->kind == OPT_OUT};
    active_volume = &volume;
    InitializeListHead(&volume.chunks); InitializeListHead(&volume.devices);
    device devices[MAX_DEVICES] = {0};
    DEVICE_OBJECT objects[MAX_DEVICES] = {0};
    unsigned original_counts[MAX_DEVICES] = {0};
    unsigned code = c->code;
    bool selected[MAX_DEVICES] = {0};
    for (unsigned i = 0; i < c->count; i++) {
        device* dev = &devices[i];
        objects[i] = (DEVICE_OBJECT){.id = i, .StackSize = i + 4};
        dev->devobj = &objects[i]; dev->fileobj = dev; dev->trim = true;
        InitializeListHead(&dev->trim_list);
        unsigned ranges = c->kind == MANY_RANGES ? MAX_RANGES : 2;
        if (c->kind == EMPTY || (c->kind == MIXED && i == 4)) ranges = 0;
        if (c->kind == NO_ELIGIBLE || (c->kind == MIXED && i == 1)) dev->readonly = true;
        if (c->kind == MIXED && i == 2) dev->devobj = NULL;
        if (c->kind == MIXED && i == 3) dev->trim = false;
        range_counts[i] = original_counts[i] = dev->num_trim_entries = ranges;
        for (unsigned j = 0; j < ranges; j++) {
            space* range = allocate(PagedPool, sizeof(*range));
            range->address = (UINT64_C(1) << 33) + i * 100000 + j * 4096;
            range->size = 4096 * (j + 1);
            expected_ranges[i][j] = (DEVICE_DATA_SET_RANGE){(int64_t)range->address, range->size};
            InsertTailList(&dev->trim_list, &range->list_entry);
        }
        selected[i] = dev->devobj && !dev->readonly && dev->trim && ranges;
        if (selected[i]) {
            faults[eligible++] = code % 3;
            code /= 3;
        }
        InsertTailList(&volume.devices, &dev->list_entry);
    }
    chunk cache = {.space_changed = true};
    InitializeListHead(&cache.space); InitializeListHead(&cache.space_size); InitializeListHead(&cache.deleting);
    if (c->kind == MERGE) {
        space* range = allocate(PagedPool, sizeof(*range));
        InsertTailList(&cache.deleting, &range->list_entry);
        InsertTailList(&volume.chunks, &cache.list_entry);
    }
    timing = c->timing; fail_batch = c->kind == CONTEXT_FAILURE; completion_status = c->status;
    executing = true;
    CHECK(trim_devices(&volume) == STATUS_SUCCESS);
    executing = false;
    active_event = NULL;
    CHECK(!pending && submitted == completed && !live_irps && !stripe_pool);
    CHECK(!volume.readonly && !volume.chunk_lock.held && !cache.held);
    bool enabled = volume.trim && !volume.options.no_trim;
    bool processed = enabled && eligible && !fail_batch;
    unsigned expected_submitted = 0, expected_irps = 0, remaining = 0;
    for (unsigned i = 0; i < eligible; i++) {
        expected_submitted += faults[i] == 0;
        expected_irps += faults[i] != 1;
    }
    CHECK(submitted == (processed ? expected_submitted : 0));
    CHECK(dmdsa_attempts == (processed ? eligible : 0));
    CHECK(irp_attempts == (processed ? expected_irps : 0));
    CHECK(batch_attempts == (unsigned)(enabled && eligible));
    CHECK(waits <= 1 && signals <= 1);
    if (!processed) CHECK(!waits && !signals);
    for (unsigned i = 0; i < c->count; i++) {
        unsigned expected = processed && selected[i] ? 0 : original_counts[i];
        CHECK(devices[i].num_trim_entries == expected);
        unsigned actual = 0;
        for (LIST_ENTRY* entry = devices[i].trim_list.Flink; entry != &devices[i].trim_list; entry = entry->Flink) actual++;
        CHECK(actual == expected);
        remaining += expected;
    }
    CHECK(live == remaining && IsListEmpty(&cache.deleting));
    for (unsigned i = 0; i < c->count; i++)
        while (!IsListEmpty(&devices[i].trim_list))
            ExFreePool(CONTAINING_RECORD(RemoveHeadList(&devices[i].trim_list), space, list_entry));
    CHECK(!live);
    active_volume = NULL;
}
int main(int argc, char** argv) {
    scenario = "case setup";
    define_cases();
    if (argc != 2) return 2;
    bool list = strcmp(argv[1], "--list") == 0;
    for (unsigned i = 0; i < case_count; i++) {
        if (list) puts(cases[i].name);
        else if (strcmp(argv[1], cases[i].name) == 0) {
            scenario = cases[i].name;
            run(&cases[i]);
            return 0;
        }
    }
    return list ? 0 : 2;
}
