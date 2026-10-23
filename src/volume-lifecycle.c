/* Copyright (c) Mark Harmstone 2016-17
 *
 * This file is part of WinBtrfs.
 *
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

// Mounted volume teardown after background worker termination.
#include "btrfs_drv.h"
#include "trim.h"


void uninit(_In_ device_extension* Vcb) {
    KIRQL irql;
    NTSTATUS Status;
    LIST_ENTRY* le;

    if (!Vcb->removing) {
        ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
        Vcb->removing = true;
        ExReleaseResourceLite(&Vcb->tree_lock);
    }

    // References keep request objects alive, but a raw writer can reuse their
    // addresses as soon as this mount releases ownership.
    trim_drain(Vcb);

    if (Vcb->vde && Vcb->vde->mounted_device == Vcb->devobj)
        Vcb->vde->mounted_device = NULL;

    IoAcquireVpbSpinLock(&irql);
    Vcb->Vpb->Flags &= ~VPB_MOUNTED;
    Vcb->Vpb->Flags |= VPB_DIRECT_WRITES_ALLOWED;
    Vcb->Vpb->DeviceObject = NULL;
    IoReleaseVpbSpinLock(irql);

    // FIXME - needs global_loading_lock to be held
    if (Vcb->list_entry.Flink)
        RemoveEntryList(&Vcb->list_entry);

    if (Vcb->balance.thread) {
        Vcb->balance.paused = false;
        Vcb->balance.stopping = true;
        KeSetEvent(&Vcb->balance.event, 0, false);
        KeWaitForSingleObject(&Vcb->balance.finished, Executive, KernelMode, false, NULL);
    }

    if (Vcb->scrub.thread) {
        Vcb->scrub.paused = false;
        Vcb->scrub.stopping = true;
        KeSetEvent(&Vcb->scrub.event, 0, false);
        KeWaitForSingleObject(&Vcb->scrub.finished, Executive, KernelMode, false, NULL);
    }

    if (Vcb->running_sends != 0) {
        bool send_cancelled = false;

        ExAcquireResourceExclusiveLite(&Vcb->send_load_lock, true);

        le = Vcb->send_ops.Flink;
        while (le != &Vcb->send_ops) {
            send_info* send = CONTAINING_RECORD(le, send_info, list_entry);

            if (!send->cancelling) {
                send->cancelling = true;
                send_cancelled = true;
                send->ccb = NULL;
                KeSetEvent(&send->cleared_event, 0, false);
            }

            le = le->Flink;
        }

        ExReleaseResourceLite(&Vcb->send_load_lock);

        if (send_cancelled) {
            while (Vcb->running_sends != 0) {
                ExAcquireResourceExclusiveLite(&Vcb->send_load_lock, true);
                ExReleaseResourceLite(&Vcb->send_load_lock);
            }
        }
    }

    Status = registry_mark_volume_unmounted(Vcb->superblock.fsid);
    if (!NT_SUCCESS(Status) && Status != STATUS_TOO_LATE)
        WARN("registry_mark_volume_unmounted returned %08lx\n", Status);

    stop_flush_thread(Vcb);
    trim_destroy(Vcb);

    stop_calc_threads(Vcb);

    reap_fcb(Vcb->volume_fcb);
    reap_fcb(Vcb->dummy_fcb);

    if (Vcb->root_file)
        ObDereferenceObject(Vcb->root_file);

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

        if (c->cache) {
            reap_fcb(c->cache);
            c->cache = NULL;
        }

        le = le->Flink;
    }

    while (!IsListEmpty(&Vcb->all_fcbs)) {
        fcb* fcb = CONTAINING_RECORD(Vcb->all_fcbs.Flink, struct _fcb, list_entry_all);

        reap_fcb(fcb);
    }

    while (!IsListEmpty(&Vcb->sys_chunks)) {
        sys_chunk* sc = CONTAINING_RECORD(RemoveHeadList(&Vcb->sys_chunks), sys_chunk, list_entry);

        if (sc->data)
            ExFreePool(sc->data);

        ExFreePool(sc);
    }

    while (!IsListEmpty(&Vcb->roots)) {
        root* r = CONTAINING_RECORD(RemoveHeadList(&Vcb->roots), root, list_entry);

        ExDeleteResourceLite(&r->nonpaged->load_tree_lock);
        ExFreePool(r->nonpaged);
        ExFreePool(r);
    }

    while (!IsListEmpty(&Vcb->chunks)) {
        chunk* c = CONTAINING_RECORD(RemoveHeadList(&Vcb->chunks), chunk, list_entry);

        while (!IsListEmpty(&c->space)) {
            LIST_ENTRY* le2 = RemoveHeadList(&c->space);
            space* s = CONTAINING_RECORD(le2, space, list_entry);

            ExFreePool(s);
        }

        while (!IsListEmpty(&c->deleting)) {
            LIST_ENTRY* le2 = RemoveHeadList(&c->deleting);
            space* s = CONTAINING_RECORD(le2, space, list_entry);

            ExFreePool(s);
        }

        if (c->devices)
            ExFreePool(c->devices);

        if (c->cache)
            reap_fcb(c->cache);

        ExDeleteResourceLite(&c->range_locks_lock);
        ExDeleteResourceLite(&c->partial_stripes_lock);
        ExDeleteResourceLite(&c->lock);
        ExDeleteResourceLite(&c->changed_extents_lock);

        ExFreePool(c->chunk_item);
        ExFreePool(c);
    }

    while (!IsListEmpty(&Vcb->devices)) {
        device* dev = CONTAINING_RECORD(RemoveHeadList(&Vcb->devices), device, list_entry);

        while (!IsListEmpty(&dev->space)) {
            LIST_ENTRY* le2 = RemoveHeadList(&dev->space);
            space* s = CONTAINING_RECORD(le2, space, list_entry);

            ExFreePool(s);
        }

        ExFreePool(dev);
    }

    ExAcquireResourceExclusiveLite(&Vcb->scrub.stats_lock, true);
    while (!IsListEmpty(&Vcb->scrub.errors)) {
        scrub_error* err = CONTAINING_RECORD(RemoveHeadList(&Vcb->scrub.errors), scrub_error, list_entry);

        ExFreePool(err);
    }
    ExReleaseResourceLite(&Vcb->scrub.stats_lock);

    ExDeleteResourceLite(&Vcb->fcb_lock);
    ExDeleteResourceLite(&Vcb->fileref_lock);
    ExDeleteResourceLite(&Vcb->load_lock);
    ExDeleteResourceLite(&Vcb->tree_lock);
    ExDeleteResourceLite(&Vcb->chunk_lock);
    ExDeleteResourceLite(&Vcb->dirty_fcbs_lock);
    ExDeleteResourceLite(&Vcb->dirty_filerefs_lock);
    ExDeleteResourceLite(&Vcb->dirty_subvols_lock);
    ExDeleteResourceLite(&Vcb->scrub.stats_lock);
    ExDeleteResourceLite(&Vcb->send_load_lock);

    ExDeletePagedLookasideList(&Vcb->tree_data_lookaside);
    ExDeletePagedLookasideList(&Vcb->traverse_ptr_lookaside);
    ExDeletePagedLookasideList(&Vcb->batch_item_lookaside);
    ExDeletePagedLookasideList(&Vcb->fileref_lookaside);
    ExDeletePagedLookasideList(&Vcb->fcb_lookaside);
    ExDeletePagedLookasideList(&Vcb->name_bit_lookaside);
    ExDeleteNPagedLookasideList(&Vcb->range_lock_lookaside);
    ExDeleteNPagedLookasideList(&Vcb->fcb_np_lookaside);

    if (Vcb->devobj->AttachedDevice)
        IoDetachDevice(Vcb->devobj);

    IoDeleteDevice(Vcb->devobj);
}
