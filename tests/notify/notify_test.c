/* Deliberately delay real notification workers across caller name changes. */
#include "host.h"

enum allocation_kind { SOURCE, POOL, WORK };
typedef struct { void* ptr; size_t size; enum allocation_kind kind; } allocation;
typedef struct { bool present; USHORT length; WCHAR* bytes; ULONG filter, action; } report;
static allocation allocations[32];
static PIO_WORKITEM queued[4];
static report reports[4];
static unsigned queued_count, report_count, current_report, attempted_pool, attempted_work;
static unsigned fail_pool, fail_work, completed_work, reported;
static bool shutting_down;
static DEVICE_OBJECT master = { .marker = 42 };
static PDEVICE_OBJECT master_devobj = &master;
static device_extension volume;
static root subvol;
static fcb node;
static file_ref fileref;
static const WCHAR path[] = { '\\', 'f', 'i', 'l', 'e' };

enum parent_state { PARENT_LIVE, PARENT_DELETED, PARENT_MISSING };
enum path_failure { PATH_OK, PATH_QUERY_FAILED, PATH_FILL_FAILED, PATH_POOL_FAILED, PATH_TOO_LONG };
static bool hardlink_mode, shared_parent;
static enum parent_state parent_states[3];
static enum path_failure path_failure;
static file_ref parents[3];
static hardlink links[3];
static WCHAR parent_paths[3][6], link_names[3][3];
static ULONG parent_lengths[3];
static LONG parent_initial_refs;
static unsigned lookup_calls, query_calls, fill_calls, next_link_report;
static unsigned expected_links[3], expected_link_count;
static bool live_range(const void* p, size_t size);

static void check_parent_locks(void) {
    CHECK(volume.tree_lock.shared == 1 && volume.fileref_lock.exclusive == 1);
    CHECK(fileref.refcount > 0);
}

static NTSTATUS parent_filename(file_ref* fr, PUNICODE_STRING name, USHORT* offset, ULONG* length) {
    check_parent_locks();
    unsigned i;
    for (i = 0; i < 3 && fr != &parents[i]; i++) {}
    CHECK(i < 3 && fr->refcount > 0 && !fr->deleted && offset == NULL);
    if (!name->MaximumLength) {
        query_calls++;
        CHECK(length != NULL);
        if (path_failure == PATH_QUERY_FAILED) return STATUS_OBJECT_NAME_NOT_FOUND;
        *length = path_failure == PATH_TOO_LONG ? 65534 : parent_lengths[i];
        return STATUS_BUFFER_OVERFLOW;
    }
    fill_calls++;
    CHECK(name->MaximumLength >= parent_lengths[i] && live_range(name->Buffer, name->MaximumLength));
    if (path_failure == PATH_FILL_FAILED) return STATUS_OBJECT_NAME_NOT_FOUND;
    CHECK(path_failure != PATH_TOO_LONG);
    memcpy(name->Buffer, parent_paths[i], parent_lengths[i]);
    name->Length = (USHORT)parent_lengths[i];
    return STATUS_SUCCESS;
}

static void check_link_report(PSTRING name, USHORT offset) {
    CHECK(next_link_report < expected_link_count);
    unsigned i = expected_links[next_link_report++];
    unsigned p = shared_parent ? 0 : i;
    bool root_parent = &parents[p] == volume.root_fileref;
    size_t prefix = parent_lengths[p] + (root_parent ? 0 : sizeof(WCHAR));
    CHECK(name->Length == prefix + links[i].name.Length && offset == prefix);
    CHECK(live_range(name->Buffer, name->Length));
    CHECK(memcmp(name->Buffer, parent_paths[p], parent_lengths[p]) == 0);
    if (!root_parent) CHECK(((WCHAR*)name->Buffer)[parent_lengths[p] / sizeof(WCHAR)] == '\\');
    CHECK(memcmp(name->Buffer + prefix, links[i].name.Buffer, links[i].name.Length) == 0);
}

static void* allocate(size_t size, enum allocation_kind kind) {
    void* p = malloc(size ? size : 1); CHECK(p != NULL);
    memset(p, 0xa5, size);
    for (unsigned i = 0; i < 32; i++) {
        if (!allocations[i].ptr) { allocations[i] = (allocation){ p, size, kind }; return p; }
    }
    CHECK(false); return NULL;
}

