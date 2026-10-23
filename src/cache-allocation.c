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

// Allocation of legacy free-space cache files.
#include "btrfs_drv.h"
#define CACHE_INCREMENTS 64

static NTSTATUS insert_cache_extent(fcb* fcb, uint64_t start, uint64_t length) {
    NTSTATUS Status;
    LIST_ENTRY* le = fcb->Vcb->chunks.Flink;
    chunk* c;
    uint64_t flags;

    flags = fcb->Vcb->data_flags;

    while (le != &fcb->Vcb->chunks) {
        c = CONTAINING_RECORD(le, chunk, list_entry);

        if (!c->readonly && !c->reloc) {
            acquire_chunk_lock(c, fcb->Vcb);

            if (c->chunk_item->type == flags && (c->chunk_item->length - c->used) >= length) {
                Status = insert_extent_chunk(fcb->Vcb, fcb, c, start, length, false,
                                             NULL, NULL, NULL, BTRFS_COMPRESS_NONE,
                                             length, false, 0);
                if (NT_SUCCESS(Status))
                    return STATUS_SUCCESS;
                else if (Status != STATUS_DISK_FULL) {
                    release_chunk_lock(c, fcb->Vcb);
                    return Status;
                }
            }

            release_chunk_lock(c, fcb->Vcb);
        }

        le = le->Flink;
    }

    Status = alloc_chunk(fcb->Vcb, flags, &c, false);

    if (!NT_SUCCESS(Status)) {
        ERR("alloc_chunk returned %08lx\n", Status);
        return Status;
    }

    acquire_chunk_lock(c, fcb->Vcb);

    if (c->chunk_item->type == flags && (c->chunk_item->length - c->used) >= length) {
        Status = insert_extent_chunk(fcb->Vcb, fcb, c, start, length, false, NULL,
                                     NULL, NULL, BTRFS_COMPRESS_NONE, length,
                                     false, 0);
        if (NT_SUCCESS(Status))
            return STATUS_SUCCESS;
        else if (Status != STATUS_DISK_FULL) {
            release_chunk_lock(c, fcb->Vcb);
            return Status;
        }
    }

    release_chunk_lock(c, fcb->Vcb);

    return STATUS_DISK_FULL;
}

