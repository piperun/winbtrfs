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

// Root creation and initial in-memory leaf construction.
#include "btrfs_drv.h"
#include "tree-search.h"

NTSTATUS create_root(_In_ _Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_ uint64_t id,
                     _Out_ root** rootptr, _In_ bool no_tree, _In_ uint64_t offset, _In_opt_ PIRP Irp) {
    NTSTATUS Status;
    root* r;
    struct btrfs_root_item* ri;
    traverse_ptr tp;

    r = ExAllocatePoolWithTag(PagedPool, sizeof(root), ALLOC_TAG);
    if (!r) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    r->nonpaged = ExAllocatePoolWithTag(NonPagedPool, sizeof(root_nonpaged), ALLOC_TAG);
    if (!r->nonpaged) {
        ERR("out of memory\n");
        ExFreePool(r);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ri = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_root_item), ALLOC_TAG);
    if (!ri) {
        ERR("out of memory\n");

        ExFreePool(r->nonpaged);
        ExFreePool(r);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    r->id = id;
    r->treeholder.address = 0;
    r->treeholder.generation = Vcb->superblock.generation;
    r->treeholder.tree = NULL;
    r->lastinode = 0;
    r->dirty = false;
    r->received = false;
    r->reserved = NULL;
    r->parent = 0;
    r->send_ops = 0;
    RtlZeroMemory(&r->root_item, sizeof(struct btrfs_root_item));
    r->root_item.refs = 1;
    r->fcbs_version = 0;
    r->checked_for_orphans = true;
    r->dropped = false;
    InitializeListHead(&r->fcbs);
    RtlZeroMemory(r->fcbs_ptrs, sizeof(LIST_ENTRY*) * 256);

    RtlCopyMemory(ri, &r->root_item, sizeof(struct btrfs_root_item));

    // We ask here for a traverse_ptr to the item we're inserting, so we can
    // copy some of the tree's variables

    Status = insert_tree_item(Vcb, Vcb->root_root, id, BTRFS_ROOT_ITEM_KEY, offset, ri, sizeof(struct btrfs_root_item), &tp, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item returned %08lx\n", Status);
        ExFreePool(ri);
        ExFreePool(r->nonpaged);
        ExFreePool(r);
        return Status;
    }

    ExInitializeResourceLite(&r->nonpaged->load_tree_lock);

    InsertTailList(&Vcb->roots, &r->list_entry);

    if (!no_tree) {
        tree* t = ExAllocatePoolWithTag(PagedPool, sizeof(tree), ALLOC_TAG);
        if (!t) {
            ERR("out of memory\n");

            delete_tree_item(Vcb, &tp);

            ExFreePool(r->nonpaged);
            ExFreePool(r);
            ExFreePool(ri);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        initialize_tree_search(t);
        t->nonpaged = NULL;

        t->is_unique = true;
        t->uniqueness_determined = true;
        t->buf = NULL;

        r->treeholder.tree = t;

        RtlZeroMemory(&t->header, sizeof(struct btrfs_header));
        memcpy(t->header.fsid, tp.tree->header.fsid, BTRFS_UUID_SIZE);
        t->header.bytenr = 0;
        t->header.flags = (BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT) | BTRFS_HEADER_FLAG_WRITTEN;
        memcpy(t->header.chunk_tree_uuid, tp.tree->header.chunk_tree_uuid,
               BTRFS_UUID_SIZE);
        t->header.generation = Vcb->superblock.generation;
        t->header.owner = id;
        t->header.nritems = 0;
        t->header.level = 0;

        t->has_address = false;
        t->size = 0;
        t->Vcb = Vcb;
        t->parent = NULL;
        t->paritem = NULL;
        t->root = r;

        InitializeListHead(&t->itemlist);

        t->new_address = 0;
        t->has_new_address = false;
        t->updated_extents = false;

        InsertTailList(&Vcb->trees, &t->list_entry);
        t->list_entry_hash.Flink = NULL;

        t->write = true;
        Vcb->need_write = true;
    }

    *rootptr = r;

    return STATUS_SUCCESS;
}
