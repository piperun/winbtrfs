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

/* Cleanup for an unpublished volume after mount failure. */
#include "mount-cleanup.h"

void cleanup_failed_mount(device_extension* Vcb, fcb* root_fcb, bool init_lookaside) {
    if (init_lookaside) {
        ExDeletePagedLookasideList(&Vcb->tree_data_lookaside);
        ExDeletePagedLookasideList(&Vcb->traverse_ptr_lookaside);
        ExDeletePagedLookasideList(&Vcb->batch_item_lookaside);
        ExDeletePagedLookasideList(&Vcb->fileref_lookaside);
        ExDeletePagedLookasideList(&Vcb->fcb_lookaside);
        ExDeletePagedLookasideList(&Vcb->name_bit_lookaside);
        ExDeleteNPagedLookasideList(&Vcb->range_lock_lookaside);
        ExDeleteNPagedLookasideList(&Vcb->fcb_np_lookaside);
    }

    if (Vcb->root_file)
        ObDereferenceObject(Vcb->root_file);
    else if (Vcb->root_fileref)
        free_fileref(Vcb->root_fileref);
    else if (root_fcb)
        free_fcb(root_fcb);

    if (root_fcb && root_fcb->refcount == 0)
        reap_fcb(root_fcb);

    if (Vcb->volume_fcb)
        reap_fcb(Vcb->volume_fcb);

    ExDeleteResourceLite(&Vcb->tree_lock);
    ExDeleteResourceLite(&Vcb->load_lock);
    ExDeleteResourceLite(&Vcb->fcb_lock);
    ExDeleteResourceLite(&Vcb->fileref_lock);
    ExDeleteResourceLite(&Vcb->chunk_lock);
    ExDeleteResourceLite(&Vcb->dirty_fcbs_lock);
    ExDeleteResourceLite(&Vcb->dirty_filerefs_lock);
    ExDeleteResourceLite(&Vcb->dirty_subvols_lock);
    ExDeleteResourceLite(&Vcb->scrub.stats_lock);

    if (Vcb->devices.Flink) {
        while (!IsListEmpty(&Vcb->devices)) {
            device* dev2 = CONTAINING_RECORD(RemoveHeadList(&Vcb->devices), device, list_entry);

            ExFreePool(dev2);
        }
    }
}
