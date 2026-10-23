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

#include "balance-relocation.h"
#include "btrfsioctl.h"
#include "crc32c.h"
#include <ntddstor.h>



static NTSTATUS add_metadata_reloc(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* items, traverse_ptr* tp,
                                   bool skinny, metadata_reloc** mr2, chunk* c, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    metadata_reloc* mr;
    struct btrfs_extent_item* ei;
    uint16_t len;
    uint64_t inline_rc;
    uint8_t* ptr;

    if (!skinny && tp->item->size < sizeof(struct btrfs_extent_item) + sizeof(struct btrfs_tree_block_info)) {
        ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n",
            tp->item->key.objectid, tp->item->key.type, tp->item->key.offset,
            tp->item->size, sizeof(struct btrfs_extent_item) + sizeof(struct btrfs_tree_block_info));
        return STATUS_INTERNAL_ERROR;
    }

    mr = ExAllocatePoolWithTag(PagedPool, sizeof(metadata_reloc), ALLOC_TAG);
    if (!mr) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mr->address = tp->item->key.objectid;
    mr->data = NULL;
    mr->ei = (struct btrfs_extent_item*)tp->item->data;
    mr->system = false;
    InitializeListHead(&mr->refs);

    Status = delete_tree_item(Vcb, tp);
    if (!NT_SUCCESS(Status)) {
        ERR("delete_tree_item returned %08lx\n", Status);
        goto fail;
    }

    if (!c)
        c = get_chunk_from_address(Vcb, tp->item->key.objectid);

    if (c) {
        acquire_chunk_lock(c, Vcb);

        c->used -= Vcb->superblock.nodesize;

        Status = space_list_add(c, tp->item->key.objectid,
                                Vcb->superblock.nodesize, rollback);
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
    if (!skinny) {
        len -= sizeof(struct btrfs_tree_block_info);
        ptr += sizeof(struct btrfs_tree_block_info);
    }

    while (len > 0) {
        struct btrfs_extent_inline_ref* eir = (struct btrfs_extent_inline_ref*)ptr;
        metadata_reloc_ref* ref;

        if (len < sizeof(struct btrfs_extent_inline_ref)) {
            ERR("(%I64x,%x,%I64x) was truncated\n", tp->item->key.objectid,
                tp->item->key.type, tp->item->key.offset);
            Status = STATUS_INTERNAL_ERROR;
            goto fail;
        }

        ptr += sizeof(struct btrfs_extent_inline_ref);
        len -= sizeof(struct btrfs_extent_inline_ref);

        ref = ExAllocatePoolWithTag(PagedPool, sizeof(metadata_reloc_ref), ALLOC_TAG);
        if (!ref) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }

        switch (eir->type) {
            case BTRFS_TREE_BLOCK_REF_KEY:
            case BTRFS_SHARED_BLOCK_REF_KEY:
                ref->type = eir->type;
                ref->offset = eir->offset;
                inline_rc++;
                break;

            default:
                ERR("unexpected tree type %x\n", eir->type);
                ExFreePool(ref);
                Status = STATUS_INTERNAL_ERROR;
                goto fail;
        }

        ref->parent = NULL;
        ref->top = false;
        InsertTailList(&mr->refs, &ref->list_entry);
    }

    if (inline_rc < ei->refs) { // look for non-inline entries
        traverse_ptr tp2 = *tp, next_tp;

        while (find_next_item(Vcb, &tp2, &next_tp, false, NULL)) {
            tp2 = next_tp;

            if (tp2.item->key.objectid == tp->item->key.objectid) {
                switch (tp2.item->key.type) {
                    case BTRFS_TREE_BLOCK_REF_KEY: {
                        metadata_reloc_ref* ref = ExAllocatePoolWithTag(PagedPool, sizeof(metadata_reloc_ref), ALLOC_TAG);
                        if (!ref) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto fail;
                        }

                        ref->type = BTRFS_TREE_BLOCK_REF_KEY;
                        ref->offset = tp2.item->key.offset;
                        ref->parent = NULL;
                        ref->top = false;
                        InsertTailList(&mr->refs, &ref->list_entry);

                        Status = delete_tree_item(Vcb, &tp2);
                        if (!NT_SUCCESS(Status)) {
                            ERR("delete_tree_item returned %08lx\n", Status);
                            goto fail;
                        }

                        break;
                    }

                    case BTRFS_SHARED_BLOCK_REF_KEY: {
                        metadata_reloc_ref* ref = ExAllocatePoolWithTag(PagedPool, sizeof(metadata_reloc_ref), ALLOC_TAG);
                        if (!ref) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto fail;
                        }

                        ref->type = BTRFS_SHARED_BLOCK_REF_KEY;
                        ref->offset = tp2.item->key.offset;
                        ref->parent = NULL;
                        ref->top = false;
                        InsertTailList(&mr->refs, &ref->list_entry);

                        Status = delete_tree_item(Vcb, &tp2);
                        if (!NT_SUCCESS(Status)) {
                            ERR("delete_tree_item returned %08lx\n", Status);
                            goto fail;
                        }

                        break;
                    }
                }
            } else
                break;
        }
    }

    InsertTailList(items, &mr->list_entry);

    if (mr2)
        *mr2 = mr;

    return STATUS_SUCCESS;

fail:
    while (!IsListEmpty(&mr->refs)) {
        metadata_reloc_ref* ref = CONTAINING_RECORD(RemoveHeadList(&mr->refs),
                                                    metadata_reloc_ref, list_entry);

        ExFreePool(ref);
    }

    ExFreePool(mr);

    return Status;
}

