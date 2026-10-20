#include "host.h"

static const char* scenario;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)

enum { MAX_RECORDS = 16, MAX_POOLS = 32, MAX_VALUE = 1024 };
static struct { void* pointer; size_t size; } pools[MAX_POOLS];
static unsigned attempts, fail_at, live;

static void* allocate(size_t size) {
    if (++attempts == fail_at)
        return NULL;
    for (unsigned i = 0; i < MAX_POOLS; i++) {
        if (!pools[i].pointer) {
            void* p = malloc(size);
            CHECK(p);
            memset(p, 0xa5, size);
            pools[i].pointer = p;
            pools[i].size = size;
            live++;
            return p;
        }
    }
    CHECK(false);
    return NULL;
}
static size_t allocation_size(const void* pointer) {
    for (unsigned i = 0; i < MAX_POOLS; i++)
        if (pools[i].pointer == pointer)
            return pools[i].size;
    CHECK(false);
    return 0;
}
static void ExFreePool(void* pointer) {
    CHECK(pointer);
    for (unsigned i = 0; i < MAX_POOLS; i++) {
        if (pools[i].pointer == pointer) {
            free(pointer);
            pools[i].pointer = NULL;
            live--;
            return;
        }
    }
    CHECK(false);
}

#include "ea_types.inc"
#include "parse_ea.inc"
#include "services.inc"
#include "set_ea.inc"

typedef struct {
    char name[256];
    uint16_t value_length;
    uint8_t value[MAX_VALUE], flags;
} record;
enum kind { PAIR, SINGLE, TRIPLE, CHAIN, LARGE, DUPLICATE_GROW, DUPLICATE_SHRINK,
            DUPLICATE_LAST, LXSS_MIXED, LXSS_ONLY, LXSS_SHORT };
typedef struct test_case {
    char name[80]; enum kind kind; unsigned n, v, fail;
    void (*run)(const struct test_case*);
} test_case;
static test_case cases[160];
static unsigned case_count;
static void define_size_cases(void);

static void add_case(const char* name, enum kind kind, unsigned n, unsigned v, unsigned fail) {
    CHECK(case_count < sizeof(cases) / sizeof(cases[0]));
    test_case* c = &cases[case_count++];
    CHECK(snprintf(c->name, sizeof(c->name), "%s", name) < (int)sizeof(c->name));
    c->kind = kind;
    c->n = n;
    c->v = v;
    c->fail = fail;
}
static void define_cases(void) {
    char name[80];
    for (unsigned n = 1; n <= 4; n++) {
        for (unsigned v = 0; v <= 7; v++) {
            snprintf(name, sizeof(name), "pair-name-%u-value-%u", n, v);
            add_case(name, PAIR, n, v, 0);
        }
    }
    for (unsigned v = 0; v < 4; v++) {
        snprintf(name, sizeof(name), "singleton-value-%u", v);
        add_case(name, SINGLE, 1, v, 0);
        snprintf(name, sizeof(name), "triple-value-%u", v);
        add_case(name, TRIPLE, 1, v, 0);
    }
    add_case("sixteen-record-chain", CHAIN, 0, 0, 0);
    add_case("maximum-name-aligned", LARGE, 255, 0, 0);
    add_case("maximum-name-overlap", LARGE, 255, 1, 0);
    add_case("large-binary-value", LARGE, 1, MAX_VALUE - 1, 0);
    add_case("duplicate-grows-first-value", DUPLICATE_GROW, 0, 0, 0);
    add_case("duplicate-shrinks-first-value", DUPLICATE_SHRINK, 0, 0, 0);
    add_case("duplicate-updates-last-record", DUPLICATE_LAST, 0, 0, 0);
    add_case("lxss-between-ordinary-records", LXSS_MIXED, 0, 0, 0);
    add_case("lxss-only-no-output-buffer", LXSS_ONLY, 0, 0, 0);
    for (unsigned i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "short-lxss-%u-with-pending-records", i);
        add_case(name, LXSS_SHORT, i, 0, 0);
    }
    for (unsigned i = 1; i <= 4; i++) {
        snprintf(name, sizeof(name), "triple-allocation-failure-%u", i);
        add_case(name, TRIPLE, 1, 3, i);
        snprintf(name, sizeof(name), "lxss-allocation-failure-%u", i);
        add_case(name, LXSS_MIXED, 0, 0, i);
    }
    for (unsigned i = 1; i <= 3; i++) {
        snprintf(name, sizeof(name), "duplicate-allocation-failure-%u", i);
        add_case(name, DUPLICATE_GROW, 0, 0, i);
    }
    define_size_cases();
}

