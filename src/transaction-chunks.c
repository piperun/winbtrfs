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

// Chunk creation, retirement and transaction updates.
#include "btrfs_drv.h"
#include "transaction-chunks.h"
#include "trim.h"

static void regen_bootstrap(device_extension* Vcb) {
    sys_chunk* sc2;
    USHORT i = 0;
    LIST_ENTRY* le;

    i = 0;
    le = Vcb->sys_chunks.Flink;
    while (le != &Vcb->sys_chunks) {
        sc2 = CONTAINING_RECORD(le, sys_chunk, list_entry);

        TRACE("%I64x,%x,%I64x\n", sc2->key.objectid, sc2->key.type, sc2->key.offset);

        RtlCopyMemory(&Vcb->superblock.sys_chunk_array[i], &sc2->key, sizeof(struct btrfs_key));
        i += sizeof(struct btrfs_key);

        RtlCopyMemory(&Vcb->superblock.sys_chunk_array[i], sc2->data, sc2->size);
        i += sc2->size;

        le = le->Flink;
    }
}

static NTSTATUS add_to_bootstrap(device_extension* Vcb, uint64_t objectid, uint8_t type, uint64_t offset, void* data, uint16_t size) {
    sys_chunk* sc;
    LIST_ENTRY* le;

    if (Vcb->superblock.sys_chunk_array_size + sizeof(struct btrfs_key) + size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE) {
        ERR("error - bootstrap is full\n");
        return STATUS_INTERNAL_ERROR;
    }

    sc = ExAllocatePoolWithTag(PagedPool, sizeof(sys_chunk), ALLOC_TAG);
    if (!sc) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    sc->key.objectid = objectid;
    sc->key.type = type;
    sc->key.offset = offset;
    sc->size = size;
    sc->data = ExAllocatePoolWithTag(PagedPool, sc->size, ALLOC_TAG);
    if (!sc->data) {
        ERR("out of memory\n");
        ExFreePool(sc);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(sc->data, data, sc->size);

    le = Vcb->sys_chunks.Flink;
    while (le != &Vcb->sys_chunks) {
        sys_chunk* sc2 = CONTAINING_RECORD(le, sys_chunk, list_entry);

        if (keycmp(sc2->key, sc->key) == 1)
            break;

        le = le->Flink;
    }
    InsertTailList(le, &sc->list_entry);

    Vcb->superblock.sys_chunk_array_size += sizeof(struct btrfs_key) + size;

    regen_bootstrap(Vcb);

    return STATUS_SUCCESS;
}

NTSTATUS create_chunk(device_extension* Vcb, chunk* c, PIRP Irp) {
    struct btrfs_chunk* ci;
    struct btrfs_block_group_item* bgi;
    uint16_t i, factor;
    NTSTATUS Status;

    ci = ExAllocatePoolWithTag(PagedPool, c->size, ALLOC_TAG);
    if (!ci) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(ci, c->chunk_item, c->size);

    Status = insert_tree_item(Vcb, Vcb->chunk_root, 0x100, BTRFS_CHUNK_ITEM_KEY, c->offset, ci, c->size, NULL, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item failed\n");
        ExFreePool(ci);
        return Status;
    }

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_SYSTEM) {
        Status = add_to_bootstrap(Vcb, 0x100, BTRFS_CHUNK_ITEM_KEY, c->offset, ci, c->size);
        if (!NT_SUCCESS(Status)) {
            ERR("add_to_bootstrap returned %08lx\n", Status);
            return Status;
        }
    }

    // add BLOCK_GROUP_ITEM to tree 2

    bgi = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_block_group_item), ALLOC_TAG);
    if (!bgi) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    bgi->used = c->used;
    bgi->chunk_objectid = 0x100;
    bgi->flags = c->chunk_item->type;

    Status = insert_tree_item(Vcb, Vcb->block_group_root ? Vcb->block_group_root : Vcb->extent_root, c->offset,
                              BTRFS_BLOCK_GROUP_ITEM_KEY, c->chunk_item->length, bgi, sizeof(struct btrfs_block_group_item), NULL, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item failed\n");
        ExFreePool(bgi);
        return Status;
    }

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID0)
        factor = c->chunk_item->num_stripes;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID10)
        factor = c->chunk_item->num_stripes / c->chunk_item->sub_stripes;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5)
        factor = c->chunk_item->num_stripes - 1;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)
        factor = c->chunk_item->num_stripes - 2;
    else // SINGLE, DUPLICATE, RAID1, RAID1C3, RAID1C4
        factor = 1;

    // add DEV_EXTENTs to tree 4

    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        struct btrfs_dev_extent* de;

        de = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_dev_extent), ALLOC_TAG);
        if (!de) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        de->chunk_tree = Vcb->chunk_root->id;
        de->chunk_objectid = 0x100;
        de->chunk_offset = c->offset;
        de->length = c->chunk_item->length / factor;
        memcpy(de->chunk_tree_uuid, Vcb->chunk_root->treeholder.tree->header.chunk_tree_uuid,
               BTRFS_UUID_SIZE);

        Status = insert_tree_item(Vcb, Vcb->dev_root, c->devices[i]->devitem.devid, BTRFS_DEV_EXTENT_KEY, c->chunk_item->stripe[i].offset, de, sizeof(struct btrfs_dev_extent), NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item returned %08lx\n", Status);
            ExFreePool(de);
            return Status;
        }

        // FIXME - no point in calling this twice for the same device
        Status = update_dev_item(Vcb, c->devices[i], Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("update_dev_item returned %08lx\n", Status);
            return Status;
        }
    }

    c->created = false;
    c->oldused = c->used;

    Vcb->superblock.bytes_used += c->used;

    return STATUS_SUCCESS;
}