NTSTATUS add_metadata_reloc_parent(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* items,
                                          uint64_t address, metadata_reloc** mr2, LIST_ENTRY* rollback) {
    LIST_ENTRY* le;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    bool skinny = false;
    NTSTATUS Status;

    le = items->Flink;
    while (le != items) {
        metadata_reloc* mr = CONTAINING_RECORD(le, metadata_reloc, list_entry);

        if (mr->address == address) {
            *mr2 = mr;
            return STATUS_SUCCESS;
        }

        le = le->Flink;
    }

    searchkey.objectid = address;
    searchkey.type = BTRFS_METADATA_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (tp.item->key.objectid == address && tp.item->key.type == BTRFS_METADATA_ITEM_KEY && tp.item->size >= sizeof(struct btrfs_extent_item))
        skinny = true;
    else if (tp.item->key.objectid == address && tp.item->key.type == BTRFS_EXTENT_ITEM_KEY && tp.item->key.offset == Vcb->superblock.nodesize &&
             tp.item->size >= sizeof(struct btrfs_extent_item)) {
        struct btrfs_extent_item* ei = (struct btrfs_extent_item*)tp.item->data;

        if (!(ei->flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)) {
            ERR("EXTENT_ITEM for %I64x found, but tree flag not set\n", address);
            return STATUS_INTERNAL_ERROR;
        }
    } else {
        ERR("could not find valid EXTENT_ITEM for address %I64x\n", address);
        return STATUS_INTERNAL_ERROR;
    }

    Status = add_metadata_reloc(Vcb, items, &tp, skinny, mr2, NULL, rollback);
    if (!NT_SUCCESS(Status)) {
        ERR("add_metadata_reloc returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

static void sort_metadata_reloc_refs(metadata_reloc* mr) {
    LIST_ENTRY newlist, *le;

    if (mr->refs.Flink == mr->refs.Blink) // 0 or 1 items
        return;

    // insertion sort

    InitializeListHead(&newlist);

    while (!IsListEmpty(&mr->refs)) {
        metadata_reloc_ref* ref = CONTAINING_RECORD(RemoveHeadList(&mr->refs), metadata_reloc_ref, list_entry);
        bool inserted = false;

        if (ref->type == BTRFS_TREE_BLOCK_REF_KEY)
            ref->hash = ref->offset;
        else if (ref->type == BTRFS_SHARED_BLOCK_REF_KEY)
            ref->hash = ref->parent->new_address;

        le = newlist.Flink;
        while (le != &newlist) {
            metadata_reloc_ref* ref2 = CONTAINING_RECORD(le, metadata_reloc_ref, list_entry);

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

    newlist.Flink->Blink = &mr->refs;
    newlist.Blink->Flink = &mr->refs;
    mr->refs.Flink = newlist.Flink;
    mr->refs.Blink = newlist.Blink;
}

static NTSTATUS add_metadata_reloc_extent_item(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, metadata_reloc* mr) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    uint64_t rc = 0;
    uint16_t inline_len;
    bool all_inline = true;
    metadata_reloc_ref* first_noninline = NULL;
    struct btrfs_extent_item* ei;
    uint8_t* ptr;

    inline_len = sizeof(struct btrfs_extent_item);
    if (!(Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA))
        inline_len += sizeof(struct btrfs_tree_block_info);

    sort_metadata_reloc_refs(mr);

    le = mr->refs.Flink;
    while (le != &mr->refs) {
        metadata_reloc_ref* ref = CONTAINING_RECORD(le, metadata_reloc_ref, list_entry);

        rc++;

        if (all_inline) {
            if ((ULONG)(inline_len + sizeof(struct btrfs_extent_inline_ref)) > (Vcb->superblock.nodesize >> 2)) {
                all_inline = false;
                first_noninline = ref;
            } else
                inline_len += sizeof(struct btrfs_extent_inline_ref);
        }

        le = le->Flink;
    }

    ei = ExAllocatePoolWithTag(PagedPool, inline_len, ALLOC_TAG);
    if (!ei) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ei->refs = rc;
    ei->generation = mr->ei->generation;
    ei->flags = mr->ei->flags;
    ptr = (uint8_t*)&ei[1];

    if (!(Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)) {
        struct btrfs_tree_block_info* ei2 = (struct btrfs_tree_block_info*)ptr;

        ei2->key = *(struct btrfs_key*)&mr->data[1];
        ei2->level = mr->data->level;

        ptr += sizeof(struct btrfs_tree_block_info);
    }

    le = mr->refs.Flink;
    while (le != &mr->refs) {
        struct btrfs_extent_inline_ref* eir;

        metadata_reloc_ref* ref = CONTAINING_RECORD(le, metadata_reloc_ref, list_entry);

        if (ref == first_noninline)
            break;

        eir = (struct btrfs_extent_inline_ref*)ptr;
        ptr += sizeof(struct btrfs_extent_inline_ref);

        eir->type = ref->type;

        if (ref->type == BTRFS_TREE_BLOCK_REF_KEY)
            eir->offset = ref->offset;
        else if (ref->type == BTRFS_SHARED_BLOCK_REF_KEY)
            eir->offset = ref->parent->new_address;

        le = le->Flink;
    }

    if (Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)
        Status = insert_tree_item(Vcb, Vcb->extent_root, mr->new_address, BTRFS_METADATA_ITEM_KEY, mr->data->level, ei, inline_len, NULL, NULL);
    else
        Status = insert_tree_item(Vcb, Vcb->extent_root, mr->new_address, BTRFS_EXTENT_ITEM_KEY, Vcb->superblock.nodesize, ei, inline_len, NULL, NULL);

    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item returned %08lx\n", Status);
        ExFreePool(ei);
        return Status;
    }

    if (!all_inline) {
        le = &first_noninline->list_entry;

        while (le != &mr->refs) {
            metadata_reloc_ref* ref = CONTAINING_RECORD(le, metadata_reloc_ref, list_entry);

            if (ref->type == BTRFS_TREE_BLOCK_REF_KEY) {
                Status = insert_tree_item(Vcb, Vcb->extent_root, mr->new_address,
                                          BTRFS_TREE_BLOCK_REF_KEY, ref->offset, NULL,
                                          0, NULL, NULL);
                if (!NT_SUCCESS(Status)) {
                    ERR("insert_tree_item returned %08lx\n", Status);
                    return Status;
                }
            } else if (ref->type == BTRFS_SHARED_BLOCK_REF_KEY) {
                Status = insert_tree_item(Vcb, Vcb->extent_root, mr->new_address,
                                          BTRFS_SHARED_BLOCK_REF_KEY, ref->parent->new_address,
                                          NULL, 0, NULL, NULL);
                if (!NT_SUCCESS(Status)) {
                    ERR("insert_tree_item returned %08lx\n", Status);
                    return Status;
                }
            }

            le = le->Flink;
        }
    }

    if (ei->flags & BTRFS_BLOCK_FLAG_FULL_BACKREF || mr->data->flags & BTRFS_HEADER_FLAG_RELOC || !(mr->data->flags & BTRFS_BACKREF_REV_MASK)) {
        if (mr->data->level > 0) {
            uint16_t i;
            struct btrfs_key_ptr* in = (struct btrfs_key_ptr*)&mr->data[1];

            for (i = 0; i < mr->data->nritems; i++) {
                uint64_t sbrrc = find_extent_shared_tree_refcount(Vcb, in[i].blockptr, mr->address, NULL);

                if (sbrrc > 0) {
                    Status = increase_extent_refcount_shared_block(Vcb, in[i].blockptr, mr->new_address,
                                                                   &in[i].key, mr->data->level - 1, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("increase_extent_refcount_shared_block returned %08lx\n", Status);
                        return Status;
                    }

                    Status = decrease_extent_refcount_shared_block(Vcb, in[i].blockptr, mr->address,
                                                                   NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("decrease_extent_refcount_shared_block returned %08lx\n", Status);
                        return Status;
                    }
                }
            }
        } else {
            uint16_t i;
            struct btrfs_item* ln = (struct btrfs_item*)&mr->data[1];

            for (i = 0; i < mr->data->nritems; i++) {
                if (ln[i].key.type == BTRFS_EXTENT_DATA_KEY && ln[i].size >= sizeof(struct btrfs_file_extent_item)) {
                    struct btrfs_file_extent_item* ed = (struct btrfs_file_extent_item*)((uint8_t*)mr->data + sizeof(struct btrfs_header) + ln[i].offset);

                    if (ed->type == BTRFS_FILE_EXTENT_REG || ed->type == BTRFS_FILE_EXTENT_PREALLOC) {
                        if (ed->disk_num_bytes > 0) { // not sparse
                            uint32_t sdrrc = find_extent_shared_data_refcount(Vcb, ed->disk_bytenr, mr->address, NULL);

                            if (sdrrc > 0) {
                                chunk* c;

                                Status = increase_extent_refcount_shared_data(Vcb, ed->disk_bytenr,
                                                                              ed->disk_num_bytes, mr->new_address,
                                                                              sdrrc, NULL);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("increase_extent_refcount_shared_data returned %08lx\n", Status);
                                    return Status;
                                }

                                Status = decrease_extent_refcount_shared_data(Vcb, ed->disk_bytenr,
                                                                              ed->disk_num_bytes,
                                                                              mr->address, sdrrc,
                                                                              false, NULL);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("decrease_extent_refcount_shared_data returned %08lx\n", Status);
                                    return Status;
                                }

                                c = get_chunk_from_address(Vcb, ed->disk_bytenr);

                                if (c) {
                                    // check changed_extents

                                    ExAcquireResourceExclusiveLite(&c->changed_extents_lock, true);

                                    le = c->changed_extents.Flink;

                                    while (le != &c->changed_extents) {
                                        changed_extent* ce = CONTAINING_RECORD(le, changed_extent, list_entry);

                                        if (ce->address == ed->disk_bytenr) {
                                            LIST_ENTRY* le2;

                                            le2 = ce->refs.Flink;
                                            while (le2 != &ce->refs) {
                                                changed_extent_ref* cer = CONTAINING_RECORD(le2, changed_extent_ref, list_entry);

                                                if (cer->type == BTRFS_SHARED_DATA_REF_KEY && cer->offset == mr->address) {
                                                    cer->offset = mr->new_address;
                                                    break;
                                                }

                                                le2 = le2->Flink;
                                            }

                                            le2 = ce->old_refs.Flink;
                                            while (le2 != &ce->old_refs) {
                                                changed_extent_ref* cer = CONTAINING_RECORD(le2, changed_extent_ref, list_entry);

                                                if (cer->type == BTRFS_SHARED_DATA_REF_KEY && cer->offset == mr->address) {
                                                    cer->offset = mr->new_address;
                                                    break;
                                                }

                                                le2 = le2->Flink;
                                            }

                                            break;
                                        }

                                        le = le->Flink;
                                    }

                                    ExReleaseResourceLite(&c->changed_extents_lock);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS write_metadata_items(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* items,
                                     LIST_ENTRY* data_items, chunk* c, LIST_ENTRY* rollback) {
    LIST_ENTRY tree_writes, *le;
    NTSTATUS Status;
    traverse_ptr tp;
    uint8_t level, max_level = 0;
    chunk* newchunk = NULL;

    InitializeListHead(&tree_writes);

    le = items->Flink;
    while (le != items) {
        metadata_reloc* mr = CONTAINING_RECORD(le, metadata_reloc, list_entry);
        LIST_ENTRY* le2;
        chunk* pc;

        mr->data = ExAllocatePoolWithTag(PagedPool, Vcb->superblock.nodesize, ALLOC_TAG);
        if (!mr->data) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Status = read_data(Vcb, mr->address, Vcb->superblock.nodesize, NULL, true, (uint8_t*)mr->data,
                           c && mr->address >= c->offset && mr->address < c->offset + c->chunk_item->length ? c : NULL, &pc, NULL, 0, false, NormalPagePriority);
        if (!NT_SUCCESS(Status)) {
            ERR("read_data returned %08lx\n", Status);
            return Status;
        }

        if (pc->chunk_item->type & BTRFS_BLOCK_GROUP_SYSTEM)
            mr->system = true;

        if (data_items && mr->data->level == 0) {
            le2 = data_items->Flink;
            while (le2 != data_items) {
                data_reloc* dr = CONTAINING_RECORD(le2, data_reloc, list_entry);
                struct btrfs_item* ln = (struct btrfs_item*)&mr->data[1];
                uint16_t i;

                for (i = 0; i < mr->data->nritems; i++) {
                    if (ln[i].key.type == BTRFS_EXTENT_DATA_KEY && ln[i].size >= sizeof(struct btrfs_file_extent_item)) {
                        struct btrfs_file_extent_item* ed = (struct btrfs_file_extent_item*)((uint8_t*)mr->data + sizeof(struct btrfs_header) + ln[i].offset);

                        if (ed->type == BTRFS_FILE_EXTENT_REG || ed->type == BTRFS_FILE_EXTENT_PREALLOC) {
                            if (ed->disk_bytenr == dr->address)
                                ed->disk_bytenr = dr->new_address;
                        }
                    }
                }

                le2 = le2->Flink;
            }
        }

        if (mr->data->level > max_level)
            max_level = mr->data->level;

        le2 = mr->refs.Flink;
        while (le2 != &mr->refs) {
            metadata_reloc_ref* ref = CONTAINING_RECORD(le2, metadata_reloc_ref, list_entry);

            switch (ref->type) {
                case BTRFS_TREE_BLOCK_REF_KEY: {
                    struct btrfs_key* firstitem;
                    root* r = NULL;
                    LIST_ENTRY* le3;
                    tree* t;

                    firstitem = (struct btrfs_key*)&mr->data[1];

                    le3 = Vcb->roots.Flink;
                    while (le3 != &Vcb->roots) {
                        root* r2 = CONTAINING_RECORD(le3, root, list_entry);

                        if (r2->id == ref->offset) {
                            r = r2;
                            break;
                        }

                        le3 = le3->Flink;
                    }

                    if (!r) {
                        ERR("could not find subvol with id %I64x\n", ref->offset);
                        return STATUS_INTERNAL_ERROR;
                    }

                    Status = find_item_to_level(Vcb, r, &tp, firstitem, false, mr->data->level + 1, NULL);
                    if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
                        ERR("find_item_to_level returned %08lx\n", Status);
                        return Status;
                    }

                    t = tp.tree;
                    while (t && t->header.level < mr->data->level + 1) {
                        t = t->parent;
                    }

                    if (!t)
                        ref->top = true;
                    else {
                        metadata_reloc* mr2;

                        Status = add_metadata_reloc_parent(Vcb, items, t->header.bytenr, &mr2, rollback);
                        if (!NT_SUCCESS(Status)) {
                            ERR("add_metadata_reloc_parent returned %08lx\n", Status);
                            return Status;
                        }

                        ref->parent = mr2;
                    }

                    break;
                }

                case BTRFS_SHARED_BLOCK_REF_KEY: {
                    metadata_reloc* mr2;

                    Status = add_metadata_reloc_parent(Vcb, items, ref->offset, &mr2, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_metadata_reloc_parent returned %08lx\n", Status);
                        return Status;
                    }

                    ref->parent = mr2;

                    break;
                }
            }

            le2 = le2->Flink;
        }

        le = le->Flink;
    }

    le = items->Flink;
    while (le != items) {
        metadata_reloc* mr = CONTAINING_RECORD(le, metadata_reloc, list_entry);
        LIST_ENTRY* le2;
        uint32_t hash;

        mr->t = NULL;

        hash = calc_crc32c(0xffffffff, (uint8_t*)&mr->address, sizeof(uint64_t));

        le2 = Vcb->trees_ptrs[hash >> 24];

        if (le2) {
            while (le2 != &Vcb->trees_hash) {
                tree* t = CONTAINING_RECORD(le2, tree, list_entry_hash);

                if (t->header.bytenr == mr->address) {
                    mr->t = t;
                    break;
                } else if (t->hash > hash)
                    break;

                le2 = le2->Flink;
            }
        }

        le = le->Flink;
    }

    for (level = 0; level <= max_level; level++) {
        le = items->Flink;
        while (le != items) {
            metadata_reloc* mr = CONTAINING_RECORD(le, metadata_reloc, list_entry);

            if (mr->data->level == level) {
                bool done = false;
                LIST_ENTRY* le2;
                tree_write* tw;
                uint64_t flags;
                tree* t3;

                if (mr->system)
                    flags = Vcb->system_flags;
                else if (Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS)
                    flags = Vcb->data_flags;
                else
                    flags = Vcb->metadata_flags;

                if (newchunk) {
                    acquire_chunk_lock(newchunk, Vcb);

                    if (newchunk->chunk_item->type == flags && find_metadata_address_in_chunk(Vcb, newchunk, &mr->new_address)) {
                        newchunk->used += Vcb->superblock.nodesize;

                        Status = space_list_subtract(newchunk, mr->new_address,
                                                     Vcb->superblock.nodesize,
                                                     rollback);
                        if (!NT_SUCCESS(Status)) {
                            release_chunk_lock(newchunk, Vcb);
                            return Status;
                        }

                        done = true;
                    }

                    release_chunk_lock(newchunk, Vcb);
                }

                if (!done) {
                    ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);

                    le2 = Vcb->chunks.Flink;
                    while (le2 != &Vcb->chunks) {
                        chunk* c2 = CONTAINING_RECORD(le2, chunk, list_entry);

                        if (!c2->readonly && !c2->reloc && c2 != newchunk && c2->chunk_item->type == flags) {
                            acquire_chunk_lock(c2, Vcb);

                            if ((c2->chunk_item->length - c2->used) >= Vcb->superblock.nodesize) {
                                if (find_metadata_address_in_chunk(Vcb, c2, &mr->new_address)) {
                                    c2->used += Vcb->superblock.nodesize;

                                    Status = space_list_subtract(c2, mr->new_address,
                                                                 Vcb->superblock.nodesize,
                                                                 rollback);
                                    release_chunk_lock(c2, Vcb);

                                    if (!NT_SUCCESS(Status))
                                        return Status;

                                    newchunk = c2;
                                    done = true;
                                    break;
                                }
                            }

                            release_chunk_lock(c2, Vcb);
                        }

                        le2 = le2->Flink;
                    }

                    // allocate new chunk if necessary
                    if (!done) {
                        Status = alloc_chunk(Vcb, flags, &newchunk, false);

                        if (!NT_SUCCESS(Status)) {
                            ERR("alloc_chunk returned %08lx\n", Status);
                            ExReleaseResourceLite(&Vcb->chunk_lock);
                            goto end;
                        }

                        acquire_chunk_lock(newchunk, Vcb);

                        newchunk->balance_num = Vcb->balance.balance_num;

                        if (!find_metadata_address_in_chunk(Vcb, newchunk, &mr->new_address)) {
                            release_chunk_lock(newchunk, Vcb);
                            ExReleaseResourceLite(&Vcb->chunk_lock);
                            ERR("could not find address in new chunk\n");
                            Status = STATUS_DISK_FULL;
                            goto end;
                        } else {
                            newchunk->used += Vcb->superblock.nodesize;

                            Status = space_list_subtract(newchunk, mr->new_address,
                                                         Vcb->superblock.nodesize,
                                                         rollback);
                            if (!NT_SUCCESS(Status)) {
                                release_chunk_lock(newchunk, Vcb);
                                goto end;
                            }
                        }

                        release_chunk_lock(newchunk, Vcb);
                    }

                    ExReleaseResourceLite(&Vcb->chunk_lock);
                }

                // update parents
                le2 = mr->refs.Flink;
                while (le2 != &mr->refs) {
                    metadata_reloc_ref* ref = CONTAINING_RECORD(le2, metadata_reloc_ref, list_entry);

                    if (ref->parent) {
                        uint16_t i;
                        struct btrfs_key_ptr* in = (struct btrfs_key_ptr*)&ref->parent->data[1];

                        for (i = 0; i < ref->parent->data->nritems; i++) {
                            if (in[i].blockptr == mr->address) {
                                in[i].blockptr = mr->new_address;
                                break;
                            }
                        }

                        if (ref->parent->t) {
                            LIST_ENTRY* le3;

                            le3 = ref->parent->t->itemlist.Flink;
                            while (le3 != &ref->parent->t->itemlist) {
                                tree_data* td = CONTAINING_RECORD(le3, tree_data, list_entry);

                                if (!td->inserted && td->treeholder.address == mr->address)
                                    td->treeholder.address = mr->new_address;

                                le3 = le3->Flink;
                            }
                        }
                    } else if (ref->top && ref->type == BTRFS_TREE_BLOCK_REF_KEY) {
                        LIST_ENTRY* le3;
                        root* r = NULL;

                        // alter ROOT_ITEM

                        le3 = Vcb->roots.Flink;
                        while (le3 != &Vcb->roots) {
                            root* r2 = CONTAINING_RECORD(le3, root, list_entry);

                            if (r2->id == ref->offset) {
                                r = r2;
                                break;
                            }

                            le3 = le3->Flink;
                        }

                        if (r) {
                            r->treeholder.address = mr->new_address;

                            if (r == Vcb->root_root)
                                Vcb->superblock.root = mr->new_address;
                            else if (r == Vcb->chunk_root)
                                Vcb->superblock.chunk_root = mr->new_address;
                            else if (r->root_item.bytenr == mr->address) {
                                struct btrfs_key searchkey;
                                struct btrfs_root_item* ri;

                                r->root_item.bytenr = mr->new_address;

                                searchkey.objectid = r->id;
                                searchkey.type = BTRFS_ROOT_ITEM_KEY;
                                searchkey.offset = 0xffffffffffffffff;

                                Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, NULL);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("find_item returned %08lx\n", Status);
                                    goto end;
                                }

                                if (tp.item->key.objectid != searchkey.objectid || tp.item->key.type != searchkey.type) {
                                    ERR("could not find ROOT_ITEM for tree %I64x\n", searchkey.objectid);
                                    Status = STATUS_INTERNAL_ERROR;
                                    goto end;
                                }

                                ri = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_root_item), ALLOC_TAG);
                                if (!ri) {
                                    ERR("out of memory\n");
                                    Status = STATUS_INSUFFICIENT_RESOURCES;
                                    goto end;
                                }

                                RtlCopyMemory(ri, &r->root_item, sizeof(struct btrfs_root_item));

                                Status = delete_tree_item(Vcb, &tp);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("delete_tree_item returned %08lx\n", Status);
                                    goto end;
                                }

                                Status = insert_tree_item(Vcb, Vcb->root_root, tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, ri, sizeof(struct btrfs_root_item), NULL, NULL);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("insert_tree_item returned %08lx\n", Status);
                                    goto end;
                                }
                            }
                        }
                    }

                    le2 = le2->Flink;
                }

                mr->data->bytenr = mr->new_address;

                t3 = mr->t;

                while (t3) {
                    uint8_t h;
                    bool inserted;
                    tree* t4 = NULL;

                    // check if tree loaded more than once
                    if (t3->list_entry.Flink != &Vcb->trees_hash) {
                        tree* nt = CONTAINING_RECORD(t3->list_entry_hash.Flink, tree, list_entry_hash);

                        if (nt->header.bytenr == t3->header.bytenr)
                            t4 = nt;
                    }

                    t3->header.bytenr = mr->new_address;

                    h = t3->hash >> 24;

                    if (Vcb->trees_ptrs[h] == &t3->list_entry_hash) {
                        if (t3->list_entry_hash.Flink == &Vcb->trees_hash)
                            Vcb->trees_ptrs[h] = NULL;
                        else {
                            tree* t2 = CONTAINING_RECORD(t3->list_entry_hash.Flink, tree, list_entry_hash);

                            if (t2->hash >> 24 == h)
                                Vcb->trees_ptrs[h] = &t2->list_entry_hash;
                            else
                                Vcb->trees_ptrs[h] = NULL;
                        }
                    }

                    RemoveEntryList(&t3->list_entry_hash);

                    t3->hash = calc_crc32c(0xffffffff, (uint8_t*)&t3->header.bytenr, sizeof(uint64_t));
                    h = t3->hash >> 24;

                    if (!Vcb->trees_ptrs[h]) {
                        uint8_t h2 = h;

                        le2 = Vcb->trees_hash.Flink;

                        if (h2 > 0) {
                            h2--;
                            do {
                                if (Vcb->trees_ptrs[h2]) {
                                    le2 = Vcb->trees_ptrs[h2];
                                    break;
                                }

                                h2--;
                            } while (h2 > 0);
                        }
                    } else
                        le2 = Vcb->trees_ptrs[h];

                    inserted = false;
                    while (le2 != &Vcb->trees_hash) {
                        tree* t2 = CONTAINING_RECORD(le2, tree, list_entry_hash);

                        if (t2->hash >= t3->hash) {
                            InsertHeadList(le2->Blink, &t3->list_entry_hash);
                            inserted = true;
                            break;
                        }

                        le2 = le2->Flink;
                    }

                    if (!inserted)
                        InsertTailList(&Vcb->trees_hash, &t3->list_entry_hash);

                    if (!Vcb->trees_ptrs[h] || t3->list_entry_hash.Flink == Vcb->trees_ptrs[h])
                        Vcb->trees_ptrs[h] = &t3->list_entry_hash;

                    if (data_items && level == 0) {
                        le2 = data_items->Flink;

                        while (le2 != data_items) {
                            data_reloc* dr = CONTAINING_RECORD(le2, data_reloc, list_entry);
                            LIST_ENTRY* le3 = t3->itemlist.Flink;

                            while (le3 != &t3->itemlist) {
                                tree_data* td = CONTAINING_RECORD(le3, tree_data, list_entry);

                                if (!td->inserted && td->key.type == BTRFS_EXTENT_DATA_KEY && td->size >= sizeof(struct btrfs_file_extent_item)) {
                                    struct btrfs_file_extent_item* ed = (struct btrfs_file_extent_item*)td->data;

                                    if (ed->type == BTRFS_FILE_EXTENT_REG || ed->type == BTRFS_FILE_EXTENT_PREALLOC) {
                                        if (ed->disk_bytenr == dr->address)
                                            ed->disk_bytenr = dr->new_address;
                                    }
                                }

                                le3 = le3->Flink;
                            }

                            le2 = le2->Flink;
                        }
                    }

                    t3 = t4;
                }

                calc_tree_checksum(Vcb, mr->data);

                tw = ExAllocatePoolWithTag(PagedPool, sizeof(tree_write), ALLOC_TAG);
                if (!tw) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }

                tw->address = mr->new_address;
                tw->length = Vcb->superblock.nodesize;
                tw->data = (uint8_t*)mr->data;
                tw->allocated = false;

                if (IsListEmpty(&tree_writes))
                    InsertTailList(&tree_writes, &tw->list_entry);
                else {
                    bool inserted = false;

                    le2 = tree_writes.Flink;
                    while (le2 != &tree_writes) {
                        tree_write* tw2 = CONTAINING_RECORD(le2, tree_write, list_entry);

                        if (tw2->address > tw->address) {
                            InsertHeadList(le2->Blink, &tw->list_entry);
                            inserted = true;
                            break;
                        }

                        le2 = le2->Flink;
                    }

                    if (!inserted)
                        InsertTailList(&tree_writes, &tw->list_entry);
                }
            }

            le = le->Flink;
        }
    }

    Status = do_tree_writes(Vcb, &tree_writes, true);
    if (!NT_SUCCESS(Status)) {
        ERR("do_tree_writes returned %08lx\n", Status);
        goto end;
    }

    le = items->Flink;
    while (le != items) {
        metadata_reloc* mr = CONTAINING_RECORD(le, metadata_reloc, list_entry);

        Status = add_metadata_reloc_extent_item(Vcb, mr);
        if (!NT_SUCCESS(Status)) {
            ERR("add_metadata_reloc_extent_item returned %08lx\n", Status);
            goto end;
        }

        le = le->Flink;
    }

    Status = STATUS_SUCCESS;

end:
    while (!IsListEmpty(&tree_writes)) {
        tree_write* tw = CONTAINING_RECORD(RemoveHeadList(&tree_writes), tree_write, list_entry);

        if (tw->allocated)
            ExFreePool(tw->data);

        ExFreePool(tw);
    }

    return Status;
}

NTSTATUS balance_metadata_chunk(device_extension* Vcb, chunk* c, bool* changed) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    bool b;
    LIST_ENTRY items, rollback;
    uint32_t loaded = 0;

    TRACE("chunk %I64x\n", c->offset);

    InitializeListHead(&rollback);
    InitializeListHead(&items);

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_METADATA_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        goto end;
    }

    do {
        traverse_ptr next_tp;

        if (tp.item->key.objectid >= c->offset + c->chunk_item->length)
            break;

        if (tp.item->key.objectid >= c->offset && (tp.item->key.type == BTRFS_EXTENT_ITEM_KEY || tp.item->key.type == BTRFS_METADATA_ITEM_KEY)) {
            bool tree = false, skinny = false;

            if (tp.item->key.type == BTRFS_METADATA_ITEM_KEY && tp.item->size >= sizeof(struct btrfs_extent_item)) {
                tree = true;
                skinny = true;
            } else if (tp.item->key.type == BTRFS_EXTENT_ITEM_KEY && tp.item->key.offset == Vcb->superblock.nodesize &&
                       tp.item->size >= sizeof(struct btrfs_extent_item)) {
                struct btrfs_extent_item* ei = (struct btrfs_extent_item*)tp.item->data;

                if (ei->flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
                    tree = true;
            }

            if (tree) {
                Status = add_metadata_reloc(Vcb, &items, &tp, skinny, NULL, c, &rollback);

                if (!NT_SUCCESS(Status)) {
                    ERR("add_metadata_reloc returned %08lx\n", Status);
                    goto end;
                }

                loaded++;

                if (loaded >= 64) // only do 64 at a time
                    break;
            }
        }

        b = find_next_item(Vcb, &tp, &next_tp, false, NULL);

        if (b)
            tp = next_tp;
    } while (b);

    if (IsListEmpty(&items)) {
        *changed = false;
        Status = STATUS_SUCCESS;
        goto end;
    } else
        *changed = true;

    Status = write_metadata_items(Vcb, &items, NULL, c, &rollback);
    if (!NT_SUCCESS(Status)) {
        ERR("write_metadata_items returned %08lx\n", Status);
        goto end;
    }

    Status = STATUS_SUCCESS;

    Vcb->need_write = true;

end:
    if (NT_SUCCESS(Status)) {
        Status = do_write(Vcb, NULL);
        if (!NT_SUCCESS(Status))
            ERR("do_write returned %08lx\n", Status);
    }

    if (NT_SUCCESS(Status))
        clear_rollback(&rollback);
    else
        do_rollback(Vcb, &rollback);

    free_trees(Vcb);

    ExReleaseResourceLite(&Vcb->tree_lock);

    while (!IsListEmpty(&items)) {
        metadata_reloc* mr = CONTAINING_RECORD(RemoveHeadList(&items), metadata_reloc, list_entry);

        while (!IsListEmpty(&mr->refs)) {
            metadata_reloc_ref* ref = CONTAINING_RECORD(RemoveHeadList(&mr->refs), metadata_reloc_ref, list_entry);

            ExFreePool(ref);
        }

        if (mr->data)
            ExFreePool(mr->data);

        ExFreePool(mr);
    }

    return Status;
}

