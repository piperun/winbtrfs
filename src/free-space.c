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

#include "btrfs_drv.h"
#include "space-list.h"
#include "crc32c.h"

// Number of increments in the size of each cache inode, in sectors. Should
// this be a constant number of sectors, a constant 256 KB, or what?
#define CACHE_INCREMENTS    64

static NTSTATUS remove_free_space_inode(device_extension* Vcb, uint64_t inode,
                                        LIST_ENTRY* batchlist, PIRP Irp) {
    NTSTATUS Status;
    fcb* fcb;

    Status = open_fcb(Vcb, Vcb->root_root, inode, BTRFS_FT_REG_FILE, NULL, false, NULL, &fcb, PagedPool, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("open_fcb returned %08lx\n", Status);
        return Status;
    }

    mark_fcb_dirty(fcb);

    if (fcb->inode_item.size > 0) {
        Status = excise_extents(fcb->Vcb, fcb, 0, sector_align(fcb->inode_item.size, fcb->Vcb->superblock.sectorsize),
                                Irp, NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("excise_extents returned %08lx\n", Status);
            free_fcb(fcb);
            return Status;
        }
    }

    fcb->deleted = true;

    Status = flush_fcb(fcb, false, batchlist, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("flush_fcb returned %08lx\n", Status);
        free_fcb(fcb);
        return Status;
    }

    free_fcb(fcb);

    return STATUS_SUCCESS;
}

NTSTATUS clear_free_space_cache(device_extension* Vcb, LIST_ENTRY* batchlist, PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp, next_tp;
    NTSTATUS Status;
    bool b;

    searchkey.objectid = BTRFS_FREE_SPACE_OBJECTID;
    searchkey.type = 0;
    searchkey.offset = 0;

    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return Status;
    }

    do {
        if (tp.item->key.objectid > searchkey.objectid || (tp.item->key.objectid == searchkey.objectid && tp.item->key.type > searchkey.type))
            break;

        if (tp.item->key.objectid == searchkey.objectid && tp.item->key.type == searchkey.type) {
            if (tp.item->size >= sizeof(struct btrfs_free_space_header)) {
                struct btrfs_free_space_header* fsi = (struct btrfs_free_space_header*)tp.item->data;

                if (fsi->location.type != BTRFS_INODE_ITEM_KEY)
                    WARN("key (%I64x,%x,%I64x) does not point to an INODE_ITEM\n", fsi->location.objectid, fsi->location.type, fsi->location.offset);
                else {
                    LIST_ENTRY* le;

                    Status = remove_free_space_inode(Vcb, fsi->location.objectid, batchlist, Irp);

                    if (!NT_SUCCESS(Status))
                        ERR("remove_free_space_inode for (%I64x,%x,%I64x) returned %08lx\n", fsi->location.objectid, fsi->location.type, fsi->location.offset, Status);
                    else {
                        Status = delete_tree_item(Vcb, &tp);
                        if (!NT_SUCCESS(Status)) {
                            ERR("delete_tree_item returned %08lx\n", Status);
                            return Status;
                        }
                    }

                    le = Vcb->chunks.Flink;
                    while (le != &Vcb->chunks) {
                        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

                        if (c->offset == tp.item->key.offset && c->cache) {
                            reap_fcb(c->cache);
                            c->cache = NULL;
                        }

                        le = le->Flink;
                    }
                }
            } else
                WARN("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_free_space_header));
        }

        b = find_next_item(Vcb, &tp, &next_tp, false, Irp);
        if (b)
            tp = next_tp;
    } while (b);

    Status = STATUS_SUCCESS;

    if (Vcb->space_root) {
        searchkey.objectid = 0;
        searchkey.type = 0;
        searchkey.offset = 0;

        Status = find_item(Vcb, Vcb->space_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("find_item returned %08lx\n", Status);
            return Status;
        }

        do {
            Status = delete_tree_item(Vcb, &tp);
            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                return Status;
            }

            b = find_next_item(Vcb, &tp, &next_tp, false, Irp);
            if (b)
                tp = next_tp;
        } while (b);
    }

    // regenerate free space tree
    if (Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE) {
        LIST_ENTRY* le;

        ExAcquireResourceSharedLite(&Vcb->chunk_lock, true);

        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

            if (!c->cache_loaded) {
                acquire_chunk_lock(c, Vcb);

                Status = load_cache_chunk(Vcb, c, NULL);
                if (!NT_SUCCESS(Status)) {
                    ERR("load_cache_chunk(%I64x) returned %08lx\n", c->offset, Status);
                    release_chunk_lock(c, Vcb);
                    ExReleaseResourceLite(&Vcb->chunk_lock);
                    return Status;
                }

                c->changed = true;
                c->space_changed = true;

                release_chunk_lock(c, Vcb);
            }

            le = le->Flink;
        }

        ExReleaseResourceLite(&Vcb->chunk_lock);
    }

    return Status;
}

