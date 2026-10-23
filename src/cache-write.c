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

// Serialization of legacy and tree free-space caches.
#include "btrfs_drv.h"
#include "trim.h"


static NTSTATUS copy_space_list(LIST_ENTRY* old_list, LIST_ENTRY* new_list) {
    LIST_ENTRY* le;

    le = old_list->Flink;
    while (le != old_list) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        space* s2;

        s2 = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);
        if (!s2) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        s2->address = s->address;
        s2->size = s->size;

        InsertTailList(new_list, &s2->list_entry);

        le = le->Flink;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS update_chunk_cache(device_extension* Vcb, chunk* c, struct btrfs_timespec* now,
                                   LIST_ENTRY* batchlist, PIRP Irp) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    struct btrfs_free_space_header* fsi;
    void* data;
    uint64_t num_entries, *cachegen, off;
    uint32_t *checksums, num_sectors;
    LIST_ENTRY space_list, deleting;

    data = ExAllocatePoolWithTag(NonPagedPool, (ULONG)c->cache->inode_item.size, ALLOC_TAG);
    if (!data) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(data, (ULONG)c->cache->inode_item.size);

    InitializeListHead(&space_list);
    InitializeListHead(&deleting);

    Status = copy_space_list(&c->space, &space_list);
    if (!NT_SUCCESS(Status)) {
        ERR("copy_space_list returned %08lx\n", Status);
        goto end;
    }

    Status = copy_space_list(&c->deleting, &deleting);
    if (!NT_SUCCESS(Status)) {
        ERR("copy_space_list returned %08lx\n", Status);
        goto end;
    }

    Status = space_list_merge(&space_list, NULL, &deleting);
    if (NT_SUCCESS(Status))
        Status = trim_merge_pending(c, &space_list, NULL);
    if (!NT_SUCCESS(Status))
        goto end;

    num_entries = 0;
    num_sectors = (uint32_t)(c->cache->inode_item.size >> Vcb->sector_shift);
    off = (sizeof(uint32_t) * num_sectors) + sizeof(uint64_t);

    while (!IsListEmpty(&space_list)) {
        struct btrfs_free_space_entry* fse;

        space* s = CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry);

        if ((off + sizeof(struct btrfs_free_space_entry)) >> Vcb->sector_shift != off >> Vcb->sector_shift)
            off = sector_align(off, Vcb->superblock.sectorsize);

        fse = (struct btrfs_free_space_entry*)((uint8_t*)data + off);

        fse->offset = s->address;
        fse->bytes = s->size;
        fse->type = BTRFS_FREE_SPACE_EXTENT;
        num_entries++;

        off += sizeof(struct btrfs_free_space_entry);

        ExFreePool(s);
    }

    // update INODE_ITEM

    c->cache->inode_item.generation = Vcb->superblock.generation;
    c->cache->inode_item.transid = Vcb->superblock.generation;
    c->cache->inode_item.sequence++;
    c->cache->inode_item.ctime = *now;

    Status = flush_fcb(c->cache, true, batchlist, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("flush_fcb returned %08lx\n", Status);
        goto end;
    }

    // update free_space item

    searchkey.objectid = BTRFS_FREE_SPACE_OBJECTID;
    searchkey.type = 0;
    searchkey.offset = c->offset;

    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        goto end;
    }

    if (keycmp(searchkey, tp.item->key)) {
        ERR("could not find (%I64x,%x,%I64x) in root_root\n", searchkey.objectid, searchkey.type, searchkey.offset);
        Status = STATUS_INTERNAL_ERROR;
        goto end;
    }

    if (tp.item->size < sizeof(struct btrfs_free_space_header)) {
        ERR("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_free_space_header));
        Status = STATUS_INTERNAL_ERROR;
        goto end;
    }

    fsi = (struct btrfs_free_space_header*)tp.item->data;

    fsi->generation = Vcb->superblock.generation;
    fsi->num_entries = num_entries;
    fsi->num_bitmaps = 0;

    // set cache generation

    cachegen = (uint64_t*)((uint8_t*)data + (sizeof(uint32_t) * num_sectors));
    *cachegen = Vcb->superblock.generation;

    // calculate cache checksums

    checksums = (uint32_t*)data;

    // FIXME - if we know sector is fully zeroed, use cached checksum

    for (uint32_t i = 0; i < num_sectors; i++) {
        if (i << Vcb->sector_shift > sizeof(uint32_t) * num_sectors)
            checksums[i] = ~calc_crc32c(0xffffffff, (uint8_t*)data + (i << Vcb->sector_shift), Vcb->superblock.sectorsize);
        else if ((i + 1) << Vcb->sector_shift < sizeof(uint32_t) * num_sectors)
            checksums[i] = 0; // FIXME - test this
        else
            checksums[i] = ~calc_crc32c(0xffffffff, (uint8_t*)data + (sizeof(uint32_t) * num_sectors), ((i + 1) << Vcb->sector_shift) - (sizeof(uint32_t) * num_sectors));
    }

    // write cache

    Status = do_write_file(c->cache, 0, c->cache->inode_item.size, data, NULL,
                           false, 0, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("do_write_file returned %08lx\n", Status);

        // Writing the cache isn't critical, so we don't return an error if writing fails. This means
        // we can still flush on a degraded mount if metadata is RAID1 but data is RAID0.
    }

    Status = STATUS_SUCCESS;

end:
    while (!IsListEmpty(&space_list)) {
        ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
    }

    while (!IsListEmpty(&deleting)) {
        ExFreePool(CONTAINING_RECORD(RemoveHeadList(&deleting), space, list_entry));
    }

    ExFreePool(data);

    return Status;
}

