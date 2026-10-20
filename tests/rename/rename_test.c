#include "host.h"
#include "types.inc"
static const char* scenario;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)
static struct { void* pointer; size_t size; } pools[64];
static unsigned attempts, fail_at, live;
static bool executing;
static void* request_utf8;
static void* allocate(size_t bytes) {
    if (++attempts == fail_at)
        return NULL;
    void* pointer = malloc(bytes ? bytes : 1);
    CHECK(pointer);
    memset(pointer, 0xa5, bytes);
    for (unsigned i = 0; i < 64; i++) {
        if (!pools[i].pointer) {
            pools[i].pointer = pointer;
            pools[i].size = bytes;
            live++;
            if (executing && attempts == 1)
                request_utf8 = pointer;
            return pointer;
        }
    }
    CHECK(false);
    return NULL;
}
static void ExFreePool(void* pointer) {
    CHECK(pointer);
    for (unsigned i = 0; i < 64; i++) {
        if (pools[i].pointer == pointer) {
            pools[i].pointer = NULL;
            live--;
            free(pointer);
            return;
        }
    }
    CHECK(false);
}
#include "free_fileref.inc"
#include "services.inc"
#include "valid_name.inc"
#include "crc.inc"
#define calc_crc32c calc_crc32c_sw
#include "same_directory.inc"
#include "dispatch.inc"

enum route { RELATIVE, TARGET, ABSOLUTE, OTHER_DIRECTORY, OTHER_SUBVOLUME };
enum behavior { NORMAL, SELF, REPLACE, COLLISION, DENIED, OPEN_CHILD, BAD_NAME, PARENT_MISSING,
                PARENT_REPARSE, DIRECTORY, LONG_NAME, UNICODE_NAME, STREAM, REAPABLE_PARENT };
typedef struct {
    const char* name;
    enum route route;
    enum behavior behavior;
    unsigned allocation_failure, path_failure, repeats;
    bool move_failure;
    NTSTATUS status;
} test_case;
static const test_case cases[] = {
    {"relative-file", RELATIVE, NORMAL, 0, 0, 1, false, STATUS_SUCCESS},
    {"target-directory-file", TARGET, NORMAL, 0, 0, 1, false, STATUS_SUCCESS},
    {"absolute-parent-lookup", ABSOLUTE, NORMAL, 0, 0, 1, false, STATUS_SUCCESS},
    {"relative-directory", RELATIVE, DIRECTORY, 0, 0, 1, false, STATUS_SUCCESS},
    {"target-directory", TARGET, DIRECTORY, 0, 0, 1, false, STATUS_SUCCESS},
    {"rename-to-self", RELATIVE, SELF, 0, 0, 1, false, STATUS_SUCCESS},
    {"replace-file", TARGET, REPLACE, 0, 0, 1, false, STATUS_SUCCESS},
    {"relative-long-name", RELATIVE, LONG_NAME, 0, 0, 1, false, STATUS_SUCCESS},
    {"target-unicode-name", TARGET, UNICODE_NAME, 0, 0, 1, false, STATUS_SUCCESS},
    {"repeated-renames", RELATIVE, NORMAL, 0, 0, 100, false, STATUS_SUCCESS},
    {"parent-can-reach-zero-after-close", RELATIVE, REAPABLE_PARENT, 0, 0, 1, false, STATUS_SUCCESS},
    {"collision", RELATIVE, COLLISION, 0, 0, 1, false, STATUS_OBJECT_NAME_COLLISION},
    {"access-denied", TARGET, DENIED, 0, 0, 1, false, STATUS_ACCESS_DENIED},
    {"source-has-open-children", RELATIVE, OPEN_CHILD, 0, 0, 1, false, STATUS_ACCESS_DENIED},
    {"invalid-name", RELATIVE, BAD_NAME, 0, 0, 1, false, STATUS_OBJECT_NAME_INVALID},
    {"parent-lookup-failure", ABSOLUTE, PARENT_MISSING, 0, 0, 1, false, STATUS_OBJECT_PATH_NOT_FOUND},
    {"parent-lookup-reparse", ABSOLUTE, PARENT_REPARSE, 0, 0, 1, false, STATUS_OBJECT_PATH_NOT_FOUND},
    {"request-allocation-failure", RELATIVE, NORMAL, 1, 0, 1, false, STATUS_INSUFFICIENT_RESOURCES},
    {"old-path-allocation-failure", RELATIVE, NORMAL, 2, 0, 1, false, STATUS_INSUFFICIENT_RESOURCES},
    {"old-name-allocation-failure", TARGET, NORMAL, 3, 0, 1, false, STATUS_INSUFFICIENT_RESOURCES},
    {"new-utf8-allocation-failure", RELATIVE, NORMAL, 4, 0, 1, false, STATUS_INSUFFICIENT_RESOURCES},
    {"new-name-allocation-failure", TARGET, NORMAL, 5, 0, 1, false, STATUS_INSUFFICIENT_RESOURCES},
    {"uppercase-allocation-failure", RELATIVE, NORMAL, 6, 0, 1, false, STATUS_INSUFFICIENT_RESOURCES},
    {"new-path-allocation-failure", TARGET, NORMAL, 7, 0, 1, false, STATUS_INSUFFICIENT_RESOURCES},
    {"old-path-query-failure", RELATIVE, NORMAL, 0, 1, 1, false, STATUS_UNSUCCESSFUL},
    {"old-path-fill-failure", TARGET, NORMAL, 0, 2, 1, false, STATUS_UNSUCCESSFUL},
    {"new-path-query-failure", RELATIVE, NORMAL, 0, 3, 1, false, STATUS_UNSUCCESSFUL},
    {"new-path-fill-failure", TARGET, NORMAL, 0, 4, 1, false, STATUS_UNSUCCESSFUL},
    {"cross-directory-transfer", OTHER_DIRECTORY, NORMAL, 0, 0, 1, false, STATUS_SUCCESS},
    {"cross-subvolume-transfer", OTHER_SUBVOLUME, NORMAL, 0, 0, 1, false, STATUS_SUCCESS},
    {"cross-directory-failure", OTHER_DIRECTORY, NORMAL, 0, 0, 1, true, STATUS_INSUFFICIENT_RESOURCES},
    {"cross-subvolume-failure", OTHER_SUBVOLUME, NORMAL, 0, 0, 1, true, STATUS_INSUFFICIENT_RESOURCES},
    {"stream-dispatch", RELATIVE, STREAM, 0, 0, 1, false, STATUS_SUCCESS},
};

