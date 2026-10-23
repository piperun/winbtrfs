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

// Allocate chunks and choose backing stripes; does not write file data.
#include "btrfs_drv.h"

typedef struct {
    space* dh;
    device* device;
} stripe;

__attribute__((nonnull(1)))
static uint64_t find_new_chunk_address(device_extension* Vcb, uint64_t size) {
    uint64_t lastaddr;
    LIST_ENTRY* le;

    lastaddr = 0xc00000;

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

        if (c->offset >= lastaddr + size)
            return lastaddr;

        lastaddr = c->offset + c->chunk_item->length;

        le = le->Flink;
    }

    return lastaddr;
}

__attribute__((nonnull(1,2)))
static bool find_new_dup_stripes(device_extension* Vcb, stripe* stripes, uint64_t max_stripe_size, bool full_size) {
    uint64_t devusage = 0xffffffffffffffff;
    space *devdh1 = NULL, *devdh2 = NULL;
    LIST_ENTRY* le;
    device* dev2 = NULL;

    le = Vcb->devices.Flink;

    while (le != &Vcb->devices) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);

        if (!dev->readonly && !dev->reloc && dev->devobj) {
            uint64_t usage = (dev->devitem.bytes_used * 4096) / dev->devitem.total_bytes;

            // favour devices which have been used the least
            if (usage < devusage) {
                if (!IsListEmpty(&dev->space)) {
                    LIST_ENTRY* le2;
                    space *dh1 = NULL, *dh2 = NULL;

                    le2 = dev->space.Flink;
                    while (le2 != &dev->space) {
                        space* dh = CONTAINING_RECORD(le2, space, list_entry);

                        if (dh->size >= max_stripe_size && (!dh1 || !dh2 || dh->size < dh1->size)) {
                            dh2 = dh1;
                            dh1 = dh;
                        }

                        le2 = le2->Flink;
                    }

                    if (dh1 && (dh2 || dh1->size >= 2 * max_stripe_size)) {
                        dev2 = dev;
                        devusage = usage;
                        devdh1 = dh1;
                        devdh2 = dh2 ? dh2 : dh1;
                    }
                }
            }
        }

        le = le->Flink;
    }

    if (!devdh1) {
        uint64_t size = 0;

        // Can't find hole of at least max_stripe_size; look for the largest one we can find

        if (full_size)
            return false;

        le = Vcb->devices.Flink;
        while (le != &Vcb->devices) {
            device* dev = CONTAINING_RECORD(le, device, list_entry);

            if (!dev->readonly && !dev->reloc) {
                if (!IsListEmpty(&dev->space)) {
                    LIST_ENTRY* le2;
                    space *dh1 = NULL, *dh2 = NULL;

                    le2 = dev->space.Flink;
                    while (le2 != &dev->space) {
                        space* dh = CONTAINING_RECORD(le2, space, list_entry);

                        if (!dh1 || !dh2 || dh->size < dh1->size) {
                            dh2 = dh1;
                            dh1 = dh;
                        }

                        le2 = le2->Flink;
                    }

                    if (dh1) {
                        uint64_t devsize;

                        if (dh2)
                            devsize = max(dh1->size / 2, min(dh1->size, dh2->size));
                        else
                            devsize = dh1->size / 2;

                        if (devsize > size) {
                            dev2 = dev;
                            devdh1 = dh1;

                            if (dh2 && min(dh1->size, dh2->size) > dh1->size / 2)
                                devdh2 = dh2;
                            else
                                devdh2 = dh1;

                            size = devsize;
                        }
                    }
                }
            }

            le = le->Flink;
        }

        if (!devdh1)
            return false;
    }

    stripes[0].device = stripes[1].device = dev2;
    stripes[0].dh = devdh1;
    stripes[1].dh = devdh2;

    return true;
}