static bool should_write_fst_bitmaps(device_extension* Vcb, chunk* c,
                                     LIST_ENTRY* space_list) {
    LIST_ENTRY* le;
    size_t num_bitmaps, bitmap_size, extent_size = 0;
    size_t bitmap_range = (BTRFS_FREE_SPACE_BITMAP_SIZE * 8) << Vcb->sector_shift;

    num_bitmaps = c->chunk_item->length / bitmap_range;
    bitmap_size = (sizeof(struct btrfs_item) + BTRFS_FREE_SPACE_BITMAP_SIZE) * num_bitmaps;

    if (c->chunk_item->length % bitmap_range) {
        bitmap_size += sizeof(struct btrfs_item);
        bitmap_size += sector_align((c->chunk_item->length % bitmap_range) >> Vcb->sector_shift, 8) / 8;
    }

    le = space_list->Flink;
    while (le != space_list) {
        extent_size += sizeof(struct btrfs_item);

        if (extent_size > bitmap_size)
            return true;

        le = le->Flink;
    }

    if (extent_size == 0)
        return false;

    if (c->using_fst_bitmaps) {
        if (bitmap_size <= 100 * sizeof(struct btrfs_item) || extent_size > bitmap_size - (100 * sizeof(struct btrfs_item)))
            return true;
    }

    return false;
}

static void mark_tree_for_writing_in_place(device_extension* Vcb, tree* t) {
    t->write = true;

    while (t) {
        if (t->paritem && t->paritem->ignore) {
            t->paritem->ignore = false;
            t->parent->header.nritems++;
            t->parent->size += sizeof(struct btrfs_key_ptr);
        }

        t->header.generation = Vcb->superblock.generation;
        t = t->parent;
    }
}