NTSTATUS add_space_entry(LIST_ENTRY* list, LIST_ENTRY* list_size, uint64_t offset, uint64_t size) {
    space* s;

    s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);

    if (!s) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    s->address = offset;
    s->size = size;

    if (IsListEmpty(list))
        InsertTailList(list, &s->list_entry);
    else {
        space* s2 = CONTAINING_RECORD(list->Blink, space, list_entry);

        if (s2->address <= offset)
            InsertTailList(list, &s->list_entry);
        else {
            LIST_ENTRY* le;

            le = list->Flink;
            while (le != list) {
                s2 = CONTAINING_RECORD(le, space, list_entry);

                if (s2->address > offset) {
                    InsertTailList(le, &s->list_entry);
                    goto size;
                }

                le = le->Flink;
            }
        }
    }

size:
    if (!list_size)
        return STATUS_SUCCESS;

    if (IsListEmpty(list_size))
        InsertTailList(list_size, &s->list_entry_size);
    else {
        space* s2 = CONTAINING_RECORD(list_size->Blink, space, list_entry_size);

        if (s2->size >= size)
            InsertTailList(list_size, &s->list_entry_size);
        else {
            LIST_ENTRY* le;

            le = list_size->Flink;
            while (le != list_size) {
                s2 = CONTAINING_RECORD(le, space, list_entry_size);

                if (s2->size <= size) {
                    InsertHeadList(le->Blink, &s->list_entry_size);
                    return STATUS_SUCCESS;
                }

                le = le->Flink;
            }
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS load_free_space_bitmap(device_extension* Vcb, chunk* c, uint64_t offset, void* data, uint64_t* total_space) {
    RTL_BITMAP bmph;
    uint32_t i, len, *dwords = data;
    ULONG runlength, index;
    NTSTATUS Status;

    // flip bits
    for (i = 0; i < Vcb->superblock.sectorsize / sizeof(uint32_t); i++) {
        dwords[i] = ~dwords[i];
    }

    len = Vcb->superblock.sectorsize * 8;

    RtlInitializeBitMap(&bmph, data, len);

    index = 0;
    runlength = RtlFindFirstRunClear(&bmph, &index);

    while (runlength != 0) {
        uint64_t addr, length;

        if (index >= len)
            break;

        if (index + runlength >= len) {
            runlength = len - index;

            if (runlength == 0)
                break;
        }

        addr = offset + (index << Vcb->sector_shift);
        length = runlength << Vcb->sector_shift;

        if (addr >= c->offset + c->chunk_item->length || length > c->offset + c->chunk_item->length - addr) {
            WARN("invalidating cache for chunk %I64x: free space at %I64x of %I64x bytes in bitmap at %I64x extends past end of chunk\n", c->offset, addr, length, offset);
            return STATUS_NOT_FOUND;
        }

        Status = add_space_entry(&c->space, &c->space_size, addr, length);
        if (!NT_SUCCESS(Status)) {
            ERR("add_space_entry returned %08lx\n", Status);
            return Status;
        }

        index += runlength;
        *total_space += length;

        runlength = RtlFindNextForwardRunClear(&bmph, index, &index);
    }

    return STATUS_SUCCESS;
}


typedef struct {
    uint64_t stripe;
    LIST_ENTRY list_entry;
} superblock_stripe;

static NTSTATUS add_superblock_stripe(LIST_ENTRY* stripes, uint64_t off, uint64_t len) {
    uint64_t i;

    for (i = 0; i < len; i++) {
        LIST_ENTRY* le;
        superblock_stripe* ss;
        bool ignore = false;

        le = stripes->Flink;
        while (le != stripes) {
            ss = CONTAINING_RECORD(le, superblock_stripe, list_entry);

            if (ss->stripe == off + i) {
                ignore = true;
                break;
            }

            le = le->Flink;
        }

        if (ignore)
            continue;

        ss = ExAllocatePoolWithTag(PagedPool, sizeof(superblock_stripe), ALLOC_TAG);
        if (!ss) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ss->stripe = off + i;
        InsertTailList(stripes, &ss->list_entry);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS get_superblock_size(chunk* c, uint64_t* size) {
    NTSTATUS Status;
    struct btrfs_chunk* ci = c->chunk_item;
    uint64_t off_start, off_end, space = 0;
    uint16_t i = 0, j;
    LIST_ENTRY stripes;

    InitializeListHead(&stripes);

    while (superblock_addrs[i] != 0) {
        if (ci->type & BTRFS_BLOCK_GROUP_RAID0 || ci->type & BTRFS_BLOCK_GROUP_RAID10) {
            for (j = 0; j < ci->num_stripes; j++) {
                ULONG sub_stripes = max(ci->sub_stripes, 1);

                if (c->chunk_item->stripe[j].offset + (ci->length * ci->num_stripes / sub_stripes) > superblock_addrs[i] && c->chunk_item->stripe[j].offset <= superblock_addrs[i] + sizeof(struct btrfs_super_block)) {
                    off_start = superblock_addrs[i] - c->chunk_item->stripe[j].offset;
                    off_start -= off_start % ci->stripe_len;
                    off_start *= ci->num_stripes / sub_stripes;
                    off_start += (j / sub_stripes) * ci->stripe_len;

                    off_end = off_start + ci->stripe_len;

                    Status = add_superblock_stripe(&stripes, off_start / ci->stripe_len, 1);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_superblock_stripe returned %08lx\n", Status);
                        goto end;
                    }
                }
            }
        } else if (ci->type & BTRFS_BLOCK_GROUP_RAID5) {
            for (j = 0; j < ci->num_stripes; j++) {
                uint64_t stripe_size = ci->length / (ci->num_stripes - 1);

                if (c->chunk_item->stripe[j].offset + stripe_size > superblock_addrs[i] && c->chunk_item->stripe[j].offset <= superblock_addrs[i] + sizeof(struct btrfs_super_block)) {
                    off_start = superblock_addrs[i] - c->chunk_item->stripe[j].offset;
                    off_start -= off_start % (ci->stripe_len * (ci->num_stripes - 1));
                    off_start *= ci->num_stripes - 1;

                    off_end = off_start + (ci->stripe_len * (ci->num_stripes - 1));

                    Status = add_superblock_stripe(&stripes, off_start / ci->stripe_len, (off_end - off_start) / ci->stripe_len);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_superblock_stripe returned %08lx\n", Status);
                        goto end;
                    }
                }
            }
        } else if (ci->type & BTRFS_BLOCK_GROUP_RAID6) {
            for (j = 0; j < ci->num_stripes; j++) {
                uint64_t stripe_size = ci->length / (ci->num_stripes - 2);

                if (c->chunk_item->stripe[j].offset + stripe_size > superblock_addrs[i] && c->chunk_item->stripe[j].offset <= superblock_addrs[i] + sizeof(struct btrfs_super_block)) {
                    off_start = superblock_addrs[i] - c->chunk_item->stripe[j].offset;
                    off_start -= off_start % (ci->stripe_len * (ci->num_stripes - 2));
                    off_start *= ci->num_stripes - 2;

                    off_end = off_start + (ci->stripe_len * (ci->num_stripes - 2));

                    Status = add_superblock_stripe(&stripes, off_start / ci->stripe_len, (off_end - off_start) / ci->stripe_len);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_superblock_stripe returned %08lx\n", Status);
                        goto end;
                    }
                }
            }
        } else { // SINGLE, DUPLICATE, RAID1, RAID1C3, RAID1C4
            for (j = 0; j < ci->num_stripes; j++) {
                if (c->chunk_item->stripe[j].offset + ci->length > superblock_addrs[i] && c->chunk_item->stripe[j].offset <= superblock_addrs[i] + sizeof(struct btrfs_super_block)) {
                    off_start = ((superblock_addrs[i] - c->chunk_item->stripe[j].offset) / c->chunk_item->stripe_len) * c->chunk_item->stripe_len;
                    off_end = sector_align(superblock_addrs[i] - c->chunk_item->stripe[j].offset + sizeof(struct btrfs_super_block), c->chunk_item->stripe_len);

                    Status = add_superblock_stripe(&stripes, off_start / ci->stripe_len, (off_end - off_start) / ci->stripe_len);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_superblock_stripe returned %08lx\n", Status);
                        goto end;
                    }
                }
            }
        }

        i++;
    }

    Status = STATUS_SUCCESS;

end:
    while (!IsListEmpty(&stripes)) {
        LIST_ENTRY* le = RemoveHeadList(&stripes);
        superblock_stripe* ss = CONTAINING_RECORD(le, superblock_stripe, list_entry);

        space++;

        ExFreePool(ss);
    }

    if (NT_SUCCESS(Status))
        *size = space * ci->stripe_len;

    return Status;
}

