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
    LIST_ENTRY* le;

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

    if (Vcb->dummy_fcb)
        reap_fcb(Vcb->dummy_fcb);

    // Mount initializes these list heads as it goes, in a volume extension that starts zeroed.
    if (Vcb->chunks.Flink) {
        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

            // Chunks can share a cache FCB, so drop each chunk's reference and reap once below.
            if (c->cache) {
                free_fcb(c->cache);
                c->cache = NULL;
            }

            le = le->Flink;
        }
    }

    // Only FCBs nothing references: the root stream file can still point at the root FCB.
    if (Vcb->all_fcbs.Flink)
        reap_fcbs(Vcb);

    if (Vcb->roots.Flink && Vcb->trees.Flink) {
        // Not free_trees: it also reaps filerefs from Vcb->root_fileref, which may not exist yet.
        le = Vcb->roots.Flink;
        while (le != &Vcb->roots) {
            free_trees_root(Vcb, CONTAINING_RECORD(le, root, list_entry));

            le = le->Flink;
        }
    }

    if (Vcb->sys_chunks.Flink) {
        while (!IsListEmpty(&Vcb->sys_chunks)) {
            sys_chunk* sc = CONTAINING_RECORD(RemoveHeadList(&Vcb->sys_chunks), sys_chunk, list_entry);

            if (sc->data)
                ExFreePool(sc->data);

            ExFreePool(sc);
        }
    }

    if (Vcb->roots.Flink) {
        while (!IsListEmpty(&Vcb->roots)) {
            root* r = CONTAINING_RECORD(RemoveHeadList(&Vcb->roots), root, list_entry);

            ExDeleteResourceLite(&r->nonpaged->load_tree_lock);
            ExFreePool(r->nonpaged);
            ExFreePool(r);
        }
    }

    if (Vcb->chunks.Flink) {
        while (!IsListEmpty(&Vcb->chunks)) {
            chunk* c = CONTAINING_RECORD(RemoveHeadList(&Vcb->chunks), chunk, list_entry);

            while (!IsListEmpty(&c->space)) {
                space* s = CONTAINING_RECORD(RemoveHeadList(&c->space), space, list_entry);

                ExFreePool(s);
            }

            while (!IsListEmpty(&c->deleting)) {
                space* s = CONTAINING_RECORD(RemoveHeadList(&c->deleting), space, list_entry);

                ExFreePool(s);
            }

            if (c->devices)
                ExFreePool(c->devices);

            ExDeleteResourceLite(&c->range_locks_lock);
            ExDeleteResourceLite(&c->partial_stripes_lock);
            ExDeleteResourceLite(&c->lock);
            ExDeleteResourceLite(&c->changed_extents_lock);

            ExFreePool(c->chunk_item);
            ExFreePool(c);
        }
    }

    // FCB and tree destructors return allocations to these lookaside lists.
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

            if (dev2->space.Flink) {
                while (!IsListEmpty(&dev2->space)) {
                    space* s = CONTAINING_RECORD(RemoveHeadList(&dev2->space), space, list_entry);

                    ExFreePool(s);
                }
            }

            ExFreePool(dev2);
        }
    }
}
