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

// Build data relocation references; allocation and metadata updates retain their caller locks.
#include "balance-relocation.h"
#include "btrfsioctl.h"
#include "crc32c.h"
#include <ntddstor.h>

static NTSTATUS data_reloc_add_tree_edr(_Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* metadata_items,
                                        data_reloc* dr, struct btrfs_extent_data_ref* edr, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    root* r = NULL;
    metadata_reloc* mr;
    uint64_t last_tree = 0;
    data_reloc_ref* ref;

    le = Vcb->roots.Flink;
    while (le != &Vcb->roots) {
        root* r2 = CONTAINING_RECORD(le, root, list_entry);

        if (r2->id == edr->root) {
            r = r2;
            break;
        }

        le = le->Flink;
    }

    if (!r) {
        ERR("could not find subvol %I64x\n", edr->root);
        return STATUS_INTERNAL_ERROR;
    }

    searchkey.objectid = edr->objectid;
    searchkey.type = BTRFS_EXTENT_DATA_KEY;
    searchkey.offset = 0;

    Status = find_item(Vcb, r, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (tp.item->key.objectid < searchkey.objectid || (tp.item->key.objectid == searchkey.objectid && tp.item->key.type < searchkey.type)) {
        traverse_ptr tp2;

        if (find_next_item(Vcb, &tp, &tp2, false, NULL))
            tp = tp2;
        else {
            ERR("could not find EXTENT_DATA for inode %I64x in root %I64x\n", searchkey.objectid, r->id);
            return STATUS_INTERNAL_ERROR;
        }
    }

    ref = NULL;

    while (tp.item->key.objectid == searchkey.objectid && tp.item->key.type == searchkey.type) {
        traverse_ptr tp2;

        if (tp.item->size >= offsetof(struct btrfs_file_extent_item, disk_bytenr)) {
            struct btrfs_file_extent_item* ed = (struct btrfs_file_extent_item*)tp.item->data;

            if ((ed->type == BTRFS_FILE_EXTENT_PREALLOC || ed->type == BTRFS_FILE_EXTENT_REG) && tp.item->size >= sizeof(struct btrfs_file_extent_item)) {
                if (ed->disk_bytenr == dr->address && ed->disk_num_bytes == dr->size && tp.item->key.offset - ed->offset == edr->offset) {
                    if (ref && last_tree == tp.tree->header.bytenr)
                        ref->edr.count++;
                    else {
                        ref = ExAllocatePoolWithTag(PagedPool, sizeof(data_reloc_ref), ALLOC_TAG);
                        if (!ref) {
                            ERR("out of memory\n");
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }

                        ref->type = BTRFS_EXTENT_DATA_REF_KEY;
                        RtlCopyMemory(&ref->edr, edr, sizeof(struct btrfs_extent_data_ref));
                        ref->edr.count = 1;

                        Status = add_metadata_reloc_parent(Vcb, metadata_items, tp.tree->header.bytenr, &mr, rollback);
                        if (!NT_SUCCESS(Status)) {
                            ERR("add_metadata_reloc_parent returned %08lx\n", Status);
                            ExFreePool(ref);
                            return Status;
                        }

                        last_tree = tp.tree->header.bytenr;
                        ref->parent = mr;

                        InsertTailList(&dr->refs, &ref->list_entry);
                    }
                }
            }
        }

        if (find_next_item(Vcb, &tp, &tp2, false, NULL))
            tp = tp2;
        else
            break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS add_data_reloc(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* items, LIST_ENTRY* metadata_items,
                               traverse_ptr* tp, chunk* c, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    data_reloc* dr;
    struct btrfs_extent_item* ei;
    uint16_t len;
    uint64_t inline_rc;
    uint8_t* ptr;

    dr = ExAllocatePoolWithTag(PagedPool, sizeof(data_reloc), ALLOC_TAG);
    if (!dr) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    dr->address = tp->item->key.objectid;
    dr->size = tp->item->key.offset;
    dr->ei = (struct btrfs_extent_item*)tp->item->data;
    InitializeListHead(&dr->refs);

    Status = delete_tree_item(Vcb, tp);
    if (!NT_SUCCESS(Status)) {
        ERR("delete_tree_item returned %08lx\n", Status);
        goto fail;
    }

    if (!c)
        c = get_chunk_from_address(Vcb, tp->item->key.objectid);

    if (c) {
        acquire_chunk_lock(c, Vcb);

        c->used -= tp->item->key.offset;

        Status = space_list_add(c, tp->item->key.objectid, tp->item->key.offset,
                                rollback);
        if (!NT_SUCCESS(Status)) {
            release_chunk_lock(c, Vcb);
            goto fail;
        }

        release_chunk_lock(c, Vcb);
    }

    ei = (struct btrfs_extent_item*)tp->item->data;
    inline_rc = 0;

    len = tp->item->size - sizeof(struct btrfs_extent_item);
    ptr = (uint8_t*)tp->item->data + sizeof(struct btrfs_extent_item);

    while (len > 0) {
        struct btrfs_extent_inline_ref* eir;

        eir = (struct btrfs_extent_inline_ref*)ptr;

        if (len < sizeof(struct btrfs_extent_inline_ref)) {
            ERR("(%I64x,%x,%I64x) was truncated\n", tp->item->key.objectid,
                tp->item->key.type, tp->item->key.offset);
            Status = STATUS_INTERNAL_ERROR;
            goto fail;
        }

        ptr += sizeof(struct btrfs_extent_inline_ref);
        len -= sizeof(struct btrfs_extent_inline_ref);

        switch (eir->type) {
            case BTRFS_EXTENT_DATA_REF_KEY: {
                struct btrfs_extent_data_ref* edr = (struct btrfs_extent_data_ref*)(ptr - sizeof(uint64_t));

                if (len < sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t)) {
                    ERR("(%I64x,%x,%I64x) was truncated\n", tp->item->key.objectid,
                        tp->item->key.type, tp->item->key.offset);
                    Status = STATUS_INTERNAL_ERROR;
                    goto fail;
                }

                ptr += sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t);
                len -= sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t);

                inline_rc += edr->count;

                Status = data_reloc_add_tree_edr(Vcb, metadata_items, dr, edr, rollback);
                if (!NT_SUCCESS(Status)) {
                    ERR("data_reloc_add_tree_edr returned %08lx\n", Status);
                    goto fail;
                }

                break;
            }

            case BTRFS_SHARED_DATA_REF_KEY: {
                struct btrfs_shared_data_ref* sdr = (struct btrfs_shared_data_ref*)ptr;
                metadata_reloc* mr;
                data_reloc_ref* ref;

                if (len < sizeof(struct btrfs_shared_data_ref)) {
                    ERR("(%I64x,%x,%I64x) was truncated\n", tp->item->key.objectid,
                        tp->item->key.type, tp->item->key.offset);
                    Status = STATUS_INTERNAL_ERROR;
                    goto fail;
                }

                ptr += sizeof(struct btrfs_shared_data_ref);
                len -= sizeof(struct btrfs_shared_data_ref);

                ref = ExAllocatePoolWithTag(PagedPool, sizeof(data_reloc_ref), ALLOC_TAG);
                if (!ref) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto fail;
                }

                ref->type = BTRFS_SHARED_DATA_REF_KEY;
                ref->offset = eir->offset;
                ref->sdr.count = sdr->count;

                inline_rc += ref->sdr.count;

                Status = add_metadata_reloc_parent(Vcb, metadata_items, ref->offset, &mr, rollback);
                if (!NT_SUCCESS(Status)) {
                    ERR("add_metadata_reloc_parent returned %08lx\n", Status);
                    ExFreePool(ref);
                    goto fail;
                }

                ref->parent = mr;

                InsertTailList(&dr->refs, &ref->list_entry);

                break;
            }

            default:
                ERR("unexpected tree type %x\n", eir->type);
                Status = STATUS_INTERNAL_ERROR;
                goto fail;
        }
    }

    if (inline_rc < ei->refs) { // look for non-inline entries
        traverse_ptr tp2 = *tp, next_tp;

        while (find_next_item(Vcb, &tp2, &next_tp, false, NULL)) {
            tp2 = next_tp;

            if (tp2.item->key.objectid == tp->item->key.objectid) {
                if (tp2.item->key.type == BTRFS_EXTENT_DATA_REF_KEY && tp2.item->size >= sizeof(struct btrfs_extent_data_ref)) {
                    Status = data_reloc_add_tree_edr(Vcb, metadata_items, dr,
                                                     (struct btrfs_extent_data_ref*)tp2.item->data,
                                                     rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("data_reloc_add_tree_edr returned %08lx\n", Status);
                        goto fail;
                    }

                    Status = delete_tree_item(Vcb, &tp2);
                    if (!NT_SUCCESS(Status)) {
                        ERR("delete_tree_item returned %08lx\n", Status);
                        goto fail;
                    }
                } else if (tp2.item->key.type == BTRFS_SHARED_DATA_REF_KEY && tp2.item->size >= sizeof(struct btrfs_shared_data_ref)) {
                    metadata_reloc* mr;
                    data_reloc_ref* ref;

                    ref = ExAllocatePoolWithTag(PagedPool, sizeof(data_reloc_ref), ALLOC_TAG);
                    if (!ref) {
                        ERR("out of memory\n");
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        goto fail;
                    }

                    ref->type = BTRFS_SHARED_DATA_REF_KEY;
                    ref->offset = tp2.item->key.offset;
                    ref->sdr.count = *((uint32_t*)tp2.item->data);

                    Status = add_metadata_reloc_parent(Vcb, metadata_items,
                                                       ref->offset, &mr, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_metadata_reloc_parent returned %08lx\n", Status);
                        ExFreePool(ref);
                        goto fail;
                    }

                    ref->parent = mr;
                    InsertTailList(&dr->refs, &ref->list_entry);

                    Status = delete_tree_item(Vcb, &tp2);
                    if (!NT_SUCCESS(Status)) {
                        ERR("delete_tree_item returned %08lx\n", Status);
                        goto fail;
                    }
                }
            } else
                break;
        }
    }

    InsertTailList(items, &dr->list_entry);

    return STATUS_SUCCESS;