static UNICODE_STRING make_name(const WCHAR* text, size_t length) {
    UNICODE_STRING result = {(USHORT)(length * 2), (USHORT)(length * 2), allocate(length * 2)};
    memcpy(result.Buffer, text, length * 2);
    return result;
}
static void run_once(const test_case* c, bool extended) {
    attempts = fail_at = 0;
    executing = false;
    device_extension volume = {.superblock.generation = 7};
    root tree = {.id = 257}, other_tree = {.id = 258};
    ERESOURCE resources[3] = {0};
    fcb_nonpaged nonpaged = {0};
    fcb parent_node = {.Vcb = &volume, .subvol = &tree, .inode = 100, .type = BTRFS_FT_DIR,
                       .Header.Resource = &resources[0], .nonpaged = &nonpaged};
    fcb destination_node = {.Vcb = &volume, .subvol = c->route == OTHER_SUBVOLUME ? &other_tree : &tree,
                            .inode = 101, .Header.Resource = &resources[1]};
    fcb node = {.Vcb = &volume, .subvol = &tree, .inode = 1000,
               .type = c->behavior == DIRECTORY ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE,
               .Header.Resource = &resources[2]};
    fcb collision_node = {.Vcb = &volume, .subvol = &tree, .Header.Resource = &resources[1]};
    LONG parent_base = c->behavior == REAPABLE_PARENT ? 1 : 2;
    file_ref parent = {.fcb = &parent_node, .refcount = parent_base};
    file_ref target = {.fcb = &destination_node, .refcount = 1};
    file_ref existing = {.fcb = &collision_node, .refcount = 1};
    WCHAR original[] = {'o','l','d','.','t','x','t'};
    dir_child dc = {.name = make_name(original, 7), .name_uc = make_name(original, 7),
                    .utf8 = {.Length = 7, .MaximumLength = 7, .Buffer = allocate(7)}};
    memcpy(dc.utf8.Buffer, "old.txt", 7);
    parent_node.inode_item.size = 1000;
    file_ref ref = {.fcb = &node, .parent = &parent, .dc = &dc, .refcount = 1};
    ccb context = {.fileref = &ref};
    FILE_OBJECT object = {.FsContext = &node, .FsContext2 = &context};
    source_ref = &ref;
    bool cross = c->route == OTHER_DIRECTORY || c->route == OTHER_SUBVOLUME;
    destination = cross ? &target : &parent;
    ccb target_context = {.fileref = destination};
    FILE_OBJECT target_object = {.FsContext2 = &target_context};
    union { uint64_t align; unsigned char bytes[1024]; } storage = {0};
    FILE_RENAME_INFORMATION_EX* request = (void*)storage.bytes;
    WCHAR wanted[255];
    unsigned units = 7;
    memcpy(wanted, (WCHAR[]){'n','e','w','.','t','x','t'}, 14);
    if (c->behavior == LONG_NAME) {
        units = 200;
        for (unsigned i = 0; i < units; i++) wanted[i] = 'a' + i % 26;
    } else if (c->behavior == UNICODE_NAME) {
        units = 4;
        memcpy(wanted, (WCHAR[]){0x6587, 0x4ef6, 0x00e9, 'x'}, 8);
    } else if (c->behavior == BAD_NAME)
        wanted[0] = '*';
    if (c->route == ABSOLUTE) {
        memmove(wanted + 1, wanted, units * 2);
        wanted[0] = '\\';
        units++;
    }
    memcpy(request->FileName, wanted, units * 2);
    request->FileNameLength = units * 2;
    if (extended)
        request->Flags = c->behavior == REPLACE ? FILE_RENAME_REPLACE_IF_EXISTS : 0;
    else
        request->ReplaceIfExists = c->behavior == REPLACE;
    collision = c->behavior == SELF ? &ref : c->behavior == REPLACE || c->behavior == COLLISION ? &existing : NULL;
    deny_access = c->behavior == DENIED;
    children_open = c->behavior == OPEN_CHILD;
    parent_lookup_status = c->behavior == PARENT_REPARSE ? STATUS_REPARSE :
                           c->behavior == PARENT_MISSING ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_SUCCESS;
    cross_volume = c->route == OTHER_SUBVOLUME;
    fail_move = c->move_failure;
    fail_path_call = c->path_failure;
    old_parent_hold = NULL;
    node.ads = c->behavior == STREAM;
    IRP irp = {.AssociatedIrp.SystemBuffer = request, .RequestorMode = UserMode};
    PFILE_OBJECT tfo = c->route == TARGET || cross ? &target_object : NULL;
    for (unsigned repetition = 0; repetition < c->repeats; repetition++) {
        subject_contexts = clear_calls = rollback_calls = path_calls = notifications = move_calls = 0;
        request_utf8 = NULL;
        attempts = 0;
        fail_at = c->allocation_failure;
        executing = true;
        NTSTATUS status = set_rename_information(&volume, &irp, &object, tfo, extended);
        executing = false;
        CHECK(status == c->status);
        CHECK(parent.refcount == parent_base && target.refcount == (cross && NT_SUCCESS(status) ? 2 : 1));
        CHECK(ref.refcount == 1 && existing.refcount == 1);
        CHECK(!volume.tree_lock.held && !volume.fileref_lock.held && !subject_contexts && !nonpaged.dir_children_lock.held);
        for (unsigned i = 0; i < 3; i++) CHECK(!resources[i].held);
        CHECK(clear_calls == (unsigned)NT_SUCCESS(status) && rollback_calls == (unsigned)!NT_SUCCESS(status));
        if (c->allocation_failure) CHECK(attempts == c->allocation_failure);
        if (NT_SUCCESS(status) && !cross && c->behavior != STREAM) {
            CHECK(ref.parent == &parent && dc.name.Length == units * 2);
            CHECK(memcmp(dc.name.Buffer, wanted, dc.name.Length) == 0);
            CHECK(node.inode_item_changed && parent_node.inode_item_changed && notifications == 3);
            CHECK(parent_node.inode_item.size == UINT64_C(1000) + 2 * dc.utf8.Length - 14);
        }
        CHECK(move_calls == (unsigned)cross);
        /* The dispatcher currently retains this separate temporary on success and
         * error paths. Record that existing leak explicitly and reclaim it only in
         * fixture teardown; this regression targets parent-reference ownership. */
        if (request_utf8) ExFreePool(request_utf8);
    }
    free_fileref(ref.parent);
    if (old_parent_hold) free_fileref(old_parent_hold);
    CHECK(parent.refcount == parent_base - 1 && target.refcount == 1);
    ExFreePool(dc.name.Buffer);
    ExFreePool(dc.name_uc.Buffer);
    ExFreePool(dc.utf8.Buffer);
    if (ref.oldutf8.Buffer) ExFreePool(ref.oldutf8.Buffer);
    CHECK(!live);
    source_ref = destination = collision = old_parent_hold = NULL;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    for (unsigned ex = 0; ex < 2; ex++) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char label[128];
            snprintf(label, sizeof(label), "%s-%s", ex ? "extended" : "legacy", cases[i].name);
            if (strcmp(argv[1], "--list") == 0) puts(label);
            else if (strcmp(argv[1], label) == 0) {
                scenario = argv[1];
                run_once(&cases[i], ex != 0);
                return 0;
            }
        }
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
