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

// Device removal and unallocated space discard during balance.
#include "btrfs_drv.h"
#include "balance-worker.h"
#include <mountmgr.h>
#include <ntddstor.h>
#include "btrfsioctl.h"
#include "crc32c.h"

static NTSTATUS remove_superblocks(device* dev) {
    NTSTATUS Status;
    struct btrfs_super_block* sb;
    int i = 0;

    sb = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_super_block), ALLOC_TAG);
    if (!sb) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(sb, sizeof(struct btrfs_super_block));

    while (superblock_addrs[i] > 0 && dev->devitem.total_bytes >= superblock_addrs[i] + sizeof(struct btrfs_super_block)) {
        Status = write_data_phys(dev->devobj, dev->fileobj, superblock_addrs[i], sb, sizeof(struct btrfs_super_block));

        if (!NT_SUCCESS(Status)) {
            ExFreePool(sb);
            return Status;
        }

        i++;
    }

    ExFreePool(sb);

    return STATUS_SUCCESS;
}

NTSTATUS finish_removing_device(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, device* dev) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    LIST_ENTRY* le;
    volume_device_extension* vde;

    if (Vcb->need_write) {
        Status = do_write(Vcb, NULL);

        if (!NT_SUCCESS(Status))
            ERR("do_write returned %08lx\n", Status);
    } else
        Status = STATUS_SUCCESS;

    free_trees(Vcb);

    if (!NT_SUCCESS(Status))
        return Status;

    // remove entry in chunk tree

    searchkey.objectid = 1;
    searchkey.type = BTRFS_DEV_ITEM_KEY;
    searchkey.offset = dev->devitem.devid;

    Status = find_item(Vcb, Vcb->chunk_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (!keycmp(searchkey, tp.item->key)) {
        Status = delete_tree_item(Vcb, &tp);

        if (!NT_SUCCESS(Status)) {
            ERR("delete_tree_item returned %08lx\n", Status);
            return Status;
        }
    }

    // remove stats entry in device tree

    searchkey.objectid = 0;
    searchkey.type = BTRFS_PERSISTENT_ITEM_KEY;
    searchkey.offset = dev->devitem.devid;

    Status = find_item(Vcb, Vcb->dev_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (!keycmp(searchkey, tp.item->key)) {
        Status = delete_tree_item(Vcb, &tp);

        if (!NT_SUCCESS(Status)) {
            ERR("delete_tree_item returned %08lx\n", Status);
            return Status;
        }
    }

    // update superblock

    Vcb->superblock.num_devices--;
    Vcb->superblock.total_bytes -= dev->devitem.total_bytes;
    Vcb->devices_loaded--;

    RemoveEntryList(&dev->list_entry);

    // flush

    Status = do_write(Vcb, NULL);
    if (!NT_SUCCESS(Status))
        ERR("do_write returned %08lx\n", Status);

    free_trees(Vcb);

    if (!NT_SUCCESS(Status))
        return Status;

    if (!dev->readonly && dev->devobj) {
        Status = remove_superblocks(dev);
        if (!NT_SUCCESS(Status))
            WARN("remove_superblocks returned %08lx\n", Status);
    }

    // remove entry in volume list

    vde = Vcb->vde;

    if (dev->devobj) {
        pdo_device_extension* pdode = vde->pdode;

        ExAcquireResourceExclusiveLite(&pdode->child_lock, true);

        le = pdode->children.Flink;
        while (le != &pdode->children) {
            volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

            if (RtlCompareMemory(&dev->devitem.uuid, &vc->uuid, BTRFS_UUID_SIZE) == BTRFS_UUID_SIZE) {
                PFILE_OBJECT FileObject;
                PDEVICE_OBJECT mountmgr;
                UNICODE_STRING mmdevpath;

                pdode->children_loaded--;

                if (vc->had_drive_letter) { // re-add entry to mountmgr
                    RtlInitUnicodeString(&mmdevpath, MOUNTMGR_DEVICE_NAME);
                    Status = IoGetDeviceObjectPointer(&mmdevpath, FILE_READ_ATTRIBUTES, &FileObject, &mountmgr);
                    if (!NT_SUCCESS(Status))
                        ERR("IoGetDeviceObjectPointer returned %08lx\n", Status);
                    else {
                        MOUNTDEV_NAME mdn;

                        Status = dev_ioctl(dev->devobj, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, &mdn, sizeof(MOUNTDEV_NAME), true, NULL);
                        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
                            ERR("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME returned %08lx\n", Status);
                        else {
                            MOUNTDEV_NAME* mdn2;
                            ULONG mdnsize = (ULONG)offsetof(MOUNTDEV_NAME, Name[0]) + mdn.NameLength;

                            mdn2 = ExAllocatePoolWithTag(PagedPool, mdnsize, ALLOC_TAG);
                            if (!mdn2)
                                ERR("out of memory\n");
                            else {
                                Status = dev_ioctl(dev->devobj, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, mdn2, mdnsize, true, NULL);
                                if (!NT_SUCCESS(Status))
                                    ERR("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME returned %08lx\n", Status);
                                else {
                                    UNICODE_STRING name;

                                    name.Buffer = mdn2->Name;
                                    name.Length = name.MaximumLength = mdn2->NameLength;

                                    Status = mountmgr_add_drive_letter(mountmgr, &name);
                                    if (!NT_SUCCESS(Status))
                                        WARN("mountmgr_add_drive_letter returned %08lx\n", Status);
                                }

                                ExFreePool(mdn2);
                            }
                        }

                        ObDereferenceObject(FileObject);
                    }
                }

                ExFreePool(vc->pnp_name.Buffer);
                RemoveEntryList(&vc->list_entry);

                ObDereferenceObject(vc->fileobj);
                ExFreePool(vc);

                break;
            }

            le = le->Flink;
        }

        if (pdode->children_loaded > 0 && vde->device->Characteristics & FILE_REMOVABLE_MEDIA) {
            vde->device->Characteristics &= ~FILE_REMOVABLE_MEDIA;

            le = pdode->children.Flink;
            while (le != &pdode->children) {
                volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

                if (vc->devobj->Characteristics & FILE_REMOVABLE_MEDIA) {
                    vde->device->Characteristics |= FILE_REMOVABLE_MEDIA;
                    break;
                }

                le = le->Flink;
            }
        }

        pdode->num_children = Vcb->superblock.num_devices;

        ExReleaseResourceLite(&pdode->child_lock);

        // free dev

        if (dev->trim && !dev->readonly && !Vcb->options.no_trim)
            trim_whole_device(dev);
    }

    while (!IsListEmpty(&dev->space)) {
        LIST_ENTRY* le2 = RemoveHeadList(&dev->space);
        space* s = CONTAINING_RECORD(le2, space, list_entry);

        ExFreePool(s);
    }

    ExFreePool(dev);

    if (Vcb->trim) {
        Vcb->trim = false;

        le = Vcb->devices.Flink;
        while (le != &Vcb->devices) {
            device* dev2 = CONTAINING_RECORD(le, device, list_entry);

            if (dev2->trim) {
                Vcb->trim = true;
                break;
            }

            le = le->Flink;
        }
    }

    FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_CHANGE_SIZE);

    return STATUS_SUCCESS;
}