static bool live_range(const void* p, size_t size) {
    uintptr_t address = (uintptr_t)p;
    for (unsigned i = 0; i < 32; i++) {
        uintptr_t start = (uintptr_t)allocations[i].ptr;
        if (start && address >= start && address - start <= allocations[i].size &&
            size <= allocations[i].size - (address - start)) return true;
    }
    return false;
}

static void release(void* p, enum allocation_kind kind) {
    for (unsigned i = 0; i < 32; i++) {
        if (allocations[i].ptr == p && p) {
            CHECK(allocations[i].kind == kind);
            allocations[i].ptr = NULL; free(p); return;
        }
    }
    CHECK(false);
}

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK(pool == PagedPool && tag == ALLOC_TAG);
    return ++attempted_pool == fail_pool ? NULL : allocate(size, POOL);
}

static void ExFreePool(void* p) { release(p, POOL); }

static PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT object) {
    CHECK(object == master_devobj);
    if (++attempted_work == fail_work) return NULL;
    PIO_WORKITEM item = allocate(sizeof(*item), WORK);
    item->number = attempted_work; return item;
}

static void IoFreeWorkItem(PIO_WORKITEM item) {
    CHECK(volume.tree_lock.shared == 0 && volume.fileref_lock.exclusive == 0);
    release(item, WORK); completed_work++;
}

static void IoQueueWorkItem(PIO_WORKITEM item, work_callback callback, unsigned queue, void* context) {
    CHECK(queue == DelayedWorkQueue && queued_count < 4 && fileref.refcount >= 2);
    CHECK(live_range(context, 1) && live_range(item, sizeof(*item)));
    item->callback = callback; item->context = context; queued[queued_count++] = item;
}

static void ExAcquireResourceSharedLite(ERESOURCE* r, bool wait) {
    CHECK(r == &volume.tree_lock && wait && !r->shared && !r->exclusive);
    CHECK(fileref.refcount > 0); r->shared++;
}

static void ExAcquireResourceExclusiveLite(ERESOURCE* r, bool wait) {
    CHECK(r == &volume.fileref_lock && wait && !r->shared && !r->exclusive);
    CHECK(volume.tree_lock.shared == 1); r->exclusive++;
}

static void ExReleaseResourceLite(ERESOURCE* r) {
    if (r == &volume.tree_lock) {
        CHECK(r->shared == 1 && volume.fileref_lock.exclusive == 0); r->shared--;
    } else {
        CHECK(r == &volume.fileref_lock && r->exclusive == 1); r->exclusive--;
    }
}

