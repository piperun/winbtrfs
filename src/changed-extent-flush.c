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

// Flush one pending extent; callers supply exclusive tree serialization.
#include "btrfs_drv.h"

NTSTATUS flush_changed_extent(device_extension* Vcb, chunk* c, changed_extent* ce,
                                     PIRP Irp) {
    LIST_ENTRY *le, *le2;
    NTSTATUS Status;
    uint64_t old_size;

    if (ce->count == 0 && ce->old_count == 0) {
        while (!IsListEmpty(&ce->refs)) {
            changed_extent_ref* cer = CONTAINING_RECORD(RemoveHeadList(&ce->refs), changed_extent_ref, list_entry);
            ExFreePool(cer);
        }

        while (!IsListEmpty(&ce->old_refs)) {
            changed_extent_ref* cer = CONTAINING_RECORD(RemoveHeadList(&ce->old_refs), changed_extent_ref, list_entry);
            ExFreePool(cer);
        }

        Status = STATUS_SUCCESS;
        goto end;
    }

    le = ce->refs.Flink;
    while (le != &ce->refs) {
        changed_extent_ref* cer = CONTAINING_RECORD(le, changed_extent_ref, list_entry);
        uint32_t old_count = 0;

        if (cer->type == BTRFS_EXTENT_DATA_REF_KEY) {
            le2 = ce->old_refs.Flink;
            while (le2 != &ce->old_refs) {
                changed_extent_ref* cer2 = CONTAINING_RECORD(le2, changed_extent_ref, list_entry);

                if (cer2->type == BTRFS_EXTENT_DATA_REF_KEY && cer2->edr.root == cer->edr.root && cer2->edr.objectid == cer->edr.objectid && cer2->edr.offset == cer->edr.offset) {
                    old_count = cer2->edr.count;
                    break;
                }

                le2 = le2->Flink;
            }

            old_size = ce->old_count > 0 ? ce->old_size : ce->size;

            if (cer->edr.count > old_count) {
                Status = increase_extent_refcount_data(Vcb, ce->address, old_size, cer->edr.root, cer->edr.objectid, cer->edr.offset, cer->edr.count - old_count, Irp);

                if (!NT_SUCCESS(Status)) {
                    ERR("increase_extent_refcount_data returned %08lx\n", Status);
                    return Status;
                }
            }
        } else if (cer->type == BTRFS_SHARED_DATA_REF_KEY) {
            le2 = ce->old_refs.Flink;
            while (le2 != &ce->old_refs) {
                changed_extent_ref* cer2 = CONTAINING_RECORD(le2, changed_extent_ref, list_entry);

                if (cer2->type == BTRFS_SHARED_DATA_REF_KEY && cer2->offset == cer->offset) {
                    RemoveEntryList(&cer2->list_entry);
                    ExFreePool(cer2);
                    break;
                }

                le2 = le2->Flink;
            }
        }

        le = le->Flink;
    }

    le = ce->refs.Flink;
    while (le != &ce->refs) {
        changed_extent_ref* cer = CONTAINING_RECORD(le, changed_extent_ref, list_entry);
        LIST_ENTRY* le3 = le->Flink;
        uint32_t old_count = 0;

        if (cer->type == BTRFS_EXTENT_DATA_REF_KEY) {
            le2 = ce->old_refs.Flink;
            while (le2 != &ce->old_refs) {
                changed_extent_ref* cer2 = CONTAINING_RECORD(le2, changed_extent_ref, list_entry);

                if (cer2->type == BTRFS_EXTENT_DATA_REF_KEY && cer2->edr.root == cer->edr.root && cer2->edr.objectid == cer->edr.objectid && cer2->edr.offset == cer->edr.offset) {
                    old_count = cer2->edr.count;

                    RemoveEntryList(&cer2->list_entry);
                    ExFreePool(cer2);
                    break;
                }

                le2 = le2->Flink;
            }

            old_size = ce->old_count > 0 ? ce->old_size : ce->size;

            if (cer->edr.count < old_count) {
                Status = decrease_extent_refcount_data(Vcb, ce->address, old_size, cer->edr.root, cer->edr.objectid, cer->edr.offset,
                                                       old_count - cer->edr.count, ce->superseded, Irp);

                if (!NT_SUCCESS(Status)) {
                    ERR("decrease_extent_refcount_data returned %08lx\n", Status);
                    return Status;
                }
            }

            if (ce->size != ce->old_size && ce->old_count > 0) {
                struct btrfs_key searchkey;
                traverse_ptr tp;
                void* data;

                searchkey.objectid = ce->address;
                searchkey.type = BTRFS_EXTENT_ITEM_KEY;
                searchkey.offset = ce->old_size;

                Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
                if (!NT_SUCCESS(Status)) {
                    ERR("error - find_item returned %08lx\n", Status);
                    return Status;
                }

                if (keycmp(searchkey, tp.item->key)) {
                    ERR("could not find (%I64x,%x,%I64x) in extent tree\n", searchkey.objectid, searchkey.type, searchkey.offset);
                    return STATUS_INTERNAL_ERROR;
                }

                if (tp.item->size > 0) {
                    data = ExAllocatePoolWithTag(PagedPool, tp.item->size, ALLOC_TAG);

                    if (!data) {
                        ERR("out of memory\n");
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    RtlCopyMemory(data, tp.item->data, tp.item->size);
                } else
                    data = NULL;

                Status = insert_tree_item(Vcb, Vcb->extent_root, ce->address, BTRFS_EXTENT_ITEM_KEY, ce->size, data, tp.item->size, NULL, Irp);
                if (!NT_SUCCESS(Status)) {
                    ERR("insert_tree_item returned %08lx\n", Status);
                    if (data) ExFreePool(data);
                    return Status;
                }

                Status = delete_tree_item(Vcb, &tp);
                if (!NT_SUCCESS(Status)) {
                    ERR("delete_tree_item returned %08lx\n", Status);
                    return Status;
                }
            }
        }

        RemoveEntryList(&cer->list_entry);
        ExFreePool(cer);

        le = le3;
    }

#ifdef DEBUG_PARANOID
    if (!IsListEmpty(&ce->old_refs))
        WARN("old_refs not empty\n");
#endif

    Status = STATUS_SUCCESS;

end:
    if (ce->count == 0 && !ce->superseded) {
        c->used -= ce->size;
        Status = space_list_add(c, ce->address, ce->size, NULL);
    }

    remove_changed_extent(c, ce);
    ExFreePool(ce);

    return Status;
}
