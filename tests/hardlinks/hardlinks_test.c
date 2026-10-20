#include "host.h"
#include "types.inc"

static const char* scenario;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)
_Static_assert(offsetof(FILE_LINK_ENTRY_FULL_ID_INFORMATION, FileName) == 24, "Windows full ID name offset");
_Static_assert(offsetof(FILE_LINKS_FULL_ID_INFORMATION, Entry) == 8, "Windows full ID list header");

static device_extension volume;
static ERESOURCE file_resource;
static root subvolume;
static file_ref parents[2];
static unsigned opens, failed_opens;
static NTSTATUS open_status[2];

static void ExAcquireResourceExclusiveLite(ERESOURCE* resource, bool wait) {
    CHECK(wait && resource->held == 0);
    resource->held = 1;
}
static void ExAcquireResourceSharedLite(ERESOURCE* resource, bool wait) {
    CHECK(wait && resource->held == 0 && volume.fileref_lock.held == 1);
    resource->held = 2;
}
static void ExReleaseResourceLite(ERESOURCE* resource) {
    CHECK(resource->held != 0);
    if (resource == &volume.fileref_lock)
        CHECK(file_resource.held == 0);
    resource->held = 0;
}
static LONG InterlockedDecrement(LONG* value) {
    CHECK(*value > 1);
    return --*value;
}
#include "free_fileref.inc"