static void copy_balance_args(btrfs_balance_opts* opts, struct btrfs_disk_balance_args* args) {
    if (opts->flags & BTRFS_BALANCE_OPTS_PROFILES) {
        args->profiles = opts->profiles;
        args->flags |= BTRFS_BALANCE_ARGS_PROFILES;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_USAGE) {
        if (args->usage_min == 0) {
            args->flags |= BTRFS_BALANCE_ARGS_USAGE_RANGE;
            args->usage_min = opts->usage_start;
            args->usage_max = opts->usage_end;
        } else {
            args->flags |= BTRFS_BALANCE_ARGS_USAGE;
            args->usage = opts->usage_end;
        }
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_DEVID) {
        args->devid = opts->devid;
        args->flags |= BTRFS_BALANCE_ARGS_DEVID;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_DRANGE) {
        args->pstart = opts->drange_start;
        args->pend = opts->drange_end;
        args->flags |= BTRFS_BALANCE_ARGS_DRANGE;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_VRANGE) {
        args->vstart = opts->vrange_start;
        args->vend = opts->vrange_end;
        args->flags |= BTRFS_BALANCE_ARGS_VRANGE;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_CONVERT) {
        args->target = opts->convert;
        args->flags |= BTRFS_BALANCE_ARGS_CONVERT;

        if (opts->flags & BTRFS_BALANCE_OPTS_SOFT)
            args->flags |= BTRFS_BALANCE_ARGS_SOFT;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_LIMIT) {
        if (args->limit_min == 0) {
            args->flags |= BTRFS_BALANCE_ARGS_LIMIT_RANGE;
            args->limit_min = (uint32_t)opts->limit_start;
            args->limit_max = (uint32_t)opts->limit_end;
        } else {
            args->flags |= BTRFS_BALANCE_ARGS_LIMIT;
            args->limit = opts->limit_end;
        }
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_STRIPES) {
        args->stripes_min = opts->stripes_start;
        args->stripes_max = opts->stripes_end;
        args->flags |= BTRFS_BALANCE_ARGS_STRIPES_RANGE;
    }
}