__attribute__((nonnull(1,2)))
static bool find_new_stripe(device_extension* Vcb, stripe* stripes, uint16_t i, uint64_t max_stripe_size, bool allow_missing, bool full_size) {
    uint64_t k, devusage = 0xffffffffffffffff;
    space* devdh = NULL;
    LIST_ENTRY* le;
    device* dev2 = NULL;

    le = Vcb->devices.Flink;
    while (le != &Vcb->devices) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);
        uint64_t usage;
        bool skip = false;

        if (dev->readonly || dev->reloc || (!dev->devobj && !allow_missing)) {
            le = le->Flink;
            continue;
        }

        // skip this device if it already has a stripe
        if (i > 0) {
            for (k = 0; k < i; k++) {
                if (stripes[k].device == dev) {
                    skip = true;
                    break;
                }
            }
        }

        if (!skip) {
            usage = (dev->devitem.bytes_used * 4096) / dev->devitem.total_bytes;

            // favour devices which have been used the least
            if (usage < devusage) {
                if (!IsListEmpty(&dev->space)) {
                    LIST_ENTRY* le2;

                    le2 = dev->space.Flink;
                    while (le2 != &dev->space) {
                        space* dh = CONTAINING_RECORD(le2, space, list_entry);

                        if ((dev2 != dev && dh->size >= max_stripe_size) ||
                            (dev2 == dev && dh->size >= max_stripe_size && dh->size < devdh->size)
                        ) {
                            devdh = dh;
                            dev2 = dev;
                            devusage = usage;
                        }

                        le2 = le2->Flink;
                    }
                }
            }
        }

        le = le->Flink;
    }

    if (!devdh) {
        // Can't find hole of at least max_stripe_size; look for the largest one we can find

        if (full_size)
            return false;

        le = Vcb->devices.Flink;
        while (le != &Vcb->devices) {
            device* dev = CONTAINING_RECORD(le, device, list_entry);
            bool skip = false;

            if (dev->readonly || dev->reloc || (!dev->devobj && !allow_missing)) {
                le = le->Flink;
                continue;
            }

            // skip this device if it already has a stripe
            if (i > 0) {
                for (k = 0; k < i; k++) {
                    if (stripes[k].device == dev) {
                        skip = true;
                        break;
                    }
                }
            }

            if (!skip) {
                if (!IsListEmpty(&dev->space)) {
                    LIST_ENTRY* le2;

                    le2 = dev->space.Flink;
                    while (le2 != &dev->space) {
                        space* dh = CONTAINING_RECORD(le2, space, list_entry);

                        if (!devdh || devdh->size < dh->size) {
                            devdh = dh;
                            dev2 = dev;
                        }

                        le2 = le2->Flink;
                    }
                }
            }

            le = le->Flink;
        }

        if (!devdh)
            return false;
    }

    stripes[i].dh = devdh;
    stripes[i].device = dev2;

    return true;
}