static NTSTATUS update_free_space_info(device_extension* Vcb, chunk* c,
                                       uint32_t extent_count, uint32_t flags,
                                       PIRP Irp) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    struct btrfs_free_space_info* fsi;
    traverse_ptr tp;

    // change BTRFS_FREE_SPACE_INFO_KEY in place if present, and insert otherwise

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_FREE_SPACE_INFO_KEY;
    searchkey.offset = c->chunk_item->length;

    Status = find_item(Vcb, Vcb->space_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (NT_SUCCESS(Status) && !keycmp(tp.item->key, searchkey)) {
        if (tp.item->size == sizeof(struct btrfs_free_space_info)) {
            // change in place if possible

            fsi = (struct btrfs_free_space_info*)tp.item->data;

            fsi->extent_count = extent_count;
            fsi->flags = flags;

            mark_tree_for_writing_in_place(Vcb, tp.tree);

            return STATUS_SUCCESS;
        } else
            delete_tree_item(Vcb, &tp);
    }

    // insert FREE_SPACE_INFO

    fsi = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_free_space_info), ALLOC_TAG);
    if (!fsi) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fsi->extent_count = extent_count;
    fsi->flags = flags;

    Status = insert_tree_item(Vcb, Vcb->space_root, c->offset, BTRFS_FREE_SPACE_INFO_KEY, c->chunk_item->length, fsi, sizeof(*fsi),
                              NULL, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS delete_fst_entries(device_extension* Vcb, chunk* c, PIRP Irp) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_FREE_SPACE_EXTENT_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->space_root, &tp, &searchkey, false, Irp);
    if (Status == STATUS_NOT_FOUND)
        return STATUS_SUCCESS;
    else if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    while (tp.item->key.objectid < c->offset + c->chunk_item->length) {
        traverse_ptr next_tp;

        if (tp.item->key.objectid >= c->offset && (tp.item->key.type == BTRFS_FREE_SPACE_EXTENT_KEY || tp.item->key.type == BTRFS_FREE_SPACE_BITMAP_KEY)) {
            Status = delete_tree_item(Vcb, &tp);
            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                return Status;
            }
        }

        if (!find_next_item(Vcb, &tp, &next_tp, false, NULL))
            break;

        tp = next_tp;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS setup_fst_bitmaps(device_extension* Vcb, chunk* c, PIRP Irp) {
    NTSTATUS Status;
    uint64_t off = c->offset;

    while (off < c->offset + c->chunk_item->length) {
        size_t size, bufsize;
        uint8_t* buf;

        size = (BTRFS_FREE_SPACE_BITMAP_SIZE * 8) << Vcb->sector_shift;
        bufsize = BTRFS_FREE_SPACE_BITMAP_SIZE;

        if (off + size > c->offset + c->chunk_item->length) {
            size = c->offset + c->chunk_item->length - off;
            bufsize = sector_align(size >> Vcb->sector_shift, 8) / 8;
        }

        buf = ExAllocatePoolWithTag(NonPagedPool, bufsize, ALLOC_TAG);
        if (!buf) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        memset(buf, 0, bufsize);

        Status = insert_tree_item(Vcb, Vcb->space_root, off,
                                  BTRFS_FREE_SPACE_BITMAP_KEY, size, buf,
                                  bufsize, NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item returned %08lx\n", Status);
            ExFreePool(buf);
            return Status;
        }

        off += size;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS check_fst_bitmaps_sane(device_extension* Vcb, chunk* c, PIRP Irp) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp, next_tp;
    uint64_t exp_start, max_len;

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_FREE_SPACE_EXTENT_KEY; // sic
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->space_root, &tp, &searchkey, false, Irp);

    if (Status == STATUS_NOT_FOUND)
        goto recreate;
    else if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (tp.item->key.objectid == c->offset && tp.item->key.type == BTRFS_FREE_SPACE_INFO_KEY) {
        if (!find_next_item(Vcb, &tp, &next_tp, false, Irp))
            goto recreate;

        tp = next_tp;
    }

    exp_start = c->offset;
    max_len = c->chunk_item->length;

    do {
        uint32_t exp_len;

        if (tp.item->key.objectid != exp_start || tp.item->key.type != BTRFS_FREE_SPACE_BITMAP_KEY)
            goto recreate;

        if (tp.item->key.offset < Vcb->superblock.sectorsize || tp.item->key.offset > max_len)
            goto recreate;

        if (tp.item->key.offset & (Vcb->superblock.sectorsize - 1))
            goto recreate;

        exp_len = sector_align(tp.item->key.offset / Vcb->superblock.sectorsize, 8) / 8;

        if (tp.item->size != exp_len)
            goto recreate;

        exp_start = tp.item->key.objectid + tp.item->key.offset;
        max_len = c->offset + c->chunk_item->length - exp_start;

        if (max_len == 0)
            break;

        if (!find_next_item(Vcb, &tp, &next_tp, false, Irp))
            goto recreate;

        tp = next_tp;
    } while (true);

    return STATUS_SUCCESS;

recreate:
    Status = delete_fst_entries(Vcb, c, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("delete_fst_entries returned %08lx\n", Status);
        return Status;
    }

    Status = setup_fst_bitmaps(Vcb, c, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("setup_fst_bitmaps returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS update_chunk_cache_tree_bitmaps(device_extension* Vcb, chunk* c,
                                                PIRP Irp, LIST_ENTRY* space_list) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    uint64_t exp_start, max_len;
    uint32_t fsi_count = 0;

    if (!c->using_fst_bitmaps) {
        Status = delete_fst_entries(Vcb, c, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_fst_entries returned %08lx\n", Status);
            return Status;
        }

        Status = setup_fst_bitmaps(Vcb, c, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("setup_fst_bitmaps returned %08lx\n", Status);
            return Status;
        }
    } else {
        Status = check_fst_bitmaps_sane(Vcb, c, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("check_fst_bitmaps_sane returned %08lx\n", Status);
            return Status;
        }
    }

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_FREE_SPACE_BITMAP_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->space_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    exp_start = c->offset;
    max_len = c->chunk_item->length;

    do {
        RTL_BITMAP bmph;
        uint32_t exp_len;
        ULONG* buf;
        traverse_ptr next_tp;

        if (tp.item->key.objectid != exp_start || tp.item->key.type != BTRFS_FREE_SPACE_BITMAP_KEY) {
            ERR("found (%I64x,%x,%I64x), expected (%I64x,%x,x)\n",
                tp.item->key.objectid, tp.item->key.type, tp.item->key.offset,
                exp_start, BTRFS_FREE_SPACE_BITMAP_KEY);
            return STATUS_INTERNAL_ERROR;
        }

        if (tp.item->key.offset < Vcb->superblock.sectorsize ||
            tp.item->key.offset > max_len) {
            ERR("found (%I64x,%x,%I64x), expected offset of %x to %I64x\n",
                tp.item->key.objectid, tp.item->key.type, tp.item->key.offset,
                Vcb->superblock.sectorsize, max_len);
            return STATUS_INTERNAL_ERROR;
        }

        if (tp.item->key.offset & (Vcb->superblock.sectorsize - 1)) {
            ERR("found (%I64x,%x,%I64x), offset not aligned to sector size %x\n",
                tp.item->key.objectid, tp.item->key.type, tp.item->key.offset,
                Vcb->superblock.sectorsize);
            return STATUS_INTERNAL_ERROR;
        }

        exp_len = sector_align(tp.item->key.offset / Vcb->superblock.sectorsize, 8) / 8;

        if (tp.item->size != exp_len) {
            ERR("(%I64x,%x,%I64x): size was %x, expected %x\n",
                tp.item->key.objectid, tp.item->key.type, tp.item->key.offset,
                tp.item->size, exp_len);
            return STATUS_INTERNAL_ERROR;
        }

        mark_tree_for_writing_in_place(Vcb, tp.tree);

        buf = ExAllocatePoolWithTag(NonPagedPool, sector_align(tp.item->size, sizeof(ULONG)), ALLOC_TAG);
        if (!buf) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlInitializeBitMap(&bmph, buf, tp.item->size * 8);

        RtlClearAllBits(&bmph);

        while (!IsListEmpty(space_list)) {
            space* s = CONTAINING_RECORD(space_list->Flink, space, list_entry);

            if (s->address >= tp.item->key.objectid + tp.item->key.offset)
                break;

            if (s->address + s->size <= tp.item->key.objectid + tp.item->key.offset) {
                RtlSetBits(&bmph, (s->address - tp.item->key.objectid) >> Vcb->sector_shift,
                           s->size >> Vcb->sector_shift);
                RemoveEntryList(&s->list_entry);
                ExFreePool(s);

                fsi_count++;
            } else {
                uint64_t overrun = s->address + s->size - tp.item->key.objectid - tp.item->key.offset;

                RtlSetBits(&bmph, (s->address - tp.item->key.objectid) >> Vcb->sector_shift,
                           (s->size - overrun) >> Vcb->sector_shift);

                s->address += s->size - overrun;
                s->size = overrun;

                break;
            }
        }

        memcpy(tp.item->data, buf, tp.item->size);
        ExFreePool(buf);

        exp_start = tp.item->key.objectid + tp.item->key.offset;
        max_len = c->offset + c->chunk_item->length - exp_start;

        if (max_len == 0)
            break;

        if (!find_next_item(Vcb, &tp, &next_tp, false, Irp))
            return STATUS_INTERNAL_ERROR;

        tp = next_tp;
    } while (true);

    if (!IsListEmpty(space_list)) {
        ERR("space list was not empty\n");
        return STATUS_INTERNAL_ERROR;
    }

    c->using_fst_bitmaps = true;

    Status = update_free_space_info(Vcb, c, fsi_count,
                                    BTRFS_FREE_SPACE_USING_BITMAPS, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("update_free_space_info returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS update_chunk_cache_tree(device_extension* Vcb, chunk* c, PIRP Irp) {
    NTSTATUS Status;
    LIST_ENTRY space_list;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    uint32_t fsi_count = 0;

    InitializeListHead(&space_list);

    Status = copy_space_list(&c->space, &space_list);
    if (!NT_SUCCESS(Status)) {
        ERR("copy_space_list returned %08lx\n", Status);

        while (!IsListEmpty(&space_list)) {
            ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
        }

        return Status;
    }

    Status = space_list_merge(&space_list, NULL, &c->deleting);
    if (NT_SUCCESS(Status))
        Status = trim_merge_pending(c, &space_list, NULL);
    if (!NT_SUCCESS(Status)) {
        while (!IsListEmpty(&space_list)) {
            ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
        }

        return Status;
    }

    if (should_write_fst_bitmaps(Vcb, c, &space_list)) {
        Status = update_chunk_cache_tree_bitmaps(Vcb, c, Irp, &space_list);

        if (!NT_SUCCESS(Status)) {
            while (!IsListEmpty(&space_list)) {
                ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
            }
        }

        return Status;
    }

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_FREE_SPACE_EXTENT_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->space_root, &tp, &searchkey, false, Irp);
    if (Status == STATUS_NOT_FOUND)
        goto after_tree_walk;
    else if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);

        while (!IsListEmpty(&space_list)) {
            ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
        }

        return Status;
    }

    while (!IsListEmpty(&space_list) && tp.item->key.objectid < c->offset + c->chunk_item->length) {
        traverse_ptr next_tp;

        if (tp.item->key.type == BTRFS_FREE_SPACE_EXTENT_KEY) {
            space* s = CONTAINING_RECORD(space_list.Flink, space, list_entry);

            if (s->address < tp.item->key.objectid || (s->address == tp.item->key.objectid && s->size < tp.item->key.offset)) {
                // add entry

                Status = insert_tree_item(Vcb, Vcb->space_root, s->address, BTRFS_FREE_SPACE_EXTENT_KEY, s->size, NULL, 0,
                                          NULL, Irp);
                if (!NT_SUCCESS(Status)) {
                    ERR("insert_tree_item returned %08lx\n", Status);

                    while (!IsListEmpty(&space_list)) {
                        ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
                    }

                    return Status;
                }

                fsi_count++;

                RemoveHeadList(&space_list);
                ExFreePool(s);
                continue;
            } else if (s->address == tp.item->key.objectid && s->size == tp.item->key.offset) {
                // unchanged entry

                fsi_count++;

                RemoveHeadList(&space_list);
                ExFreePool(s);
            } else {
                // remove entry

                Status = delete_tree_item(Vcb, &tp);
                if (!NT_SUCCESS(Status)) {
                    ERR("delete_tree_item returned %08lx\n", Status);

                    while (!IsListEmpty(&space_list)) {
                        ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
                    }

                    return Status;
                }
            }
        } else if (tp.item->key.type == BTRFS_FREE_SPACE_BITMAP_KEY) {
            Status = delete_tree_item(Vcb, &tp);
            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);

                while (!IsListEmpty(&space_list)) {
                    ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
                }

                return Status;
            }
        }

        if (!find_next_item(Vcb, &tp, &next_tp, false, Irp))
            goto after_tree_walk;

        tp = next_tp;
    }

    // after loop, delete remaining tree items for this chunk

    while (tp.item->key.objectid < c->offset + c->chunk_item->length) {
        traverse_ptr next_tp;

        if (tp.item->key.type == BTRFS_FREE_SPACE_EXTENT_KEY || tp.item->key.type == BTRFS_FREE_SPACE_BITMAP_KEY) {
            Status = delete_tree_item(Vcb, &tp);
            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                return Status;
            }
        }

        if (!find_next_item(Vcb, &tp, &next_tp, false, NULL))
            break;

        tp = next_tp;
    }