fail:
    while (!IsListEmpty(&dr->refs)) {
        data_reloc_ref* ref = CONTAINING_RECORD(RemoveHeadList(&dr->refs),
                                                data_reloc_ref, list_entry);

        ExFreePool(ref);
    }

    ExFreePool(dr);

    return Status;
}

static void sort_data_reloc_refs(data_reloc* dr) {
    LIST_ENTRY newlist, *le;

    if (IsListEmpty(&dr->refs))
        return;

    // insertion sort

    InitializeListHead(&newlist);

    while (!IsListEmpty(&dr->refs)) {
        data_reloc_ref* ref = CONTAINING_RECORD(RemoveHeadList(&dr->refs), data_reloc_ref, list_entry);
        bool inserted = false;

        if (ref->type == BTRFS_EXTENT_DATA_REF_KEY)
            ref->hash = get_extent_data_ref_hash2(ref->edr.root, ref->edr.objectid, ref->edr.offset);
        else if (ref->type == BTRFS_SHARED_DATA_REF_KEY)
            ref->hash = ref->parent->new_address;

        le = newlist.Flink;
        while (le != &newlist) {
            data_reloc_ref* ref2 = CONTAINING_RECORD(le, data_reloc_ref, list_entry);

            if (ref->type < ref2->type || (ref->type == ref2->type && ref->hash > ref2->hash)) {
                InsertHeadList(le->Blink, &ref->list_entry);
                inserted = true;
                break;
            }

            le = le->Flink;
        }

        if (!inserted)
            InsertTailList(&newlist, &ref->list_entry);
    }

    le = newlist.Flink;
    while (le != &newlist) {
        data_reloc_ref* ref = CONTAINING_RECORD(le, data_reloc_ref, list_entry);

        if (le->Flink != &newlist) {
            data_reloc_ref* ref2 = CONTAINING_RECORD(le->Flink, data_reloc_ref, list_entry);

            if (ref->type == BTRFS_EXTENT_DATA_REF_KEY && ref2->type == BTRFS_EXTENT_DATA_REF_KEY && ref->edr.root == ref2->edr.root &&
                ref->edr.objectid == ref2->edr.objectid && ref->edr.offset == ref2->edr.offset) {
                RemoveEntryList(&ref2->list_entry);
                ref->edr.count += ref2->edr.count;
                ExFreePool(ref2);
                continue;
            }
        }

        le = le->Flink;
    }

    newlist.Flink->Blink = &dr->refs;
    newlist.Blink->Flink = &dr->refs;
    dr->refs.Flink = newlist.Flink;
    dr->refs.Blink = newlist.Blink;
}