NTSTATUS add_balance_item(device_extension* Vcb) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    struct btrfs_balance_item* bi;

    searchkey.objectid = BTRFS_BALANCE_OBJECTID;
    searchkey.type = BTRFS_TEMPORARY_ITEM_KEY;
    searchkey.offset = 0;

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        goto end;
    }

    if (!keycmp(tp.item->key, searchkey)) {
        Status = delete_tree_item(Vcb, &tp);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_tree_item returned %08lx\n", Status);
            goto end;
        }
    }

    bi = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_balance_item), ALLOC_TAG);
    if (!bi) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    RtlZeroMemory(bi, sizeof(struct btrfs_balance_item));

    if (Vcb->balance.opts[BALANCE_OPTS_DATA].flags & BTRFS_BALANCE_OPTS_ENABLED) {
        bi->flags |= BTRFS_BALANCE_DATA;
        copy_balance_args(&Vcb->balance.opts[BALANCE_OPTS_DATA], &bi->data);
    }

    if (Vcb->balance.opts[BALANCE_OPTS_METADATA].flags & BTRFS_BALANCE_OPTS_ENABLED) {
        bi->flags |= BTRFS_BALANCE_METADATA;
        copy_balance_args(&Vcb->balance.opts[BALANCE_OPTS_METADATA], &bi->meta);
    }

    if (Vcb->balance.opts[BALANCE_OPTS_SYSTEM].flags & BTRFS_BALANCE_OPTS_ENABLED) {
        bi->flags |= BTRFS_BALANCE_SYSTEM;
        copy_balance_args(&Vcb->balance.opts[BALANCE_OPTS_SYSTEM], &bi->sys);
    }

    Status = insert_tree_item(Vcb, Vcb->root_root, BTRFS_BALANCE_OBJECTID, BTRFS_TEMPORARY_ITEM_KEY, 0, bi, sizeof(struct btrfs_balance_item), NULL, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item returned %08lx\n", Status);
        ExFreePool(bi);
        goto end;
    }

    Status = STATUS_SUCCESS;