static NTSTATUS allocate_cache_chunk(device_extension* Vcb, chunk* c, bool* changed,
                                     LIST_ENTRY* batchlist, PIRP Irp) {
    LIST_ENTRY* le;
    NTSTATUS Status;
    uint64_t num_entries, new_cache_size, i;
    uint32_t num_sectors;
    bool realloc_extents = false;

    // FIXME - also do bitmaps
    // FIXME - make sure this works when sector_size is not 4096

    *changed = false;

    num_entries = 0;

    // num_entries is the number of entries in c->space and c->deleting - it might
    // be slightly higher then what we end up writing, but doing it this way is much
    // quicker and simpler.
    if (!IsListEmpty(&c->space)) {
        le = c->space.Flink;
        while (le != &c->space) {
            num_entries++;

            le = le->Flink;
        }
    }

    if (!IsListEmpty(&c->deleting)) {
        le = c->deleting.Flink;
        while (le != &c->deleting) {
            num_entries++;

            le = le->Flink;
        }
    }

    new_cache_size = sizeof(uint64_t) + (num_entries * sizeof(struct btrfs_free_space_entry));

    num_sectors = (uint32_t)sector_align(new_cache_size, Vcb->superblock.sectorsize) >> Vcb->sector_shift;
    num_sectors = (uint32_t)sector_align(num_sectors, CACHE_INCREMENTS);

    // adjust for padding
    // FIXME - there must be a more efficient way of doing this
    new_cache_size = sizeof(uint64_t) + (sizeof(uint32_t) * num_sectors);
    for (i = 0; i < num_entries; i++) {
        if ((new_cache_size >> Vcb->sector_shift) != ((new_cache_size + sizeof(struct btrfs_free_space_entry)) >> Vcb->sector_shift))
            new_cache_size = sector_align(new_cache_size, Vcb->superblock.sectorsize);

        new_cache_size += sizeof(struct btrfs_free_space_entry);
    }

    new_cache_size = sector_align(new_cache_size, CACHE_INCREMENTS << Vcb->sector_shift);

    TRACE("chunk %I64x: cache_size = %I64x, new_cache_size = %I64x\n", c->offset, c->cache ? c->cache->inode_item.size : 0, new_cache_size);

    if (c->cache) {
        if (new_cache_size > c->cache->inode_item.size)
            realloc_extents = true;
        else {
            le = c->cache->extents.Flink;

            while (le != &c->cache->extents) {
                extent* ext = CONTAINING_RECORD(le, extent, list_entry);

                if (!ext->ignore && (ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC)) {
                    if (ext->extent_data.disk_num_bytes != 0) {
                        chunk* c2 = get_chunk_from_address(Vcb, ext->extent_data.disk_bytenr);

                        if (c2 && (c2->readonly || c2->reloc)) {
                            realloc_extents = true;
                            break;
                        }
                    }
                }

                le = le->Flink;
            }
        }
    }

    if (!c->cache) {
        struct btrfs_free_space_header* fsi;
        struct btrfs_key searchkey;
        traverse_ptr tp;

        // create new inode

        c->cache = create_fcb(Vcb, PagedPool);
        if (!c->cache) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        c->cache->Vcb = Vcb;

        c->cache->inode_item.size = new_cache_size;
        c->cache->inode_item.nbytes = new_cache_size;
        c->cache->inode_item.nlink = 1;
        c->cache->inode_item.mode = S_IRUSR | S_IWUSR | __S_IFREG;
        c->cache->inode_item.flags = BTRFS_INODE_NODATASUM | BTRFS_INODE_NODATACOW | BTRFS_INODE_NOCOMPRESS | BTRFS_INODE_PREALLOC;

        c->cache->Header.IsFastIoPossible = fast_io_possible(c->cache);
        c->cache->Header.AllocationSize.QuadPart = 0;
        c->cache->Header.FileSize.QuadPart = 0;
        c->cache->Header.ValidDataLength.QuadPart = 0;

        c->cache->subvol = Vcb->root_root;

        c->cache->inode = InterlockedIncrement64(&Vcb->root_root->lastinode);
        c->cache->hash = calc_crc32c(0xffffffff, (uint8_t*)&c->cache->inode, sizeof(uint64_t));

        c->cache->type = BTRFS_FT_REG_FILE;
        c->cache->created = true;

        // create new free space entry

        fsi = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_free_space_header), ALLOC_TAG);
        if (!fsi) {
            ERR("out of memory\n");
            reap_fcb(c->cache);
            c->cache = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        searchkey.objectid = BTRFS_FREE_SPACE_OBJECTID;
        searchkey.type = 0;
        searchkey.offset = c->offset;

        Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            ExFreePool(fsi);
            reap_fcb(c->cache);
            c->cache = NULL;
            return Status;
        }

        if (!keycmp(searchkey, tp.item->key)) {
            Status = delete_tree_item(Vcb, &tp);
            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                ExFreePool(fsi);
                reap_fcb(c->cache);
                c->cache = NULL;
                return Status;
            }
        }

        fsi->location.objectid = c->cache->inode;
        fsi->location.type = BTRFS_INODE_ITEM_KEY;
        fsi->location.offset = 0;

        Status = insert_tree_item(Vcb, Vcb->root_root, BTRFS_FREE_SPACE_OBJECTID, 0, c->offset, fsi, sizeof(struct btrfs_free_space_header), NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item returned %08lx\n", Status);
            ExFreePool(fsi);
            reap_fcb(c->cache);
            c->cache = NULL;
            return Status;
        }

        // allocate space

        Status = insert_cache_extent(c->cache, 0, new_cache_size);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_cache_extent returned %08lx\n", Status);
            reap_fcb(c->cache);
            c->cache = NULL;
            return Status;
        }

        c->cache->extents_changed = true;
        InsertTailList(&Vcb->all_fcbs, &c->cache->list_entry_all);

        add_fcb_to_subvol(c->cache);

        Status = flush_fcb(c->cache, true, batchlist, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("flush_fcb returned %08lx\n", Status);
            free_fcb(c->cache);
            c->cache = NULL;
            return Status;
        }

        *changed = true;
    } else if (realloc_extents) {
        struct btrfs_key searchkey;
        traverse_ptr tp;

        TRACE("reallocating extents\n");

        // add free_space entry to tree cache

        searchkey.objectid = BTRFS_FREE_SPACE_OBJECTID;
        searchkey.type = 0;
        searchkey.offset = c->offset;

        Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        if (keycmp(searchkey, tp.item->key)) {
            ERR("could not find (%I64x,%x,%I64x) in root_root\n", searchkey.objectid, searchkey.type, searchkey.offset);
            return STATUS_INTERNAL_ERROR;
        }

        if (tp.item->size < sizeof(struct btrfs_free_space_header)) {
            ERR("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_free_space_header));
            return STATUS_INTERNAL_ERROR;
        }

        tp.tree->write = true;

        // remove existing extents

        if (c->cache->inode_item.size > 0) {
            le = c->cache->extents.Flink;

            while (le != &c->cache->extents) {
                extent* ext = CONTAINING_RECORD(le, extent, list_entry);

                if (!ext->ignore && (ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC)) {
                    if (ext->extent_data.disk_num_bytes != 0) {
                        chunk* c2 = get_chunk_from_address(Vcb, ext->extent_data.disk_bytenr);

                        if (c2) {
                            c2->changed = true;
                            c2->space_changed = true;
                        }
                    }
                }

                le = le->Flink;
            }

            Status = excise_extents(Vcb, c->cache, 0, c->cache->inode_item.size,
                                    Irp, NULL);
            if (!NT_SUCCESS(Status)) {
                ERR("excise_extents returned %08lx\n", Status);
                return Status;
            }
        }

        // add new extent

        Status = insert_cache_extent(c->cache, 0, new_cache_size);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_cache_extent returned %08lx\n", Status);
            return Status;
        }

        // modify INODE_ITEM

        c->cache->inode_item.size = new_cache_size;
        c->cache->inode_item.nbytes = new_cache_size;

        Status = flush_fcb(c->cache, true, batchlist, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("flush_fcb returned %08lx\n", Status);
            return Status;
        }

        *changed = true;
    } else {
        struct btrfs_key searchkey;
        traverse_ptr tp;

        // add INODE_ITEM and free_space entry to tree cache, for writing later

        searchkey.objectid = c->cache->inode;
        searchkey.type = BTRFS_INODE_ITEM_KEY;
        searchkey.offset = 0;

        Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        if (keycmp(searchkey, tp.item->key)) {
            struct btrfs_inode_item* ii;

            ii = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_inode_item), ALLOC_TAG);
            if (!ii) {
                ERR("out of memory\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            RtlCopyMemory(ii, &c->cache->inode_item, sizeof(struct btrfs_inode_item));

            Status = insert_tree_item(Vcb, Vcb->root_root, c->cache->inode, BTRFS_INODE_ITEM_KEY, 0, ii, sizeof(struct btrfs_inode_item), NULL, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("insert_tree_item returned %08lx\n", Status);
                ExFreePool(ii);
                return Status;
            }

            *changed = true;
        } else {
            if (tp.item->size < sizeof(struct btrfs_inode_item)) {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_inode_item));
                return STATUS_INTERNAL_ERROR;
            }

            tp.tree->write = true;
        }

        searchkey.objectid = BTRFS_FREE_SPACE_OBJECTID;
        searchkey.type = 0;
        searchkey.offset = c->offset;

        Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        if (keycmp(searchkey, tp.item->key)) {
            ERR("could not find (%I64x,%x,%I64x) in root_root\n", searchkey.objectid, searchkey.type, searchkey.offset);
            return STATUS_INTERNAL_ERROR;
        }

        if (tp.item->size < sizeof(struct btrfs_free_space_header)) {
            ERR("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_free_space_header));
            return STATUS_INTERNAL_ERROR;
        }

        tp.tree->write = true;
    }

    // FIXME - reduce inode allocation if cache is shrinking. Make sure to avoid infinite write loops

    return STATUS_SUCCESS;
}

NTSTATUS allocate_cache(device_extension* Vcb, bool* changed, PIRP Irp) {
    LIST_ENTRY *le, batchlist;
    NTSTATUS Status;

    *changed = false;

    InitializeListHead(&batchlist);

    ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

        if (c->space_changed && c->chunk_item->length >= 0x6400000) { // 100MB
            bool b;

            acquire_chunk_lock(c, Vcb);
            Status = allocate_cache_chunk(Vcb, c, &b, &batchlist, Irp);
            release_chunk_lock(c, Vcb);

            if (b)
                *changed = true;

            if (!NT_SUCCESS(Status)) {
                ERR("allocate_cache_chunk(%I64x) returned %08lx\n", c->offset, Status);
                ExReleaseResourceLite(&Vcb->chunk_lock);
                clear_batch_list(Vcb, &batchlist);
                return Status;
            }
        }

        le = le->Flink;
    }

    ExReleaseResourceLite(&Vcb->chunk_lock);

    Status = commit_batch_list(Vcb, &batchlist, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("commit_batch_list returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}
