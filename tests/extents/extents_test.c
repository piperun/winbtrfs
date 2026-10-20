/* Test actual range merging, error propagation and temporary-list ownership. */
#include "host.h"

enum operation { UPDATE, ADD, CHECKSUM, SPACE, LOAD };
enum trimming { UNTRIMMED, PREFIX, SUFFIX };
typedef struct { enum operation op; uint64_t address, length, offset; } call;
static const uint64_t base = 0x100000, page = 4096;
static device_extension volume = { .sector_shift = 12 };
static root subvol = { .id = 0x1234 };
static IRP request = { .marker = 0x5678 };
static struct btrfs_chunk chunk_item;
static chunk storage;
static fcb file;
static extent extents[3];
static call expected[16];
static unsigned expected_count, call_count, fail_call, allocations, fail_allocation;
static unsigned pool_live, lookups;
static void* pools[3];
static bool missing_chunk, no_csum;
static NTSTATUS failure;

static void expect(enum operation op, uint64_t address, uint64_t length, uint64_t offset) {
    CHECK(expected_count < 16);
    expected[expected_count++] = (call){ op, address, length, offset };
}

static NTSTATUS record(enum operation op, uint64_t address, uint64_t length, uint64_t offset) {
    CHECK(call_count < expected_count);
    call e = expected[call_count++];
    CHECK(e.op == op && e.address == address && e.length == length && e.offset == offset);
    return call_count == fail_call ? failure : STATUS_SUCCESS;
}

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK(pool == PagedPool && tag == ALLOC_TAG);
    if (++allocations == fail_allocation) return NULL;
    CHECK(pool_live < 3);
    void* p = malloc(size); CHECK(p != NULL);
    memset(p, 0xa5, size);
    for (unsigned i = 0; i < 3; i++) {
        if (!pools[i]) { pools[i] = p; pool_live++; return p; }
    }
    CHECK(false); return NULL;
}

static void ExFreePool(void* p) {
    for (unsigned i = 0; i < 3; i++) {
        if (pools[i] == p && p) { pools[i] = NULL; pool_live--; free(p); return; }
    }
    CHECK(false);
}

static chunk* get_chunk_from_address(device_extension* v, uint64_t address) {
    CHECK(v == &volume && address >= storage.offset);
    CHECK(address < storage.offset + chunk_item.length);
    lookups++;
    return missing_chunk ? NULL : &storage;
}

static NTSTATUS update_changed_extent_ref(device_extension* v, chunk* c, uint64_t address,
    uint64_t size, uint64_t root_id, uint64_t inode, uint64_t offset, int32_t count,
    bool no_checksum, bool superseded, PIRP irp, LIST_ENTRY* rollback) {
    CHECK(v == &volume && c == &storage && storage.locks == 0);
    CHECK(root_id == subvol.id && inode == file.inode && count == -1);
    CHECK(no_checksum == no_csum && superseded && irp == &request && rollback == NULL);
    return record(UPDATE, address, size, offset);
}

static NTSTATUS add_changed_extent_ref(chunk* c, uint64_t address, uint64_t size,
    uint64_t root_id, uint64_t inode, uint64_t offset, uint32_t count,
    bool no_checksum, LIST_ENTRY* rollback) {
    CHECK(c == &storage && storage.locks == 0);
    CHECK(root_id == subvol.id && inode == file.inode && count == 1);
    CHECK(no_checksum == no_csum && rollback == NULL);
    return record(ADD, address, size, offset);
}

static NTSTATUS add_checksum_entry(device_extension* v, uint64_t address, ULONG length,
    void* csum, PIRP irp) {
    CHECK(v == &volume && !no_csum && csum == NULL && irp == NULL && storage.locks == 0);
    return record(CHECKSUM, address, length, 0);
}

static void acquire_chunk_lock(chunk* c, device_extension* v) {
    CHECK(c == &storage && v == &volume && c->locks == 0); c->locks++;
}