static NTSTATUS open_fileref_by_inode(device_extension* vcb, root* tree, uint64_t inode,
                                     file_ref** out, PIRP irp) {
    CHECK(vcb == &volume && tree == &subvolume && irp);
    CHECK(volume.fileref_lock.held == 1 && file_resource.held == 2);
    for (unsigned i = 0; i < 2; i++) {
        if (parents[i].fcb->inode == inode) {
            opens++;
            if (!NT_SUCCESS(open_status[i])) {
                failed_opens++;
                return open_status[i];
            }
            parents[i].refcount++;
            *out = &parents[i];
            return STATUS_SUCCESS;
        }
    }
    CHECK(false);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
#include "queries.inc"

enum shape { SINGLE, MISSING_DC, ADS, ROOT, SUBVOLUME, FALLBACK, CACHED, DELETED_CHILD,
             DUMMY_BEFORE_CHILD, DUMMY_ONLY, DELETED_PARENT, MISSING_PARENT, OOM_PARENT,
             MIXED_PARENTS, SAME_PARENT, MULTIPLE, MULTIPLE_ODD, UNICODE_NAME };
enum capacity { NORMAL, SHORT_HEADER, HEADER_ONLY, EXACT, ONE_SHORT, FIRST_ONLY };
typedef struct { const char* name; enum shape shape; enum capacity capacity; unsigned repeats; } test_case;
static const test_case cases[] = {
    {"single", SINGLE, NORMAL, 1},
    {"single-exact-buffer", SINGLE, EXACT, 1},
    {"single-short-buffer", SINGLE, ONE_SHORT, 1},
    {"single-missing-directory-entry", MISSING_DC, NORMAL, 1},
    {"stream-query", ADS, NORMAL, 1},
    {"short-header", SINGLE, SHORT_HEADER, 1},
    {"root", ROOT, NORMAL, 1},
    {"subvolume-root", SUBVOLUME, NORMAL, 1},
    {"uncached-link", FALLBACK, NORMAL, 1},
    {"cached-renamed-link", CACHED, NORMAL, 1},
    {"cached-deleted-link", DELETED_CHILD, NORMAL, 1},
    {"dummy-before-cached-link", DUMMY_BEFORE_CHILD, NORMAL, 1},
    {"dummy-with-uncached-link", DUMMY_ONLY, NORMAL, 1},
    {"deleted-parent", DELETED_PARENT, NORMAL, 1},
    {"deleted-parent-repeated", DELETED_PARENT, NORMAL, 100},
    {"deleted-parent-small-buffer", DELETED_PARENT, HEADER_ONLY, 1},
    {"missing-parent", MISSING_PARENT, NORMAL, 1},
    {"parent-allocation-failure", OOM_PARENT, NORMAL, 1},
    {"deleted-and-live-parents", MIXED_PARENTS, NORMAL, 1},
    {"two-links-same-parent", SAME_PARENT, NORMAL, 1},
    {"two-parents", MULTIPLE, NORMAL, 1},
    {"two-parents-exact-buffer", MULTIPLE, EXACT, 1},
    {"two-parents-partial-buffer", MULTIPLE, FIRST_ONLY, 1},
    {"two-parents-header-only", MULTIPLE, HEADER_ONLY, 1},
    {"two-parents-unaligned-header-only", MULTIPLE_ODD, HEADER_ONLY, 1},
    {"unicode-name", UNICODE_NAME, NORMAL, 1},
};

static UNICODE_STRING name(const char* text, WCHAR* buffer) {
    size_t n = strlen(text);
    for (size_t i = 0; i < n; i++)
        buffer[i] = (unsigned char)text[i];
    return (UNICODE_STRING){(uint16_t)(n * 2), (uint16_t)(n * 2), buffer};
}
typedef struct { UNICODE_STRING name; uint64_t inode, subvol; } expected_entry;

static void run(const test_case* c, bool full) {
    memset(&volume, 0, sizeof(volume));
    memset(&file_resource, 0, sizeof(file_resource));
    subvolume.id = UINT64_C(0x1234567887654321);
    WCHAR names[4][64];
    UNICODE_STRING original = name("original", names[0]);
    UNICODE_STRING cached = name("renamed-longer-name", names[1]);
    UNICODE_STRING other = name("other", names[2]);
    if (c->shape == MULTIPLE_ODD)
        original = name("a", names[0]);
    WCHAR unicode[] = {0x6587, 0x4ef6, 0xd83d, 0xde80};
    if (c->shape == UNICODE_NAME)
        original = (UNICODE_STRING){sizeof(unicode), sizeof(unicode), unicode};
    fcb parent_nodes[2] = {
        {.inode = UINT64_C(0x1234567812345678), .subvol = &subvolume},
        {.inode = UINT64_C(0x7654321876543210), .subvol = &subvolume},
    };
    memset(parents, 0, sizeof(parents));
    for (unsigned i = 0; i < 2; i++) {
        parents[i].fcb = &parent_nodes[i];
        parents[i].refcount = 1;
        InitializeListHead(&parents[i].children);
        open_status[i] = STATUS_SUCCESS;
    }
    fcb node = {.Vcb = &volume, .subvol = &subvolume, .inode = 1000,
                .Header.Resource = &file_resource};
    InitializeListHead(&node.hardlinks);
    dir_child dc[2] = {{.index = 5, .name = original}, {.index = 5, .name = cached}};
    file_ref ref = {.fcb = &node, .parent = &parents[0], .dc = &dc[0]};
    file_ref root_ref = {.fcb = &parent_nodes[0]};
    volume.root_fileref = &root_ref;
    hardlink links[2] = {
        {.parent = parent_nodes[0].inode, .index = 5, .name = original},
        {.parent = parent_nodes[1].inode, .index = 6, .name = other},
    };
    file_ref children[2] = {{.dc = NULL}, {.dc = &dc[1]}};
    expected_entry expected[2] = {{original, parent_nodes[0].inode, subvolume.id}};
    unsigned expected_count = 1, link_count = 0;
    NTSTATUS want = STATUS_SUCCESS;
    switch (c->shape) {
        case SINGLE: case UNICODE_NAME: break;
        case MISSING_DC: ref.dc = NULL; want = STATUS_INVALID_PARAMETER; break;
        case ADS: node.ads = true; want = STATUS_INVALID_PARAMETER; break;
        case ROOT:
            node.inode = BTRFS_FIRST_FREE_OBJECTID;
            volume.root_fileref = &ref;
            ref.dc = NULL;
            expected[0] = (expected_entry){name(".", names[3]), 0, 0};
            break;
        case SUBVOLUME:
            node.inode = BTRFS_FIRST_FREE_OBJECTID;
            if (!full)
                expected[0].inode = 0;
            break;
        case FALLBACK: link_count = 1; break;
        case CACHED: case DELETED_CHILD: case DUMMY_BEFORE_CHILD: case DUMMY_ONLY:
            link_count = 1;
            if (c->shape == DUMMY_BEFORE_CHILD || c->shape == DUMMY_ONLY)
                InsertTailList(&parents[0].children, &children[0].list_entry);
            if (c->shape != DUMMY_ONLY) {
                InsertTailList(&parents[0].children, &children[1].list_entry);
                expected[0].name = cached;
            }
            if (c->shape == DELETED_CHILD) {
                children[1].deleted = true;
                expected_count = 0;
            }
            break;
        case DELETED_PARENT:
            link_count = 1;
            parents[0].deleted = true;
            expected_count = 0;
            break;
        case MISSING_PARENT: case OOM_PARENT:
            link_count = 1;
            open_status[0] = c->shape == MISSING_PARENT ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_INSUFFICIENT_RESOURCES;
            expected_count = 0;
            break;
        case MIXED_PARENTS:
            link_count = 2;
            parents[0].deleted = true;
            expected[0] = (expected_entry){other, parent_nodes[1].inode, subvolume.id};
            break;
        case SAME_PARENT: case MULTIPLE: case MULTIPLE_ODD:
            link_count = expected_count = 2;
            if (c->shape == SAME_PARENT)
                links[1].parent = parent_nodes[0].inode;
            expected[1] = (expected_entry){other, links[1].parent, subvolume.id};
            break;
    }
    for (unsigned i = 0; i < link_count; i++)
        InsertTailList(&node.hardlinks, &links[i].list_entry);
    unsigned needed = 8, first_size = 0;
    for (unsigned i = 0; i < expected_count; i++) {
        needed = (unsigned)sector_align(needed, 8);
        needed += (full ? 24 : 22) + expected[i].name.Length;
        if (!i)
            first_size = needed;
    }
    unsigned capacity = 512;
    switch (c->capacity) {
        case NORMAL: break;
        case SHORT_HEADER: capacity = 7; want = STATUS_INVALID_PARAMETER; break;
        case HEADER_ONLY: capacity = 8; break;
        case EXACT: capacity = needed; break;
        case ONE_SHORT: capacity = needed - 1; break;
        case FIRST_ONLY: capacity = first_size; break;
    }
    unsigned returned = 0, used = 8, cursor = 8;
    if (want == STATUS_SUCCESS) {
        for (unsigned i = 0; i < expected_count; i++) {
            cursor = (unsigned)sector_align(cursor, 8) + (full ? 24 : 22) + expected[i].name.Length;
            if (cursor <= capacity) {
                returned++;
                used = cursor;
            }
        }
        if (needed > capacity)
            want = STATUS_BUFFER_OVERFLOW;
    }
    for (unsigned repeat = 0; repeat < c->repeats; repeat++) {
        union { uint64_t align; unsigned char bytes[544]; } output;
        memset(output.bytes, 0xa5, sizeof(output.bytes));
        unsigned char* buffer = output.bytes + 16;
        LONG remaining = (LONG)capacity;
        IRP irp = {0};
        opens = failed_opens = 0;
        NTSTATUS status = full ? fill_in_hard_link_full_id_information((void*)buffer, &ref, &irp, &remaining) :
                                 fill_in_hard_link_information((void*)buffer, &ref, &irp, &remaining);
        CHECK(status == want);
        CHECK(!volume.fileref_lock.held && !file_resource.held);
        CHECK(parents[0].refcount == 1 && parents[1].refcount == 1);
        CHECK(opens == (want == STATUS_INVALID_PARAMETER ? 0 : link_count));
        CHECK(failed_opens == (unsigned)(c->shape == MISSING_PARENT || c->shape == OOM_PARENT));
        for (unsigned i = 0; i < 16; i++)
            CHECK(output.bytes[i] == 0xa5);
        for (unsigned i = 16 + capacity; i < sizeof(output.bytes); i++)
            CHECK(output.bytes[i] == 0xa5);
        if (want == STATUS_INVALID_PARAMETER) {
            CHECK(remaining == (LONG)capacity);
            continue;
        }
        ULONG header[2];
        memcpy(header, buffer, sizeof(header));
        CHECK(header[0] == needed && header[1] == returned);
        CHECK(remaining == (LONG)(capacity - used));
        unsigned offset = 8;
        for (unsigned i = 0; i < returned; i++) {
            ULONG next, units;
            uint64_t inode, tree = 0;
            memcpy(&next, buffer + offset, 4);
            memcpy(&inode, buffer + offset + (full ? 4 : 8), 8);
            if (full)
                memcpy(&tree, buffer + offset + 12, 8);
            memcpy(&units, buffer + offset + (full ? 20 : 16), 4);
            CHECK(inode == expected[i].inode && (!full || tree == expected[i].subvol));
            CHECK(units * 2 == expected[i].name.Length);
            CHECK(memcmp(buffer + offset + (full ? 24 : 20), expected[i].name.Buffer, units * 2) == 0);
            CHECK(next == (i + 1 == returned ? 0 : sector_align((full ? 24 : 22) + units * 2, 8)));
            offset += next;
        }
    }
    volume.root_fileref = NULL;
    for (unsigned i = 0; i < 2; i++)
        parents[i].fcb = NULL;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    for (unsigned format = 0; format < 2; format++) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char label[128];
            snprintf(label, sizeof(label), "%s-%s", format ? "full-id" : "ordinary", cases[i].name);
            if (strcmp(argv[1], "--list") == 0)
                puts(label);
            else if (strcmp(argv[1], label) == 0) {
                scenario = label;
                run(&cases[i], format != 0);
                return 0;
            }
        }
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
