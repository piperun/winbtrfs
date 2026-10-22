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

static int compare_changed_extent(const changed_extent* ce, uint64_t address, uint64_t size) {
    if (ce->address != address)
        return ce->address < address ? -1 : 1;
    if (ce->size != size)
        return ce->size < size ? -1 : 1;
    return 0;
}

static unsigned extent_height(const changed_extent* ce) {
    return ce ? ce->height : 0;
}

static void update_extent_height(changed_extent* ce) {
    unsigned left = extent_height(ce->left), right = extent_height(ce->right);
    ce->height = (uint8_t)(1 + (left > right ? left : right));
}

static void replace_extent_node(chunk* c, changed_extent* old, changed_extent* replacement) {
    if (!old->parent)
        c->changed_extents_root = replacement;
    else if (old->parent->left == old)
        old->parent->left = replacement;
    else
        old->parent->right = replacement;
    if (replacement)
        replacement->parent = old->parent;
}

static changed_extent* rotate_extent_left(chunk* c, changed_extent* ce) {
    changed_extent* pivot = ce->right;
    ce->right = pivot->left;
    if (ce->right)
        ce->right->parent = ce;
    replace_extent_node(c, ce, pivot);
    pivot->left = ce;
    ce->parent = pivot;
    update_extent_height(ce);
    update_extent_height(pivot);
    return pivot;
}

static changed_extent* rotate_extent_right(chunk* c, changed_extent* ce) {
    changed_extent* pivot = ce->left;
    ce->left = pivot->right;
    if (ce->left)
        ce->left->parent = ce;
    replace_extent_node(c, ce, pivot);
    pivot->right = ce;
    ce->parent = pivot;
    update_extent_height(ce);
    update_extent_height(pivot);
    return pivot;
}

static void rebalance_changed_extents(chunk* c, changed_extent* ce) {
    while (ce) {
        int balance;
        update_extent_height(ce);
        balance = (int)extent_height(ce->left) - (int)extent_height(ce->right);
        if (balance > 1) {
            if (extent_height(ce->left->right) > extent_height(ce->left->left))
                rotate_extent_left(c, ce->left);
            ce = rotate_extent_right(c, ce);
        } else if (balance < -1) {
            if (extent_height(ce->right->left) > extent_height(ce->right->right))
                rotate_extent_right(c, ce->right);
            ce = rotate_extent_left(c, ce);
        }
        ce = ce->parent;
    }
}

void initialize_changed_extents(chunk* c) {
    InitializeListHead(&c->changed_extents);
    c->changed_extents_root = NULL;
}

void insert_changed_extent(chunk* c, changed_extent* ce) {
    changed_extent** link = &c->changed_extents_root;
    changed_extent* parent = NULL;

    // Upper-bound insertion preserves append order among equal keys. Rotations
    // preserve this in-order identity sequence, even when equal keys move left.
    while (*link) {
        parent = *link;
        if (compare_changed_extent(parent, ce->address, ce->size) > 0)
            link = &parent->left;
        else
            link = &parent->right;
    }
    ce->parent = parent;
    ce->left = ce->right = NULL;
    ce->height = 1;
    *link = ce;
    InsertTailList(&c->changed_extents, &ce->list_entry);
    rebalance_changed_extents(c, parent);
}

void remove_changed_extent(chunk* c, changed_extent* ce) {
    changed_extent* parent;

    if (ce->left && ce->right) {
        changed_extent* successor = ce->right;
        while (successor->left)
            successor = successor->left;
        if (successor->parent != ce) {
            parent = successor->parent;
            replace_extent_node(c, successor, successor->right);
            successor->right = ce->right;
            successor->right->parent = successor;
        } else
            parent = successor;
        // Move the node itself; copying its payload would change list identity.
        replace_extent_node(c, ce, successor);
        successor->left = ce->left;
        successor->left->parent = successor;
    } else {
        parent = ce->parent;
        replace_extent_node(c, ce, ce->left ? ce->left : ce->right);
    }
    RemoveEntryList(&ce->list_entry);
    ce->parent = ce->left = ce->right = NULL;
    ce->height = 1;
    rebalance_changed_extents(c, parent);
}

void move_changed_extent(chunk* source, chunk* destination, changed_extent* ce, uint64_t address) {
    remove_changed_extent(source, ce);
    ce->address = address;
    insert_changed_extent(destination, ce);
}

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
    changed_extent* ce = c->changed_extents_root;
    changed_extent* found = NULL;

    while (ce) {
        int order = compare_changed_extent(ce, address, size);
        if (order >= 0) {
            if (order == 0)
                found = ce;
            ce = ce->left;
        } else
            ce = ce->right;
    }
    if (found)
        return found;

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

    insert_changed_extent(c, ce);

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
            remove_changed_extent(c, ce);
            ExFreePool(ce);
            goto end;
        }

        if (tp.item->key.objectid != searchkey.objectid || tp.item->key.type != searchkey.type) {
            ERR("could not find address %I64x in extent tree\n", address);
            Status = STATUS_INTERNAL_ERROR;
            remove_changed_extent(c, ce);
            ExFreePool(ce);
            goto end;
        }

        if (tp.item->key.offset != size) {
            ERR("extent %I64x had size %I64x, not %I64x as expected\n", address, tp.item->key.offset, size);
            Status = STATUS_INTERNAL_ERROR;
            remove_changed_extent(c, ce);
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
            remove_changed_extent(c, ce);
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
                remove_changed_extent(c, ce);
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
            remove_changed_extent(c, ce);
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