static void remove_from_bootstrap(device_extension* Vcb, uint64_t objectid, uint8_t type, uint64_t offset) {
    sys_chunk* sc2;
    LIST_ENTRY* le;

    le = Vcb->sys_chunks.Flink;
    while (le != &Vcb->sys_chunks) {
        sc2 = CONTAINING_RECORD(le, sys_chunk, list_entry);

        if (sc2->key.objectid == objectid && sc2->key.type == type && sc2->key.offset == offset) {
            RemoveEntryList(&sc2->list_entry);

            Vcb->superblock.sys_chunk_array_size -= sizeof(struct btrfs_key) + sc2->size;

            ExFreePool(sc2->data);
            ExFreePool(sc2);
            regen_bootstrap(Vcb);
            return;
        }

        le = le->Flink;
    }
}

static NTSTATUS drop_chunk(device_extension* Vcb, chunk* c, LIST_ENTRY* batchlist,
                           PIRP Irp) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    uint64_t i, factor;

    TRACE("dropping chunk %I64x\n", c->offset);

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID0)
        factor = c->chunk_item->num_stripes;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID10)
        factor = c->chunk_item->num_stripes / c->chunk_item->sub_stripes;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5)
        factor = c->chunk_item->num_stripes - 1;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)
        factor = c->chunk_item->num_stripes - 2;
    else // SINGLE, DUPLICATE, RAID1, RAID1C3, RAID1C4
        factor = 1;

    // do TRIM
    if (Vcb->trim && !Vcb->options.no_trim) {
        uint64_t len = c->chunk_item->length / factor;

        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (c->devices[i] && c->devices[i]->devobj && !c->devices[i]->readonly && c->devices[i]->trim)
                add_trim_entry_avoid_sb(Vcb, c->devices[i], c->chunk_item->stripe[i].offset, len);
        }
    }

    if (!c->cache) {
        Status = load_stored_free_space_cache(Vcb, c, true, Irp);

        if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND)
            WARN("load_stored_free_space_cache returned %08lx\n", Status);
    }

    // remove free space cache
    if (c->cache) {
        c->cache->deleted = true;

        Status = excise_extents(Vcb, c->cache, 0, c->cache->inode_item.size, Irp,
                                NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("excise_extents returned %08lx\n", Status);
            return Status;
        }

        Status = flush_fcb(c->cache, true, batchlist, Irp);

        free_fcb(c->cache);

        if (c->cache->refcount == 0) {
            reap_fcb(c->cache);
            c->cache = NULL;
        }

        if (!NT_SUCCESS(Status)) {
            ERR("flush_fcb returned %08lx\n", Status);
            return Status;
        }

        searchkey.objectid = BTRFS_FREE_SPACE_OBJECTID;
        searchkey.type = 0;
        searchkey.offset = c->offset;

        Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        if (!keycmp(tp.item->key, searchkey)) {
            Status = delete_tree_item(Vcb, &tp);
            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                return Status;
            }
        }
    }

    if (Vcb->space_root) {
        Status = insert_tree_item_batch(batchlist, Vcb, Vcb->space_root, c->offset, BTRFS_FREE_SPACE_INFO_KEY, c->chunk_item->length,
                                        NULL, 0, Batch_DeleteFreeSpace);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item_batch returned %08lx\n", Status);
            return Status;
        }
    }

    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        if (!c->created) {
            // remove DEV_EXTENTs from tree 4
            searchkey.objectid = c->chunk_item->stripe[i].devid;
            searchkey.type = BTRFS_DEV_EXTENT_KEY;
            searchkey.offset = c->chunk_item->stripe[i].offset;

            Status = find_item(Vcb, Vcb->dev_root, &tp, &searchkey, false, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("error - find_item returned %08lx\n", Status);
                return Status;
            }

            if (!keycmp(tp.item->key, searchkey)) {
                Status = delete_tree_item(Vcb, &tp);
                if (!NT_SUCCESS(Status)) {
                    ERR("delete_tree_item returned %08lx\n", Status);
                    return Status;
                }

                if (tp.item->size >= sizeof(struct btrfs_dev_extent)) {
                    struct btrfs_dev_extent* de = (struct btrfs_dev_extent*)tp.item->data;

                    c->devices[i]->devitem.bytes_used -= de->length;

                    if (Vcb->balance.thread && Vcb->balance.shrinking && Vcb->balance.opts[0].devid == c->devices[i]->devitem.devid) {
                        if (c->chunk_item->stripe[i].offset < Vcb->balance.opts[0].drange_start) {
                            uint64_t end = min(c->chunk_item->stripe[i].offset + de->length, Vcb->balance.opts[0].drange_start);

                            Status = space_list_add2(&c->devices[i]->space, NULL,
                                                     c->chunk_item->stripe[i].offset,
                                                     end - c->chunk_item->stripe[i].offset,
                                                     NULL, NULL);
                            if (!NT_SUCCESS(Status))
                                return Status;
                        }
                    } else {
                        Status = space_list_add2(&c->devices[i]->space, NULL,
                                                 c->chunk_item->stripe[i].offset,
                                                 de->length, NULL, NULL);
                        if (!NT_SUCCESS(Status))
                            return Status;
                    }
                }
            } else
                WARN("could not find (%I64x,%x,%I64x) in dev tree\n", searchkey.objectid, searchkey.type, searchkey.offset);
        } else {
            uint64_t len = c->chunk_item->length / factor;

            c->devices[i]->devitem.bytes_used -= len;

            if (Vcb->balance.thread && Vcb->balance.shrinking && Vcb->balance.opts[0].devid == c->devices[i]->devitem.devid) {
                if (c->chunk_item->stripe[i].offset < Vcb->balance.opts[0].drange_start) {
                    uint64_t end = min(c->chunk_item->stripe[i].offset + len, Vcb->balance.opts[0].drange_start);

                    Status = space_list_add2(&c->devices[i]->space, NULL,
                                             c->chunk_item->stripe[i].offset,
                                             end - c->chunk_item->stripe[i].offset,
                                             NULL, NULL);
                    if (!NT_SUCCESS(Status))
                        return Status;
                }
            } else {
                Status = space_list_add2(&c->devices[i]->space, NULL,
                                         c->chunk_item->stripe[i].offset,
                                         len, NULL, NULL);
                if (!NT_SUCCESS(Status))
                    return Status;
            }
        }
    }

    // modify btrfs_dev_items in chunk tree
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        if (c->devices[i]) {
            uint64_t j;
            struct btrfs_dev_item* di;

            searchkey.objectid = 1;
            searchkey.type = BTRFS_DEV_ITEM_KEY;
            searchkey.offset = c->devices[i]->devitem.devid;

            Status = find_item(Vcb, Vcb->chunk_root, &tp, &searchkey, false, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("error - find_item returned %08lx\n", Status);
                return Status;
            }

            if (!keycmp(tp.item->key, searchkey)) {
                Status = delete_tree_item(Vcb, &tp);
                if (!NT_SUCCESS(Status)) {
                    ERR("delete_tree_item returned %08lx\n", Status);
                    return Status;
                }

                di = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_dev_item), ALLOC_TAG);
                if (!di) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(di, &c->devices[i]->devitem, sizeof(struct btrfs_dev_item));

                Status = insert_tree_item(Vcb, Vcb->chunk_root, 1, BTRFS_DEV_ITEM_KEY, c->devices[i]->devitem.devid, di, sizeof(struct btrfs_dev_item), NULL, Irp);
                if (!NT_SUCCESS(Status)) {
                    ERR("insert_tree_item returned %08lx\n", Status);
                    return Status;
                }
            }

            for (j = i + 1; j < c->chunk_item->num_stripes; j++) {
                if (c->devices[j] == c->devices[i])
                    c->devices[j] = NULL;
            }
        }
    }

    if (!c->created) {
        // remove CHUNK_ITEM from chunk tree
        searchkey.objectid = 0x100;
        searchkey.type = BTRFS_CHUNK_ITEM_KEY;
        searchkey.offset = c->offset;

        Status = find_item(Vcb, Vcb->chunk_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        if (!keycmp(tp.item->key, searchkey)) {
            Status = delete_tree_item(Vcb, &tp);

            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                return Status;
            }
        } else
            WARN("could not find CHUNK_ITEM for chunk %I64x\n", c->offset);

        // remove BLOCK_GROUP_ITEM from extent tree
        searchkey.objectid = c->offset;
        searchkey.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
        searchkey.offset = 0xffffffffffffffff;

        Status = find_item(Vcb, Vcb->block_group_root ? Vcb->block_group_root : Vcb->extent_root,
                           &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        if (tp.item->key.objectid == searchkey.objectid && tp.item->key.type == searchkey.type) {
            Status = delete_tree_item(Vcb, &tp);

            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                return Status;
            }
        } else
            WARN("could not find BLOCK_GROUP_ITEM for chunk %I64x\n", c->offset);
    }

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_SYSTEM)
        remove_from_bootstrap(Vcb, 0x100, BTRFS_CHUNK_ITEM_KEY, c->offset);

    RemoveEntryList(&c->list_entry);

    // clear raid56 incompat flag if dropping last RAID5/6 chunk

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5 || c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6) {
        LIST_ENTRY* le;
        bool clear_flag = true;

        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c2 = CONTAINING_RECORD(le, chunk, list_entry);

            if (c2->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5 || c2->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6) {
                clear_flag = false;
                break;
            }

            le = le->Flink;
        }

        if (clear_flag)
            Vcb->superblock.incompat_flags &= ~BTRFS_FEATURE_INCOMPAT_RAID56;
    }

    // clear raid1c34 incompat flag if dropping last RAID5/6 chunk

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C3 || c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C4) {
        LIST_ENTRY* le;
        bool clear_flag = true;

        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c2 = CONTAINING_RECORD(le, chunk, list_entry);

            if (c2->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C3 || c2->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C4) {
                clear_flag = false;
                break;
            }

            le = le->Flink;
        }

        if (clear_flag)
            Vcb->superblock.incompat_flags &= ~BTRFS_FEATURE_INCOMPAT_RAID1C34;
    }

    Vcb->superblock.bytes_used -= c->oldused;

    ExFreePool(c->chunk_item);
    ExFreePool(c->devices);

    while (!IsListEmpty(&c->space)) {
        space* s = CONTAINING_RECORD(c->space.Flink, space, list_entry);

        RemoveEntryList(&s->list_entry);
        ExFreePool(s);
    }

    while (!IsListEmpty(&c->deleting)) {
        space* s = CONTAINING_RECORD(c->deleting.Flink, space, list_entry);

        RemoveEntryList(&s->list_entry);
        ExFreePool(s);
    }

    release_chunk_lock(c, Vcb);

    ExDeleteResourceLite(&c->partial_stripes_lock);
    ExDeleteResourceLite(&c->range_locks_lock);
    ExDeleteResourceLite(&c->lock);
    ExDeleteResourceLite(&c->changed_extents_lock);

    ExFreePool(c);

    return STATUS_SUCCESS;
}

