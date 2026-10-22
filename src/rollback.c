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

// Apply and release transaction rollback records without changing failure policy.
#include "btrfs_drv.h"

__attribute__((nonnull(1)))
void clear_rollback(LIST_ENTRY* rollback) {
    while (!IsListEmpty(rollback)) {
        rollback_item* ri = CONTAINING_RECORD(RemoveHeadList(rollback), rollback_item, list_entry);

        ExFreePool(ri);
    }
}

__attribute__((nonnull(1,2)))
void do_rollback(device_extension* Vcb, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    rollback_item* ri;

    while (!IsListEmpty(rollback)) {
        LIST_ENTRY* le = RemoveTailList(rollback);
        ri = CONTAINING_RECORD(le, rollback_item, list_entry);

        switch (ri->type) {
            case ROLLBACK_INSERT_EXTENT:
                ri->extent.ext->ignore = true;

                switch (ri->extent.ext->extent_data.type) {
                    case BTRFS_FILE_EXTENT_REG:
                    case BTRFS_FILE_EXTENT_PREALLOC: {
                        if (ri->extent.ext->extent_data.disk_num_bytes != 0)
                            ri->extent.fcb->inode_item.nbytes -= ri->extent.ext->extent_data.num_bytes;

                        break;
                    }

                    case BTRFS_FILE_EXTENT_INLINE:
                        ri->extent.fcb->inode_item.nbytes -= ri->extent.ext->extent_data.ram_bytes;
                    break;
                }

                break;

            case ROLLBACK_DELETE_EXTENT:
                ri->extent.ext->ignore = false;

                switch (ri->extent.ext->extent_data.type) {
                    case BTRFS_FILE_EXTENT_REG:
                    case BTRFS_FILE_EXTENT_PREALLOC:
                        if (ri->extent.ext->extent_data.disk_num_bytes != 0)
                            ri->extent.fcb->inode_item.nbytes += ri->extent.ext->extent_data.num_bytes;
                        break;

                    case BTRFS_FILE_EXTENT_INLINE:
                        ri->extent.fcb->inode_item.nbytes += ri->extent.ext->extent_data.ram_bytes;
                    break;
                }

                break;

            case ROLLBACK_ADD_SPACE:
            case ROLLBACK_SUBTRACT_SPACE:
                if (ri->space.chunk)
                    acquire_chunk_lock(ri->space.chunk, Vcb);

                if (ri->type == ROLLBACK_ADD_SPACE) {
                    Status = space_list_subtract2(ri->space.list, ri->space.list_size,
                                                  ri->space.address, ri->space.length,
                                                  NULL, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("space_list_subtract2 returned %08lx\n", Status);

                        if (ri->space.chunk)
                            release_chunk_lock(ri->space.chunk, Vcb);

                        ExFreePool(ri);

                        goto fail;
                    }
                } else {
                    Status = space_list_add2(ri->space.list, ri->space.list_size,
                                             ri->space.address, ri->space.length,
                                             NULL, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("space_list_add2 returned %08lx\n", Status);

                        if (ri->space.chunk)
                            release_chunk_lock(ri->space.chunk, Vcb);

                        ExFreePool(ri);

                        goto fail;
                    }
                }

                if (ri->space.chunk) {
                    if (ri->type == ROLLBACK_ADD_SPACE)
                        ri->space.chunk->used += ri->space.length;
                    else
                        ri->space.chunk->used -= ri->space.length;
                }

                if (ri->space.chunk) {
                    LIST_ENTRY* le2 = le->Blink;

                    while (le2 != rollback) {
                        LIST_ENTRY* le3 = le2->Blink;
                        rollback_item* ri2 = CONTAINING_RECORD(le2, rollback_item, list_entry);

                        if (ri2->type == ROLLBACK_ADD_SPACE || ri2->type == ROLLBACK_SUBTRACT_SPACE) {
                            if (ri2->space.chunk == ri->space.chunk) {
                                if (ri2->type == ROLLBACK_ADD_SPACE) {
                                    Status = space_list_subtract2(ri2->space.list,
                                                                  ri2->space.list_size,
                                                                  ri2->space.address,
                                                                  ri2->space.length,
                                                                  NULL, NULL);
                                    if (!NT_SUCCESS(Status)) {
                                        ERR("space_list_subtract2 returned %08lx\n", Status);
                                        release_chunk_lock(ri->space.chunk, Vcb);
                                        ExFreePool(ri);
                                        goto fail;
                                    }

                                    ri->space.chunk->used += ri2->space.length;
                                } else {
                                    Status = space_list_add2(ri2->space.list,
                                                             ri2->space.list_size,
                                                             ri2->space.address,
                                                             ri2->space.length,
                                                             NULL, NULL);
                                    if (!NT_SUCCESS(Status)) {
                                        ERR("space_list_add2 returned %08lx\n", Status);
                                        release_chunk_lock(ri->space.chunk, Vcb);
                                        ExFreePool(ri);
                                        goto fail;
                                    }

                                    ri->space.chunk->used -= ri2->space.length;
                                }

                                RemoveEntryList(&ri2->list_entry);
                                ExFreePool(ri2);
                            }
                        }

                        le2 = le3;
                    }

                    release_chunk_lock(ri->space.chunk, Vcb);
                }

                break;

            case ROLLBACK_UPDATE_CHANGED_EXTENT_REF: {
                chunk* c = get_chunk_from_address(Vcb, ri->changed_extent_ref.address);

                if (c) {
                    changed_extent* ce;

                    Status = update_changed_extent_ref(Vcb, c, ri->changed_extent_ref.address,
                                                       ri->changed_extent_ref.size,
                                                       ri->changed_extent_ref.root,
                                                       ri->changed_extent_ref.objid,
                                                       ri->changed_extent_ref.offset,
                                                       -ri->changed_extent_ref.count,
                                                       false, false, NULL, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("update_changed_extent_ref returned %08lx\n", Status);
                        ExFreePool(ri);
                        goto fail;
                    }

                    ExAcquireResourceExclusiveLite(&c->changed_extents_lock, true);

                    ce = get_changed_extent_item(c, ri->changed_extent_ref.address,
                                                 ri->changed_extent_ref.size, false);
                    if (!ce) { // shouldn't happen
                        ERR("get_changed_extent_item returned NULL\n");
                        ExFreePool(ri);
                        ExReleaseResourceLite(&c->changed_extents_lock);
                        Status = STATUS_INTERNAL_ERROR;
                        goto fail;
                    }

                    if (ce->count == 0 && ce->old_count == 0) {
                        while (!IsListEmpty(&ce->refs)) {
                            changed_extent_ref* cer = CONTAINING_RECORD(RemoveHeadList(&ce->refs), changed_extent_ref, list_entry);
                            ExFreePool(cer);
                        }

                        remove_changed_extent(c, ce);
                        ExFreePool(ce);
                    }

                    ExReleaseResourceLite(&c->changed_extents_lock);
                }

                break;
            }

            case ROLLBACK_ADD_DIR_CHILD: {
                dir_child* dc = ri->dir_child.dc;

                ExAcquireResourceExclusiveLite(&ri->dir_child.parent->nonpaged->dir_children_lock,
                                               true);
                RemoveEntryList(&dc->list_entry_index);
                remove_dir_child_from_hash_lists(ri->dir_child.parent, dc);
                ExReleaseResourceLite(&ri->dir_child.parent->nonpaged->dir_children_lock);

                ExFreePool(dc->utf8.Buffer);
                ExFreePool(dc->name.Buffer);
                ExFreePool(dc->name_uc.Buffer);
                ExFreePool(dc);

                break;
            }
        }

        ExFreePool(ri);
    }

    return;

fail:
    ERR("do_rollback failed, going readonly\n");

    while (!IsListEmpty(rollback)) {
        LIST_ENTRY* le = RemoveTailList(rollback);
        ri = CONTAINING_RECORD(le, rollback_item, list_entry);

        ExFreePool(ri);
    }

    Vcb->readonly = true;
    FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_FORCED_CLOSED);
}
