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

/* Load the roots, chunks and device free-space metadata used during mount. */
#ifdef _DEBUG
#define DEBUG
#endif

#include "mount.h"

static bool get_last_inode(_In_ _Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_ root* r, _In_opt_ PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp, prev_tp;
    NTSTATUS Status;

    // get last entry
    searchkey.objectid = 0xffffffffffffffff;
    searchkey.type = 0xff;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, r, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return false;
    }

    if ((tp.item->key.type == BTRFS_INODE_ITEM_KEY || tp.item->key.type == BTRFS_ROOT_ITEM_KEY) && tp.item->key.objectid <= BTRFS_LAST_FREE_OBJECTID) {
        r->lastinode = tp.item->key.objectid;
        TRACE("last inode for tree %I64x is %I64x\n", r->id, r->lastinode);
        return true;
    }

    while (find_prev_item(Vcb, &tp, &prev_tp, Irp)) {
        tp = prev_tp;

        TRACE("moving on to %I64x,%x,%I64x\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset);

        if ((tp.item->key.type == BTRFS_INODE_ITEM_KEY || tp.item->key.type == BTRFS_ROOT_ITEM_KEY) && tp.item->key.objectid <= BTRFS_LAST_FREE_OBJECTID) {
            r->lastinode = tp.item->key.objectid;
            TRACE("last inode for tree %I64x is %I64x\n", r->id, r->lastinode);
            return true;
        }
    }

    r->lastinode = BTRFS_FIRST_FREE_OBJECTID;

    WARN("no INODE_ITEMs in tree %I64x\n", r->id);

    return true;
}

