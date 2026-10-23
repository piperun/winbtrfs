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

// Committed free-space maintenance and RAID discard mapping.
#include "btrfs_drv.h"
#include "trim.h"
#include "transaction-chunks.h"

void add_trim_entry(device* dev, uint64_t address, uint64_t size) {
    space* s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);
    if (!s) {
        ERR("out of memory\n");
        return;
    }

    s->address = address;
    s->size = size;
    dev->num_trim_entries++;

    InsertTailList(&dev->trim_list, &s->list_entry);
}


NTSTATUS clean_space_cache(device_extension* Vcb) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    chunk* c;

    TRACE("(%p)\n", Vcb);

    trim_commit(Vcb);

    ExAcquireResourceSharedLite(&Vcb->chunk_lock, true);

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        c = CONTAINING_RECORD(le, chunk, list_entry);

        if (c->space_changed) {
            acquire_chunk_lock(c, Vcb);

            if (c->space_changed) {
                Status = space_list_merge(&c->space, &c->space_size,
                                          &c->deleting);
                if (!NT_SUCCESS(Status)) {
                    release_chunk_lock(c, Vcb);
                    ExReleaseResourceLite(&Vcb->chunk_lock);
                    return Status;
                }

                while (!IsListEmpty(&c->deleting)) {
                    space* s = CONTAINING_RECORD(RemoveHeadList(&c->deleting), space, list_entry);

                    ExFreePool(s);
                }
            }

            c->space_changed = false;

            release_chunk_lock(c, Vcb);
        }

        le = le->Flink;
    }

    ExReleaseResourceLite(&Vcb->chunk_lock);

    return STATUS_SUCCESS;
}

void add_trim_entry_avoid_sb(device_extension* Vcb, device* dev, uint64_t address, uint64_t size) {
    int i;
    ULONG sblen = (ULONG)sector_align(sizeof(struct btrfs_super_block), Vcb->superblock.sectorsize);

    i = 0;
    while (superblock_addrs[i] != 0) {
        if (superblock_addrs[i] + sblen >= address && superblock_addrs[i] < address + size) {
            if (superblock_addrs[i] > address)
                add_trim_entry(dev, address, superblock_addrs[i] - address);

            if (size <= superblock_addrs[i] + sblen - address)
                return;

            size -= superblock_addrs[i] + sblen - address;
            address = superblock_addrs[i] + sblen;
        } else if (superblock_addrs[i] > address + size)
            break;

        i++;
    }

    add_trim_entry(dev, address, size);
}
