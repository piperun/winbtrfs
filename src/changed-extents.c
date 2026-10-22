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

// Pending extent reference changes; callers retain the existing chunk/tree locks.
#include "btrfs_drv.h"

static uint32_t find_extent_data_refcount(device_extension* Vcb, uint64_t address, uint64_t size, uint64_t root, uint64_t objid, uint64_t offset, PIRP Irp) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    uint64_t inline_rc;

    searchkey.objectid = address;
    searchkey.type = BTRFS_EXTENT_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return 0;
    }

    if (tp.item->key.objectid != searchkey.objectid || tp.item->key.type != searchkey.type) {
        TRACE("could not find address %I64x in extent tree\n", address);
        return 0;
    }

    if (tp.item->key.offset != size) {
        ERR("extent %I64x had size %I64x, not %I64x as expected\n", address, tp.item->key.offset, size);
        return 0;
    }

    inline_rc = 0;

    if (tp.item->size >= sizeof(struct btrfs_extent_item)) {
        struct btrfs_extent_item* ei = (struct btrfs_extent_item*)tp.item->data;
        uint32_t len = tp.item->size - sizeof(struct btrfs_extent_item);
        uint8_t* ptr = (uint8_t*)&ei[1];

        while (len > 0) {
            struct btrfs_extent_inline_ref* eir = (struct btrfs_extent_inline_ref*)ptr;

            if (len < sizeof(struct btrfs_extent_inline_ref)) {
                ERR("(%I64x,%x,%I64x) was truncated\n", tp.item->key.objectid,
                    tp.item->key.type, tp.item->key.offset);

                return 0;
            }

            ptr += sizeof(struct btrfs_extent_inline_ref);
            len -= sizeof(struct btrfs_extent_inline_ref);

            switch (eir->type) {
                case BTRFS_EXTENT_DATA_REF_KEY: {
                    struct btrfs_extent_data_ref* sectedr = (struct btrfs_extent_data_ref*)(ptr - sizeof(uint64_t));

                    if (len < sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t)) {
                        ERR("(%I64x,%x,%I64x) was truncated\n", tp.item->key.objectid,
                            tp.item->key.type, tp.item->key.offset);
                        return 0;
                    }

                    ptr += sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t);
                    len -= sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t);
                    inline_rc += sectedr->count;

                    if (sectedr->root == root && sectedr->objectid == objid && sectedr->offset == offset)
                        return sectedr->count;

                    break;
                }

                case BTRFS_SHARED_DATA_REF_KEY: {
                    struct btrfs_shared_data_ref* sdr = (struct btrfs_shared_data_ref*)ptr;

                    if (len < sizeof(struct btrfs_shared_data_ref)) {
                        ERR("(%I64x,%x,%I64x) was truncated\n", tp.item->key.objectid,
                            tp.item->key.type, tp.item->key.offset);
                        return 0;
                    }

                    ptr += sizeof(struct btrfs_shared_data_ref);
                    len -= sizeof(struct btrfs_shared_data_ref);
                    inline_rc += sdr->count;

                    break;
                }

                default:
                    ERR("(%I64x,%x,%I64x): unexpected type %x\n",
                        tp.item->key.objectid, tp.item->key.type,
                        tp.item->key.offset, eir->type);
                    return 0;
            }
        }

        if (inline_rc == ei->refs)
            return 0;
    }

    searchkey.objectid = address;
    searchkey.type = BTRFS_EXTENT_DATA_REF_KEY;
    searchkey.offset = get_extent_data_ref_hash2(root, objid, offset);

    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return 0;
    }

    if (!keycmp(searchkey, tp.item->key)) {
        if (tp.item->size < sizeof(struct btrfs_extent_data_ref))
            ERR("(%I64x,%x,%I64x) has size %u, not %Iu as expected\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_extent_data_ref));
        else {
            struct btrfs_extent_data_ref* edr = (struct btrfs_extent_data_ref*)tp.item->data;

            return edr->count;
        }
    }

    return 0;
}

