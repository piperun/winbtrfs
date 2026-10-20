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

/* Volume identity, geometry, writability and passthrough IOCTL handlers. */
#include "btrfs_drv.h"
#include <mountdev.h>
#include <ntddvol.h>
#include <ntddstor.h>
#include <ntdddisk.h>

#define IOCTL_VOLUME_IS_DYNAMIC CTL_CODE(IOCTL_VOLUME_BASE, 18, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VOLUME_POST_ONLINE CTL_CODE(IOCTL_VOLUME_BASE, 25, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

static NTSTATUS vol_query_device_name(volume_device_extension* vde, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PMOUNTDEV_NAME name;

    if (IrpSp->FileObject && IrpSp->FileObject->FsContext)
        return STATUS_INVALID_PARAMETER;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_NAME)) {
        Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
        return STATUS_BUFFER_TOO_SMALL;
    }

    name = Irp->AssociatedIrp.SystemBuffer;
    name->NameLength = vde->name.Length;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < offsetof(MOUNTDEV_NAME, Name[0]) + name->NameLength) {
        Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
        return STATUS_BUFFER_OVERFLOW;
    }

    RtlCopyMemory(name->Name, vde->name.Buffer, vde->name.Length);

    Irp->IoStatus.Information = offsetof(MOUNTDEV_NAME, Name[0]) + name->NameLength;

    return STATUS_SUCCESS;
}

static NTSTATUS vol_query_unique_id(volume_device_extension* vde, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    MOUNTDEV_UNIQUE_ID* mduid;
    pdo_device_extension* pdode;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_UNIQUE_ID)) {
        Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
        return STATUS_BUFFER_TOO_SMALL;
    }

    mduid = Irp->AssociatedIrp.SystemBuffer;
    mduid->UniqueIdLength = BTRFS_UUID_SIZE;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < offsetof(MOUNTDEV_UNIQUE_ID, UniqueId[0]) + mduid->UniqueIdLength) {
        Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
        return STATUS_BUFFER_OVERFLOW;
    }

    if (!vde->pdo)
        return STATUS_INVALID_PARAMETER;

    pdode = vde->pdode;

    RtlCopyMemory(mduid->UniqueId, &pdode->uuid, BTRFS_UUID_SIZE);

    Irp->IoStatus.Information = offsetof(MOUNTDEV_UNIQUE_ID, UniqueId[0]) + mduid->UniqueIdLength;

    return STATUS_SUCCESS;
}

static NTSTATUS vol_is_dynamic(PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    uint8_t* buf;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength == 0 || !Irp->AssociatedIrp.SystemBuffer)
        return STATUS_INVALID_PARAMETER;

    buf = (uint8_t*)Irp->AssociatedIrp.SystemBuffer;

    *buf = 1;

    Irp->IoStatus.Information = 1;

    return STATUS_SUCCESS;
}

static NTSTATUS vol_check_verify(volume_device_extension* vde) {
    pdo_device_extension* pdode = vde->pdode;
    NTSTATUS Status;
    LIST_ENTRY* le;

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    le = pdode->children.Flink;
    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

        Status = dev_ioctl(vc->devobj, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, false, NULL);
        if (!NT_SUCCESS(Status))
            goto end;

        le = le->Flink;
    }

    Status = STATUS_SUCCESS;

end:
    ExReleaseResourceLite(&pdode->child_lock);

    return Status;
}

