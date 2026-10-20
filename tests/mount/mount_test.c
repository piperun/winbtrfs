/* Execute the actual mount failure exit and real FCB destructor. */
#include "host.h"
#include "services.inc"
#include "free.inc"
#include "free_fileref.inc"
#include "reap.inc"
#include "cleanup.inc"
#include "failure.inc"

static void exercise(unsigned stage, unsigned devices, NTSTATUS failure) {
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
    if (stage >= 4) volume.volume_fcb = make_fcb(false, false);
    fcb* root_fcb = stage >= 5 ? make_fcb(stage >= 6, stage == 7) : NULL;
    new_device.DeviceExtension = &volume;
    NTSTATUS status = fail_mount(&volume, root_fcb, initialized_lists, failure, &new_device);
    CHECK(status == failure && new_device.deleted && device_deletions == 1);
    CHECK(!allocations && !resource_live);
    unsigned count = stage >= 5 ? 2 : stage == 4 ? 1 : 0;
    CHECK(freed_fcbs == count && volume.fcb_np_lookaside.returned == count);
    for (unsigned i = 0; i < 8; i++) CHECK(!lists[i]->live && !lists[i]->outstanding);
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
            exercise(stage, counts[d], error ? STATUS_INTERNAL_ERROR : STATUS_INSUFFICIENT_RESOURCES); return 0;
        }
    CHECK(strcmp(argv[1], "--list") == 0); return 0;
}
