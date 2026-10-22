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

/* FSCTL_BTRFS_ADD_DEVICE: adding a device to a mounted volume. */
#include "add-device.h"
#include <ntddstor.h>
#include <ntdddisk.h>

extern LIST_ENTRY VcbList;
extern ERESOURCE global_loading_lock;
extern PDRIVER_OBJECT drvobj;

static NTSTATUS is_device_part_of_mounted_btrfs_raid(PDEVICE_OBJECT devobj, PFILE_OBJECT fileobj) {
    NTSTATUS Status;
    ULONG to_read;
    struct btrfs_super_block* sb;
    uint8_t fsuuid[BTRFS_UUID_SIZE], devuuid[BTRFS_UUID_SIZE];
    LIST_ENTRY* le;

    to_read = devobj->SectorSize == 0 ? sizeof(struct btrfs_super_block) : (ULONG)sector_align(sizeof(struct btrfs_super_block), devobj->SectorSize);

    sb = ExAllocatePoolWithTag(PagedPool, to_read, ALLOC_TAG);
    if (!sb) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = sync_read_phys(devobj, fileobj, superblock_addrs[0], to_read, (uint8_t*)sb, true);
    if (!NT_SUCCESS(Status)) {
        ERR("sync_read_phys returned %08lx\n", Status);
        ExFreePool(sb);
        return Status;
    }

    if (sb->magic != BTRFS_MAGIC) {
        TRACE("device is not Btrfs\n");
        ExFreePool(sb);
        return STATUS_SUCCESS;
    }

    if (!check_superblock_checksum(sb)) {
        TRACE("device has Btrfs magic, but invalid superblock checksum\n");
        ExFreePool(sb);
        return STATUS_SUCCESS;
    }

    memcpy(fsuuid, sb->fsid, BTRFS_UUID_SIZE);
    memcpy(devuuid, sb->dev_item.uuid, BTRFS_UUID_SIZE);

    ExFreePool(sb);

    ExAcquireResourceSharedLite(&global_loading_lock, true);

    le = VcbList.Flink;

    while (le != &VcbList) {
        device_extension* Vcb = CONTAINING_RECORD(le, device_extension, list_entry);

        if (RtlCompareMemory(&Vcb->superblock.fsid, &fsuuid, BTRFS_UUID_SIZE) == BTRFS_UUID_SIZE) {
            LIST_ENTRY* le2;

            ExAcquireResourceSharedLite(&Vcb->tree_lock, true);

            if (Vcb->superblock.num_devices > 1) {
                le2 = Vcb->devices.Flink;
                while (le2 != &Vcb->devices) {
                    device* dev = CONTAINING_RECORD(le2, device, list_entry);

                    if (RtlCompareMemory(&dev->devitem.uuid, &devuuid, BTRFS_UUID_SIZE) == BTRFS_UUID_SIZE) {
                        ExReleaseResourceLite(&Vcb->tree_lock);
                        ExReleaseResourceLite(&global_loading_lock);
                        return STATUS_DEVICE_NOT_READY;
                    }

                    le2 = le2->Flink;
                }
            }

            ExReleaseResourceLite(&Vcb->tree_lock);
            ExReleaseResourceLite(&global_loading_lock);
            return STATUS_SUCCESS;
        }

        le = le->Flink;
    }

    ExReleaseResourceLite(&global_loading_lock);

    return STATUS_SUCCESS;
}

NTSTATUS add_device(device_extension* Vcb, PIRP Irp, KPROCESSOR_MODE processor_mode) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;
    PFILE_OBJECT fileobj, mountmgrfo;
    PDEVICE_OBJECT DeviceObject;
    HANDLE h;
    LIST_ENTRY* le;
    device* dev;
    struct btrfs_dev_item* di;
    uint64_t dev_id, size;
    uint8_t* mb;
    uint64_t* stats;
    UNICODE_STRING mmdevpath, pnp_name, pnp_name2;
    volume_child* vc;
    PDEVICE_OBJECT mountmgr;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    STORAGE_DEVICE_NUMBER sdn;
    volume_device_extension* vde;
    pdo_device_extension* pdode;
    const GUID* pnp_guid;
    GET_LENGTH_INFORMATION gli;

    pnp_name.Buffer = NULL;

    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), processor_mode))
        return STATUS_PRIVILEGE_NOT_HELD;

    if (Vcb->direct_mount || !Vcb->vde) {
        WARN("not allowing second device to be added to direct-mount device\n");
        return STATUS_NOT_SUPPORTED;
    }

    if (Vcb->readonly) // FIXME - handle adding R/W device to seeding device
        return STATUS_MEDIA_WRITE_PROTECTED;