NTSTATUS load_stored_free_space_cache(device_extension* Vcb, chunk* c, bool load_only, PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    struct btrfs_free_space_header* fsi;
    uint64_t inode, *generation;
    uint8_t* data;
    NTSTATUS Status;
    uint32_t *checksums, crc32, num_sectors, num_valid_sectors, size;
    struct btrfs_free_space_entry* fse;
    uint64_t num_entries, num_bitmaps, extent_length, bmpnum, off, total_space = 0, superblock_size;
    uint64_t bitmap_entries = 0;
    LIST_ENTRY *le, rollback;

    // FIXME - does this break if Vcb->superblock.sectorsize is not 4096?

    TRACE("(%p, %I64x)\n", Vcb, c->offset);

    searchkey.objectid = BTRFS_FREE_SPACE_OBJECTID;
    searchkey.type = 0;
    searchkey.offset = c->offset;

    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return Status;
    }

    if (keycmp(tp.item->key, searchkey)) {
        TRACE("(%I64x,%x,%I64x) not found\n", searchkey.objectid, searchkey.type, searchkey.offset);
        return STATUS_NOT_FOUND;
    }

    if (tp.item->size < sizeof(struct btrfs_free_space_header)) {
        WARN("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_free_space_header));
        return STATUS_NOT_FOUND;
    }

    fsi = (struct btrfs_free_space_header*)tp.item->data;

    if (fsi->location.type != BTRFS_INODE_ITEM_KEY) {
        WARN("cache pointed to something other than an INODE_ITEM\n");
        return STATUS_NOT_FOUND;
    }

    inode = fsi->location.objectid;
    num_entries = fsi->num_entries;
    num_bitmaps = fsi->num_bitmaps;

    Status = open_fcb(Vcb, Vcb->root_root, inode, BTRFS_FT_REG_FILE, NULL, false, NULL, &c->cache, PagedPool, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("open_fcb returned %08lx\n", Status);
        return STATUS_NOT_FOUND;
    }

    if (load_only)
        return STATUS_SUCCESS;

    if (c->cache->inode_item.size == 0) {
        WARN("cache had zero length\n");
        free_fcb(c->cache);
        c->cache = NULL;
        return STATUS_NOT_FOUND;
    }

    c->cache->inode_item.flags |= BTRFS_INODE_NODATACOW;

    if (num_entries == 0 && num_bitmaps == 0)
        return STATUS_SUCCESS;

    size = (uint32_t)sector_align(c->cache->inode_item.size, Vcb->superblock.sectorsize);

    data = ExAllocatePoolWithTag(PagedPool, size, ALLOC_TAG);

    if (!data) {
        ERR("out of memory\n");
        free_fcb(c->cache);
        c->cache = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (c->chunk_item->length < 0x6400000) { // 100 MB
        WARN("deleting free space cache for chunk smaller than 100MB\n");
        goto clearcache;
    }

    Status = read_file(c->cache, data, 0, c->cache->inode_item.size, NULL, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("read_file returned %08lx\n", Status);
        goto clearcache;
    }

    if (size > c->cache->inode_item.size)
        RtlZeroMemory(&data[c->cache->inode_item.size], (ULONG)(size - c->cache->inode_item.size));

    num_sectors = size >> Vcb->sector_shift;

    generation = (uint64_t*)(data + (num_sectors * sizeof(uint32_t)));

    if (*generation != fsi->generation) {
        WARN("free space cache generation for %I64x was %I64x, expected %I64x\n", c->offset, *generation, fsi->generation);
        goto clearcache;
    }

    extent_length = (num_sectors * sizeof(uint32_t)) + sizeof(uint64_t) + (num_entries * sizeof(struct btrfs_free_space_entry));

    num_valid_sectors = (ULONG)((sector_align(extent_length, Vcb->superblock.sectorsize) >> Vcb->sector_shift) + num_bitmaps);

    if (num_valid_sectors > num_sectors) {
        ERR("free space cache for %I64x was %u sectors, expected at least %u\n", c->offset, num_sectors, num_valid_sectors);
        goto clearcache;
    }

    checksums = (uint32_t*)data;

    for (uint32_t i = 0; i < num_valid_sectors; i++) {
        if (i << Vcb->sector_shift > sizeof(uint32_t) * num_sectors)
            crc32 = ~calc_crc32c(0xffffffff, &data[i << Vcb->sector_shift], Vcb->superblock.sectorsize);
        else if ((i + 1) << Vcb->sector_shift < sizeof(uint32_t) * num_sectors)
            crc32 = 0; // FIXME - test this
        else
            crc32 = ~calc_crc32c(0xffffffff, &data[sizeof(uint32_t) * num_sectors], ((i + 1) << Vcb->sector_shift) - (sizeof(uint32_t) * num_sectors));

        if (crc32 != checksums[i]) {
            WARN("checksum %u was %08x, expected %08x\n", i, crc32, checksums[i]);
            goto clearcache;
        }
    }

    off = (sizeof(uint32_t) * num_sectors) + sizeof(uint64_t);

    bmpnum = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        if ((off + sizeof(struct btrfs_free_space_entry)) >> Vcb->sector_shift != off >> Vcb->sector_shift)
            off = sector_align(off, Vcb->superblock.sectorsize);

        if (off > size || sizeof(*fse) > size - off) {
            WARN("free space cache entry was outside the buffer\n");
            goto clearcache;
        }

        fse = (struct btrfs_free_space_entry*)&data[off];

        if (fse->type == BTRFS_FREE_SPACE_EXTENT) {
            if (fse->bytes == 0 || fse->offset < c->offset || fse->offset >= c->offset + c->chunk_item->length ||
                fse->bytes > c->offset + c->chunk_item->length - fse->offset) {
                WARN("invalidating cache for chunk %I64x: entry at %I64x of %I64x bytes is empty or outside chunk\n", c->offset, fse->offset, fse->bytes);
                goto clearcache;
            }

            Status = add_space_entry(&c->space, &c->space_size, fse->offset, fse->bytes);
            if (!NT_SUCCESS(Status)) {
                ERR("add_space_entry returned %08lx\n", Status);
                ExFreePool(data);
                goto end;
            }

            total_space += fse->bytes;
        } else if (fse->type == BTRFS_FREE_SPACE_BITMAP) {
            if (++bitmap_entries > num_bitmaps) {
                WARN("free space cache had more bitmap entries than its header\n");
                goto clearcache;
            }
        } else {
            ERR("unknown free-space type %x\n", fse->type);
        }

        off += sizeof(struct btrfs_free_space_entry);
    }

    if (bitmap_entries != num_bitmaps) {
        WARN("free space cache had fewer bitmap entries than its header\n");
        goto clearcache;
    }

    if (num_bitmaps > 0) {
        bmpnum = sector_align(off, Vcb->superblock.sectorsize) >> Vcb->sector_shift;
        off = (sizeof(uint32_t) * num_sectors) + sizeof(uint64_t);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((off + sizeof(struct btrfs_free_space_entry)) >> Vcb->sector_shift != off >> Vcb->sector_shift)
                off = sector_align(off, Vcb->superblock.sectorsize);

            if (off > size || sizeof(*fse) > size - off) {
                WARN("free space cache entry was outside the buffer\n");
                goto clearcache;
            }

            fse = (struct btrfs_free_space_entry*)&data[off];

            if (fse->type == BTRFS_FREE_SPACE_BITMAP) {
                if (fse->offset < c->offset || fse->offset >= c->offset + c->chunk_item->length) {
                    WARN("invalidating cache for chunk %I64x: bitmap at %I64x is outside chunk\n", c->offset, fse->offset);
                    goto clearcache;
                }

                if (bmpnum >= num_sectors) {
                    WARN("free space cache bitmap was outside the buffer\n");
                    goto clearcache;
                }

                Status = load_free_space_bitmap(Vcb, c, fse->offset, &data[bmpnum << Vcb->sector_shift], &total_space);
                if (Status == STATUS_NOT_FOUND)
                    goto clearcache;
                else if (!NT_SUCCESS(Status)) {
                    ERR("load_free_space_bitmap returned %08lx\n", Status);
                    ExFreePool(data);
                    goto end;
                }

                bmpnum++;
            }

            off += sizeof(struct btrfs_free_space_entry);
        }
    }

    // do sanity check

    Status = get_superblock_size(c, &superblock_size);
    if (!NT_SUCCESS(Status)) {
        ERR("get_superblock_size returned %08lx\n", Status);
        ExFreePool(data);
        goto end;
    }

    if (c->chunk_item->length - c->used != total_space + superblock_size) {
        WARN("invalidating cache for chunk %I64x: space was %I64x, expected %I64x\n", c->offset, total_space + superblock_size, c->chunk_item->length - c->used);
        goto clearcache;
    }

    le = c->space.Flink;
    while (le != &c->space) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        LIST_ENTRY* le2 = le->Flink;

        if (le2 != &c->space) {
            space* s2 = CONTAINING_RECORD(le2, space, list_entry);

            if (s2->address < s->address + s->size) {
                WARN("invalidating cache for chunk %I64x: entry at %I64x overlaps entry at %I64x\n", c->offset, s2->address, s->address);
                goto clearcache;
            }

            if (s2->address == s->address + s->size) {
                s->size += s2->size;

                RemoveEntryList(&s2->list_entry);
                RemoveEntryList(&s2->list_entry_size);
                ExFreePool(s2);

                RemoveEntryList(&s->list_entry_size);
                order_space_entry(s, &c->space_size);

                le2 = le;
            }
        }

        le = le2;
    }

    ExFreePool(data);

    return STATUS_SUCCESS;