static void set_record(record* r, unsigned n, char letter, unsigned v, uint8_t flags) {
    CHECK(n > 0 && n < sizeof(r->name) && v <= MAX_VALUE);
    memset(r->name, letter, n);
    r->name[n] = 0;
    r->value_length = (uint16_t)v;
    r->flags = flags;
    for (unsigned i = 0; i < v; i++)
        r->value[i] = (uint8_t)(i * 37);
    if (v)
        r->value[v - 1] = 0xff;
}
static void set_lxss(record* r, const char* name, unsigned length, uint32_t value) {
    set_record(r, (unsigned)strlen(name), 'x', length, 0);
    strcpy(r->name, name);
    memcpy(r->value, &value, sizeof(value));
}

static size_t record_size(const record* r) { return 8 + strlen(r->name) + 1 + r->value_length; }
static size_t align4(size_t size) { return (size + 3) & ~(size_t)3; }

/* Construct a valid, independently packed input chain, including the NUL. */
static void* make_input(const record* input, unsigned count, size_t* size) {
    *size = 0;
    for (unsigned i = 0; i < count; i++)
        *size += i + 1 == count ? record_size(&input[i]) : align4(record_size(&input[i]));
    char* buffer = calloc(1, *size);
    CHECK(buffer);
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        FILE_FULL_EA_INFORMATION* ea = (void*)(buffer + offset);
        const record* r = &input[i];
        ea->NextEntryOffset = i + 1 == count ? 0 : (ULONG)align4(record_size(r));
        ea->Flags = r->flags;
        ea->EaNameLength = (uint8_t)strlen(r->name);
        ea->EaValueLength = r->value_length;
        memcpy(ea->EaName, r->name, ea->EaNameLength + 1);
        memcpy(ea->EaName + ea->EaNameLength + 1, r->value, r->value_length);
        offset += align4(record_size(r));
    }
    return buffer;
}

/* Follow the published offsets, checking all payloads and the exact end. */
static void check_output(const fcb* node, const record* const* expected, unsigned count) {
    CHECK(node->ea_changed == (count > 0));
    if (!count) {
        CHECK(!node->ea_xattr.Buffer && !node->ea_xattr.Length && !node->ealen);
        CHECK(live == 0);
        return;
    }
    CHECK(node->ea_xattr.Buffer && live == 1);
    size_t total = 0;
    uint32_t ealen = 4;
    for (unsigned i = 0; i < count; i++) {
        total += i + 1 == count ? record_size(expected[i]) : align4(record_size(expected[i]));
        ealen += 5 + strlen(expected[i]->name) + expected[i]->value_length;
    }
    CHECK(node->ea_xattr.Length == total && node->ea_xattr.MaximumLength == total);
    CHECK(allocation_size(node->ea_xattr.Buffer) == total);
    CHECK(node->ealen == ealen);
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        const record* r = expected[i];
        CHECK(offset % 4 == 0 && offset + record_size(r) <= total);
        const FILE_FULL_EA_INFORMATION* ea = (const void*)(node->ea_xattr.Buffer + offset);
        CHECK(ea->EaNameLength == strlen(r->name));
        CHECK(ea->EaValueLength == r->value_length && ea->Flags == r->flags);
        for (unsigned j = 0; j < ea->EaNameLength; j++)
            CHECK((unsigned char)ea->EaName[j] == uppercase((unsigned char)r->name[j]));
        CHECK(ea->EaName[ea->EaNameLength] == 0);
        CHECK(memcmp(ea->EaName + ea->EaNameLength + 1, r->value, r->value_length) == 0);
        if (i + 1 < count) {
            CHECK(ea->NextEntryOffset == align4(record_size(r)));
            CHECK(ea->NextEntryOffset >= record_size(r));
            offset += ea->NextEntryOffset;
        } else {
            CHECK(ea->NextEntryOffset == 0);
            CHECK(offset + record_size(r) == total);
        }
    }
}

