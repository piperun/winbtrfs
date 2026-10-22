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

// Relocate one data chunk and update its cached extent references.
#include "balance-relocation.h"
#include "btrfsioctl.h"
#include "crc32c.h"
#include <ntddstor.h>

NTSTATUS balance_data_chunk(device_extension* Vcb, chunk* c, bool* changed) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    bool b;
    LIST_ENTRY items, metadata_items, rollback, *le;
    uint64_t loaded = 0, num_loaded = 0;
    chunk* newchunk = NULL;
    uint8_t* data = NULL;

    TRACE("chunk %I64x\n", c->offset);

    InitializeListHead(&rollback);
    InitializeListHead(&items);
    InitializeListHead(&metadata_items);

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    searchkey.objectid = c->offset;
    searchkey.type = BTRFS_EXTENT_ITEM_KEY;
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

        if (tp.item->key.objectid >= c->offset && tp.item->key.type == BTRFS_EXTENT_ITEM_KEY) {
            bool tree = false;

            if (tp.item->key.type == BTRFS_EXTENT_ITEM_KEY && tp.item->size >= sizeof(struct btrfs_extent_item)) {
                struct btrfs_extent_item* ei = (struct btrfs_extent_item*)tp.item->data;

                if (ei->flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
                    tree = true;
            }

            if (!tree) {
                Status = add_data_reloc(Vcb, &items, &metadata_items, &tp, c, &rollback);

                if (!NT_SUCCESS(Status)) {
                    ERR("add_data_reloc returned %08lx\n", Status);
                    goto end;
                }

                loaded += tp.item->key.offset;
                num_loaded++;

                if (loaded >= 0x1000000 || num_loaded >= 100) // only do so much at a time, so we don't block too obnoxiously
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

    data = ExAllocatePoolWithTag(PagedPool, BALANCE_UNIT, ALLOC_TAG);
    if (!data) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    le = items.Flink;
    while (le != &items) {
        data_reloc* dr = CONTAINING_RECORD(le, data_reloc, list_entry);
        bool done = false;
        LIST_ENTRY* le2;
        void* csum;
        RTL_BITMAP bmp;
        ULONG* bmparr;
        ULONG bmplen, runlength, index, lastoff;

        if (newchunk) {
            acquire_chunk_lock(newchunk, Vcb);

            if (find_data_address_in_chunk(Vcb, newchunk, dr->size, &dr->new_address)) {
                newchunk->used += dr->size;

                Status = space_list_subtract(newchunk, dr->new_address,
                                             dr->size, &rollback);
                if (!NT_SUCCESS(Status)) {
                    release_chunk_lock(newchunk, Vcb);
                    goto end;
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

                if (!c2->readonly && !c2->reloc && c2 != newchunk && c2->chunk_item->type == Vcb->data_flags) {
                    acquire_chunk_lock(c2, Vcb);

                    if ((c2->chunk_item->length - c2->used) >= dr->size) {
                        if (find_data_address_in_chunk(Vcb, c2, dr->size, &dr->new_address)) {
                            c2->used += dr->size;

                            Status = space_list_subtract(c2, dr->new_address,
                                                         dr->size, &rollback);
                            release_chunk_lock(c2, Vcb);

                            if (!NT_SUCCESS(Status)) {
                                ExReleaseResourceLite(&Vcb->chunk_lock);
                                goto end;
                            }

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
                Status = alloc_chunk(Vcb, Vcb->data_flags, &newchunk, false);

                if (!NT_SUCCESS(Status)) {
                    ERR("alloc_chunk returned %08lx\n", Status);
                    ExReleaseResourceLite(&Vcb->chunk_lock);
                    goto end;
                }

                acquire_chunk_lock(newchunk, Vcb);

                newchunk->balance_num = Vcb->balance.balance_num;

                if (!find_data_address_in_chunk(Vcb, newchunk, dr->size, &dr->new_address)) {
                    release_chunk_lock(newchunk, Vcb);
                    ExReleaseResourceLite(&Vcb->chunk_lock);
                    ERR("could not find address in new chunk\n");
                    Status = STATUS_DISK_FULL;
                    goto end;
                } else {
                    newchunk->used += dr->size;

                    Status = space_list_subtract(newchunk, dr->new_address,
                                                 dr->size, &rollback);
                    if (!NT_SUCCESS(Status)) {
                        release_chunk_lock(newchunk, Vcb);
                        goto end;
                    }
                }

                release_chunk_lock(newchunk, Vcb);
            }

            ExReleaseResourceLite(&Vcb->chunk_lock);
        }

        dr->newchunk = newchunk;

        bmplen = (ULONG)(dr->size >> Vcb->sector_shift);

        bmparr = ExAllocatePoolWithTag(PagedPool, (ULONG)sector_align(bmplen + 1, sizeof(ULONG)), ALLOC_TAG);
        if (!bmparr) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        csum = ExAllocatePoolWithTag(PagedPool, (ULONG)((dr->size * Vcb->csum_size) >> Vcb->sector_shift), ALLOC_TAG);
        if (!csum) {
            ERR("out of memory\n");
            ExFreePool(bmparr);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        RtlInitializeBitMap(&bmp, bmparr, bmplen);
        RtlSetAllBits(&bmp); // 1 = no csum, 0 = csum

        searchkey.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
        searchkey.type = BTRFS_EXTENT_CSUM_KEY;
        searchkey.offset = dr->address;

        Status = find_item(Vcb, Vcb->checksum_root, &tp, &searchkey, false, NULL);
        if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
            ERR("find_item returned %08lx\n", Status);
            ExFreePool(csum);
            ExFreePool(bmparr);
            goto end;
        }

        if (Status != STATUS_NOT_FOUND) {
            do {
                traverse_ptr next_tp;

                if (tp.item->key.type == BTRFS_EXTENT_CSUM_KEY) {
                    if (tp.item->key.offset >= dr->address + dr->size)
                        break;
                    else if (tp.item->size >= Vcb->csum_size && tp.item->key.offset + (((unsigned int)tp.item->size << Vcb->sector_shift) / Vcb->csum_size) >= dr->address) {
                        uint64_t cs = max(dr->address, tp.item->key.offset);
                        uint64_t ce = min(dr->address + dr->size, tp.item->key.offset + (((unsigned int)tp.item->size << Vcb->sector_shift) / Vcb->csum_size));

                        RtlCopyMemory((uint8_t*)csum + (((cs - dr->address) * Vcb->csum_size) >> Vcb->sector_shift),
                                      tp.item->data + (((cs - tp.item->key.offset) * Vcb->csum_size) >> Vcb->sector_shift),
                                      (ULONG)(((ce - cs) * Vcb->csum_size) >> Vcb->sector_shift));

                        RtlClearBits(&bmp, (ULONG)((cs - dr->address) >> Vcb->sector_shift), (ULONG)((ce - cs) >> Vcb->sector_shift));

                        if (ce == dr->address + dr->size)
                            break;
                    }
                }

                if (find_next_item(Vcb, &tp, &next_tp, false, NULL))
                    tp = next_tp;
                else
                    break;
            } while (true);
        }

        lastoff = 0;
        runlength = RtlFindFirstRunClear(&bmp, &index);

        while (runlength != 0) {
            if (index >= bmplen)
                break;

            if (index + runlength >= bmplen) {
                runlength = bmplen - index;

                if (runlength == 0)
                    break;
            }

            if (index > lastoff) {
                ULONG off = lastoff;
                ULONG size = index - lastoff;

                // handle no csum run
                do {
                    ULONG rl;

                    if (size << Vcb->sector_shift > BALANCE_UNIT)
                        rl = BALANCE_UNIT >> Vcb->sector_shift;
                    else
                        rl = size;

                    Status = read_data(Vcb, dr->address + (off << Vcb->sector_shift), rl << Vcb->sector_shift, NULL, false, data,
                                       c, NULL, NULL, 0, false, NormalPagePriority);
                    if (!NT_SUCCESS(Status)) {
                        ERR("read_data returned %08lx\n", Status);
                        ExFreePool(csum);
                        ExFreePool(bmparr);
                        goto end;
                    }

                    Status = write_data_complete(Vcb, dr->new_address + (off << Vcb->sector_shift), data, rl << Vcb->sector_shift,
                                                 NULL, newchunk, false, 0, NormalPagePriority);
                    if (!NT_SUCCESS(Status)) {
                        ERR("write_data_complete returned %08lx\n", Status);
                        ExFreePool(csum);
                        ExFreePool(bmparr);
                        goto end;
                    }

                    size -= rl;
                    off += rl;
                } while (size > 0);
            }

            Status = add_checksum_entry(Vcb, dr->new_address + (index << Vcb->sector_shift),
                                        runlength, (uint8_t*)csum + (index * Vcb->csum_size),
                                        NULL);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(csum);
                ExFreePool(bmparr);
                goto end;
            }

            Status = add_checksum_entry(Vcb, dr->address + (index << Vcb->sector_shift),
                                        runlength, NULL, NULL);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(csum);
                ExFreePool(bmparr);
                goto end;
            }

            // handle csum run
            do {
                ULONG rl;

                if (runlength << Vcb->sector_shift > BALANCE_UNIT)
                    rl = BALANCE_UNIT >> Vcb->sector_shift;
                else
                    rl = runlength;

                Status = read_data(Vcb, dr->address + (index << Vcb->sector_shift), rl << Vcb->sector_shift,
                                   (uint8_t*)csum + (index * Vcb->csum_size), false, data, c, NULL, NULL, 0, false, NormalPagePriority);
                if (!NT_SUCCESS(Status)) {
                    ERR("read_data returned %08lx\n", Status);
                    ExFreePool(csum);
                    ExFreePool(bmparr);
                    goto end;
                }

                Status = write_data_complete(Vcb, dr->new_address + (index << Vcb->sector_shift), data, rl << Vcb->sector_shift,
                                             NULL, newchunk, false, 0, NormalPagePriority);
                if (!NT_SUCCESS(Status)) {
                    ERR("write_data_complete returned %08lx\n", Status);
                    ExFreePool(csum);
                    ExFreePool(bmparr);
                    goto end;
                }

                runlength -= rl;
                index += rl;
            } while (runlength > 0);

            lastoff = index;
            runlength = RtlFindNextForwardRunClear(&bmp, index, &index);
        }

        ExFreePool(csum);
        ExFreePool(bmparr);

        // handle final nocsum run
        if (lastoff < dr->size >> Vcb->sector_shift) {
            ULONG off = lastoff;
            ULONG size = (ULONG)((dr->size >> Vcb->sector_shift) - lastoff);

            do {
                ULONG rl;

                if (size << Vcb->sector_shift > BALANCE_UNIT)
                    rl = BALANCE_UNIT >> Vcb->sector_shift;
                else
                    rl = size;

                Status = read_data(Vcb, dr->address + (off << Vcb->sector_shift), rl << Vcb->sector_shift, NULL, false, data,
                                   c, NULL, NULL, 0, false, NormalPagePriority);
                if (!NT_SUCCESS(Status)) {
                    ERR("read_data returned %08lx\n", Status);
                    goto end;
                }

                Status = write_data_complete(Vcb, dr->new_address + (off << Vcb->sector_shift), data, rl << Vcb->sector_shift,
                                             NULL, newchunk, false, 0, NormalPagePriority);
                if (!NT_SUCCESS(Status)) {
                    ERR("write_data_complete returned %08lx\n", Status);
                    goto end;
                }

                size -= rl;
                off += rl;
            } while (size > 0);
        }

        le = le->Flink;
    }

    ExFreePool(data);
    data = NULL;

    Status = write_metadata_items(Vcb, &metadata_items, &items, NULL, &rollback);
    if (!NT_SUCCESS(Status)) {
        ERR("write_metadata_items returned %08lx\n", Status);
        goto end;
    }

    le = items.Flink;
    while (le != &items) {
        data_reloc* dr = CONTAINING_RECORD(le, data_reloc, list_entry);

        Status = add_data_reloc_extent_item(Vcb, dr);
        if (!NT_SUCCESS(Status)) {
            ERR("add_data_reloc_extent_item returned %08lx\n", Status);
            goto end;
        }

        le = le->Flink;
    }

    le = c->changed_extents.Flink;
    while (le != &c->changed_extents) {
        LIST_ENTRY *le2, *le3;
        changed_extent* ce = CONTAINING_RECORD(le, changed_extent, list_entry);

        le3 = le->Flink;

        le2 = items.Flink;
        while (le2 != &items) {
            data_reloc* dr = CONTAINING_RECORD(le2, data_reloc, list_entry);

            if (ce->address == dr->address) {
                ce->address = dr->new_address;
                RemoveEntryList(&ce->list_entry);
                InsertTailList(&dr->newchunk->changed_extents, &ce->list_entry);
                break;
            }

            le2 = le2->Flink;
        }

        le = le3;
    }

    Status = STATUS_SUCCESS;

    Vcb->need_write = true;

end:
    if (NT_SUCCESS(Status)) {
        // update extents in cache inodes before we flush
        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c2 = CONTAINING_RECORD(le, chunk, list_entry);

            if (c2->cache) {
                LIST_ENTRY* le2;

                ExAcquireResourceExclusiveLite(c2->cache->Header.Resource, true);

                le2 = c2->cache->extents.Flink;
                while (le2 != &c2->cache->extents) {
                    extent* ext = CONTAINING_RECORD(le2, extent, list_entry);

                    if (!ext->ignore) {
                        if (ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC) {
                            if (ext->extent_data.disk_num_bytes > 0 && ext->extent_data.disk_bytenr >= c->offset && ext->extent_data.disk_bytenr < c->offset + c->chunk_item->length) {
                                LIST_ENTRY* le3 = items.Flink;
                                while (le3 != &items) {
                                    data_reloc* dr = CONTAINING_RECORD(le3, data_reloc, list_entry);

                                    if (ext->extent_data.disk_bytenr == dr->address) {
                                        ext->extent_data.disk_bytenr = dr->new_address;
                                        break;
                                    }

                                    le3 = le3->Flink;
                                }
                            }
                        }
                    }

                    le2 = le2->Flink;
                }

                ExReleaseResourceLite(c2->cache->Header.Resource);
            }

            le = le->Flink;
        }

        Status = do_write(Vcb, NULL);
        if (!NT_SUCCESS(Status))
            ERR("do_write returned %08lx\n", Status);
    }

    if (NT_SUCCESS(Status)) {
        clear_rollback(&rollback);

        // update open FCBs
        // FIXME - speed this up(?)

        le = Vcb->all_fcbs.Flink;
        while (le != &Vcb->all_fcbs) {
            struct _fcb* fcb = CONTAINING_RECORD(le, struct _fcb, list_entry_all);
            LIST_ENTRY* le2;

            ExAcquireResourceExclusiveLite(fcb->Header.Resource, true);

            le2 = fcb->extents.Flink;
            while (le2 != &fcb->extents) {
                extent* ext = CONTAINING_RECORD(le2, extent, list_entry);

                if (!ext->ignore) {
                    if (ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC) {
                        if (ext->extent_data.disk_num_bytes > 0 && ext->extent_data.disk_bytenr >= c->offset && ext->extent_data.disk_bytenr < c->offset + c->chunk_item->length) {
                            LIST_ENTRY* le3 = items.Flink;
                            while (le3 != &items) {
                                data_reloc* dr = CONTAINING_RECORD(le3, data_reloc, list_entry);

                                if (ext->extent_data.disk_bytenr == dr->address) {
                                    ext->extent_data.disk_bytenr = dr->new_address;
                                    break;
                                }

                                le3 = le3->Flink;
                            }
                        }
                    }
                }

                le2 = le2->Flink;
            }

            ExReleaseResourceLite(fcb->Header.Resource);

            le = le->Flink;
        }
    } else
        do_rollback(Vcb, &rollback);

    free_trees(Vcb);

    ExReleaseResourceLite(&Vcb->tree_lock);

    if (data)
        ExFreePool(data);

    while (!IsListEmpty(&items)) {
        data_reloc* dr = CONTAINING_RECORD(RemoveHeadList(&items), data_reloc, list_entry);

        while (!IsListEmpty(&dr->refs)) {
            data_reloc_ref* ref = CONTAINING_RECORD(RemoveHeadList(&dr->refs), data_reloc_ref, list_entry);

            ExFreePool(ref);
        }

        ExFreePool(dr);
    }

    while (!IsListEmpty(&metadata_items)) {
        metadata_reloc* mr = CONTAINING_RECORD(RemoveHeadList(&metadata_items), metadata_reloc, list_entry);

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