changed_extent* get_changed_extent_item(chunk* c, uint64_t address, uint64_t size,
                                        bool no_csum) {
    LIST_ENTRY* le;
    changed_extent* ce;

    le = c->changed_extents.Flink;
    while (le != &c->changed_extents) {
        ce = CONTAINING_RECORD(le, changed_extent, list_entry);

        if (ce->address == address && ce->size == size)
            return ce;

        le = le->Flink;
    }

    ce = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent), ALLOC_TAG);
    if (!ce) {
        ERR("out of memory\n");
        return NULL;
    }

    ce->address = address;
    ce->size = size;
    ce->old_size = size;
    ce->count = 0;
    ce->old_count = 0;
    ce->no_csum = no_csum;
    ce->superseded = false;
    InitializeListHead(&ce->refs);
    InitializeListHead(&ce->old_refs);

    InsertTailList(&c->changed_extents, &ce->list_entry);

    return ce;
}

NTSTATUS update_changed_extent_ref(device_extension* Vcb, chunk* c, uint64_t address,
                                   uint64_t size, uint64_t root, uint64_t objid,
                                   uint64_t offset, int32_t count, bool no_csum,
                                   bool superseded, PIRP Irp, LIST_ENTRY* rollback) {
    LIST_ENTRY* le;
    changed_extent* ce;
    changed_extent_ref* cer;
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    uint32_t old_count;
    rollback_item* ri;

    ExAcquireResourceExclusiveLite(&c->changed_extents_lock, true);

    ce = get_changed_extent_item(c, address, size, no_csum);

    if (!ce) {
        ERR("get_changed_extent_item failed\n");
        Status = STATUS_INTERNAL_ERROR;
        goto end;
    }

    if (IsListEmpty(&ce->refs) && IsListEmpty(&ce->old_refs)) { // new entry
        searchkey.objectid = address;
        searchkey.type = BTRFS_EXTENT_ITEM_KEY;
        searchkey.offset = 0xffffffffffffffff;

        Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            RemoveEntryList(&ce->list_entry);
            ExFreePool(ce);
            goto end;
        }

        if (tp.item->key.objectid != searchkey.objectid || tp.item->key.type != searchkey.type) {
            ERR("could not find address %I64x in extent tree\n", address);
            Status = STATUS_INTERNAL_ERROR;
            RemoveEntryList(&ce->list_entry);
            ExFreePool(ce);
            goto end;
        }

        if (tp.item->key.offset != size) {
            ERR("extent %I64x had size %I64x, not %I64x as expected\n", address, tp.item->key.offset, size);
            Status = STATUS_INTERNAL_ERROR;
            RemoveEntryList(&ce->list_entry);
            ExFreePool(ce);
            goto end;
        }

        if (tp.item->size == sizeof(struct btrfs_extent_item_v0)) {
            struct btrfs_extent_item_v0* eiv0 = (struct btrfs_extent_item_v0*)tp.item->data;

            ce->count = ce->old_count = eiv0->refs;
        } else if (tp.item->size >= sizeof(struct btrfs_extent_item)) {
            struct btrfs_extent_item* ei = (struct btrfs_extent_item*)tp.item->data;

            ce->count = ce->old_count = ei->refs;
        } else {
            ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_extent_item));
            Status = STATUS_INTERNAL_ERROR;
            RemoveEntryList(&ce->list_entry);
            ExFreePool(ce);
            goto end;
        }
    }

    if (rollback) {
        ri = ExAllocatePoolWithTag(PagedPool, sizeof(rollback_item), ALLOC_TAG);
        if (!ri) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        ri->type = ROLLBACK_UPDATE_CHANGED_EXTENT_REF;
        ri->changed_extent_ref.address = address;
        ri->changed_extent_ref.size = size;
        ri->changed_extent_ref.root = root;
        ri->changed_extent_ref.objid = objid;
        ri->changed_extent_ref.offset = offset;
        ri->changed_extent_ref.count = count;
    }

    le = ce->refs.Flink;
    while (le != &ce->refs) {
        cer = CONTAINING_RECORD(le, changed_extent_ref, list_entry);

        if (cer->type == BTRFS_EXTENT_DATA_REF_KEY && cer->edr.root == root && cer->edr.objectid == objid && cer->edr.offset == offset) {
            ce->count += count;
            cer->edr.count += count;
            Status = STATUS_SUCCESS;

            if (superseded)
                ce->superseded = true;

            if (rollback)
                InsertTailList(rollback, &ri->list_entry);

            goto end;
        }

        le = le->Flink;
    }

    old_count = find_extent_data_refcount(Vcb, address, size, root, objid, offset, Irp);

    if (old_count > 0) {
        cer = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent_ref), ALLOC_TAG);

        if (!cer) {
            ERR("out of memory\n");

            if (rollback)
                ExFreePool(ri);

            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        cer->type = BTRFS_EXTENT_DATA_REF_KEY;
        cer->edr.root = root;
        cer->edr.objectid = objid;
        cer->edr.offset = offset;
        cer->edr.count = old_count;

        InsertTailList(&ce->old_refs, &cer->list_entry);
    }

    cer = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent_ref), ALLOC_TAG);

    if (!cer) {
        ERR("out of memory\n");

        if (rollback)
            ExFreePool(ri);

        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    cer->type = BTRFS_EXTENT_DATA_REF_KEY;
    cer->edr.root = root;
    cer->edr.objectid = objid;
    cer->edr.offset = offset;
    cer->edr.count = old_count + count;

    InsertTailList(&ce->refs, &cer->list_entry);

    ce->count += count;

    if (superseded)
        ce->superseded = true;

    if (rollback)
        InsertTailList(rollback, &ri->list_entry);

    Status = STATUS_SUCCESS;

end:
    ExReleaseResourceLite(&c->changed_extents_lock);

    return Status;
}