static void release_chunk_lock(chunk* c, device_extension* v) {
    CHECK(c == &storage && v == &volume && c->locks == 1); c->locks--;
}

static NTSTATUS load_cache_chunk(device_extension* v, chunk* c, PIRP irp) {
    CHECK(v == &volume && c == &storage && c->locks == 1 && irp == NULL);
    NTSTATUS status = record(LOAD, 0, 0, 0);
    if (NT_SUCCESS(status)) c->cache_loaded = true;
    return status;
}

static NTSTATUS space_list_add(chunk* c, uint64_t address, uint64_t length,
    LIST_ENTRY* rollback) {
    CHECK(c == &storage && c->locks == 1 && rollback == NULL);
    return record(SPACE, address, length, 0);
}

#include "ranges.inc"

static void setup(enum trimming trim, unsigned type, bool checksums) {
    chunk_item.length = 16 * page;
    storage = (chunk){ .offset = base, .used = (3 + (trim != UNTRIMMED)) * page,
                       .chunk_item = &chunk_item, .cache_loaded = true };
    file = (fcb){ .Vcb = &volume, .subvol = &subvol, .inode = 0xabcdef };
    InitializeListHead(&file.extents);
    no_csum = !checksums;
    file.inode_item.flags = no_csum ? BTRFS_INODE_NODATASUM : 0;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t offset = i * page + (trim == PREFIX ? page : 0);
        extents[i] = (extent){ .offset = offset, .unique = true,
            .extent_data = { .generation = 99, .ram_bytes = page,
                .compression = BTRFS_COMPRESS_NONE, .type = type,
                .disk_bytenr = base + i * page + (trim == PREFIX && i > 0 ? page : 0),
                .disk_num_bytes = page, .num_bytes = page } };
        if ((trim == PREFIX && i == 0) || (trim == SUFFIX && i == 2)) {
            extents[i].extent_data.disk_num_bytes += page;
            extents[i].extent_data.ram_bytes += page;
            if (trim == PREFIX) extents[i].extent_data.offset = page;
        }
        InsertHeadList(file.extents.Blink, &extents[i].list_entry);
    }
}

static unsigned trim_calls(enum trimming trim, bool load) {
    if (trim == UNTRIMMED) return 0;
    unsigned i = trim == PREFIX ? 0 : 2;
    uint64_t address = extents[i].extent_data.disk_bytenr;
    expect(UPDATE, address, 2 * page, extents[i].offset - extents[i].extent_data.offset);
    expect(ADD, address + (trim == PREFIX ? page : 0), page, extents[i].offset);
    uint64_t freed = trim == PREFIX ? base : base + 3 * page;
    if (!no_csum) expect(CHECKSUM, freed, 1, 0);
    if (load) expect(LOAD, 0, 0, 0);
    expect(SPACE, freed, page, 0);
    return expected_count;
}

static unsigned merge_calls(enum trimming trim) {
    uint64_t address = base + (trim == PREFIX ? page : 0);
    unsigned start = expected_count;
    for (unsigned i = 0; i < 3; i++) {
        expect(UPDATE, address + i * page, page, extents[i].offset);
        expect(ADD, address, 3 * page, trim == PREFIX ? page : 0);
    }
    return start;
}