NTSTATUS add_data_reloc_extent_item(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, data_reloc* dr) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    uint64_t rc = 0;
    uint16_t inline_len;
    bool all_inline = true;
    data_reloc_ref* first_noninline = NULL;
    struct btrfs_extent_item* ei;
    uint8_t* ptr;

    inline_len = sizeof(struct btrfs_extent_item);

    sort_data_reloc_refs(dr);

    le = dr->refs.Flink;
    while (le != &dr->refs) {
        data_reloc_ref* ref = CONTAINING_RECORD(le, data_reloc_ref, list_entry);
        uint16_t extlen = sizeof(struct btrfs_extent_inline_ref);

        switch (ref->type) {
            case BTRFS_EXTENT_DATA_REF_KEY:
                extlen += sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t);
                rc += ref->edr.count;
                break;

            case BTRFS_SHARED_DATA_REF_KEY:
                extlen += sizeof(struct btrfs_shared_data_ref);
                rc++;
                break;
        }

        if (all_inline) {
            if ((ULONG)(inline_len + extlen) > (Vcb->superblock.nodesize >> 2)) {
                all_inline = false;
                first_noninline = ref;
            } else
                inline_len += extlen;
        }

        le = le->Flink;
    }

    ei = ExAllocatePoolWithTag(PagedPool, inline_len, ALLOC_TAG);
    if (!ei) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ei->refs = rc;
    ei->generation = dr->ei->generation;
    ei->flags = dr->ei->flags;
    ptr = (uint8_t*)&ei[1];

    le = dr->refs.Flink;
    while (le != &dr->refs) {
        struct btrfs_extent_inline_ref* eir;

        data_reloc_ref* ref = CONTAINING_RECORD(le, data_reloc_ref, list_entry);

        if (ref == first_noninline)
            break;

        eir = (struct btrfs_extent_inline_ref*)ptr;
        eir->type = ref->type;

        ptr += sizeof(struct btrfs_extent_inline_ref);

        switch (ref->type) {
            case BTRFS_EXTENT_DATA_REF_KEY: {
                struct btrfs_extent_data_ref* edr = (struct btrfs_extent_data_ref*)(ptr - sizeof(uint64_t));

                RtlCopyMemory(edr, &ref->edr, sizeof(struct btrfs_extent_data_ref));

                ptr += sizeof(struct btrfs_extent_data_ref) - sizeof(uint64_t);

                break;
            }

            case BTRFS_SHARED_DATA_REF_KEY: {
                struct btrfs_shared_data_ref* sdr = (struct btrfs_shared_data_ref*)ptr;

                eir->offset = ref->parent->new_address;
                sdr->count = ref->sdr.count;

                ptr += sizeof(struct btrfs_shared_data_ref);

                break;
            }
        }

        le = le->Flink;
    }

    Status = insert_tree_item(Vcb, Vcb->extent_root, dr->new_address, BTRFS_EXTENT_ITEM_KEY, dr->size, ei, inline_len, NULL, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item returned %08lx\n", Status);
        return Status;
    }

    if (!all_inline) {
        le = &first_noninline->list_entry;

        while (le != &dr->refs) {
            data_reloc_ref* ref = CONTAINING_RECORD(le, data_reloc_ref, list_entry);

            switch (ref->type) {
                case BTRFS_EXTENT_DATA_REF_KEY: {
                    struct btrfs_extent_data_ref* edr;

                    edr = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_extent_data_ref), ALLOC_TAG);
                    if (!edr) {
                        ERR("out of memory\n");
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    RtlCopyMemory(edr, &ref->edr, sizeof(struct btrfs_extent_data_ref));

                    Status = insert_tree_item(Vcb, Vcb->extent_root, dr->new_address,
                                              BTRFS_EXTENT_DATA_REF_KEY, ref->hash, edr,
                                              sizeof(struct btrfs_extent_data_ref),
                                              NULL, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("insert_tree_item returned %08lx\n", Status);
                        return Status;
                    }

                    break;
                }

                case BTRFS_SHARED_DATA_REF_KEY: {
                    struct btrfs_shared_data_ref* sdr;

                    sdr = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_shared_data_ref),
                                                ALLOC_TAG);
                    if (!sdr) {
                        ERR("out of memory\n");
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    sdr->count = ref->sdr.count;

                    Status = insert_tree_item(Vcb, Vcb->extent_root, dr->new_address,
                                              BTRFS_SHARED_DATA_REF_KEY, ref->parent->new_address,
                                              sdr, sizeof(struct btrfs_shared_data_ref),
                                              NULL, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("insert_tree_item returned %08lx\n", Status);
                        return Status;
                    }

                    break;
                }
            }

            le = le->Flink;
        }
    }

    return STATUS_SUCCESS;
}