end:
    if (NT_SUCCESS(Status)) {
        Status = do_write(Vcb, NULL);
        if (!NT_SUCCESS(Status))
            ERR("do_write returned %08lx\n", Status);
    }

    free_trees(Vcb);

    ExReleaseResourceLite(&Vcb->tree_lock);

    return Status;
}

NTSTATUS remove_balance_item(device_extension* Vcb) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;

    searchkey.objectid = BTRFS_BALANCE_OBJECTID;
    searchkey.type = BTRFS_TEMPORARY_ITEM_KEY;
    searchkey.offset = 0;

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        goto end;
    }

    if (!keycmp(tp.item->key, searchkey)) {
        Status = delete_tree_item(Vcb, &tp);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_tree_item returned %08lx\n", Status);
            goto end;
        }

        Status = do_write(Vcb, NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("do_write returned %08lx\n", Status);
            goto end;
        }

        free_trees(Vcb);
    }

    Status = STATUS_SUCCESS;

end:
    ExReleaseResourceLite(&Vcb->tree_lock);

    return Status;
}

static void load_balance_args(btrfs_balance_opts* opts, struct btrfs_disk_balance_args* args) {
    opts->flags = BTRFS_BALANCE_OPTS_ENABLED;

    if (args->flags & BTRFS_BALANCE_ARGS_PROFILES) {
        opts->flags |= BTRFS_BALANCE_OPTS_PROFILES;
        opts->profiles = args->profiles;
    }

    if (args->flags & BTRFS_BALANCE_ARGS_USAGE) {
        opts->flags |= BTRFS_BALANCE_OPTS_USAGE;

        opts->usage_start = 0;
        opts->usage_end = (uint8_t)args->usage;
    } else if (args->flags & BTRFS_BALANCE_ARGS_USAGE_RANGE) {
        opts->flags |= BTRFS_BALANCE_OPTS_USAGE;

        opts->usage_start = (uint8_t)args->usage_min;
        opts->usage_end = (uint8_t)args->usage_max;
    }

    if (args->flags & BTRFS_BALANCE_ARGS_DEVID) {
        opts->flags |= BTRFS_BALANCE_OPTS_DEVID;
        opts->devid = args->devid;
    }

    if (args->flags & BTRFS_BALANCE_ARGS_DRANGE) {
        opts->flags |= BTRFS_BALANCE_OPTS_DRANGE;
        opts->drange_start = args->pstart;
        opts->drange_end = args->pend;
    }

    if (args->flags & BTRFS_BALANCE_ARGS_VRANGE) {
        opts->flags |= BTRFS_BALANCE_OPTS_VRANGE;
        opts->vrange_start = args->vstart;
        opts->vrange_end = args->vend;
    }

    if (args->flags & BTRFS_BALANCE_ARGS_LIMIT) {
        opts->flags |= BTRFS_BALANCE_OPTS_LIMIT;

        opts->limit_start = 0;
        opts->limit_end = args->limit;
    } else if (args->flags & BTRFS_BALANCE_ARGS_LIMIT_RANGE) {
        opts->flags |= BTRFS_BALANCE_OPTS_LIMIT;

        opts->limit_start = args->limit_min;
        opts->limit_end = args->limit_max;
    }

    if (args->flags & BTRFS_BALANCE_ARGS_STRIPES_RANGE) {
        opts->flags |= BTRFS_BALANCE_OPTS_STRIPES;

        opts->stripes_start = (uint16_t)args->stripes_min;
        opts->stripes_end = (uint16_t)args->stripes_max;
    }

    if (args->flags & BTRFS_BALANCE_ARGS_CONVERT) {
        opts->flags |= BTRFS_BALANCE_OPTS_CONVERT;
        opts->convert = args->target;

        if (args->flags & BTRFS_BALANCE_ARGS_SOFT)
            opts->flags |= BTRFS_BALANCE_OPTS_SOFT;
    }
}