static NTSTATUS vol_get_disk_extents(volume_device_extension* vde, PIRP Irp) {
    pdo_device_extension* pdode = vde->pdode;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    LIST_ENTRY* le;
    ULONG num_extents = 0, i, max_extents = 1;
    NTSTATUS Status;
    VOLUME_DISK_EXTENTS *ext, *ext3;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(VOLUME_DISK_EXTENTS))
        return STATUS_BUFFER_TOO_SMALL;

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    le = pdode->children.Flink;
    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);
        VOLUME_DISK_EXTENTS ext2;

        Status = dev_ioctl(vc->devobj, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext2, sizeof(VOLUME_DISK_EXTENTS), false, NULL);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW) {
            ERR("IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS returned %08lx\n", Status);
            goto end;
        }

        num_extents += ext2.NumberOfDiskExtents;

        if (ext2.NumberOfDiskExtents > max_extents)
            max_extents = ext2.NumberOfDiskExtents;

        le = le->Flink;
    }

    ext = Irp->AssociatedIrp.SystemBuffer;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < offsetof(VOLUME_DISK_EXTENTS, Extents[0]) + (num_extents * sizeof(DISK_EXTENT))) {
        Irp->IoStatus.Information = offsetof(VOLUME_DISK_EXTENTS, Extents[0]);
        ext->NumberOfDiskExtents = num_extents;
        Status = STATUS_BUFFER_OVERFLOW;
        goto end;
    }

    ext3 = ExAllocatePoolWithTag(PagedPool, offsetof(VOLUME_DISK_EXTENTS, Extents[0]) + (max_extents * sizeof(DISK_EXTENT)), ALLOC_TAG);
    if (!ext3) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    i = 0;
    ext->NumberOfDiskExtents = 0;

    le = pdode->children.Flink;
    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

        Status = dev_ioctl(vc->devobj, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, ext3,
                           (ULONG)offsetof(VOLUME_DISK_EXTENTS, Extents[0]) + (max_extents * sizeof(DISK_EXTENT)), false, NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS returned %08lx\n", Status);
            ExFreePool(ext3);
            goto end;
        }

        if (i + ext3->NumberOfDiskExtents > num_extents) {
            Irp->IoStatus.Information = offsetof(VOLUME_DISK_EXTENTS, Extents[0]);
            ext->NumberOfDiskExtents = i + ext3->NumberOfDiskExtents;
            Status = STATUS_BUFFER_OVERFLOW;
            ExFreePool(ext3);
            goto end;
        }

        RtlCopyMemory(&ext->Extents[i], ext3->Extents, sizeof(DISK_EXTENT) * ext3->NumberOfDiskExtents);
        i += ext3->NumberOfDiskExtents;

        le = le->Flink;
    }

    ExFreePool(ext3);

    Status = STATUS_SUCCESS;

    ext->NumberOfDiskExtents = i;
    Irp->IoStatus.Information = offsetof(VOLUME_DISK_EXTENTS, Extents[0]) + (i * sizeof(DISK_EXTENT));

end:
    ExReleaseResourceLite(&pdode->child_lock);

    return Status;
}

static NTSTATUS vol_is_writable(volume_device_extension* vde) {
    pdo_device_extension* pdode = vde->pdode;
    NTSTATUS Status;
    LIST_ENTRY* le;
    bool writable = false;

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    le = pdode->children.Flink;
    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

        Status = dev_ioctl(vc->devobj, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, true, NULL);

        if (NT_SUCCESS(Status)) {
            writable = true;
            break;
        } else if (Status != STATUS_MEDIA_WRITE_PROTECTED)
            goto end;

        le = le->Flink;
    }

    Status = writable ? STATUS_SUCCESS : STATUS_MEDIA_WRITE_PROTECTED;

end:
    ExReleaseResourceLite(&pdode->child_lock);

    return STATUS_SUCCESS;
}

static NTSTATUS vol_get_length(volume_device_extension* vde, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    pdo_device_extension* pdode = vde->pdode;
    GET_LENGTH_INFORMATION* gli;
    LIST_ENTRY* le;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(GET_LENGTH_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    gli = (GET_LENGTH_INFORMATION*)Irp->AssociatedIrp.SystemBuffer;

    gli->Length.QuadPart = 0;

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    le = pdode->children.Flink;
    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

        gli->Length.QuadPart += vc->size;

        le = le->Flink;
    }

    ExReleaseResourceLite(&pdode->child_lock);

    Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);

    return STATUS_SUCCESS;
}

