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

// Tree block checksums, serialization and queued physical writes.
#include "btrfs_drv.h"
#include "tree-transaction.h"
#include "zstd/lib/common/xxhash.h"
#include "crc32c.h"

NTSTATUS do_tree_writes(device_extension* Vcb, LIST_ENTRY* tree_writes, bool no_free) {
    chunk* c;
    LIST_ENTRY* le;
    tree_write* tw;
    NTSTATUS Status;
    ULONG i, num_bits;
    write_data_context* wtc;
    ULONG bit_num = 0;
    bool raid56 = false;

    // merge together runs
    c = NULL;
    le = tree_writes->Flink;
    while (le != tree_writes) {
        tw = CONTAINING_RECORD(le, tree_write, list_entry);

        if (!c || tw->address < c->offset || tw->address >= c->offset + c->chunk_item->length)
            c = get_chunk_from_address(Vcb, tw->address);
        else {
            tree_write* tw2 = CONTAINING_RECORD(le->Blink, tree_write, list_entry);

            if (tw->address == tw2->address + tw2->length) {
                uint8_t* data = ExAllocatePoolWithTag(NonPagedPool, tw2->length + tw->length, ALLOC_TAG);

                if (!data) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(data, tw2->data, tw2->length);
                RtlCopyMemory(&data[tw2->length], tw->data, tw->length);

                if (!no_free || tw2->allocated)
                    ExFreePool(tw2->data);

                tw2->data = data;
                tw2->length += tw->length;
                tw2->allocated = true;

                if (!no_free || tw->allocated)
                    ExFreePool(tw->data);

                RemoveEntryList(&tw->list_entry);
                ExFreePool(tw);

                le = tw2->list_entry.Flink;
                continue;
            }
        }

        tw->c = c;

        if (c->chunk_item->type & (BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6))
            raid56 = true;

        le = le->Flink;
    }

    num_bits = 0;

    le = tree_writes->Flink;
    while (le != tree_writes) {
        tw = CONTAINING_RECORD(le, tree_write, list_entry);

        num_bits++;

        le = le->Flink;
    }

    wtc = ExAllocatePoolWithTag(NonPagedPool, sizeof(write_data_context) * num_bits, ALLOC_TAG);
    if (!wtc) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    le = tree_writes->Flink;

    while (le != tree_writes) {
        tw = CONTAINING_RECORD(le, tree_write, list_entry);

        TRACE("address: %I64x, size: %x\n", tw->address, tw->length);

        KeInitializeEvent(&wtc[bit_num].Event, NotificationEvent, false);
        InitializeListHead(&wtc[bit_num].stripes);
        wtc[bit_num].need_wait = false;
        wtc[bit_num].stripes_left = 0;
        wtc[bit_num].parity1 = wtc[bit_num].parity2 = wtc[bit_num].scratch = NULL;
        wtc[bit_num].mdl = wtc[bit_num].parity1_mdl = wtc[bit_num].parity2_mdl = NULL;

        Status = write_data(Vcb, tw->address, tw->data, tw->length, &wtc[bit_num], NULL, NULL, false, 0, HighPagePriority);
        if (!NT_SUCCESS(Status)) {
            ERR("write_data returned %08lx\n", Status);

            for (i = 0; i < num_bits; i++) {
                free_write_data_stripes(&wtc[i]);
            }
            ExFreePool(wtc);

            return Status;
        }

        bit_num++;

        le = le->Flink;
    }

    for (i = 0; i < num_bits; i++) {
        if (wtc[i].stripes.Flink != &wtc[i].stripes) {
            // launch writes and wait
            le = wtc[i].stripes.Flink;
            while (le != &wtc[i].stripes) {
                write_data_stripe* stripe = CONTAINING_RECORD(le, write_data_stripe, list_entry);

                if (stripe->status != WriteDataStatus_Ignore) {
                    wtc[i].need_wait = true;
                    IoCallDriver(stripe->device->devobj, stripe->Irp);
                }

                le = le->Flink;
            }
        }
    }

    for (i = 0; i < num_bits; i++) {
        if (wtc[i].need_wait)
            KeWaitForSingleObject(&wtc[i].Event, Executive, KernelMode, false, NULL);
    }

    for (i = 0; i < num_bits; i++) {
        le = wtc[i].stripes.Flink;
        while (le != &wtc[i].stripes) {
            write_data_stripe* stripe = CONTAINING_RECORD(le, write_data_stripe, list_entry);

            if (stripe->status != WriteDataStatus_Ignore && !NT_SUCCESS(stripe->iosb.Status)) {
                Status = stripe->iosb.Status;
                log_device_error(Vcb, stripe->device, BTRFS_DEV_STAT_WRITE_ERRS);
                break;
            }

            le = le->Flink;
        }

        free_write_data_stripes(&wtc[i]);
    }

    ExFreePool(wtc);

    if (raid56) {
        c = NULL;

        le = tree_writes->Flink;
        while (le != tree_writes) {
            tw = CONTAINING_RECORD(le, tree_write, list_entry);

            if (tw->c != c) {
                c = tw->c;

                ExAcquireResourceExclusiveLite(&c->partial_stripes_lock, true);

                while (!IsListEmpty(&c->partial_stripes)) {
                    partial_stripe* ps = CONTAINING_RECORD(RemoveHeadList(&c->partial_stripes), partial_stripe, list_entry);

                    Status = flush_partial_stripe(Vcb, c, ps);

                    if (ps->bmparr)
                        ExFreePool(ps->bmparr);

                    ExFreePool(ps);

                    if (!NT_SUCCESS(Status)) {
                        ERR("flush_partial_stripe returned %08lx\n", Status);
                        ExReleaseResourceLite(&c->partial_stripes_lock);
                        return Status;
                    }
                }

                ExReleaseResourceLite(&c->partial_stripes_lock);
            }

            le = le->Flink;
        }
    }

    return STATUS_SUCCESS;
}