NTSTATUS start_balance(device_extension* Vcb, void* data, ULONG length, KPROCESSOR_MODE processor_mode) {
    NTSTATUS Status;
    btrfs_start_balance* bsb = (btrfs_start_balance*)data;
    OBJECT_ATTRIBUTES oa;
    uint8_t i;

    if (length < sizeof(btrfs_start_balance) || !data)
        return STATUS_INVALID_PARAMETER;

    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), processor_mode))
        return STATUS_PRIVILEGE_NOT_HELD;

    if (Vcb->locked) {
        WARN("cannot start balance while locked\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (Vcb->scrub.thread) {
        WARN("cannot start balance while scrub running\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (Vcb->balance.thread) {
        WARN("balance already running\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (Vcb->readonly)
        return STATUS_MEDIA_WRITE_PROTECTED;

    if (!(bsb->opts[BALANCE_OPTS_DATA].flags & BTRFS_BALANCE_OPTS_ENABLED) &&
        !(bsb->opts[BALANCE_OPTS_METADATA].flags & BTRFS_BALANCE_OPTS_ENABLED) &&
        !(bsb->opts[BALANCE_OPTS_SYSTEM].flags & BTRFS_BALANCE_OPTS_ENABLED))
        return STATUS_SUCCESS;

    for (i = 0; i < 3; i++) {
        if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_ENABLED) {
            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_PROFILES) {
                bsb->opts[i].profiles &= BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP | BTRFS_BLOCK_GROUP_RAID10 |
                                         BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6 | BTRFS_AVAIL_ALLOC_BIT_SINGLE | BTRFS_BLOCK_GROUP_RAID1C3 |
                                         BTRFS_BLOCK_GROUP_RAID1C4;

                if (bsb->opts[i].profiles == 0)
                    return STATUS_INVALID_PARAMETER;
            }

            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_DEVID) {
                if (bsb->opts[i].devid == 0)
                    return STATUS_INVALID_PARAMETER;
            }

            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_DRANGE) {
                if (bsb->opts[i].drange_start > bsb->opts[i].drange_end)
                    return STATUS_INVALID_PARAMETER;
            }

            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_VRANGE) {
                if (bsb->opts[i].vrange_start > bsb->opts[i].vrange_end)
                    return STATUS_INVALID_PARAMETER;
            }

            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_LIMIT) {
                bsb->opts[i].limit_start = max(1, bsb->opts[i].limit_start);
                bsb->opts[i].limit_end = max(1, bsb->opts[i].limit_end);

                if (bsb->opts[i].limit_start > bsb->opts[i].limit_end)
                    return STATUS_INVALID_PARAMETER;
            }

            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_STRIPES) {
                bsb->opts[i].stripes_start = max(1, bsb->opts[i].stripes_start);
                bsb->opts[i].stripes_end = max(1, bsb->opts[i].stripes_end);

                if (bsb->opts[i].stripes_start > bsb->opts[i].stripes_end)
                    return STATUS_INVALID_PARAMETER;
            }

            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_USAGE) {
                bsb->opts[i].usage_start = min(100, bsb->opts[i].stripes_start);
                bsb->opts[i].usage_end = min(100, bsb->opts[i].stripes_end);

                if (bsb->opts[i].stripes_start > bsb->opts[i].stripes_end)
                    return STATUS_INVALID_PARAMETER;
            }

            if (bsb->opts[i].flags & BTRFS_BALANCE_OPTS_CONVERT) {
                if (bsb->opts[i].convert != BTRFS_BLOCK_GROUP_RAID0 && bsb->opts[i].convert != BTRFS_BLOCK_GROUP_RAID1 &&
                    bsb->opts[i].convert != BTRFS_BLOCK_GROUP_DUP && bsb->opts[i].convert != BTRFS_BLOCK_GROUP_RAID10 &&
                    bsb->opts[i].convert != BTRFS_BLOCK_GROUP_RAID5 && bsb->opts[i].convert != BTRFS_BLOCK_GROUP_RAID6 &&
                    bsb->opts[i].convert != BTRFS_AVAIL_ALLOC_BIT_SINGLE && bsb->opts[i].convert != BTRFS_BLOCK_GROUP_RAID1C3 &&
                    bsb->opts[i].convert != BTRFS_BLOCK_GROUP_RAID1C4)
                    return STATUS_INVALID_PARAMETER;
            }
        }
    }

    RtlCopyMemory(&Vcb->balance.opts[BALANCE_OPTS_DATA], &bsb->opts[BALANCE_OPTS_DATA], sizeof(btrfs_balance_opts));
    RtlCopyMemory(&Vcb->balance.opts[BALANCE_OPTS_METADATA], &bsb->opts[BALANCE_OPTS_METADATA], sizeof(btrfs_balance_opts));
    RtlCopyMemory(&Vcb->balance.opts[BALANCE_OPTS_SYSTEM], &bsb->opts[BALANCE_OPTS_SYSTEM], sizeof(btrfs_balance_opts));

    Vcb->balance.paused = false;
    Vcb->balance.removing = false;
    Vcb->balance.shrinking = false;
    Vcb->balance.stopping = false;
    Vcb->balance.status = STATUS_SUCCESS;
    KeInitializeEvent(&Vcb->balance.event, NotificationEvent, !Vcb->balance.paused);
    KeInitializeEvent(&Vcb->balance.finished, NotificationEvent, false);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    Status = PsCreateSystemThread(&Vcb->balance.thread, 0, &oa, NULL, NULL, balance_thread, Vcb);
    if (!NT_SUCCESS(Status)) {
        ERR("PsCreateSystemThread returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS look_for_balance_item(_Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    struct btrfs_balance_item* bi;
    OBJECT_ATTRIBUTES oa;
    int i;

    searchkey.objectid = BTRFS_BALANCE_OBJECTID;
    searchkey.type = BTRFS_TEMPORARY_ITEM_KEY;
    searchkey.offset = 0;

    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (keycmp(tp.item->key, searchkey)) {
        TRACE("no balance item found\n");
        return STATUS_NOT_FOUND;
    }

    if (tp.item->size < sizeof(struct btrfs_balance_item)) {
        WARN("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset,
             tp.item->size, sizeof(struct btrfs_balance_item));
        return STATUS_INTERNAL_ERROR;
    }

    bi = (struct btrfs_balance_item*)tp.item->data;

    if (bi->flags & BTRFS_BALANCE_DATA)
        load_balance_args(&Vcb->balance.opts[BALANCE_OPTS_DATA], &bi->data);

    if (bi->flags & BTRFS_BALANCE_METADATA)
        load_balance_args(&Vcb->balance.opts[BALANCE_OPTS_METADATA], &bi->meta);

    if (bi->flags & BTRFS_BALANCE_SYSTEM)
        load_balance_args(&Vcb->balance.opts[BALANCE_OPTS_SYSTEM], &bi->sys);

    // do the heuristics that Linux driver does

    for (i = 0; i < 3; i++) {
        if (Vcb->balance.opts[i].flags & BTRFS_BALANCE_OPTS_ENABLED) {
            // if converting, don't redo chunks already done

            if (Vcb->balance.opts[i].flags & BTRFS_BALANCE_OPTS_CONVERT)
                Vcb->balance.opts[i].flags |= BTRFS_BALANCE_OPTS_SOFT;

            // don't balance chunks more than 90% filled - presumably these
            // have already been done

            if (!(Vcb->balance.opts[i].flags & BTRFS_BALANCE_OPTS_USAGE) &&
                !(Vcb->balance.opts[i].flags & BTRFS_BALANCE_OPTS_CONVERT)
            ) {
                Vcb->balance.opts[i].flags |= BTRFS_BALANCE_OPTS_USAGE;
                Vcb->balance.opts[i].usage_start = 0;
                Vcb->balance.opts[i].usage_end = 90;
            }
        }
    }

    if (Vcb->readonly || Vcb->options.skip_balance)
        Vcb->balance.paused = true;
    else
        Vcb->balance.paused = false;

    Vcb->balance.removing = false;
    Vcb->balance.shrinking = false;
    Vcb->balance.stopping = false;
    Vcb->balance.status = STATUS_SUCCESS;
    KeInitializeEvent(&Vcb->balance.event, NotificationEvent, !Vcb->balance.paused);
    KeInitializeEvent(&Vcb->balance.finished, NotificationEvent, false);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    Status = PsCreateSystemThread(&Vcb->balance.thread, 0, &oa, NULL, NULL, balance_thread, Vcb);
    if (!NT_SUCCESS(Status)) {
        ERR("PsCreateSystemThread returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS query_balance(device_extension* Vcb, void* data, ULONG length) {
    btrfs_query_balance* bqb = (btrfs_query_balance*)data;

    if (length < sizeof(btrfs_query_balance) || !data)
        return STATUS_INVALID_PARAMETER;

    if (!Vcb->balance.thread) {
        bqb->status = BTRFS_BALANCE_STOPPED;

        if (!NT_SUCCESS(Vcb->balance.status)) {
            bqb->status |= BTRFS_BALANCE_ERROR;
            bqb->error = Vcb->balance.status;
        }

        return STATUS_SUCCESS;
    }

    bqb->status = Vcb->balance.paused ? BTRFS_BALANCE_PAUSED : BTRFS_BALANCE_RUNNING;

    if (Vcb->balance.removing)
        bqb->status |= BTRFS_BALANCE_REMOVAL;

    if (Vcb->balance.shrinking)
        bqb->status |= BTRFS_BALANCE_SHRINKING;

    if (!NT_SUCCESS(Vcb->balance.status))
        bqb->status |= BTRFS_BALANCE_ERROR;

    bqb->chunks_left = Vcb->balance.chunks_left;
    bqb->total_chunks = Vcb->balance.total_chunks;
    bqb->error = Vcb->balance.status;
    RtlCopyMemory(&bqb->data_opts, &Vcb->balance.opts[BALANCE_OPTS_DATA], sizeof(btrfs_balance_opts));
    RtlCopyMemory(&bqb->metadata_opts, &Vcb->balance.opts[BALANCE_OPTS_METADATA], sizeof(btrfs_balance_opts));
    RtlCopyMemory(&bqb->system_opts, &Vcb->balance.opts[BALANCE_OPTS_SYSTEM], sizeof(btrfs_balance_opts));

    return STATUS_SUCCESS;
}

NTSTATUS pause_balance(device_extension* Vcb, KPROCESSOR_MODE processor_mode) {
    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), processor_mode))
        return STATUS_PRIVILEGE_NOT_HELD;

    if (!Vcb->balance.thread)
        return STATUS_DEVICE_NOT_READY;

    if (Vcb->balance.paused)
        return STATUS_DEVICE_NOT_READY;

    Vcb->balance.paused = true;
    KeClearEvent(&Vcb->balance.event);

    return STATUS_SUCCESS;
}

