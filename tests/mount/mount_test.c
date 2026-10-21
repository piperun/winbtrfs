/* Execute the actual mount failure exit and real FCB destructor. */
#include "host.h"
#include "services.inc"
#include "free.inc"
#include "free_fileref.inc"
#include "reap.inc"
#include "reap_fcbs.inc"
#include "trees.inc"
#include "cleanup.inc"
#include "failure.inc"

static void exercise(unsigned stage, unsigned devices, NTSTATUS failure, unsigned dummy_stage, unsigned loaded, bool root_file) {
    if (stage == 0) {
        CHECK(fail_mount(NULL, NULL, false, failure, NULL) == failure);
        CHECK(!allocations && !device_deletions); return;
    }
    ERESOURCE* resources[] = { &volume.tree_lock, &volume.load_lock, &volume.fcb_lock, &volume.fileref_lock,
        &volume.chunk_lock, &volume.dirty_fcbs_lock, &volume.dirty_filerefs_lock, &volume.dirty_subvols_lock, &volume.scrub.stats_lock };
    for (unsigned i = 0; i < 9; i++) init_resource(resources[i]);
    volume.tree_lock.held = volume.load_lock.held = 1;
    initialized_lists = stage >= 3;
    LOOKASIDE* lists[] = { &volume.tree_data_lookaside, &volume.traverse_ptr_lookaside, &volume.batch_item_lookaside,
        &volume.fileref_lookaside, &volume.fcb_lookaside, &volume.name_bit_lookaside, &volume.range_lock_lookaside, &volume.fcb_np_lookaside };
    if (initialized_lists) for (unsigned i = 0; i < 8; i++) {
        lists[i]->live = true; lists[i]->nonpaged = i >= 6;
    }
    if (stage >= 2) {
        InitializeListHead(&volume.devices);
        for (unsigned i = 0; i < devices; i++) {
            device* dev = allocate(sizeof(*dev), NULL); InsertTailList(&volume.devices, &dev->list_entry);
        }
    } else CHECK(!devices);
    InitializeListHead(&subvolume.fcbs);
    make_metadata(loaded);
    if (stage >= 4) volume.volume_fcb = make_fcb(NULL, false);
    if (dummy_stage) {
        volume.dummy_fcb = make_fcb(dummy_subvol, false);
        volume.dummy_fcb->hash = 0;
        if (dummy_stage >= 2)
            volume.dummy_fcb->hash_ptrs = allocate(sizeof(LIST_ENTRY*) * 256, NULL);
        if (dummy_stage >= 3)
            volume.dummy_fcb->hash_ptrs_uc = allocate(sizeof(LIST_ENTRY*) * 256, NULL);
    }
    fcb* root_fcb = stage >= 5 ? make_fcb(stage >= 6 ? root_subvol : NULL, stage == 7) : NULL;
    file_ref* root_fileref = NULL;
    if (root_file) {
        /* Mount links the root FCB in once its fileref exists, then opens the root stream file on it. */
        CHECK(stage == 6 && loaded >= LOADED_CACHES);
        InsertTailList(&root_subvol->fcbs, &root_fcb->list_entry);
        InsertTailList(&volume.all_fcbs, &root_fcb->list_entry_all);
        root_fileref = allocate(sizeof(*root_fileref), &volume.fileref_lookaside); root_fileref->refcount = 1;
        volume.root_fileref = root_fileref; volume.root_file = &root_stream_file;
        /* The FCB, its nonpaged part with three resources, and the fileref stay with the stream file. */
        kept_allocations = 3; kept_resources = 3;
    }
    new_device.DeviceExtension = &volume;
    NTSTATUS status = fail_mount(&volume, root_fcb, initialized_lists, failure, &new_device);
    CHECK(status == failure && new_device.deleted && device_deletions == 1);
    CHECK(allocations == kept_allocations && resource_live == kept_resources);
    CHECK(root_file_dereferences == root_file);
    if (root_file) CHECK(root_fcb->refcount == 1 && root_fcb->nonpaged->resource.live && root_fileref->refcount == 1);
    unsigned caches = loaded >= LOADED_CACHES ? 3 : 0;
    unsigned count = (stage >= 5 && !root_file ? 2 : stage >= 4 ? 1 : 0) + (dummy_stage != 0) + caches;
    CHECK(freed_fcbs == count && volume.fcb_np_lookaside.returned == count);
    CHECK(volume.fcb_lookaside.returned == caches && volume.tree_data_lookaside.returned == loaded_items);
    for (unsigned i = 0; i < 8; i++)
        CHECK(!lists[i]->live && lists[i]->outstanding == (root_file && (lists[i] == &volume.fcb_np_lookaside ||
                                                                         lists[i] == &volume.fileref_lookaside)));
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const char* stages[] = { "no-device", "early-device", "device-list", "empty-lookasides", "volume-fcb",
        "root-fcb", "root-subvolume", "root-payload-control" };
    const unsigned counts[] = { 0, 1, 3 };
    for (unsigned stage = 0; stage < 8; stage++) for (unsigned d = 0; d < (stage < 2 ? 1u : 3u); d++)
        for (unsigned error = 0; error < 2; error++) {
            char name[100]; snprintf(name, sizeof(name), "%s-devices%u-error%u", stages[stage], counts[d], error);
            if (strcmp(argv[1], "--list") == 0) { puts(name); continue; }
            if (strcmp(argv[1], name) != 0) continue;
            exercise(stage, counts[d], error ? STATUS_INTERNAL_ERROR : STATUS_INSUFFICIENT_RESOURCES, 0, LOADED_NONE, false);
            return 0;
        }
    const char* dummy_stages[] = { "dummy-no-hashes", "dummy-one-hash", "dummy-two-hashes",
        "dummy-root-fcb", "dummy-root-subvolume", "dummy-root-payload-control" };
    const unsigned root_stages[] = { 4, 4, 4, 5, 6, 7 };
    const unsigned hash_stages[] = { 1, 2, 3, 3, 3, 3 };
    for (unsigned stage = 0; stage < 6; stage++) for (unsigned d = 0; d < 3; d++)
        for (unsigned error = 0; error < 2; error++) {
            char name[100]; snprintf(name, sizeof(name), "%s-devices%u-error%u", dummy_stages[stage], counts[d], error);
            if (strcmp(argv[1], "--list") == 0) { puts(name); continue; }
            if (strcmp(argv[1], name) != 0) continue;
            exercise(root_stages[stage], counts[d],
                error ? STATUS_INTERNAL_ERROR : STATUS_INSUFFICIENT_RESOURCES, hash_stages[stage], LOADED_NONE, false); return 0;
        }
    /* Failures after mount has loaded volume metadata, with the FCBs that exist at that point. The last
     * stage fails after the root stream file was created, which still references the root FCB. */
    const char* loaded_names[] = { "system-chunks", "chunk-tree", "all-roots", "chunk-caches", "chunk-caches-root-subvolume",
        "root-stream-file" };
    const unsigned loaded_levels[] = { LOADED_SYSTEM_CHUNKS, LOADED_CHUNK_TREE, LOADED_ROOTS, LOADED_CACHES, LOADED_CACHES,
        LOADED_CACHES };
    const unsigned loaded_fcb_stages[] = { 2, 3, 3, 4, 6, 6 };
    const unsigned loaded_dummy_stages[] = { 0, 0, 0, 0, 3, 3 };
    for (unsigned stage = 0; stage < 6; stage++) for (unsigned d = 0; d < 3; d++)
        for (unsigned error = 0; error < 2; error++) {
            char name[100]; snprintf(name, sizeof(name), "%s-devices%u-error%u", loaded_names[stage], counts[d], error);
            if (strcmp(argv[1], "--list") == 0) { puts(name); continue; }
            if (strcmp(argv[1], name) != 0) continue;
            exercise(loaded_fcb_stages[stage], counts[d], error ? STATUS_INTERNAL_ERROR : STATUS_INSUFFICIENT_RESOURCES,
                loaded_dummy_stages[stage], loaded_levels[stage], stage == 5); return 0;
        }
    CHECK(strcmp(argv[1], "--list") == 0); return 0;
}