void trim_unalloc_space(_Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, device* dev) {
    DEVICE_MANAGE_DATA_SET_ATTRIBUTES* dmdsa;
    DEVICE_DATA_SET_RANGE* ranges;
    ULONG datalen, i;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    bool b;
    uint64_t lastoff = 0x100000; // don't TRIM the first megabyte, in case someone has been daft enough to install GRUB there
    LIST_ENTRY* le;

    dev->num_trim_entries = 0;

    searchkey.objectid = dev->devitem.devid;
    searchkey.type = BTRFS_DEV_EXTENT_KEY;
    searchkey.offset = 0;

    Status = find_item(Vcb, Vcb->dev_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return;
    }

    do {
        traverse_ptr next_tp;

        if (tp.item->key.objectid == dev->devitem.devid && tp.item->key.type == BTRFS_DEV_EXTENT_KEY) {
            if (tp.item->size >= sizeof(struct btrfs_dev_extent)) {
                struct btrfs_dev_extent* de = (struct btrfs_dev_extent*)tp.item->data;

                if (tp.item->key.offset > lastoff)
                    add_trim_entry_avoid_sb(Vcb, dev, lastoff, tp.item->key.offset - lastoff);

                lastoff = tp.item->key.offset + de->length;
            } else {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_dev_extent));
                return;
            }
        }

        b = find_next_item(Vcb, &tp, &next_tp, false, NULL);

        if (b) {
            tp = next_tp;
            if (tp.item->key.objectid > searchkey.objectid || (tp.item->key.objectid == searchkey.objectid && tp.item->key.type > searchkey.type))
                break;
        }
    } while (b);

    if (lastoff < dev->devitem.total_bytes)
        add_trim_entry_avoid_sb(Vcb, dev, lastoff, dev->devitem.total_bytes - lastoff);

    if (dev->num_trim_entries == 0)
        return;

    datalen = (ULONG)sector_align(sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES), sizeof(uint64_t)) + (dev->num_trim_entries * sizeof(DEVICE_DATA_SET_RANGE));

    dmdsa = ExAllocatePoolWithTag(PagedPool, datalen, ALLOC_TAG);
    if (!dmdsa) {
        ERR("out of memory\n");
        goto end;
    }

    dmdsa->Size = sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES);
    dmdsa->Action = DeviceDsmAction_Trim;
    dmdsa->Flags = DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED;
    dmdsa->ParameterBlockOffset = 0;
    dmdsa->ParameterBlockLength = 0;
    dmdsa->DataSetRangesOffset = (ULONG)sector_align(sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES), sizeof(uint64_t));
    dmdsa->DataSetRangesLength = dev->num_trim_entries * sizeof(DEVICE_DATA_SET_RANGE);

    ranges = (DEVICE_DATA_SET_RANGE*)((uint8_t*)dmdsa + dmdsa->DataSetRangesOffset);

    i = 0;
    le = dev->trim_list.Flink;
    while (le != &dev->trim_list) {
        space* s = CONTAINING_RECORD(le, space, list_entry);

        ranges[i].StartingOffset = s->address;
        ranges[i].LengthInBytes = s->size;
        i++;

        le = le->Flink;
    }

    Status = dev_ioctl(dev->devobj, IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES, dmdsa, datalen, NULL, 0, true, NULL);
    if (!NT_SUCCESS(Status))
        WARN("IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES returned %08lx\n", Status);

    ExFreePool(dmdsa);

end:
    while (!IsListEmpty(&dev->trim_list)) {
        space* s = CONTAINING_RECORD(RemoveHeadList(&dev->trim_list), space, list_entry);
        ExFreePool(s);
    }

    dev->num_trim_entries = 0;
}