static NTSTATUS vol_get_drive_geometry(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    volume_device_extension* vde = DeviceObject->DeviceExtension;
    pdo_device_extension* pdode = vde->pdode;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    DISK_GEOMETRY* geom;
    uint64_t length;
    LIST_ENTRY* le;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DISK_GEOMETRY))
        return STATUS_BUFFER_TOO_SMALL;

    length = 0;

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    le = pdode->children.Flink;
    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

        length += vc->size;

        le = le->Flink;
    }

    ExReleaseResourceLite(&pdode->child_lock);

    geom = (DISK_GEOMETRY*)Irp->AssociatedIrp.SystemBuffer;
    geom->BytesPerSector = DeviceObject->SectorSize == 0 ? 0x200 : DeviceObject->SectorSize;
    geom->SectorsPerTrack = 0x3f;
    geom->TracksPerCylinder = 0xff;
    geom->Cylinders.QuadPart = length / (UInt32x32To64(geom->TracksPerCylinder, geom->SectorsPerTrack) * geom->BytesPerSector);
    geom->MediaType = DeviceObject->Characteristics & FILE_REMOVABLE_MEDIA ? RemovableMedia : FixedMedia;

    Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);

    return STATUS_SUCCESS;
}

static NTSTATUS vol_get_gpt_attributes(PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    VOLUME_GET_GPT_ATTRIBUTES_INFORMATION* vggai;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    vggai = (VOLUME_GET_GPT_ATTRIBUTES_INFORMATION*)Irp->AssociatedIrp.SystemBuffer;

    vggai->GptAttributes = 0;

    Irp->IoStatus.Information = sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION);

    return STATUS_SUCCESS;
}