__attribute__((nonnull(1,3)))
NTSTATUS alloc_chunk(device_extension* Vcb, uint64_t flags, chunk** pc, bool full_size) {
    NTSTATUS Status;
    uint64_t max_stripe_size, max_chunk_size, stripe_size, stripe_length, factor;
    uint64_t total_size = 0, logaddr;
    uint16_t i, type, num_stripes, sub_stripes, max_stripes, min_stripes, allowed_missing;
    stripe* stripes = NULL;
    uint16_t cisize;
    chunk* c = NULL;
    space* s = NULL;
    LIST_ENTRY* le;

    le = Vcb->devices.Flink;
    while (le != &Vcb->devices) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);
        total_size += dev->devitem.total_bytes;

        le = le->Flink;
    }

    TRACE("total_size = %I64x\n", total_size);

    // We purposely check for DATA first - mixed blocks have the same size
    // as DATA ones.
    if (flags & BTRFS_BLOCK_GROUP_DATA) {
        max_stripe_size = 0x40000000; // 1 GB
        max_chunk_size = 10 * max_stripe_size;
    } else if (flags & BTRFS_BLOCK_GROUP_METADATA) {
        if (total_size > 0xC80000000) // 50 GB
            max_stripe_size = 0x40000000; // 1 GB
        else
            max_stripe_size = 0x10000000; // 256 MB

        max_chunk_size = max_stripe_size;
    } else if (flags & BTRFS_BLOCK_GROUP_SYSTEM) {
        max_stripe_size = 0x2000000; // 32 MB
        max_chunk_size = 2 * max_stripe_size;
    } else {
        ERR("unknown chunk type\n");
        return STATUS_INTERNAL_ERROR;
    }

    if (flags & BTRFS_BLOCK_GROUP_DUP) {
        min_stripes = 2;
        max_stripes = 2;
        sub_stripes = 0;
        type = BTRFS_BLOCK_GROUP_DUP;
        allowed_missing = 0;
    } else if (flags & BTRFS_BLOCK_GROUP_RAID0) {
        min_stripes = 2;
        max_stripes = (uint16_t)min(0xffff, Vcb->superblock.num_devices);
        sub_stripes = 0;
        type = BTRFS_BLOCK_GROUP_RAID0;
        allowed_missing = 0;
    } else if (flags & BTRFS_BLOCK_GROUP_RAID1) {
        min_stripes = 2;
        max_stripes = 2;
        sub_stripes = 1;
        type = BTRFS_BLOCK_GROUP_RAID1;
        allowed_missing = 1;
    } else if (flags & BTRFS_BLOCK_GROUP_RAID10) {
        min_stripes = 4;
        max_stripes = (uint16_t)min(0xffff, Vcb->superblock.num_devices);
        sub_stripes = 2;
        type = BTRFS_BLOCK_GROUP_RAID10;
        allowed_missing = 1;
    } else if (flags & BTRFS_BLOCK_GROUP_RAID5) {
        min_stripes = 3;
        max_stripes = (uint16_t)min(0xffff, Vcb->superblock.num_devices);
        sub_stripes = 1;
        type = BTRFS_BLOCK_GROUP_RAID5;
        allowed_missing = 1;
    } else if (flags & BTRFS_BLOCK_GROUP_RAID6) {
        min_stripes = 4;
        max_stripes = 257;
        sub_stripes = 1;
        type = BTRFS_BLOCK_GROUP_RAID6;
        allowed_missing = 2;
    } else if (flags & BTRFS_BLOCK_GROUP_RAID1C3) {
        min_stripes = 3;
        max_stripes = 3;
        sub_stripes = 1;
        type = BTRFS_BLOCK_GROUP_RAID1C3;
        allowed_missing = 2;
    } else if (flags & BTRFS_BLOCK_GROUP_RAID1C4) {
        min_stripes = 4;
        max_stripes = 4;
        sub_stripes = 1;
        type = BTRFS_BLOCK_GROUP_RAID1C4;
        allowed_missing = 3;
    } else { // SINGLE
        min_stripes = 1;
        max_stripes = 1;
        sub_stripes = 1;
        type = 0;
        allowed_missing = 0;
    }

    if (max_chunk_size > total_size / 10) {  // cap at 10%
        max_chunk_size = total_size / 10;
        max_stripe_size = max_chunk_size / min_stripes;
    }

    if (max_stripe_size > total_size / (10 * min_stripes))
        max_stripe_size = total_size / (10 * min_stripes);

    TRACE("would allocate a new chunk of %I64x bytes and stripe %I64x\n", max_chunk_size, max_stripe_size);

    stripes = ExAllocatePoolWithTag(PagedPool, sizeof(stripe) * max_stripes, ALLOC_TAG);
    if (!stripes) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    num_stripes = 0;

    if (type == BTRFS_BLOCK_GROUP_DUP) {
        if (!find_new_dup_stripes(Vcb, stripes, max_stripe_size, full_size)) {
            Status = STATUS_DISK_FULL;
            goto end;
        } else
            num_stripes = max_stripes;
    } else {
        for (i = 0; i < max_stripes; i++) {
            if (!find_new_stripe(Vcb, stripes, i, max_stripe_size, false, full_size))
                break;
            else
                num_stripes++;
        }
    }

    if (num_stripes < min_stripes && Vcb->options.allow_degraded && allowed_missing > 0) {
        uint16_t added_missing = 0;

        for (i = num_stripes; i < max_stripes; i++) {
            if (!find_new_stripe(Vcb, stripes, i, max_stripe_size, true, full_size))
                break;
            else {
                added_missing++;
                if (added_missing >= allowed_missing)
                    break;
            }
        }

        num_stripes += added_missing;
    }

    // for RAID10, round down to an even number of stripes
    if (type == BTRFS_BLOCK_GROUP_RAID10 && (num_stripes % sub_stripes) != 0) {
        num_stripes -= num_stripes % sub_stripes;
    }

    if (num_stripes < min_stripes) {
        WARN("found %u stripes, needed at least %u\n", num_stripes, min_stripes);
        Status = STATUS_DISK_FULL;
        goto end;
    }

    c = ExAllocatePoolWithTag(NonPagedPool, sizeof(chunk), ALLOC_TAG);
    if (!c) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    c->devices = NULL;

    cisize = offsetof(struct btrfs_chunk, stripe) + (num_stripes * sizeof(struct btrfs_stripe));
    c->chunk_item = ExAllocatePoolWithTag(NonPagedPool, cisize, ALLOC_TAG);
    if (!c->chunk_item) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    stripe_length = 0x10000; // FIXME? BTRFS_STRIPE_LEN in kernel

    if (type == BTRFS_BLOCK_GROUP_DUP && stripes[1].dh == stripes[0].dh)
        stripe_size = min(stripes[0].dh->size / 2, max_stripe_size);
    else {
        stripe_size = max_stripe_size;
        for (i = 0; i < num_stripes; i++) {
            if (stripes[i].dh->size < stripe_size)
                stripe_size = stripes[i].dh->size;
        }
    }

    if (type == BTRFS_BLOCK_GROUP_RAID0)
        factor = num_stripes;
    else if (type == BTRFS_BLOCK_GROUP_RAID10)
        factor = num_stripes / sub_stripes;
    else if (type == BTRFS_BLOCK_GROUP_RAID5)
        factor = num_stripes - 1;
    else if (type == BTRFS_BLOCK_GROUP_RAID6)
        factor = num_stripes - 2;
    else
        factor = 1; // SINGLE, DUPLICATE, RAID1, RAID1C3, RAID1C4

    if (stripe_size * factor > max_chunk_size)
        stripe_size = max_chunk_size / factor;

    if (stripe_size % stripe_length > 0)
        stripe_size -= stripe_size % stripe_length;

    if (stripe_size == 0) {
        ERR("not enough free space found (stripe_size == 0)\n");
        Status = STATUS_DISK_FULL;
        goto end;
    }

    c->chunk_item->length = stripe_size * factor;
    c->chunk_item->owner = Vcb->extent_root->id;
    c->chunk_item->stripe_len = stripe_length;
    c->chunk_item->type = flags;
    c->chunk_item->io_align = (uint32_t)c->chunk_item->stripe_len;
    c->chunk_item->io_width = (uint32_t)c->chunk_item->stripe_len;
    c->chunk_item->sector_size = stripes[0].device->devitem.sector_size;
    c->chunk_item->num_stripes = num_stripes;
    c->chunk_item->sub_stripes = sub_stripes;

    c->devices = ExAllocatePoolWithTag(NonPagedPool, sizeof(device*) * num_stripes, ALLOC_TAG);
    if (!c->devices) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    for (i = 0; i < num_stripes; i++) {
        c->chunk_item->stripe[i].devid = stripes[i].device->devitem.devid;

        if (type == BTRFS_BLOCK_GROUP_DUP && i == 1 && stripes[i].dh == stripes[0].dh)
            c->chunk_item->stripe[i].offset = stripes[0].dh->address + stripe_size;
        else
            c->chunk_item->stripe[i].offset = stripes[i].dh->address;

        memcpy(c->chunk_item->stripe[i].dev_uuid, stripes[i].device->devitem.uuid,
               BTRFS_UUID_SIZE);

        c->devices[i] = stripes[i].device;
    }

    logaddr = find_new_chunk_address(Vcb, c->chunk_item->length);

    Vcb->superblock.chunk_root_generation = Vcb->superblock.generation;

    c->size = cisize;
    c->offset = logaddr;
    c->used = c->oldused = 0;
    c->cache = c->old_cache = NULL;
    c->readonly = false;
    c->reloc = false;
    c->last_alloc_set = false;
    c->last_stripe = 0;
    c->cache_loaded = true;
    c->changed = false;
    c->space_changed = false;
    c->using_fst_bitmaps = false;
    c->balance_num = 0;

    InitializeListHead(&c->space);
    InitializeListHead(&c->space_size);
    InitializeListHead(&c->deleting);
    InitializeListHead(&c->pending_trims);
    initialize_changed_extents(c);

    InitializeListHead(&c->range_locks);
    ExInitializeResourceLite(&c->range_locks_lock);
    KeInitializeEvent(&c->range_locks_event, NotificationEvent, false);

    InitializeListHead(&c->partial_stripes);
    ExInitializeResourceLite(&c->partial_stripes_lock);

    ExInitializeResourceLite(&c->lock);
    ExInitializeResourceLite(&c->changed_extents_lock);

    s = ExAllocatePoolWithTag(NonPagedPool, sizeof(space), ALLOC_TAG);
    if (!s) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    s->address = c->offset;
    s->size = c->chunk_item->length;
    InsertTailList(&c->space, &s->list_entry);
    InsertTailList(&c->space_size, &s->list_entry_size);

    Status = protect_superblocks(c);
    if (!NT_SUCCESS(Status))
        goto end;

    for (i = 0; i < num_stripes; i++) {
        stripes[i].device->devitem.bytes_used += stripe_size;

        Status = space_list_subtract2(&stripes[i].device->space, NULL,
                                      c->chunk_item->stripe[i].offset,
                                      stripe_size, NULL, NULL);
        if (!NT_SUCCESS(Status))
            goto end;
    }

    Status = STATUS_SUCCESS;

    if (flags & BTRFS_BLOCK_GROUP_RAID5 || flags & BTRFS_BLOCK_GROUP_RAID6)
        Vcb->superblock.incompat_flags |= BTRFS_FEATURE_INCOMPAT_RAID56;

end:
    if (stripes)
        ExFreePool(stripes);

    if (!NT_SUCCESS(Status)) {
        if (c) {
            if (c->devices)
                ExFreePool(c->devices);

            if (c->chunk_item)
                ExFreePool(c->chunk_item);

            ExFreePool(c);
        }

        if (s) ExFreePool(s);
    } else {
        bool done = false;

        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c2 = CONTAINING_RECORD(le, chunk, list_entry);

            if (c2->offset > c->offset) {
                InsertHeadList(le->Blink, &c->list_entry);
                done = true;
                break;
            }

            le = le->Flink;
        }

        if (!done)
            InsertTailList(&Vcb->chunks, &c->list_entry);

        c->created = true;
        c->changed = true;
        c->space_changed = true;
        c->list_entry_balance.Flink = NULL;

        *pc = c;
    }

    return Status;
}