clearcache:
    ExFreePool(data);

    InitializeListHead(&rollback);

    // Excise the extents before deleting the header item. No load finds a cache file
    // whose header item is gone, so if excising failed after that, nothing would
    // delete the file. A failed excise is rolled back instead, and the next load
    // discards the cache again.

    Status = excise_extents(Vcb, c->cache, 0, c->cache->inode_item.size, Irp, &rollback);
    if (!NT_SUCCESS(Status)) {
        ERR("excise_extents returned %08lx\n", Status);
        do_rollback(Vcb, &rollback);
        goto end;
    }

    Status = delete_tree_item(Vcb, &tp);
    if (!NT_SUCCESS(Status)) {
        ERR("delete_tree_item returned %08lx\n", Status);
        do_rollback(Vcb, &rollback);
        goto end;
    }

    clear_rollback(&rollback);

    c->cache->deleted = true;
    mark_fcb_dirty(c->cache);

    c->old_cache = c->cache;
    c->cache = NULL;

    Status = STATUS_NOT_FOUND;

end:
    // Nothing loaded from the cache may stay, and the cache file is released unless
    // it became the chunk's old cache. After STATUS_NOT_FOUND the caller regenerates
    // the chunk's free space from the extent tree, and after an error the chunk is
    // loaded again.

    if (c->cache) {
        free_fcb(c->cache);
        c->cache = NULL;
    }

    le = c->space.Flink;
    while (le != &c->space) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        LIST_ENTRY* le2 = le->Flink;

        RemoveEntryList(&s->list_entry);
        RemoveEntryList(&s->list_entry_size);
        ExFreePool(s);

        le = le2;
    }

    return Status;
}