static NTSTATUS vol_get_device_number(volume_device_extension* vde, PIRP Irp) {
    pdo_device_extension* pdode = vde->pdode;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    volume_child* vc;
    STORAGE_DEVICE_NUMBER* sdn;

    // If only one device, return its disk number. This is needed for ejection to work.

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(STORAGE_DEVICE_NUMBER))
        return STATUS_BUFFER_TOO_SMALL;

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    if (IsListEmpty(&pdode->children) || pdode->num_children > 1) {
        ExReleaseResourceLite(&pdode->child_lock);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    vc = CONTAINING_RECORD(pdode->children.Flink, volume_child, list_entry);

    if (vc->disk_num == 0xffffffff) {
        ExReleaseResourceLite(&pdode->child_lock);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    sdn = (STORAGE_DEVICE_NUMBER*)Irp->AssociatedIrp.SystemBuffer;

    sdn->DeviceType = FILE_DEVICE_DISK;
    sdn->DeviceNumber = vc->disk_num;
    sdn->PartitionNumber = vc->part_num;

    ExReleaseResourceLite(&pdode->child_lock);

    Irp->IoStatus.Information = sizeof(STORAGE_DEVICE_NUMBER);

    return STATUS_SUCCESS;
}

_Function_class_(IO_COMPLETION_ROUTINE)
static NTSTATUS __stdcall vol_ioctl_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr) {
    KEVENT* event = conptr;

    UNUSED(DeviceObject);
    UNUSED(Irp);

    KeSetEvent(event, 0, false);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS vol_ioctl_passthrough(volume_device_extension* vde, PIRP Irp) {
    NTSTATUS Status;
    volume_child* vc;
    PIRP Irp2;
    PIO_STACK_LOCATION IrpSp, IrpSp2;
    KEVENT Event;
    pdo_device_extension* pdode = vde->pdode;

    TRACE("(%p, %p)\n", vde, Irp);

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    if (IsListEmpty(&pdode->children)) {
        ExReleaseResourceLite(&pdode->child_lock);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    vc = CONTAINING_RECORD(pdode->children.Flink, volume_child, list_entry);

    if (vc->list_entry.Flink != &pdode->children) { // more than one device
        ExReleaseResourceLite(&pdode->child_lock);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp2 = IoAllocateIrp(vc->devobj->StackSize, false);

    if (!Irp2) {
        ERR("IoAllocateIrp failed\n");
        ExReleaseResourceLite(&pdode->child_lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IrpSp = IoGetCurrentIrpStackLocation(Irp);
    IrpSp2 = IoGetNextIrpStackLocation(Irp2);

    IrpSp2->MajorFunction = IrpSp->MajorFunction;
    IrpSp2->MinorFunction = IrpSp->MinorFunction;
    IrpSp2->FileObject = vc->fileobj;

    IrpSp2->Parameters.DeviceIoControl.OutputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    IrpSp2->Parameters.DeviceIoControl.InputBufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    IrpSp2->Parameters.DeviceIoControl.IoControlCode = IrpSp->Parameters.DeviceIoControl.IoControlCode;
    IrpSp2->Parameters.DeviceIoControl.Type3InputBuffer = IrpSp->Parameters.DeviceIoControl.Type3InputBuffer;

    Irp2->AssociatedIrp.SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    Irp2->MdlAddress = Irp->MdlAddress;
    Irp2->UserBuffer = Irp->UserBuffer;
    Irp2->Flags = Irp->Flags;

    KeInitializeEvent(&Event, NotificationEvent, false);

    IoSetCompletionRoutine(Irp2, vol_ioctl_completion, &Event, true, true, true);

    Status = IoCallDriver(vc->devobj, Irp2);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, false, NULL);
        Status = Irp2->IoStatus.Status;
    }

    Irp->IoStatus.Status = Irp2->IoStatus.Status;
    Irp->IoStatus.Information = Irp2->IoStatus.Information;

    ExReleaseResourceLite(&pdode->child_lock);

    IoFreeIrp(Irp2);

    return Status;
}

static NTSTATUS vol_query_stable_guid(volume_device_extension* vde, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    MOUNTDEV_STABLE_GUID* mdsg;
    pdo_device_extension* pdode;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_STABLE_GUID)) {
        Irp->IoStatus.Information = sizeof(MOUNTDEV_STABLE_GUID);
        return STATUS_BUFFER_TOO_SMALL;
    }

    mdsg = Irp->AssociatedIrp.SystemBuffer;

    if (!vde->pdo)
        return STATUS_INVALID_PARAMETER;

    pdode = vde->pdode;

    RtlCopyMemory(&mdsg->StableGuid, &pdode->uuid, BTRFS_UUID_SIZE);

    Irp->IoStatus.Information = sizeof(MOUNTDEV_STABLE_GUID);

    return STATUS_SUCCESS;
}

NTSTATUS vol_device_control(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    volume_device_extension* vde = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    TRACE("(%p, %p)\n", DeviceObject, Irp);

    Irp->IoStatus.Information = 0;

    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
            return vol_query_device_name(vde, Irp);

        case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
            return vol_query_unique_id(vde, Irp);

        case IOCTL_STORAGE_GET_DEVICE_NUMBER:
            return vol_get_device_number(vde, Irp);

        case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
            TRACE("unhandled control code IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME\n");
            break;

        case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
            return vol_query_stable_guid(vde, Irp);

        case IOCTL_MOUNTDEV_LINK_CREATED:
            TRACE("unhandled control code IOCTL_MOUNTDEV_LINK_CREATED\n");
            break;

        case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
            return vol_get_gpt_attributes(Irp);

        case IOCTL_VOLUME_IS_DYNAMIC:
            return vol_is_dynamic(Irp);

        case IOCTL_VOLUME_ONLINE:
            Irp->IoStatus.Information = 0;
            return STATUS_SUCCESS;

        case IOCTL_VOLUME_POST_ONLINE:
            Irp->IoStatus.Information = 0;
            return STATUS_SUCCESS;

        case IOCTL_DISK_GET_DRIVE_GEOMETRY:
            return vol_get_drive_geometry(DeviceObject, Irp);

        case IOCTL_DISK_IS_WRITABLE:
            return vol_is_writable(vde);

        case IOCTL_DISK_GET_LENGTH_INFO:
            return vol_get_length(vde, Irp);

        case IOCTL_STORAGE_CHECK_VERIFY:
        case IOCTL_DISK_CHECK_VERIFY:
            return vol_check_verify(vde);

        case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS:
            return vol_get_disk_extents(vde, Irp);

        default: { // pass ioctl through if only one child device
            NTSTATUS Status = vol_ioctl_passthrough(vde, Irp);
#ifdef _DEBUG
            ULONG code = IrpSp->Parameters.DeviceIoControl.IoControlCode;

            if (NT_SUCCESS(Status))
                TRACE("passing through ioctl %lx (returning %08lx)\n", code, Status);
            else
                WARN("passing through ioctl %lx (returning %08lx)\n", code, Status);
#endif

            return Status;
        }
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