NTSTATUS resume_balance(device_extension* Vcb, KPROCESSOR_MODE processor_mode) {
    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), processor_mode))
        return STATUS_PRIVILEGE_NOT_HELD;

    if (!Vcb->balance.thread)
        return STATUS_DEVICE_NOT_READY;

    if (!Vcb->balance.paused)
        return STATUS_DEVICE_NOT_READY;

    if (Vcb->readonly)
        return STATUS_MEDIA_WRITE_PROTECTED;

    Vcb->balance.paused = false;
    KeSetEvent(&Vcb->balance.event, 0, false);

    return STATUS_SUCCESS;
}

NTSTATUS stop_balance(device_extension* Vcb, KPROCESSOR_MODE processor_mode) {
    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), processor_mode))
        return STATUS_PRIVILEGE_NOT_HELD;

    if (!Vcb->balance.thread)
        return STATUS_DEVICE_NOT_READY;

    Vcb->balance.paused = false;
    Vcb->balance.stopping = true;
    Vcb->balance.status = STATUS_SUCCESS;
    KeSetEvent(&Vcb->balance.event, 0, false);

    return STATUS_SUCCESS;
}

NTSTATUS remove_device(device_extension* Vcb, void* data, ULONG length, KPROCESSOR_MODE processor_mode) {
    uint64_t devid;
    LIST_ENTRY* le;
    device* dev = NULL;
    NTSTATUS Status;
    int i;
    uint64_t num_rw_devices;
    OBJECT_ATTRIBUTES oa;

    TRACE("(%p, %p, %lx)\n", Vcb, data, length);

    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), processor_mode))
        return STATUS_PRIVILEGE_NOT_HELD;

    if (length < sizeof(uint64_t))
        return STATUS_INVALID_PARAMETER;

    devid = *(uint64_t*)data;

    ExAcquireResourceSharedLite(&Vcb->tree_lock, true);

    if (Vcb->readonly) {
        ExReleaseResourceLite(&Vcb->tree_lock);
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    num_rw_devices = 0;

    le = Vcb->devices.Flink;
    while (le != &Vcb->devices) {
        device* dev2 = CONTAINING_RECORD(le, device, list_entry);

        if (dev2->devitem.devid == devid)
            dev = dev2;

        if (!dev2->readonly)
            num_rw_devices++;

        le = le->Flink;
    }

    if (!dev) {
        ExReleaseResourceLite(&Vcb->tree_lock);
        WARN("device %I64x not found\n", devid);
        return STATUS_NOT_FOUND;
    }

    if (!dev->readonly) {
        if (num_rw_devices == 1) {
            ExReleaseResourceLite(&Vcb->tree_lock);
            WARN("not removing last non-readonly device\n");
            return STATUS_INVALID_PARAMETER;
        }

        if (num_rw_devices == 4 &&
            ((Vcb->data_flags & BTRFS_BLOCK_GROUP_RAID10 || Vcb->metadata_flags & BTRFS_BLOCK_GROUP_RAID10 || Vcb->system_flags & BTRFS_BLOCK_GROUP_RAID10) ||
             (Vcb->data_flags & BTRFS_BLOCK_GROUP_RAID6 || Vcb->metadata_flags & BTRFS_BLOCK_GROUP_RAID6 || Vcb->system_flags & BTRFS_BLOCK_GROUP_RAID6) ||
             (Vcb->data_flags & BTRFS_BLOCK_GROUP_RAID1C4 || Vcb->metadata_flags & BTRFS_BLOCK_GROUP_RAID1C4 || Vcb->system_flags & BTRFS_BLOCK_GROUP_RAID1C4)
            )
        ) {
            ExReleaseResourceLite(&Vcb->tree_lock);
            ERR("would not be enough devices to satisfy RAID requirement (RAID6/10/1C4)\n");
            return STATUS_CANNOT_DELETE;
        }

        if (num_rw_devices == 3 &&
            ((Vcb->data_flags & BTRFS_BLOCK_GROUP_RAID5 || Vcb->metadata_flags & BTRFS_BLOCK_GROUP_RAID5 || Vcb->system_flags & BTRFS_BLOCK_GROUP_RAID5) ||
            (Vcb->data_flags & BTRFS_BLOCK_GROUP_RAID1C3 || Vcb->metadata_flags & BTRFS_BLOCK_GROUP_RAID1C3 || Vcb->system_flags & BTRFS_BLOCK_GROUP_RAID1C3))
            ) {
            ExReleaseResourceLite(&Vcb->tree_lock);
            ERR("would not be enough devices to satisfy RAID requirement (RAID5/1C3)\n");
            return STATUS_CANNOT_DELETE;
        }

        if (num_rw_devices == 2 &&
            ((Vcb->data_flags & BTRFS_BLOCK_GROUP_RAID0 || Vcb->metadata_flags & BTRFS_BLOCK_GROUP_RAID0 || Vcb->system_flags & BTRFS_BLOCK_GROUP_RAID0) ||
             (Vcb->data_flags & BTRFS_BLOCK_GROUP_RAID1 || Vcb->metadata_flags & BTRFS_BLOCK_GROUP_RAID1 || Vcb->system_flags & BTRFS_BLOCK_GROUP_RAID1))
        ) {
            ExReleaseResourceLite(&Vcb->tree_lock);
            ERR("would not be enough devices to satisfy RAID requirement (RAID0/1)\n");
            return STATUS_CANNOT_DELETE;
        }
    }

    ExReleaseResourceLite(&Vcb->tree_lock);

    if (Vcb->balance.thread) {
        WARN("balance already running\n");
        return STATUS_DEVICE_NOT_READY;
    }

    dev->reloc = true;

    RtlZeroMemory(Vcb->balance.opts, sizeof(btrfs_balance_opts) * 3);

    for (i = 0; i < 3; i++) {
        Vcb->balance.opts[i].flags = BTRFS_BALANCE_OPTS_ENABLED | BTRFS_BALANCE_OPTS_DEVID;
        Vcb->balance.opts[i].devid = devid;
    }

    Vcb->balance.paused = false;
    Vcb->balance.removing = true;
    Vcb->balance.shrinking = false;
    Vcb->balance.stopping = false;
    Vcb->balance.status = STATUS_SUCCESS;
    KeInitializeEvent(&Vcb->balance.event, NotificationEvent, !Vcb->balance.paused);
    KeInitializeEvent(&Vcb->balance.finished, NotificationEvent, false);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    Status = PsCreateSystemThread(&Vcb->balance.thread, 0, &oa, NULL, NULL, balance_thread, Vcb);
    if (!NT_SUCCESS(Status)) {
        ERR("PsCreateSystemThread returned %08lx\n", Status);
        dev->reloc = false;
        return Status;
    }

    return STATUS_SUCCESS;
}