static NTSTATUS load_stored_free_space_tree(device_extension* Vcb, chunk* c, PIRP Irp) {
    struct btrfs_key searchkey;
    struct btrfs_free_space_info* fsi;
    traverse_ptr tp, next_tp;
    NTSTATUS Status;
    ULONG* bmparr = NULL;
    ULONG bmplen = 0;
    LIST_ENTRY* le;

    TRACE("(%p, %I64x)\n", Vcb, c->offset);

    if (!Vcb->space_root)
        return STATUS_NOT_FOUND;

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_FREE_SPACE_INFO_KEY;
    searchkey.offset = c->chunk_item->length;

    Status = find_item(Vcb, Vcb->space_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (keycmp(tp.item->key, searchkey)) {
        TRACE("(%I64x,%x,%I64x) not found\n", searchkey.objectid, searchkey.type, searchkey.offset);
        return STATUS_NOT_FOUND;
    }

    if (tp.item->size < sizeof(struct btrfs_free_space_info)) {
        WARN("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_free_space_info));
        return STATUS_NOT_FOUND;
    }

    fsi = (struct btrfs_free_space_info*)tp.item->data;

    if (fsi->flags & BTRFS_FREE_SPACE_USING_BITMAPS)
        c->using_fst_bitmaps = true;

    while (find_next_item(Vcb, &tp, &next_tp, false, Irp)) {
        tp = next_tp;

        if (tp.item->key.objectid >= c->offset + c->chunk_item->length)
            break;

        // The search started at the chunk's info item, so the item starts inside the chunk.
        if ((tp.item->key.type == BTRFS_FREE_SPACE_EXTENT_KEY || tp.item->key.type == BTRFS_FREE_SPACE_BITMAP_KEY) &&
            (tp.item->key.offset == 0 || tp.item->key.offset > c->offset + c->chunk_item->length - tp.item->key.objectid)) {
            WARN("(%I64x,%x,%I64x) is empty or extends past end of chunk %I64x\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, c->offset);
            goto clearcache;
        }

        if (tp.item->key.type == BTRFS_FREE_SPACE_EXTENT_KEY) {
            Status = add_space_entry(&c->space, &c->space_size, tp.item->key.objectid, tp.item->key.offset);
            if (!NT_SUCCESS(Status)) {
                ERR("add_space_entry returned %08lx\n", Status);
                goto end;
            }
        } else if (tp.item->key.type == BTRFS_FREE_SPACE_BITMAP_KEY) {
            ULONG explen, index, runlength;
            RTL_BITMAP bmp;
            uint64_t lastoff;
            ULONG bmpl;

            explen = (ULONG)sector_align(tp.item->key.offset >> Vcb->sector_shift, 8) / 8;

            if (tp.item->size < explen) {
                WARN("(%I64x,%x,%I64x) was %u bytes, expected %lu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, explen);
                goto clearcache;
            } else if (tp.item->size == 0) {
                WARN("(%I64x,%x,%I64x) has size of 0\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset);
                goto clearcache;
            }

            if (bmplen < tp.item->size) {
                if (bmparr)
                    ExFreePool(bmparr);

                bmplen = (ULONG)sector_align(tp.item->size, sizeof(ULONG));
                bmparr = ExAllocatePoolWithTag(PagedPool, bmplen, ALLOC_TAG);
                if (!bmparr) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }
            }

            // We copy the bitmap because it supposedly has to be ULONG-aligned
            RtlCopyMemory(bmparr, tp.item->data, tp.item->size);

            bmpl = (ULONG)tp.item->key.offset >> Vcb->sector_shift;

            RtlInitializeBitMap(&bmp, bmparr, bmpl);

            lastoff = tp.item->key.objectid;

            runlength = RtlFindFirstRunClear(&bmp, &index);

            while (runlength != 0) {
                uint64_t runstart, runend;

                if (index >= bmpl)
                    break;

                if (index + runlength >= bmpl) {
                    runlength = bmpl - index;

                    if (runlength == 0)
                        break;
                }

                runstart = tp.item->key.objectid + (index << Vcb->sector_shift);
                runend = runstart + (runlength << Vcb->sector_shift);

                if (runstart > lastoff) {
                    Status = add_space_entry(&c->space, &c->space_size, lastoff, runstart - lastoff);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_space_entry returned %08lx\n", Status);
                        goto end;
                    }
                }

                lastoff = runend;

                runlength = RtlFindNextForwardRunClear(&bmp, index + runlength, &index);
            }

            if (lastoff < tp.item->key.objectid + tp.item->key.offset) {
                Status = add_space_entry(&c->space, &c->space_size, lastoff, tp.item->key.objectid + tp.item->key.offset - lastoff);
                if (!NT_SUCCESS(Status)) {
                    ERR("add_space_entry returned %08lx\n", Status);
                    goto end;
                }
            }
        }
    }

    le = c->space.Flink;
    while (le != &c->space) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        LIST_ENTRY* le2 = le->Flink;

        if (le2 != &c->space) {
            space* s2 = CONTAINING_RECORD(le2, space, list_entry);

            if (s2->address < s->address + s->size) {
                WARN("free space tree for chunk %I64x: entry at %I64x overlaps entry at %I64x\n", c->offset, s2->address, s->address);
                goto clearcache;
            }

            if (s2->address == s->address + s->size) {
                s->size += s2->size;

                RemoveEntryList(&s2->list_entry);
                RemoveEntryList(&s2->list_entry_size);
                ExFreePool(s2);

                RemoveEntryList(&s->list_entry_size);
                order_space_entry(s, &c->space_size);

                le2 = le;
            }
        }

        le = le2;
    }

    if (bmparr)
        ExFreePool(bmparr);

    return STATUS_SUCCESS;

clearcache:
    Status = STATUS_NOT_FOUND;

end:
    // Nothing loaded from the free space tree may stay. After STATUS_NOT_FOUND the
    // caller regenerates the chunk's free space from the extent tree, and after an
    // error the chunk is loaded again.

    if (bmparr)
        ExFreePool(bmparr);

    while (!IsListEmpty(&c->space)) {
        space* s = CONTAINING_RECORD(RemoveHeadList(&c->space), space, list_entry);

        RemoveEntryList(&s->list_entry_size);
        ExFreePool(s);
    }

    return Status;
}

static NTSTATUS load_free_space_cache(device_extension* Vcb, chunk* c, PIRP Irp) {
    traverse_ptr tp, next_tp;
    struct btrfs_key searchkey;
    uint64_t lastaddr;
    bool b;
    space* s;
    NTSTATUS Status;

    if (Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE && Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID) {
        Status = load_stored_free_space_tree(Vcb, c, Irp);

        if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
            ERR("load_stored_free_space_tree returned %08lx\n", Status);
            return Status;
        }
    } else if (Vcb->superblock.generation - 1 == Vcb->superblock.cache_generation) {
        Status = load_stored_free_space_cache(Vcb, c, false, Irp);

        if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
            ERR("load_stored_free_space_cache returned %08lx\n", Status);
            return Status;
        }
    } else
        Status = STATUS_NOT_FOUND;

    if (Status == STATUS_NOT_FOUND) {
        TRACE("generating free space cache for chunk %I64x\n", c->offset);

        searchkey.objectid = c->offset;
        searchkey.type = BTRFS_EXTENT_ITEM_KEY;
        searchkey.offset = 0;

        Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        lastaddr = c->offset;

        do {
            if (tp.item->key.objectid >= c->offset + c->chunk_item->length)
                break;

            if (tp.item->key.objectid >= c->offset && (tp.item->key.type == BTRFS_EXTENT_ITEM_KEY || tp.item->key.type == BTRFS_METADATA_ITEM_KEY)) {
                if (tp.item->key.objectid > lastaddr) {
                    s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);

                    if (!s) {
                        ERR("out of memory\n");

                        while (!IsListEmpty(&c->space)) {
                            space* s2 = CONTAINING_RECORD(RemoveHeadList(&c->space), space, list_entry);
                            ExFreePool(s2);
                        }

                        InitializeListHead(&c->space_size);

                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    s->address = lastaddr;
                    s->size = tp.item->key.objectid - lastaddr;
                    InsertTailList(&c->space, &s->list_entry);

                    order_space_entry(s, &c->space_size);

                    TRACE("(%I64x,%I64x)\n", s->address, s->size);
                }

                if (tp.item->key.type == BTRFS_METADATA_ITEM_KEY)
                    lastaddr = tp.item->key.objectid + Vcb->superblock.nodesize;
                else
                    lastaddr = tp.item->key.objectid + tp.item->key.offset;
            }

            b = find_next_item(Vcb, &tp, &next_tp, false, Irp);
            if (b)
                tp = next_tp;
        } while (b);

        if (lastaddr < c->offset + c->chunk_item->length) {
            s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);

            if (!s) {
                ERR("out of memory\n");

                while (!IsListEmpty(&c->space)) {
                    space* s2 = CONTAINING_RECORD(RemoveHeadList(&c->space), space, list_entry);
                    ExFreePool(s2);
                }

                InitializeListHead(&c->space_size);

                return STATUS_INSUFFICIENT_RESOURCES;
            }

            s->address = lastaddr;
            s->size = c->offset + c->chunk_item->length - lastaddr;
            InsertTailList(&c->space, &s->list_entry);

            order_space_entry(s, &c->space_size);

            TRACE("(%I64x,%I64x)\n", s->address, s->size);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS load_cache_chunk(device_extension* Vcb, chunk* c, PIRP Irp) {
    NTSTATUS Status;
    bool space_changed;
    fcb* cache;

    if (c->cache_loaded)
        return STATUS_SUCCESS;

    cache = c->cache;

    Status = load_free_space_cache(Vcb, c, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("load_free_space_cache returned %08lx\n", Status);
        return Status;
    }

    space_changed = c->space_changed;

    Status = protect_superblocks(c);
    if (!NT_SUCCESS(Status)) {
        // The next call loads the chunk again, so free what was loaded, and release
        // the v1 cache file if this load opened it, as the next call opens it again.
        // Through space_list_subtract, protect_superblocks has set space_changed,
        // and the flush would write the empty lists out as the chunk's free space,
        // so restore it too. changed is left set: nocow writes set it without the
        // chunk lock, and it only makes the flush write out partial stripes.

        while (!IsListEmpty(&c->space)) {
            space* s = CONTAINING_RECORD(RemoveHeadList(&c->space), space, list_entry);

            RemoveEntryList(&s->list_entry_size);
            ExFreePool(s);
        }

        if (c->cache && c->cache != cache) {
            free_fcb(c->cache);
            c->cache = cache;
        }

        c->space_changed = space_changed;

        return Status;
    }

    c->cache_loaded = true;

    return STATUS_SUCCESS;
}




NTSTATUS space_list_add(chunk* c, uint64_t address, uint64_t length,
                        LIST_ENTRY* rollback) {
    TRACE("(%p, %I64x, %I64x, %p)\n", c, address, length, rollback);

    c->changed = true;
    c->space_changed = true;

    return space_list_add2(&c->deleting, NULL, address, length, c, rollback);
}


NTSTATUS space_list_subtract(chunk* c, uint64_t address, uint64_t length, LIST_ENTRY* rollback) {
    NTSTATUS Status;

    c->changed = true;
    c->space_changed = true;

    Status = space_list_subtract2(&c->space, &c->space_size, address, length,
                                  c, rollback);
    if (!NT_SUCCESS(Status))
        return Status;

    return space_list_subtract2(&c->deleting, NULL, address, length, c,
                                rollback);
}