static void run(const test_case* c) {
    record input[MAX_RECORDS] = {0};
    const record* expected[MAX_RECORDS] = {0};
    unsigned count = 2, output_count = 2;
    set_record(&input[0], c->n ? c->n : 1, 'a', c->v, 0x80);
    set_record(&input[1], 4, 'b', 7, 0);
    NTSTATUS want = STATUS_SUCCESS;
    switch (c->kind) {
        case PAIR: case LARGE: break;
        case SINGLE: count = output_count = 1; break;
        case TRIPLE:
            count = output_count = 3;
            set_record(&input[1], 1, 'b', c->v, 0);
            set_record(&input[2], 2, 'c', 5, 0x80);
            break;
        case CHAIN:
            count = output_count = MAX_RECORDS;
            for (unsigned i = 0; i < count; i++)
                set_record(&input[i], i % 4 + 1, (char)('a' + i), i / 4, i % 2 ? 0x80 : 0);
            break;
        case DUPLICATE_GROW: case DUPLICATE_SHRINK: case DUPLICATE_LAST:
            count = 3;
            set_record(&input[0], 1, 'a', c->kind == DUPLICATE_SHRINK ? 7 : 0, 0x80);
            set_record(&input[2], c->kind == DUPLICATE_LAST ? 4 : 1,
                       c->kind == DUPLICATE_LAST ? 'B' : 'A',
                       c->kind == DUPLICATE_SHRINK ? 0 : 3, 0);
            break;
        case LXSS_MIXED:
            count = 3;
            set_record(&input[0], 1, 'a', 3, 0x80);
            set_lxss(&input[1], "$lxuid", 4, 1234);
            set_record(&input[2], 1, 'b', 7, 0);
            break;
        case LXSS_ONLY:
            output_count = 0;
            set_lxss(&input[0], "$lxuid", 4, 1234);
            set_lxss(&input[1], "$LXGID", 4, 5678);
            break;
        case LXSS_SHORT: {
            static const char* names[] = {"$LXUID", "$LXGID", "$LXMOD", "$LXDEV"};
            set_lxss(&input[1], names[c->n], c->n == 3 ? 7 : 3, 1234);
            count = 3;
            set_record(&input[2], 1, 'c', 3, 0);
            want = STATUS_INVALID_PARAMETER;
            break;
        }
    }
    for (unsigned i = 0; i < output_count; i++)
        expected[i] = &input[i];
    if (c->kind == DUPLICATE_GROW || c->kind == DUPLICATE_SHRINK)
        expected[0] = &input[2];
    if (c->kind == DUPLICATE_LAST || c->kind == LXSS_MIXED)
        expected[1] = &input[2];

    size_t input_size;
    void* buffer = make_input(input, count, &input_size);
    void* snapshot = malloc(input_size);
    CHECK(snapshot);
    memcpy(snapshot, buffer, input_size);
    device_extension volume = {.superblock.nodesize = 16384};
    fcb node = {.type = BTRFS_FT_REG_FILE, .sd_deleted = true, .Vcb = &volume};
    fail_at = c->fail;
    if (fail_at)
        want = STATUS_INSUFFICIENT_RESOURCES;
    CHECK(file_create_parse_ea(&node, buffer) == want);
    if (fail_at)
        CHECK(attempts == fail_at);
    if (want == STATUS_SUCCESS) {
        check_output(&node, expected, output_count);
        if (c->kind == LXSS_MIXED || c->kind == LXSS_ONLY) {
            CHECK(node.inode_item.uid == 1234 && node.sd_dirty && !node.sd_deleted);
            if (c->kind == LXSS_ONLY)
                CHECK(node.inode_item.gid == 5678);
        }
    } else {
        CHECK(live == 0 && !node.ea_changed && !node.ea_xattr.Buffer);
        CHECK(!node.ea_xattr.Length && !node.ea_xattr.MaximumLength && !node.ealen);
    }
    /* The parser borrows input storage and uppercases names in place only. */
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        const FILE_FULL_EA_INFORMATION* old = (const void*)((const char*)snapshot + offset);
        FILE_FULL_EA_INFORMATION* now = (void*)((char*)buffer + offset);
        CHECK(memcmp(now, old, 8) == 0);
        CHECK(now->EaName[now->EaNameLength] == 0);
        CHECK(memcmp(now->EaName + now->EaNameLength + 1,
                     old->EaName + old->EaNameLength + 1, old->EaValueLength) == 0);
        offset += old->NextEntryOffset;
    }
    if (node.ea_xattr.Buffer)
        ExFreePool(node.ea_xattr.Buffer);
    CHECK(live == 0);
    free(snapshot);
    free(buffer);
}

#include "size_test.inc"

int main(int argc, char** argv) {
    scenario = "case setup";
    define_cases();
    CHECK(argc == 2);
    for (unsigned i = 0; i < case_count; i++) {
        if (strcmp(argv[1], "--list") == 0)
            puts(cases[i].name);
        else if (strcmp(argv[1], cases[i].name) == 0) {
            scenario = cases[i].name;
            if (cases[i].run)
                cases[i].run(&cases[i]);
            else
                run(&cases[i]);
            return 0;
        }
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