#if defined(_WIN64)
    if (IoIs32bitProcess(Irp)) {
        if (IrpSp->Parameters.FileSystemControl.InputBufferLength != sizeof(uint32_t))
            return STATUS_INVALID_PARAMETER;

        h = (HANDLE)LongToHandle((*(uint32_t*)Irp->AssociatedIrp.SystemBuffer));
    } else {
#endif
        if (IrpSp->Parameters.FileSystemControl.InputBufferLength != sizeof(HANDLE))
            return STATUS_INVALID_PARAMETER;

        h = *(PHANDLE)Irp->AssociatedIrp.SystemBuffer;
#if defined(_WIN64)
    }
#endif

    Status = ObReferenceObjectByHandle(h, 0, *IoFileObjectType, Irp->RequestorMode, (void**)&fileobj, NULL);

    if (!NT_SUCCESS(Status)) {
        ERR("ObReferenceObjectByHandle returned %08lx\n", Status);
        return Status;
    }

    DeviceObject = fileobj->DeviceObject;

    Status = get_device_pnp_name(DeviceObject, &pnp_name, &pnp_guid);
    if (!NT_SUCCESS(Status)) {
        ERR("get_device_pnp_name returned %08lx\n", Status);
        ObDereferenceObject(fileobj);
        return Status;
    }

    // If this is a disk, we have been handed the PDO, so need to go up to find something we can use
    if (RtlCompareMemory(pnp_guid, &GUID_DEVINTERFACE_DISK, sizeof(GUID)) == sizeof(GUID) && DeviceObject->AttachedDevice)
        DeviceObject = DeviceObject->AttachedDevice;

    Status = dev_ioctl(DeviceObject, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, true, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("IOCTL_DISK_IS_WRITABLE returned %08lx\n", Status);
        ObDereferenceObject(fileobj);
        return Status;
    }

    Status = is_device_part_of_mounted_btrfs_raid(DeviceObject, fileobj);
    if (!NT_SUCCESS(Status)) {
        ERR("is_device_part_of_mounted_btrfs_raid returned %08lx\n", Status);
        ObDereferenceObject(fileobj);
        return Status;
    }

    // if disk, check it has no partitions
    if (RtlCompareMemory(pnp_guid, &GUID_DEVINTERFACE_DISK, sizeof(GUID)) == sizeof(GUID)) {
        ULONG dlisize;
        DRIVE_LAYOUT_INFORMATION_EX* dli = NULL;

        dlisize = 0;

        do {
            dlisize += 1024;

            if (dli)
                ExFreePool(dli);

            dli = ExAllocatePoolWithTag(PagedPool, dlisize, ALLOC_TAG);
            if (!dli) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end2;
            }

            Status = dev_ioctl(DeviceObject, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, dli, dlisize, true, NULL);
        } while (Status == STATUS_BUFFER_TOO_SMALL);

        if (NT_SUCCESS(Status) && dli->PartitionCount > 0) {
            ExFreePool(dli);
            ERR("not adding disk which has partitions\n");
            Status = STATUS_DEVICE_NOT_READY;
            goto end2;
        }

        ExFreePool(dli);
    }

    Status = dev_ioctl(DeviceObject, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                       &sdn, sizeof(STORAGE_DEVICE_NUMBER), true, NULL);
    if (NT_SUCCESS(Status)) {
        if (sdn.DeviceType != FILE_DEVICE_DISK) { // FIXME - accept floppies and CDs?
            WARN("device was not disk\n");
            ObDereferenceObject(fileobj);
            return STATUS_INVALID_PARAMETER;
        }
    } else {
        sdn.DeviceNumber = 0xffffffff;
        sdn.PartitionNumber = 0xffffffff;
    }

    Status = dev_ioctl(DeviceObject, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                        &gli, sizeof(gli), true, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("error reading length information: %08lx\n", Status);
        ObDereferenceObject(fileobj);
        return Status;
    }

    size = gli.Length.QuadPart;

    if (size < 0x100000) {
        ERR("device was not large enough to hold FS (%I64x bytes, need at least 1 MB)\n", size);
        ObDereferenceObject(fileobj);
        return STATUS_INTERNAL_ERROR;
    }

    volume_removal(&pnp_name);

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    if (Vcb->need_write)
        Status = do_write(Vcb, Irp);
    else
        Status = STATUS_SUCCESS;

    free_trees(Vcb);

    if (!NT_SUCCESS(Status)) {
        ERR("do_write returned %08lx\n", Status);
        goto end;
    }

    dev = ExAllocatePoolWithTag(NonPagedPool, sizeof(device), ALLOC_TAG);
    if (!dev) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    RtlZeroMemory(dev, sizeof(device));

    dev->devobj = DeviceObject;
    dev->fileobj = fileobj;
    dev->seeding = false;
    init_device(Vcb, dev, true);

    InitializeListHead(&dev->space);

    if (size > 0x100000) { // add disk hole - the first MB is marked as used
        Status = add_space_entry(&dev->space, NULL, 0x100000, size - 0x100000);
        if (!NT_SUCCESS(Status)) {
            ERR("add_space_entry returned %08lx\n", Status);
            goto end;
        }
    }

    dev_id = 0;

    le = Vcb->devices.Flink;
    while (le != &Vcb->devices) {
        device* dev2 = CONTAINING_RECORD(le, device, list_entry);

        if (dev2->devitem.devid > dev_id)
            dev_id = dev2->devitem.devid;

        le = le->Flink;
    }

    dev_id++;

    dev->devitem.devid = dev_id;
    dev->devitem.total_bytes = size;
    dev->devitem.bytes_used = 0;
    dev->devitem.io_align = Vcb->superblock.sectorsize;
    dev->devitem.io_width = Vcb->superblock.sectorsize;
    dev->devitem.sector_size = Vcb->superblock.sectorsize;
    dev->devitem.type = 0;
    dev->devitem.generation = 0;
    dev->devitem.start_offset = 0;
    dev->devitem.dev_group = 0;
    dev->devitem.seek_speed = 0;
    dev->devitem.bandwidth = 0;
    get_uuid(dev->devitem.uuid);
    memcpy(dev->devitem.fsid, Vcb->superblock.fsid, BTRFS_UUID_SIZE);

    di = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_dev_item), ALLOC_TAG);
    if (!di) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    RtlCopyMemory(di, &dev->devitem, sizeof(struct btrfs_dev_item));

    Status = insert_tree_item(Vcb, Vcb->chunk_root, 1, BTRFS_DEV_ITEM_KEY, di->devid, di, sizeof(struct btrfs_dev_item), NULL, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item returned %08lx\n", Status);
        ExFreePool(di);
        goto end;
    }

    // add stats entry to dev tree
    stats = ExAllocatePoolWithTag(PagedPool, sizeof(uint64_t) * 5, ALLOC_TAG);
    if (!stats) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    RtlZeroMemory(stats, sizeof(uint64_t) * 5);

    searchkey.objectid = 0;
    searchkey.type = BTRFS_PERSISTENT_ITEM_KEY;
    searchkey.offset = di->devid;

    Status = find_item(Vcb, Vcb->dev_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        ExFreePool(stats);
        goto end;
    }

    if (!keycmp(tp.item->key, searchkey)) {
        Status = delete_tree_item(Vcb, &tp);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_tree_item returned %08lx\n", Status);
            ExFreePool(stats);
            goto end;
        }
    }

    Status = insert_tree_item(Vcb, Vcb->dev_root, 0, BTRFS_PERSISTENT_ITEM_KEY, di->devid, stats, sizeof(uint64_t) * 5, NULL, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("insert_tree_item returned %08lx\n", Status);
        ExFreePool(stats);
        goto end;
    }

    if (dev->trim && !dev->readonly && !Vcb->options.no_trim)
        trim_whole_device(dev);

    // We clear the first megabyte of the device, so Windows doesn't identify it as another FS
    mb = ExAllocatePoolWithTag(PagedPool, 0x100000, ALLOC_TAG);
    if (!mb) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    RtlZeroMemory(mb, 0x100000);

    Status = write_data_phys(DeviceObject, fileobj, 0, mb, 0x100000);
    if (!NT_SUCCESS(Status)) {
        ERR("write_data_phys returned %08lx\n", Status);
        ExFreePool(mb);
        goto end;
    }

    ExFreePool(mb);

    vde = Vcb->vde;
    pdode = vde->pdode;

    vc = ExAllocatePoolWithTag(NonPagedPool, sizeof(volume_child), ALLOC_TAG);
    if (!vc) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    memcpy(vc->uuid, dev->devitem.uuid, BTRFS_UUID_SIZE);
    vc->devid = dev_id;
    vc->generation = Vcb->superblock.generation;
    vc->devobj = DeviceObject;
    vc->fileobj = fileobj;
    vc->notification_entry = NULL;
    vc->boot_volume = false;

    Status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange, 0, fileobj,
                                            drvobj, pnp_removal, vde->pdode, &vc->notification_entry);
    if (!NT_SUCCESS(Status))
        WARN("IoRegisterPlugPlayNotification returned %08lx\n", Status);

    pnp_name2 = pnp_name;

    if (pnp_name.Length > 4 * sizeof(WCHAR) && pnp_name.Buffer[0] == '\\' && (pnp_name.Buffer[1] == '\\' || pnp_name.Buffer[1] == '?') &&
        pnp_name.Buffer[2] == '?' && pnp_name.Buffer[3] == '\\') {
        pnp_name2.Buffer = &pnp_name2.Buffer[3];
        pnp_name2.Length -= 3 * sizeof(WCHAR);
        pnp_name2.MaximumLength -= 3 * sizeof(WCHAR);
    }

    vc->pnp_name.Length = vc->pnp_name.MaximumLength = pnp_name2.Length;

    if (pnp_name2.Length == 0)
        vc->pnp_name.Buffer = NULL;
    else {
        vc->pnp_name.Buffer = ExAllocatePoolWithTag(PagedPool, pnp_name2.Length, ALLOC_TAG);
        if (!vc->pnp_name.Buffer) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        RtlCopyMemory(vc->pnp_name.Buffer, pnp_name2.Buffer, pnp_name2.Length);
    }

    vc->size = size;
    vc->seeding = false;
    vc->disk_num = sdn.DeviceNumber;
    vc->part_num = sdn.PartitionNumber;
    vc->had_drive_letter = false;

    ExAcquireResourceExclusiveLite(&pdode->child_lock, true);
    InsertTailList(&pdode->children, &vc->list_entry);
    pdode->num_children++;
    pdode->children_loaded++;
    ExReleaseResourceLite(&pdode->child_lock);

    RtlInitUnicodeString(&mmdevpath, MOUNTMGR_DEVICE_NAME);
    Status = IoGetDeviceObjectPointer(&mmdevpath, FILE_READ_ATTRIBUTES, &mountmgrfo, &mountmgr);
    if (!NT_SUCCESS(Status))
        ERR("IoGetDeviceObjectPointer returned %08lx\n", Status);
    else {
        Status = remove_drive_letter(mountmgr, &pnp_name);
        if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND)
            WARN("remove_drive_letter returned %08lx\n", Status);

        vc->had_drive_letter = NT_SUCCESS(Status);

        ObDereferenceObject(mountmgrfo);
    }

    Vcb->superblock.num_devices++;
    Vcb->superblock.total_bytes += size;
    Vcb->devices_loaded++;
    InsertTailList(&Vcb->devices, &dev->list_entry);

    // FIXME - send notification that volume size has increased

    ObReferenceObject(DeviceObject); // for Vcb

    Status = do_write(Vcb, Irp);
    if (!NT_SUCCESS(Status))
        ERR("do_write returned %08lx\n", Status);

    ObReferenceObject(fileobj);

end:
    free_trees(Vcb);

    ExReleaseResourceLite(&Vcb->tree_lock);

end2:
    ObDereferenceObject(fileobj);

    if (pnp_name.Buffer)
        ExFreePool(pnp_name.Buffer);

    if (NT_SUCCESS(Status))
        FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_CHANGE_SIZE);

    return Status;
}