void calc_tree_checksum(device_extension* Vcb, struct btrfs_header* th) {
    switch (Vcb->superblock.csum_type) {
        case BTRFS_CSUM_TYPE_CRC32:
            *((uint32_t*)th) = ~calc_crc32c(0xffffffff, (uint8_t*)&th->fsid, Vcb->superblock.nodesize - sizeof(th->csum));
        break;

        case BTRFS_CSUM_TYPE_XXHASH:
            *((uint64_t*)th) = XXH64((uint8_t*)&th->fsid, Vcb->superblock.nodesize - sizeof(th->csum), 0);
        break;

        case BTRFS_CSUM_TYPE_SHA256:
            calc_sha256((uint8_t*)th, &th->fsid, Vcb->superblock.nodesize - sizeof(th->csum));
        break;

        case BTRFS_CSUM_TYPE_BLAKE2:
            blake2b((uint8_t*)th, BLAKE2_HASH_SIZE, &th->fsid, Vcb->superblock.nodesize - sizeof(th->csum));
        break;
    }
}

NTSTATUS write_trees(device_extension* Vcb, PIRP Irp) {
    ULONG level;
    uint8_t *data, *body;
    NTSTATUS Status;
    LIST_ENTRY* le;
    LIST_ENTRY tree_writes;
    tree_write* tw;

    TRACE("(%p)\n", Vcb);

    InitializeListHead(&tree_writes);

    for (level = 0; level <= 255; level++) {
        bool nothing_found = true;

        TRACE("level = %lu\n", level);

        le = Vcb->trees.Flink;
        while (le != &Vcb->trees) {
            tree* t = CONTAINING_RECORD(le, tree, list_entry);

            if (t->write && t->header.level == level) {
                struct btrfs_key firstitem, searchkey;
                LIST_ENTRY* le2;
                traverse_ptr tp;

                if (!t->has_new_address) {
                    ERR("error - tried to write tree with no new address\n");
                    return STATUS_INTERNAL_ERROR;
                }

                le2 = t->itemlist.Flink;
                while (le2 != &t->itemlist) {
                    tree_data* td = CONTAINING_RECORD(le2, tree_data, list_entry);
                    if (!td->ignore) {
                        firstitem = td->key;
                        break;
                    }
                    le2 = le2->Flink;
                }

                if (t->parent) {
                    t->paritem->key = firstitem;
                    t->paritem->treeholder.address = t->new_address;
                    t->paritem->treeholder.generation = Vcb->superblock.generation;
                }

                if (!(Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)) {
                    struct btrfs_extent_item* ei;
                    struct btrfs_tree_block_info* tbi;

                    searchkey.objectid = t->new_address;
                    searchkey.type = BTRFS_EXTENT_ITEM_KEY;
                    searchkey.offset = Vcb->superblock.nodesize;

                    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
                    if (!NT_SUCCESS(Status)) {
                        ERR("error - find_item returned %08lx\n", Status);
                        return Status;
                    }

                    if (keycmp(searchkey, tp.item->key)) {
                        ERR("could not find %I64x,%x,%I64x in extent_root (found %I64x,%x,%I64x instead)\n", searchkey.objectid, searchkey.type, searchkey.offset, tp.item->key.objectid, tp.item->key.type, tp.item->key.offset);
                        return STATUS_INTERNAL_ERROR;
                    }

                    if (tp.item->size < sizeof(struct btrfs_extent_item) + sizeof(struct btrfs_tree_block_info)) {
                        ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_extent_item) + sizeof(struct btrfs_tree_block_info));
                        return STATUS_INTERNAL_ERROR;
                    }

                    ei = (struct btrfs_extent_item*)tp.item->data;
                    tbi = (struct btrfs_tree_block_info*)&ei[1];

                    tbi->key = firstitem;
                }

                nothing_found = false;
            }

            le = le->Flink;
        }

        if (nothing_found)
            break;
    }

    TRACE("allocated tree extents\n");

    le = Vcb->trees.Flink;
    while (le != &Vcb->trees) {
        tree* t = CONTAINING_RECORD(le, tree, list_entry);
        LIST_ENTRY* le2;
#ifdef DEBUG_PARANOID
        uint32_t num_items = 0, size = 0;
        bool crash = false;
#endif

        if (t->write) {
#ifdef DEBUG_PARANOID
            bool first = true;
            struct btrfs_key lastkey;

            le2 = t->itemlist.Flink;
            while (le2 != &t->itemlist) {
                tree_data* td = CONTAINING_RECORD(le2, tree_data, list_entry);
                if (!td->ignore) {
                    num_items++;

                    if (!first) {
                        if (keycmp(td->key, lastkey) == 0) {
                            ERR("(%I64x,%x,%I64x): duplicate key\n", td->key.objectid, td->key.type, td->key.offset);
                            crash = true;
                        } else if (keycmp(td->key, lastkey) == -1) {
                            ERR("(%I64x,%x,%I64x): key out of order\n", td->key.objectid, td->key.type, td->key.offset);
                            crash = true;
                        }
                    } else
                        first = false;

                    lastkey = td->key;

                    if (t->header.level == 0)
                        size += td->size;
                }
                le2 = le2->Flink;
            }

            if (t->header.level == 0)
                size += num_items * sizeof(struct btrfs_item);
            else
                size += num_items * sizeof(struct btrfs_key_ptr);

            if (num_items != t->header.nritems) {
                ERR("tree %I64x, level %x: num_items was %x, expected %x\n", t->root->id, t->header.level, num_items, t->header.nritems);
                crash = true;
            }

            if (size != t->size) {
                ERR("tree %I64x, level %x: size was %x, expected %x\n", t->root->id, t->header.level, size, t->size);
                crash = true;
            }

            if (t->header.nritems == 0 && t->parent) {
                ERR("tree %I64x, level %x: tried to write empty tree with parent\n", t->root->id, t->header.level);
                crash = true;
            }

            if (t->size > Vcb->superblock.nodesize - sizeof(struct btrfs_header)) {
                ERR("tree %I64x, level %x: tried to write overlarge tree (%x > %Ix)\n", t->root->id, t->header.level, t->size, Vcb->superblock.nodesize - sizeof(struct btrfs_header));
                crash = true;
            }

            if (crash) {
                ERR("tree %p\n", t);
                le2 = t->itemlist.Flink;
                while (le2 != &t->itemlist) {
                    tree_data* td = CONTAINING_RECORD(le2, tree_data, list_entry);
                    if (!td->ignore) {
                        ERR("%I64x,%x,%I64x inserted=%u\n", td->key.objectid, td->key.type, td->key.offset, td->inserted);
                    }
                    le2 = le2->Flink;
                }
                int3;
            }
#endif
            t->header.bytenr = t->new_address;
            t->header.generation = Vcb->superblock.generation;
            t->header.owner = t->root->id;
            t->header.flags |= BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT;
            memcpy(t->header.fsid, Vcb->superblock.metadata_uuid, BTRFS_UUID_SIZE);
            t->has_address = true;

            data = ExAllocatePoolWithTag(NonPagedPool, Vcb->superblock.nodesize, ALLOC_TAG);
            if (!data) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            body = data + sizeof(struct btrfs_header);

            RtlCopyMemory(data, &t->header, sizeof(struct btrfs_header));
            RtlZeroMemory(body, Vcb->superblock.nodesize - sizeof(struct btrfs_header));

            if (t->header.level == 0) {
                struct btrfs_item* itemptr = (struct btrfs_item*)body;
                int i = 0;
                uint8_t* dataptr = data + Vcb->superblock.nodesize;

                le2 = t->itemlist.Flink;
                while (le2 != &t->itemlist) {
                    tree_data* td = CONTAINING_RECORD(le2, tree_data, list_entry);
                    if (!td->ignore) {
                        dataptr = dataptr - td->size;

                        itemptr[i].key = td->key;
                        itemptr[i].offset = (uint32_t)((uint8_t*)dataptr - (uint8_t*)body);
                        itemptr[i].size = td->size;
                        i++;

                        if (td->size > 0)
                            RtlCopyMemory(dataptr, td->data, td->size);
                    }

                    le2 = le2->Flink;
                }
            } else {
                struct btrfs_key_ptr* itemptr = (struct btrfs_key_ptr*)body;
                int i = 0;

                le2 = t->itemlist.Flink;
                while (le2 != &t->itemlist) {
                    tree_data* td = CONTAINING_RECORD(le2, tree_data, list_entry);
                    if (!td->ignore) {
                        itemptr[i].key = td->key;
                        itemptr[i].blockptr = td->treeholder.address;
                        itemptr[i].generation = td->treeholder.generation;
                        i++;
                    }

                    le2 = le2->Flink;
                }
            }

            calc_tree_checksum(Vcb, (struct btrfs_header*)data);

            tw = ExAllocatePoolWithTag(PagedPool, sizeof(tree_write), ALLOC_TAG);
            if (!tw) {
                ERR("out of memory\n");
                ExFreePool(data);
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            tw->address = t->new_address;
            tw->length = Vcb->superblock.nodesize;
            tw->data = data;
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

    Status = do_tree_writes(Vcb, &tree_writes, false);
    if (!NT_SUCCESS(Status)) {
        ERR("do_tree_writes returned %08lx\n", Status);
        goto end;
    }

    Status = STATUS_SUCCESS;

end:
    while (!IsListEmpty(&tree_writes)) {
        le = RemoveHeadList(&tree_writes);
        tw = CONTAINING_RECORD(le, tree_write, list_entry);

        if (tw->data)
            ExFreePool(tw->data);

        ExFreePool(tw);
    }

    return Status;
}