NTSTATUS read_superblock(_In_ device_extension* Vcb, _In_ PDEVICE_OBJECT device, _In_ PFILE_OBJECT fileobj, _In_ uint64_t length) {
    NTSTATUS Status;
    struct btrfs_super_block* sb;
    ULONG i, to_read;
    uint8_t valid_superblocks;

    to_read = device->SectorSize == 0 ? sizeof(struct btrfs_super_block) : (ULONG)sector_align(sizeof(struct btrfs_super_block), device->SectorSize);

    sb = ExAllocatePoolWithTag(NonPagedPool, to_read, ALLOC_TAG);
    if (!sb) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (superblock_addrs[0] + to_read > length) {
        WARN("device was too short to have any superblock\n");
        ExFreePool(sb);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    i = 0;
    valid_superblocks = 0;

    while (superblock_addrs[i] > 0) {
        if (i > 0 && superblock_addrs[i] + to_read > length)
            break;

        Status = sync_read_phys(device, fileobj, superblock_addrs[i], to_read, (PUCHAR)sb, false);
        if (!NT_SUCCESS(Status)) {
            ERR("Failed to read superblock %lu: %08lx\n", i, Status);
            ExFreePool(sb);
            return Status;
        }

        if (sb->magic != BTRFS_MAGIC) {
            if (i == 0) {
                TRACE("not a BTRFS volume\n");
                ExFreePool(sb);
                return STATUS_UNRECOGNIZED_VOLUME;
            }
        } else {
            TRACE("got superblock %lu!\n", i);

            if (sb->sectorsize == 0)
                WARN("superblock sector size was 0\n");
            else if (sb->sectorsize & (sb->sectorsize - 1))
                WARN("superblock sector size was not power of 2\n");
            else if (sb->nodesize < sizeof(struct btrfs_header) + sizeof(struct btrfs_key_ptr) || sb->nodesize > 0x10000)
                WARN("invalid node size %x\n", sb->nodesize);
            else if ((sb->nodesize % sb->sectorsize) != 0)
                WARN("node size %x was not a multiple of sectorsize %x\n", sb->nodesize, sb->sectorsize);
            else if (check_superblock_checksum(sb) && (valid_superblocks == 0 || sb->generation > Vcb->superblock.generation)) {
                RtlCopyMemory(&Vcb->superblock, sb, sizeof(struct btrfs_super_block));
                valid_superblocks++;
            }
        }

        i++;
    }

    ExFreePool(sb);

    if (valid_superblocks == 0) {
        ERR("could not find any valid superblocks\n");
        return STATUS_INTERNAL_ERROR;
    }

    TRACE("label is %s\n", Vcb->superblock.label);

    return STATUS_SUCCESS;
}

_Requires_exclusive_lock_held_(Vcb->tree_lock)
NTSTATUS add_root(_Inout_ device_extension* Vcb, _In_ uint64_t id, _In_ uint64_t addr,
                         _In_ uint64_t generation, _In_opt_ traverse_ptr* tp) {
    root* r = ExAllocatePoolWithTag(PagedPool, sizeof(root), ALLOC_TAG);
    if (!r) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    r->id = id;
    r->dirty = false;
    r->received = false;
    r->reserved = NULL;
    r->treeholder.address = addr;
    r->treeholder.tree = NULL;
    r->treeholder.generation = generation;
    r->parent = 0;
    r->send_ops = 0;
    r->fcbs_version = 0;
    r->checked_for_orphans = false;
    r->dropped = false;
    InitializeListHead(&r->fcbs);
    RtlZeroMemory(r->fcbs_ptrs, sizeof(LIST_ENTRY*) * 256);

    r->nonpaged = ExAllocatePoolWithTag(NonPagedPool, sizeof(root_nonpaged), ALLOC_TAG);
    if (!r->nonpaged) {
        ERR("out of memory\n");
        ExFreePool(r);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExInitializeResourceLite(&r->nonpaged->load_tree_lock);

    r->lastinode = 0;

    if (tp) {
        RtlCopyMemory(&r->root_item, tp->item->data, min(sizeof(struct btrfs_root_item), tp->item->size));
        if (tp->item->size < sizeof(struct btrfs_root_item))
            RtlZeroMemory(((uint8_t*)&r->root_item) + tp->item->size, sizeof(struct btrfs_root_item) - tp->item->size);
    } else
        RtlZeroMemory(&r->root_item, sizeof(struct btrfs_root_item));

    if (!Vcb->readonly && (r->id == BTRFS_ROOT_TREE_OBJECTID || r->id == BTRFS_FS_TREE_OBJECTID || (r->id >= 0x100 && !(r->id & 0xf000000000000000)))) { // FS tree root
        // FIXME - don't call this if subvol is readonly (though we will have to if we ever toggle this flag)
        get_last_inode(Vcb, r, NULL);

        if (r->id == BTRFS_ROOT_TREE_OBJECTID && r->lastinode < 0x100)
            r->lastinode = 0x100;
    }

    InsertTailList(&Vcb->roots, &r->list_entry);

    switch (r->id) {
        case BTRFS_ROOT_TREE_OBJECTID:
            Vcb->root_root = r;
            break;

        case BTRFS_EXTENT_TREE_OBJECTID:
            Vcb->extent_root = r;
            break;

        case BTRFS_CHUNK_TREE_OBJECTID:
            Vcb->chunk_root = r;
            break;

        case BTRFS_DEV_TREE_OBJECTID:
            Vcb->dev_root = r;
            break;

        case BTRFS_CSUM_TREE_OBJECTID:
            Vcb->checksum_root = r;
            break;

        case BTRFS_UUID_TREE_OBJECTID:
            Vcb->uuid_root = r;
            break;

        case BTRFS_FREE_SPACE_TREE_OBJECTID:
            Vcb->space_root = r;
            break;

        case BTRFS_DATA_RELOC_TREE_OBJECTID:
            Vcb->data_reloc_root = r;
            break;

        case BTRFS_BLOCK_GROUP_TREE_OBJECTID:
            if (Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE)
                Vcb->block_group_root = r;
            break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS look_for_roots(_Requires_exclusive_lock_held_(_Curr_->tree_lock) _In_ device_extension* Vcb, _In_opt_ PIRP Irp) {
    traverse_ptr tp, next_tp;
    struct btrfs_key searchkey;
    bool b;
    NTSTATUS Status;

    searchkey.objectid = 0;
    searchkey.type = 0;
    searchkey.offset = 0;

    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return Status;
    }

    do {
        TRACE("(%I64x,%x,%I64x)\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset);

        if (tp.item->key.type == BTRFS_ROOT_ITEM_KEY) {
            struct btrfs_root_item* ri = (struct btrfs_root_item*)tp.item->data;

            if (tp.item->size < offsetof(struct btrfs_root_item, byte_limit)) {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, offsetof(struct btrfs_root_item, byte_limit));
            } else {
                TRACE("root %I64x - address %I64x\n", tp.item->key.objectid, ri->bytenr);

                Status = add_root(Vcb, tp.item->key.objectid, ri->bytenr, ri->generation, &tp);
                if (!NT_SUCCESS(Status)) {
                    ERR("add_root returned %08lx\n", Status);
                    return Status;
                }
            }
        } else if (tp.item->key.type == BTRFS_ROOT_BACKREF_KEY && !IsListEmpty(&Vcb->roots)) {
            root* lastroot = CONTAINING_RECORD(Vcb->roots.Blink, root, list_entry);

            if (lastroot->id == tp.item->key.objectid)
                lastroot->parent = tp.item->key.offset;
        }

        b = find_next_item(Vcb, &tp, &next_tp, false, Irp);

        if (b)
            tp = next_tp;
    } while (b);

    if (!Vcb->readonly && !Vcb->data_reloc_root) {
        root* reloc_root;
        struct btrfs_inode_item* ii;
        uint16_t irlen;
        struct btrfs_inode_ref* ir;
        LARGE_INTEGER time;
        struct btrfs_timespec now;

        WARN("data reloc root doesn't exist, creating it\n");

        Status = create_root(Vcb, BTRFS_DATA_RELOC_TREE_OBJECTID, &reloc_root, false, 0, Irp);

        if (!NT_SUCCESS(Status)) {
            ERR("create_root returned %08lx\n", Status);
            return Status;
        }

        reloc_root->root_item.inode.generation = 1;
        reloc_root->root_item.inode.size = 3;
        reloc_root->root_item.inode.nbytes = Vcb->superblock.nodesize;
        reloc_root->root_item.inode.nlink = 1;
        reloc_root->root_item.inode.mode = 040755;
        reloc_root->root_item.inode.flags = BTRFS_INODE_ROOT_ITEM_INIT;
        reloc_root->root_item.root_dirid = BTRFS_FIRST_FREE_OBJECTID;
        reloc_root->root_item.bytes_used = Vcb->superblock.nodesize;

        ii = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_inode_item), ALLOC_TAG);
        if (!ii) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        KeQuerySystemTime(&time);
        win_time_to_unix(time, &now);

        RtlZeroMemory(ii, sizeof(struct btrfs_inode_item));
        ii->generation = Vcb->superblock.generation;
        ii->nbytes = Vcb->superblock.nodesize;
        ii->nlink = 1;
        ii->mode = 040755;
        ii->atime = now;
        ii->ctime = now;
        ii->mtime = now;

        Status = insert_tree_item(Vcb, reloc_root, BTRFS_FIRST_FREE_OBJECTID, BTRFS_INODE_ITEM_KEY, 0, ii, sizeof(struct btrfs_inode_item), NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item returned %08lx\n", Status);
            ExFreePool(ii);
            return Status;
        }

        irlen = (uint16_t)sizeof(struct btrfs_inode_ref) + 2;
        ir = ExAllocatePoolWithTag(PagedPool, irlen, ALLOC_TAG);
        if (!ir) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ir->index = 0;
        ir->name_len = 2;
        *(char*)&ir[1] = '.';
        *((char*)&ir[1] + 1) = '.';

        Status = insert_tree_item(Vcb, reloc_root, BTRFS_FIRST_FREE_OBJECTID, BTRFS_INODE_REF_KEY, BTRFS_FIRST_FREE_OBJECTID, ir, irlen, NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item returned %08lx\n", Status);
            ExFreePool(ir);
            return Status;
        }

        Vcb->data_reloc_root = reloc_root;
        Vcb->need_write = true;
    }

    return STATUS_SUCCESS;
}

NTSTATUS find_disk_holes(_In_ _Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_ device* dev, _In_opt_ PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp, next_tp;
    bool b;
    uint64_t lastaddr;
    NTSTATUS Status;

    InitializeListHead(&dev->space);

    searchkey.objectid = 0;
    searchkey.type = BTRFS_PERSISTENT_ITEM_KEY;
    searchkey.offset = dev->devitem.devid;

    Status = find_item(Vcb, Vcb->dev_root, &tp, &searchkey, false, Irp);
    if (NT_SUCCESS(Status) && !keycmp(tp.item->key, searchkey))
        RtlCopyMemory(dev->stats, tp.item->data, min(sizeof(uint64_t) * 5, tp.item->size));

    searchkey.objectid = dev->devitem.devid;
    searchkey.type = BTRFS_DEV_EXTENT_KEY;
    searchkey.offset = 0;

    Status = find_item(Vcb, Vcb->dev_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return Status;
    }

    lastaddr = 0;

    do {
        if (tp.item->key.objectid == dev->devitem.devid && tp.item->key.type == BTRFS_DEV_EXTENT_KEY) {
            if (tp.item->size >= sizeof(struct btrfs_dev_extent)) {
                struct btrfs_dev_extent* de = (struct btrfs_dev_extent*)tp.item->data;

                if (tp.item->key.offset > lastaddr) {
                    Status = add_space_entry(&dev->space, NULL, lastaddr, tp.item->key.offset - lastaddr);
                    if (!NT_SUCCESS(Status)) {
                        ERR("add_space_entry returned %08lx\n", Status);
                        return Status;
                    }
                }

                lastaddr = tp.item->key.offset + de->length;
            } else {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_dev_extent));
            }
        }

        b = find_next_item(Vcb, &tp, &next_tp, false, Irp);

        if (b) {
            tp = next_tp;
            if (tp.item->key.objectid > searchkey.objectid || tp.item->key.type > searchkey.type)
                break;
        }
    } while (b);

    if (lastaddr < dev->devitem.total_bytes) {
        Status = add_space_entry(&dev->space, NULL, lastaddr, dev->devitem.total_bytes - lastaddr);
        if (!NT_SUCCESS(Status)) {
            ERR("add_space_entry returned %08lx\n", Status);
            return Status;
        }
    }

    // The Linux driver doesn't like to allocate chunks within the first megabyte of a device.

    Status = space_list_subtract2(&dev->space, NULL, 0, 0x100000, NULL, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    return STATUS_SUCCESS;
}