NTSTATUS update_chunks(device_extension* Vcb, LIST_ENTRY* batchlist,
                              PIRP Irp) {
    LIST_ENTRY *le, *le2;
    NTSTATUS Status;
    uint64_t used_minus_cache;

    ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);

    // FIXME - do tree chunks before data chunks

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

        le2 = le->Flink;

        if (c->changed) {
            acquire_chunk_lock(c, Vcb);

            // flush partial stripes
            if (!Vcb->readonly && (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5 || c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)) {
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
                        release_chunk_lock(c, Vcb);
                        ExReleaseResourceLite(&Vcb->chunk_lock);
                        return Status;
                    }
                }

                ExReleaseResourceLite(&c->partial_stripes_lock);
            }

            if (c->list_entry_balance.Flink || trim_chunk_pending(c)) {
                release_chunk_lock(c, Vcb);
                le = le2;
                continue;
            }

            if (c->space_changed || c->created) {
                bool created = c->created;

                used_minus_cache = c->used;

                // subtract self-hosted cache
                if (used_minus_cache > 0 && c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA && c->cache && c->cache->inode_item.size == c->used) {
                    LIST_ENTRY* le3;

                    le3 = c->cache->extents.Flink;
                    while (le3 != &c->cache->extents) {
                        extent* ext = CONTAINING_RECORD(le3, extent, list_entry);
                        struct btrfs_file_extent_item* ed = &ext->extent_data;

                        if (!ext->ignore) {
                            if (ed->type == BTRFS_FILE_EXTENT_REG || ed->type == BTRFS_FILE_EXTENT_PREALLOC) {
                                if (ed->disk_num_bytes != 0 && ed->disk_bytenr >= c->offset && ed->disk_bytenr + ed->disk_num_bytes <= c->offset + c->chunk_item->length)
                                    used_minus_cache -= ed->disk_num_bytes;
                            }
                        }

                        le3 = le3->Flink;
                    }
                }

                if (used_minus_cache == 0) {
                    Status = drop_chunk(Vcb, c, batchlist, Irp);
                    if (!NT_SUCCESS(Status)) {
                        ERR("drop_chunk returned %08lx\n", Status);
                        release_chunk_lock(c, Vcb);
                        ExReleaseResourceLite(&Vcb->chunk_lock);
                        return Status;
                    }

                    // c is now freed, so avoid releasing non-existent lock
                    le = le2;
                    continue;
                } else if (c->created) {
                    Status = create_chunk(Vcb, c, Irp);
                    if (!NT_SUCCESS(Status)) {
                        ERR("create_chunk returned %08lx\n", Status);
                        release_chunk_lock(c, Vcb);
                        ExReleaseResourceLite(&Vcb->chunk_lock);
                        return Status;
                    }
                }

                if (used_minus_cache > 0 || created)
                    release_chunk_lock(c, Vcb);
            } else
                release_chunk_lock(c, Vcb);
        }

        le = le2;
    }

    ExReleaseResourceLite(&Vcb->chunk_lock);

    return STATUS_SUCCESS;
}