static NTSTATUS fileref_get_filename(file_ref* fr, PUNICODE_STRING name, USHORT* offset, ULONG* length) {
    if (hardlink_mode) return parent_filename(fr, name, offset, length);
    CHECK(fr == &fileref && fileref.refcount > 0);
    CHECK(volume.tree_lock.shared == 1 && volume.fileref_lock.exclusive == 1);
    if (length) *length = sizeof(path);
    if (!name->MaximumLength) return STATUS_BUFFER_OVERFLOW;
    CHECK(name->MaximumLength >= sizeof(path) && live_range(name->Buffer, sizeof(path)));
    memcpy(name->Buffer, path, sizeof(path)); name->Length = sizeof(path);
    if (offset) *offset = sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static NTSTATUS open_fileref_by_inode(device_extension* v, root* r, uint64_t inode, file_ref** fr, void* irp) {
    CHECK(hardlink_mode && v == &volume && r == &subvol && irp == NULL);
    check_parent_locks();
    unsigned i = lookup_calls++ % 3;
    CHECK(inode == links[i].parent);
    unsigned p = shared_parent ? 0 : i;
    if (parent_states[p] == PARENT_MISSING) return STATUS_OBJECT_NAME_NOT_FOUND;
    *fr = &parents[p];
    InterlockedIncrement(&(*fr)->refcount);
    return STATUS_SUCCESS;
}

static void FsRtlNotifyFilterReportChange(void* sync, LIST_ENTRY* list, PSTRING name,
    USHORT offset, PSTRING stream, PSTRING parent, ULONG filter, ULONG action,
    void* target, void* context) {
    CHECK(sync == volume.NotifySync && list == &volume.DirNotifyList);
    CHECK(parent == NULL && target == NULL && context == NULL);
    CHECK(volume.tree_lock.shared == 1 && volume.fileref_lock.exclusive == 1);
    if (hardlink_mode) check_link_report(name, offset);
    else {
        CHECK(name->Length == sizeof(path) && offset == sizeof(WCHAR));
        CHECK(memcmp(name->Buffer, path, sizeof(path)) == 0);
    }
    CHECK(current_report < report_count);
    report* wanted = &reports[current_report];
    CHECK(filter == wanted->filter && action == wanted->action);
    CHECK((stream != NULL) == wanted->present);
    if (stream) {
        /* Check lifetime before touching a potentially retired descriptor. */
        CHECK(live_range(stream, sizeof(*stream)));
        CHECK(stream->Length == wanted->length && stream->MaximumLength >= stream->Length);
        if (stream->Length) {
            CHECK(live_range(stream->Buffer, stream->Length));
            CHECK(memcmp(stream->Buffer, wanted->bytes, stream->Length) == 0);
        }
    }
    reported++;
}

#include "free_fileref.inc"
#include "context.inc"
#include "notify.inc"

static PUNICODE_STRING make_name(USHORT length, unsigned seed) {
    PUNICODE_STRING name = allocate(sizeof(*name), SOURCE);
    USHORT capacity = length <= 65526 ? length + 8 : length;
    *name = (UNICODE_STRING){ .Length = length, .MaximumLength = capacity,
        .Buffer = allocate(capacity, SOURCE) };
    for (unsigned i = 0; i < length / sizeof(WCHAR); i++)
        name->Buffer[i] = (WCHAR)((i % 5 == 0 ? 0x3bb : 'a') + seed + i % 7);
    return name;
}

static void retire_name(PUNICODE_STRING name) {
    if (name->Buffer) release(name->Buffer, SOURCE);
    release(name, SOURCE);
}

static void expect_report(PUNICODE_STRING name, ULONG filter, ULONG action) {
    CHECK(report_count < 4);
    report* r = &reports[report_count++];
    *r = (report){ .present = name != NULL, .length = name ? name->Length : 0,
                  .filter = filter, .action = action };
    if (r->length) { r->bytes = malloc(r->length); CHECK(r->bytes); memcpy(r->bytes, name->Buffer, r->length); }
}

static void drain(bool reverse) {
    for (unsigned j = 0; j < queued_count; j++) {
        current_report = reverse ? queued_count - j - 1 : j;
        PIO_WORKITEM item = queued[current_report];
        CHECK(live_range(item, sizeof(*item)));
        item->callback(master_devobj, item->context);
    }
    CHECK(completed_work == queued_count && reported == report_count);
    CHECK(fileref.refcount == 0);
}

enum lifetime { STABLE, MUTATE, REPLACE, FREE_BUFFER, FREE_NAME };
static void run_name(USHORT length, enum lifetime change) {
    PUNICODE_STRING name = make_name(length, 0);
    expect_report(name, 0x40, 10);
    queue_notification_fcb(&fileref, 0x40, 10, name);
    CHECK(queued_count == 1 && fileref.refcount == 2 && !reported);
    if (change == MUTATE) {
        for (unsigned i = 0; i < length / sizeof(WCHAR); i++) name->Buffer[i] ^= 0x123;
    } else if (change == REPLACE) {
        release(name->Buffer, SOURCE);
        name->Length = 6; name->MaximumLength = 6;
        name->Buffer = allocate(6, SOURCE); memset(name->Buffer, 0x42, 6);
    } else if (change == FREE_BUFFER) {
        release(name->Buffer, SOURCE);
        /* Keep the original pointer as a real deleted dir_child would. */
    } else if (change == FREE_NAME) {
        retire_name(name);
    }
    free_fileref(&fileref);
    drain(false);
    if (change == FREE_BUFFER) release(name, SOURCE);
    else if (change != FREE_NAME) retire_name(name);
}

static void run_batch(bool reverse) {
    PUNICODE_STRING name = make_name(12, 0);
    for (unsigned i = 0; i < 3; i++) {
        expect_report(name, 0x20 << i, 3 + i);
        queue_notification_fcb(&fileref, 0x20 << i, 3 + i, name);
        for (unsigned j = 0; j < name->Length / sizeof(WCHAR); j++) name->Buffer[j]++;
    }
    retire_name(name);
    CHECK(fileref.refcount == 4);
    free_fileref(&fileref);
    drain(reverse);
}

static void run_control(unsigned n) {
    PUNICODE_STRING name = n == 0 ? NULL : make_name(n == 1 || n == 2 ? 0 : 12, 0);
    if (n == 2) { release(name->Buffer, SOURCE); name->Buffer = NULL; name->MaximumLength = 0; }
    if (n == 3) fail_pool = 1;
    if (n == 4) fail_work = 1;
    if (n == 5) shutting_down = true;
    if (n < 3) expect_report(name, 0x10, 3);
    queue_notification_fcb(&fileref, 0x10, 3, name);
    if (n >= 3) {
        CHECK(queued_count == 0 && fileref.refcount == 1);
        CHECK(attempted_pool == (n == 5 ? 0 : 1));
        CHECK(attempted_work == (n == 4 ? 1 : 0));
    } else {
        CHECK(queued_count == 1 && fileref.refcount == 2);
    }
    if (name) retire_name(name);
    free_fileref(&fileref);
    drain(false);
}

static void setup_links(unsigned combination, bool root_parent, LONG initial_refs) {
    hardlink_mode = true;
    parent_initial_refs = initial_refs;
    node.inode_item.nlink = 3;
    volume.root_fileref = root_parent ? &parents[0] : NULL;
    for (unsigned i = 0; i < 3; i++) {
        parent_states[i] = combination % 3;
        combination /= 3;
        parents[i] = (file_ref){ .fcb = &node, .refcount = initial_refs,
                                .deleted = parent_states[i] == PARENT_DELETED };
        const WCHAR directory[] = { '\\', 'd', 'i', 'r', (WCHAR)('0' + i) };
        memcpy(parent_paths[i], directory, sizeof(directory));
        parent_lengths[i] = i == 0 && root_parent ? sizeof(WCHAR) : sizeof(directory);
        link_names[i][0] = 0x3bb; link_names[i][1] = 'l'; link_names[i][2] = (WCHAR)('0' + i);
        links[i] = (hardlink){ .parent = 100 + i,
            .name = { sizeof(link_names[i]), sizeof(link_names[i]), link_names[i] } };
        LIST_ENTRY* head = &node.hardlinks;
        links[i].list_entry = (LIST_ENTRY){ head, head->Blink };
        head->Blink->Flink = &links[i].list_entry;
        head->Blink = &links[i].list_entry;
    }
}

static void run_links(bool queued_work, unsigned repeats, bool stream, bool empty) {
    if (empty) InitializeListHead(&node.hardlinks);
    else if (path_failure == PATH_OK) {
        for (unsigned i = 0; i < 3; i++) {
            if (parent_states[shared_parent ? 0 : i] == PARENT_LIVE)
                expected_links[expected_link_count++] = i;
        }
    }
    PUNICODE_STRING name = stream ? make_name(12, 0) : NULL;
    expect_report(name, 0x20, 3);
    for (unsigned run = 0; run < repeats; run++) {
        next_link_report = 0;
        if (path_failure == PATH_POOL_FAILED)
            fail_pool = attempted_pool + (queued_work ? 2 : 1);
        if (queued_work) {
            queue_notification_fcb(&fileref, 0x20, 3, name);
            CHECK(queued_count == 1 && fileref.refcount == 2);
            if (run + 1 == repeats) free_fileref(&fileref);
            PIO_WORKITEM item = queued[0];
            CHECK(live_range(item, sizeof(*item)));
            item->callback(master_devobj, item->context);
            queued_count = 0;
        } else {
            ExAcquireResourceSharedLite(&volume.tree_lock, true);
            send_notification_fcb(&fileref, 0x20, 3, name);
            ExReleaseResourceLite(&volume.tree_lock);
        }
        CHECK(next_link_report == expected_link_count);
    }
    if (!queued_work) free_fileref(&fileref);
    CHECK(fileref.refcount == 0 && completed_work == (queued_work ? repeats : 0));
    CHECK(reported == repeats * expected_link_count);
    unsigned looked_up = empty ? 0 : path_failure == PATH_OK ? 3 : 1;
    CHECK(lookup_calls == looked_up * repeats);
    unsigned queried = path_failure == PATH_OK ? expected_link_count : 1;
    CHECK(query_calls == queried * repeats);
    unsigned filled = path_failure == PATH_QUERY_FAILED || path_failure == PATH_POOL_FAILED ||
                      path_failure == PATH_TOO_LONG ? 0 : queried;
    CHECK(fill_calls == filled * repeats);
    for (unsigned i = 0; i < 3; i++) {
        if (parents[i].refcount != parent_initial_refs) {
            fprintf(stderr, "parent %u retained %d references, expected %d after %u notifications\n",
                i, parents[i].refcount, parent_initial_refs, repeats);
            CHECK(false);
        }
    }
    if (name) retire_name(name);
}

static bool hardlink_cases(const char* wanted, bool list) {
    for (unsigned queued_work = 0; queued_work < 2; queued_work++) {
        for (unsigned combination = 0; combination < 27; combination++) {
            char name[64]; snprintf(name, sizeof(name), "hardlinks-%s-matrix-%u",
                queued_work ? "queued" : "direct", combination);
            if (list) { puts(name); continue; }
            if (strcmp(wanted, name) == 0) {
                setup_links(combination, true, 1);
                run_links(queued_work, 1, combination % 2, false); return true;
            }
        }
        for (unsigned initial = 0; initial < 2; initial++) {
            char name[64]; snprintf(name, sizeof(name), "hardlinks-%s-repeat-deleted-refs%u",
                queued_work ? "queued" : "direct", initial);
            if (list) { puts(name); continue; }
            if (strcmp(wanted, name) == 0) {
                setup_links(13, true, (LONG)initial); /* Three deleted parents. */
                run_links(queued_work, 64, true, false); return true;
            }
        }
        for (unsigned special = 0; special < 2; special++) {
            char name[64]; snprintf(name, sizeof(name), "hardlinks-%s-%s",
                queued_work ? "queued" : "direct", special ? "empty" : "shared-deleted-parent");
            if (list) { puts(name); continue; }
            if (strcmp(wanted, name) == 0) {
                setup_links(special ? 0 : 13, true, 1);
                if (!special) {
                    shared_parent = true;
                    for (unsigned i = 0; i < 3; i++) links[i].parent = links[0].parent;
                }
                run_links(queued_work, 1, true, special); return true;
            }
        }
        const char* failures[] = { "query", "fill", "pool" };
        for (unsigned root_parent = 0; root_parent < 2; root_parent++) {
            for (unsigned failure = 0; failure < 3; failure++) {
                char name[64]; snprintf(name, sizeof(name), "hardlinks-%s-%s-%s-failure",
                    queued_work ? "queued" : "direct", root_parent ? "root" : "nested", failures[failure]);
                if (list) { puts(name); continue; }
                if (strcmp(wanted, name) == 0) {
                    setup_links(0, root_parent, 1);
                    path_failure = PATH_QUERY_FAILED + failure;
                    run_links(queued_work, 1, true, false); return true;
                }
            }
        }
        char name[64]; snprintf(name, sizeof(name), "hardlinks-%s-long-path", queued_work ? "queued" : "direct");
        if (list) puts(name);
        else if (strcmp(wanted, name) == 0) {
            setup_links(0, false, 1); path_failure = PATH_TOO_LONG;
            run_links(queued_work, 1, false, false); return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    volume.NotifySync = &volume;
    node.Vcb = &volume; node.subvol = &subvol; node.inode_item.nlink = 1;
    InitializeListHead(&node.hardlinks);
    fileref.fcb = &node; fileref.refcount = 1;
    bool list = strcmp(argv[1], "--list") == 0;
    const USHORT lengths[] = { 2, 10, 514, 65534 };
    const char* changes[] = { "stable", "mutate", "replace", "free-buffer", "free-name" };
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        for (unsigned change = STABLE; change <= FREE_NAME; change++) {
            char name[64]; snprintf(name, sizeof(name), "name-%u-%s", lengths[i], changes[change]);
            if (list) { puts(name); continue; }
            if (strcmp(argv[1], name) == 0) { run_name(lengths[i], change); goto done; }
        }
    }
    for (unsigned reverse = 0; reverse < 2; reverse++) {
        char name[32]; snprintf(name, sizeof(name), "batch-%s", reverse ? "reverse" : "fifo");
        if (list) { puts(name); continue; }
        if (strcmp(argv[1], name) == 0) { run_batch(reverse); goto done; }
    }
    for (unsigned i = 0; i < 6; i++) {
        char name[32]; snprintf(name, sizeof(name), "control-%u", i);
        if (list) { puts(name); continue; }
        if (strcmp(argv[1], name) == 0) { run_control(i); goto done; }
    }
    if (hardlink_cases(argv[1], list)) goto done;
    CHECK(list); return 0;
done:
    CHECK(fileref.refcount == 0 && !volume.tree_lock.shared && !volume.fileref_lock.exclusive);
    for (unsigned i = 0; i < 32; i++) CHECK(allocations[i].ptr == NULL);
    for (unsigned i = 0; i < report_count; i++) free(reports[i].bytes);
    return 0;
}