NTSTATUS load_chunk_root(_In_ _Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_opt_ PIRP Irp) {
    traverse_ptr tp, next_tp;
    struct btrfs_key searchkey;
    bool b;
    chunk* c;
    NTSTATUS Status;

    searchkey.objectid = 0;
    searchkey.type = 0;
    searchkey.offset = 0;

    Vcb->data_flags = 0;
    Vcb->metadata_flags = 0;
    Vcb->system_flags = 0;

    Status = find_item(Vcb, Vcb->chunk_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return Status;
    }

    do {
        TRACE("(%I64x,%x,%I64x)\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset);

        if (tp.item->key.objectid == 1 && tp.item->key.type == BTRFS_DEV_ITEM_KEY) {
            if (tp.item->size < sizeof(struct btrfs_dev_item)) {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_dev_item));
            } else {
                struct btrfs_dev_item* di = (struct btrfs_dev_item*)tp.item->data;
                LIST_ENTRY* le;
                bool done = false;

                le = Vcb->devices.Flink;
                while (le != &Vcb->devices) {
                    device* dev = CONTAINING_RECORD(le, device, list_entry);

                    if (dev->devobj && RtlCompareMemory(&dev->devitem.uuid, &di->uuid, BTRFS_UUID_SIZE) == BTRFS_UUID_SIZE) {
                        RtlCopyMemory(&dev->devitem, tp.item->data, min(tp.item->size, sizeof(struct btrfs_dev_item)));

                        if (le != Vcb->devices.Flink)
                            init_device(Vcb, dev, true);

                        done = true;
                        break;
                    }

                    le = le->Flink;
                }

                if (!done && Vcb->vde) {
                    volume_device_extension* vde = Vcb->vde;
                    pdo_device_extension* pdode = vde->pdode;

                    ExAcquireResourceSharedLite(&pdode->child_lock, true);

                    if (Vcb->devices_loaded < Vcb->superblock.num_devices) {
                        le = pdode->children.Flink;

                        while (le != &pdode->children) {
                            volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

                            if (RtlCompareMemory(&di->uuid, &vc->uuid, BTRFS_UUID_SIZE) == BTRFS_UUID_SIZE) {
                                device* dev;

                                dev = ExAllocatePoolWithTag(NonPagedPool, sizeof(device), ALLOC_TAG);
                                if (!dev) {
                                    ExReleaseResourceLite(&pdode->child_lock);
                                    ERR("out of memory\n");
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                RtlZeroMemory(dev, sizeof(device));

                                dev->devobj = vc->devobj;
                                dev->fileobj = vc->fileobj;
                                RtlCopyMemory(&dev->devitem, di, min(tp.item->size, sizeof(struct btrfs_dev_item)));
                                dev->seeding = vc->seeding;
                                init_device(Vcb, dev, false);

                                if (dev->devitem.total_bytes > vc->size) {
                                    WARN("device %I64x: btrfs_dev_item says %I64x bytes, but Windows only reports %I64x\n", tp.item->key.offset,
                                         dev->devitem.total_bytes, vc->size);

                                    dev->devitem.total_bytes = vc->size;
                                }

                                dev->disk_num = vc->disk_num;
                                dev->part_num = vc->part_num;
                                add_device_to_list(Vcb, dev);
                                Vcb->devices_loaded++;

                                done = true;
                                break;
                            }

                            le = le->Flink;
                        }

                        if (!done) {
                            if (!Vcb->options.allow_degraded) {
                                ERR("volume not found: device %I64x, uuid %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", tp.item->key.offset,
                                    di->uuid[0], di->uuid[1], di->uuid[2], di->uuid[3], di->uuid[4], di->uuid[5], di->uuid[6], di->uuid[7],
                                    di->uuid[8], di->uuid[9], di->uuid[10], di->uuid[11], di->uuid[12], di->uuid[13], di->uuid[14], di->uuid[15]);
                            } else {
                                device* dev;

                                dev = ExAllocatePoolWithTag(NonPagedPool, sizeof(device), ALLOC_TAG);
                                if (!dev) {
                                    ExReleaseResourceLite(&pdode->child_lock);
                                    ERR("out of memory\n");
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                RtlZeroMemory(dev, sizeof(device));

                                // Missing device, so we keep dev->devobj as NULL
                                RtlCopyMemory(&dev->devitem, di, min(tp.item->size, sizeof(struct btrfs_dev_item)));
                                InitializeListHead(&dev->trim_list);

                                add_device_to_list(Vcb, dev);
                                Vcb->devices_loaded++;
                            }
                        }
                    } else
                        ERR("unexpected device %I64x found\n", tp.item->key.offset);

                    ExReleaseResourceLite(&pdode->child_lock);
                }
            }
        } else if (tp.item->key.type == BTRFS_CHUNK_ITEM_KEY) {
            if (tp.item->size < offsetof(struct btrfs_chunk, stripe)) {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, offsetof(struct btrfs_chunk, stripe));
            } else {
                c = ExAllocatePoolWithTag(NonPagedPool, sizeof(chunk), ALLOC_TAG);

                if (!c) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                c->size = tp.item->size;
                c->offset = tp.item->key.offset;
                c->used = c->oldused = 0;
                c->cache = c->old_cache = NULL;
                c->created = false;
                c->readonly = false;
                c->reloc = false;
                c->cache_loaded = false;
                c->changed = false;
                c->space_changed = false;
                c->using_fst_bitmaps = false;
                c->balance_num = 0;

                c->chunk_item = ExAllocatePoolWithTag(NonPagedPool, tp.item->size, ALLOC_TAG);

                if (!c->chunk_item) {
                    ERR("out of memory\n");
                    ExFreePool(c);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(c->chunk_item, tp.item->data, tp.item->size);

                if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA && c->chunk_item->type > Vcb->data_flags)
                    Vcb->data_flags = c->chunk_item->type;

                if (c->chunk_item->type & BTRFS_BLOCK_GROUP_METADATA && c->chunk_item->type > Vcb->metadata_flags)
                    Vcb->metadata_flags = c->chunk_item->type;

                if (c->chunk_item->type & BTRFS_BLOCK_GROUP_SYSTEM && c->chunk_item->type > Vcb->system_flags)
                    Vcb->system_flags = c->chunk_item->type;

                if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID10) {
                    if (c->chunk_item->sub_stripes == 0 || c->chunk_item->sub_stripes > c->chunk_item->num_stripes) {
                        ERR("chunk %I64x: invalid stripes (num_stripes %u, sub_stripes %u)\n", c->offset, c->chunk_item->num_stripes, c->chunk_item->sub_stripes);
                        ExFreePool(c->chunk_item);
                        ExFreePool(c);
                        return STATUS_INTERNAL_ERROR;
                    }
                }

                if (c->chunk_item->num_stripes > 0) {
                    uint16_t i;

                    c->devices = ExAllocatePoolWithTag(NonPagedPool, sizeof(device*) * c->chunk_item->num_stripes, ALLOC_TAG);

                    if (!c->devices) {
                        ERR("out of memory\n");
                        ExFreePool(c->chunk_item);
                        ExFreePool(c);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    for (i = 0; i < c->chunk_item->num_stripes; i++) {
                        c->devices[i] = find_device_from_uuid(Vcb, c->chunk_item->stripe[i].dev_uuid);
                        TRACE("device %u = %p\n", i, c->devices[i]);

                        if (!c->devices[i]) {
                            ERR("missing device\n");
                            ExFreePool(c->devices);
                            ExFreePool(c->chunk_item);
                            ExFreePool(c);
                            return STATUS_INTERNAL_ERROR;
                        }

                        if (c->devices[i]->readonly)
                            c->readonly = true;
                    }
                } else {
                    ERR("chunk %I64x: number of stripes is 0\n", c->offset);
                    ExFreePool(c->chunk_item);
                    ExFreePool(c);
                    return STATUS_INTERNAL_ERROR;
                }

                ExInitializeResourceLite(&c->lock);
                ExInitializeResourceLite(&c->changed_extents_lock);

                InitializeListHead(&c->space);
                InitializeListHead(&c->space_size);
                InitializeListHead(&c->deleting);
                InitializeListHead(&c->changed_extents);

                InitializeListHead(&c->range_locks);
                ExInitializeResourceLite(&c->range_locks_lock);
                KeInitializeEvent(&c->range_locks_event, NotificationEvent, false);

                InitializeListHead(&c->partial_stripes);
                ExInitializeResourceLite(&c->partial_stripes_lock);

                c->last_alloc_set = false;

                c->last_stripe = 0;

                InsertTailList(&Vcb->chunks, &c->list_entry);

                c->list_entry_balance.Flink = NULL;
            }
        }

        b = find_next_item(Vcb, &tp, &next_tp, false, Irp);

        if (b)
            tp = next_tp;
    } while (b);

    Vcb->log_to_phys_loaded = true;

    if (Vcb->data_flags == 0)
        Vcb->data_flags = BTRFS_BLOCK_GROUP_DATA | (Vcb->superblock.num_devices > 1 ? BTRFS_BLOCK_GROUP_RAID0 : 0);

    if (Vcb->metadata_flags == 0)
        Vcb->metadata_flags = BTRFS_BLOCK_GROUP_METADATA | (Vcb->superblock.num_devices > 1 ? BTRFS_BLOCK_GROUP_RAID1 : BTRFS_BLOCK_GROUP_DUP);

    if (Vcb->system_flags == 0)
        Vcb->system_flags = BTRFS_BLOCK_GROUP_SYSTEM | (Vcb->superblock.num_devices > 1 ? BTRFS_BLOCK_GROUP_RAID1 : BTRFS_BLOCK_GROUP_DUP);

    if (Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS) {
        Vcb->metadata_flags |= BTRFS_BLOCK_GROUP_DATA;
        Vcb->data_flags = Vcb->metadata_flags;
    }

    return STATUS_SUCCESS;
}