after_tree_walk:
    // after loop, insert remaining space_list entries

    while (!IsListEmpty(&space_list)) {
        space* s = CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry);

        Status = insert_tree_item(Vcb, Vcb->space_root, s->address, BTRFS_FREE_SPACE_EXTENT_KEY, s->size, NULL, 0,
                                  NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item returned %08lx\n", Status);

            while (!IsListEmpty(&space_list)) {
                ExFreePool(CONTAINING_RECORD(RemoveHeadList(&space_list), space, list_entry));
            }

            return Status;
        }

        fsi_count++;

        ExFreePool(s);
    }

    c->using_fst_bitmaps = false;

    Status = update_free_space_info(Vcb, c, fsi_count, 0, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("update_free_space_info returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS update_chunk_caches(device_extension* Vcb, PIRP Irp) {
    LIST_ENTRY *le, batchlist;
    NTSTATUS Status;
    chunk* c;
    LARGE_INTEGER time;
    struct btrfs_timespec now;

    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);

    InitializeListHead(&batchlist);

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        c = CONTAINING_RECORD(le, chunk, list_entry);

        if (c->space_changed && c->chunk_item->length >= 0x6400000) { // 100MB
            acquire_chunk_lock(c, Vcb);
            Status = update_chunk_cache(Vcb, c, &now, &batchlist, Irp);
            release_chunk_lock(c, Vcb);

            if (!NT_SUCCESS(Status)) {
                ERR("update_chunk_cache(%I64x) returned %08lx\n", c->offset, Status);
                clear_batch_list(Vcb, &batchlist);
                return Status;
            }
        }

        le = le->Flink;
    }

    Status = commit_batch_list(Vcb, &batchlist, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("commit_batch_list returned %08lx\n", Status);
        return Status;
    }

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        c = CONTAINING_RECORD(le, chunk, list_entry);

        if (c->changed && (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5 || c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)) {
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

    return STATUS_SUCCESS;
}

NTSTATUS update_chunk_caches_tree(device_extension* Vcb, PIRP Irp) {
    LIST_ENTRY *le;
    NTSTATUS Status;
    chunk* c;

    Vcb->superblock.compat_ro_flags |= BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID;

    ExAcquireResourceSharedLite(&Vcb->chunk_lock, true);

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        c = CONTAINING_RECORD(le, chunk, list_entry);

        if (c->space_changed) {
            acquire_chunk_lock(c, Vcb);
            Status = update_chunk_cache_tree(Vcb, c, Irp);
            release_chunk_lock(c, Vcb);

            if (!NT_SUCCESS(Status)) {
                ERR("update_chunk_cache_tree(%I64x) returned %08lx\n", c->offset, Status);
                ExReleaseResourceLite(&Vcb->chunk_lock);
                return Status;
            }
        }

        le = le->Flink;
    }

    ExReleaseResourceLite(&Vcb->chunk_lock);

    return STATUS_SUCCESS;
}