NTSTATUS add_changed_extent_ref(chunk* c, uint64_t address, uint64_t size,
                                uint64_t root, uint64_t objid, uint64_t offset,
                                uint32_t count, bool no_csum, LIST_ENTRY* rollback) {
    changed_extent* ce;
    changed_extent_ref* cer;
    rollback_item* ri;
    LIST_ENTRY* le;

    ce = get_changed_extent_item(c, address, size, no_csum);
    if (!ce) {
        ERR("get_changed_extent_item failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (rollback) {
        ri = ExAllocatePoolWithTag(PagedPool, sizeof(rollback_item), ALLOC_TAG);
        if (!ri) {
            ERR("out of memory\n");

            if (IsListEmpty(&ce->refs) && ce->count == 0) {
                RemoveEntryList(&ce->list_entry);
                ExFreePool(ce);
            }

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ri->type = ROLLBACK_UPDATE_CHANGED_EXTENT_REF;
        ri->changed_extent_ref.address = address;
        ri->changed_extent_ref.size = size;
        ri->changed_extent_ref.root = root;
        ri->changed_extent_ref.objid = objid;
        ri->changed_extent_ref.offset = offset;
        ri->changed_extent_ref.count = count;
    }

    le = ce->refs.Flink;
    while (le != &ce->refs) {
        cer = CONTAINING_RECORD(le, changed_extent_ref, list_entry);

        if (cer->type == BTRFS_EXTENT_DATA_REF_KEY && cer->edr.root == root && cer->edr.objectid == objid && cer->edr.offset == offset) {
            ce->count += count;
            cer->edr.count += count;

            if (rollback)
                InsertTailList(rollback, &ri->list_entry);

            return STATUS_SUCCESS;
        }

        le = le->Flink;
    }

    cer = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent_ref), ALLOC_TAG);

    if (!cer) {
        ERR("out of memory\n");

        if (IsListEmpty(&ce->refs) && ce->count == 0) {
            RemoveEntryList(&ce->list_entry);
            ExFreePool(ce);
        }

        if (rollback)
            ExFreePool(ri);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    cer->type = BTRFS_EXTENT_DATA_REF_KEY;
    cer->edr.root = root;
    cer->edr.objectid = objid;
    cer->edr.offset = offset;
    cer->edr.count = count;

    InsertTailList(&ce->refs, &cer->list_entry);

    ce->count += count;

    if (rollback)
        InsertTailList(rollback, &ri->list_entry);

    return STATUS_SUCCESS;
}