NTSTATUS load_sys_chunks(_In_ device_extension* Vcb) {
    struct btrfs_key key;
    ULONG n = Vcb->superblock.sys_chunk_array_size;

    while (n > 0) {
        if (n > sizeof(struct btrfs_key)) {
            RtlCopyMemory(&key, &Vcb->superblock.sys_chunk_array[Vcb->superblock.sys_chunk_array_size - n], sizeof(struct btrfs_key));
            n -= sizeof(struct btrfs_key);
        } else
            return STATUS_SUCCESS;

        TRACE("bootstrap: %I64x,%x,%I64x\n", key.objectid, key.type, key.offset);

        if (key.type == BTRFS_CHUNK_ITEM_KEY) {
            struct btrfs_chunk* ci;
            USHORT cisize;
            sys_chunk* sc;

            if (n < offsetof(struct btrfs_chunk, stripe))
                return STATUS_SUCCESS;

            ci = (struct btrfs_chunk*)&Vcb->superblock.sys_chunk_array[Vcb->superblock.sys_chunk_array_size - n];
            cisize = offsetof(struct btrfs_chunk, stripe) + (ci->num_stripes * sizeof(struct btrfs_stripe));

            if (n < cisize)
                return STATUS_SUCCESS;

            sc = ExAllocatePoolWithTag(PagedPool, sizeof(sys_chunk), ALLOC_TAG);

            if (!sc) {
                ERR("out of memory\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            sc->key = key;
            sc->size = cisize;
            sc->data = ExAllocatePoolWithTag(PagedPool, sc->size, ALLOC_TAG);

            if (!sc->data) {
                ERR("out of memory\n");
                ExFreePool(sc);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            RtlCopyMemory(sc->data, ci, sc->size);
            InsertTailList(&Vcb->sys_chunks, &sc->list_entry);

            n -= cisize;
        } else {
            ERR("unexpected item %I64x,%x,%I64x in bootstrap\n", key.objectid, key.type, key.offset);
            return STATUS_INTERNAL_ERROR;
        }
    }

    return STATUS_SUCCESS;
}