static void run_merge(enum trimming trim, unsigned type, bool checksums, int fail) {
    setup(trim, type, checksums);
    unsigned before_merge = trim_calls(trim, false);
    merge_calls(trim);
    if (fail >= 0) {
        fail_call = before_merge + (unsigned)fail + 1;
        failure = fail % 2 == 0 ? STATUS_IO_DEVICE_ERROR : STATUS_INSUFFICIENT_RESOURCES;
    }
    NTSTATUS status = rationalize_extents(&file, &request);
    if (status != (fail < 0 ? STATUS_SUCCESS : failure)) {
        fprintf(stderr, "returned %08x, expected %08x after call %u\n",
                (unsigned)status, (unsigned)(fail < 0 ? STATUS_SUCCESS : failure), call_count);
        CHECK(false);
    }
    CHECK(call_count == (fail < 0 ? expected_count : fail_call));
    CHECK(storage.used == 3 * page && allocations == 3 && lookups == 1);
    for (unsigned i = 0; i < 3; i++) {
        bool merged = fail < 0 || (int)(2 * i) < fail;
        struct btrfs_file_extent_item* e = &extents[i].extent_data;
        CHECK(e->disk_bytenr == base + (trim == PREFIX ? page : 0) + (merged ? 0 : i * page));
        CHECK(e->disk_num_bytes == (merged ? 3 : 1) * page);
        CHECK(e->ram_bytes == e->disk_num_bytes);
        CHECK(e->offset == (merged ? i * page : 0));
        CHECK(e->num_bytes == page && e->type == type && e->generation == 99);
    }
}

static void run_control(unsigned n) {
    setup(UNTRIMMED, BTRFS_FILE_EXTENT_REG, true);
    NTSTATUS wanted = STATUS_SUCCESS;
    switch (n) {
        case 0: InitializeListHead(&file.extents); break;
        case 1: RemoveEntryList(&extents[2].list_entry); RemoveEntryList(&extents[1].list_entry); break;
        case 2: for (unsigned i = 0; i < 3; i++) extents[i].unique = false; break;
        case 3: for (unsigned i = 0; i < 3; i++) extents[i].extent_data.compression = BTRFS_COMPRESS_ZLIB; break;
        case 4: for (unsigned i = 0; i < 3; i++) extents[i].extent_data.disk_num_bytes = 0; break;
        case 5: for (unsigned i = 0; i < 3; i++) extents[i].extent_data.type = BTRFS_FILE_EXTENT_INLINE; break;
        case 6: for (unsigned i = 0; i < 3; i++) extents[i].extent_data.disk_bytenr += i * page; break;
        case 7: case 8: case 9:
            fail_allocation = n - 6; wanted = STATUS_INSUFFICIENT_RESOURCES; break;
        case 10: missing_chunk = true; wanted = STATUS_INTERNAL_ERROR; break;
        case 11: case 12: case 13: {
            setup(PREFIX, BTRFS_FILE_EXTENT_REG, true);
            storage.cache_loaded = false;
            trim_calls(PREFIX, true); merge_calls(PREFIX);
            /* Exercise truncation errors that already use the returned status. */
            fail_call = n == 11 ? 1 : n == 12 ? 4 : 5;
            failure = STATUS_IO_DEVICE_ERROR; wanted = failure;
            break;
        }
        default: CHECK(false);
    }
    CHECK(rationalize_extents(&file, &request) == wanted);
    CHECK(call_count == (fail_call ? fail_call : 0));
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    bool list = strcmp(argv[1], "--list") == 0;
    for (unsigned trim = UNTRIMMED; trim <= SUFFIX; trim++) {
        for (unsigned prealloc = 0; prealloc < 2; prealloc++) {
            for (unsigned csum = 0; csum < 2; csum++) {
                for (int fail = -1; fail < 6; fail++) {
                    char name[96];
                    snprintf(name, sizeof(name), "merge-trim%u-%s-csum%u-%s%d", trim,
                        prealloc ? "prealloc" : "regular", csum, fail < 0 ? "success" : "fail", fail);
                    if (list) { puts(name); continue; }
                    if (strcmp(argv[1], name) == 0) {
                        run_merge(trim, prealloc ? BTRFS_FILE_EXTENT_PREALLOC : BTRFS_FILE_EXTENT_REG, csum, fail);
                        goto done;
                    }
                }
            }
        }
    }
    for (unsigned n = 0; n < 14; n++) {
        char name[32]; snprintf(name, sizeof(name), "control-%u", n);
        if (list) { puts(name); continue; }
        if (strcmp(argv[1], name) == 0) { run_control(n); goto done; }
    }
    CHECK(list);
done:
    CHECK(pool_live == 0 && storage.locks == 0);
    return 0;
}
