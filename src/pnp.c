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
#include "volume-dismount.h"

extern ERESOURCE pdo_list_lock;
extern LIST_ENTRY pdo_list;

NTSTATUS pnp_query_remove_device(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    device_extension* Vcb = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    // We might be going away imminently - do a flush so we're not caught out

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    if (Vcb->root_fileref && Vcb->root_fileref->fcb && (Vcb->root_fileref->open_count > 0 || has_open_children(Vcb->root_fileref))) {
        ExReleaseResourceLite(&Vcb->tree_lock);
        return STATUS_ACCESS_DENIED;
    }

    if (Vcb->need_write && !Vcb->readonly) {
        Status = do_write(Vcb, Irp);

        free_trees(Vcb);

        if (!NT_SUCCESS(Status)) {
            ERR("do_write returned %08lx\n", Status);
            ExReleaseResourceLite(&Vcb->tree_lock);
            return Status;
        }
    }

    ExReleaseResourceLite(&Vcb->tree_lock);

    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS bus_query_capabilities(PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_CAPABILITIES dc = IrpSp->Parameters.DeviceCapabilities.Capabilities;

    dc->UniqueID = true;
    dc->SilentInstall = true;

    return STATUS_SUCCESS;
}

static NTSTATUS bus_query_device_relations(PIRP Irp) {
    NTSTATUS Status;
    ULONG num_children;
    LIST_ENTRY* le;
    ULONG drsize, i;
    DEVICE_RELATIONS* dr;

    ExAcquireResourceSharedLite(&pdo_list_lock, true);

    num_children = 0;

    le = pdo_list.Flink;
    while (le != &pdo_list) {
        pdo_device_extension* pdode = CONTAINING_RECORD(le, pdo_device_extension, list_entry);

        if (!pdode->dont_report)
            num_children++;

        le = le->Flink;
    }

    drsize = offsetof(DEVICE_RELATIONS, Objects[0]) + (num_children * sizeof(PDEVICE_OBJECT));
    dr = ExAllocatePoolWithTag(PagedPool, drsize, ALLOC_TAG);

    if (!dr) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    dr->Count = num_children;

    i = 0;
    le = pdo_list.Flink;
    while (le != &pdo_list) {
        pdo_device_extension* pdode = CONTAINING_RECORD(le, pdo_device_extension, list_entry);

        if (!pdode->dont_report) {
            ObReferenceObject(pdode->pdo);
            dr->Objects[i] = pdode->pdo;
            i++;
        }

        le = le->Flink;
    }

    Irp->IoStatus.Information = (ULONG_PTR)dr;

    Status = STATUS_SUCCESS;

end:
    ExReleaseResourceLite(&pdo_list_lock);

    return Status;
}

static NTSTATUS bus_query_hardware_ids(PIRP Irp) {
    WCHAR* out;

    static const WCHAR ids[] = L"ROOT\\btrfs\0";

    out = ExAllocatePoolWithTag(PagedPool, sizeof(ids), ALLOC_TAG);
    if (!out) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(out, ids, sizeof(ids));

    Irp->IoStatus.Information = (ULONG_PTR)out;

    return STATUS_SUCCESS;
}

static NTSTATUS bus_pnp(bus_device_extension* bde, PIRP Irp) {
    NTSTATUS Status = Irp->IoStatus.Status;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    bool handled = false;

    switch (IrpSp->MinorFunction) {
        case IRP_MN_START_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            Status = STATUS_SUCCESS;
            handled = true;
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE:
            Status = STATUS_UNSUCCESSFUL;
            handled = true;
            break;

        case IRP_MN_QUERY_CAPABILITIES:
            Status = bus_query_capabilities(Irp);
            handled = true;
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (IrpSp->Parameters.QueryDeviceRelations.Type != BusRelations || no_pnp)
                break;

            Status = bus_query_device_relations(Irp);
            handled = true;
            break;

        case IRP_MN_QUERY_ID:
            if (IrpSp->Parameters.QueryId.IdType != BusQueryHardwareIDs)
                break;

            Status = bus_query_hardware_ids(Irp);
            handled = true;
            break;
    }

    if (!NT_SUCCESS(Status) && handled) {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return Status;
    }

    Irp->IoStatus.Status = Status;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(bde->attached_device, Irp);
}

static NTSTATUS pdo_query_device_id(pdo_device_extension* pdode, PIRP Irp) {
    WCHAR name[100], *noff, *out;
    int i;

    static const WCHAR pref[] = L"Btrfs\\";

    RtlCopyMemory(name, pref, sizeof(pref) - sizeof(WCHAR));

    noff = &name[(sizeof(pref) / sizeof(WCHAR)) - 1];
    for (i = 0; i < 16; i++) {
        *noff = hex_digit(pdode->uuid[i] >> 4); noff++;
        *noff = hex_digit(pdode->uuid[i] & 0xf); noff++;

        if (i == 3 || i == 5 || i == 7 || i == 9) {
            *noff = '-';
            noff++;
        }
    }
    *noff = 0;

    out = ExAllocatePoolWithTag(PagedPool, (wcslen(name) + 1) * sizeof(WCHAR), ALLOC_TAG);
    if (!out) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(out, name, (wcslen(name) + 1) * sizeof(WCHAR));

    Irp->IoStatus.Information = (ULONG_PTR)out;

    return STATUS_SUCCESS;
}

static NTSTATUS pdo_query_hardware_ids(PIRP Irp) {
    WCHAR* out;

    static const WCHAR ids[] = L"BtrfsVolume\0";

    out = ExAllocatePoolWithTag(PagedPool, sizeof(ids), ALLOC_TAG);
    if (!out) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(out, ids, sizeof(ids));

    Irp->IoStatus.Information = (ULONG_PTR)out;

    return STATUS_SUCCESS;
}

static NTSTATUS pdo_query_id(pdo_device_extension* pdode, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->Parameters.QueryId.IdType) {
        case BusQueryDeviceID:
            TRACE("BusQueryDeviceID\n");
            return pdo_query_device_id(pdode, Irp);

        case BusQueryHardwareIDs:
            TRACE("BusQueryHardwareIDs\n");
            return pdo_query_hardware_ids(Irp);

        default:
            break;
    }

    return Irp->IoStatus.Status;
}

typedef struct {
    IO_STATUS_BLOCK iosb;
    KEVENT Event;
    NTSTATUS Status;
} device_usage_context;

_Function_class_(IO_COMPLETION_ROUTINE)
static NTSTATUS __stdcall device_usage_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr) {
    device_usage_context* context = conptr;

    UNUSED(DeviceObject);

    context->Status = Irp->IoStatus.Status;

    IoFreeIrp(Irp);

    KeSetEvent(&context->Event, 0, false);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS send_device_usage_notification(volume_child* vc, PIO_STACK_LOCATION IrpSp, bool in_path) {
    PIRP Irp;
    PIO_STACK_LOCATION next;
    device_usage_context context;
    NTSTATUS Status;

    Irp = IoAllocateIrp(vc->devobj->StackSize, false);
    if (!Irp) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    next = IoGetNextIrpStackLocation(Irp);
    next->MajorFunction = IRP_MJ_PNP;
    next->MinorFunction = IRP_MN_DEVICE_USAGE_NOTIFICATION;
    next->Parameters.UsageNotification = IrpSp->Parameters.UsageNotification;
    next->Parameters.UsageNotification.InPath = in_path;
    next->FileObject = vc->fileobj;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    context.iosb.Status = STATUS_SUCCESS;
    Irp->UserIosb = &context.iosb;

    KeInitializeEvent(&context.Event, NotificationEvent, false);
    Irp->UserEvent = &context.Event;
    IoSetCompletionRoutine(Irp, device_usage_completion, &context, true, true, true);

    Status = IoCallDriver(vc->devobj, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&context.Event, Executive, KernelMode, false, NULL);

    return context.Status;
}

static NTSTATUS pdo_device_usage_notification(pdo_device_extension* pdode, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    LIST_ENTRY* le;
    NTSTATUS Status = STATUS_SUCCESS;

    TRACE("(%p, %p)\n", pdode, Irp);

    ExAcquireResourceSharedLite(&pdode->child_lock, true);

    le = pdode->children.Flink;

    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

        if (vc->devobj) {
            Status = send_device_usage_notification(vc, IrpSp, IrpSp->Parameters.UsageNotification.InPath);
            if (!NT_SUCCESS(Status)) {
                LIST_ENTRY* rollback = le->Blink;

                ERR("device usage notification returned %08lx\n", Status);
                while (rollback != &pdode->children) {
                    volume_child* previous = CONTAINING_RECORD(rollback, volume_child, list_entry);

                    if (previous->devobj) {
                        NTSTATUS rollback_status = send_device_usage_notification(previous, IrpSp, !IrpSp->Parameters.UsageNotification.InPath);
                        if (!NT_SUCCESS(rollback_status))
                            ERR("device usage rollback returned %08lx\n", rollback_status);
                    }

                    rollback = rollback->Blink;
                }
                break;
            }
        }

        le = le->Flink;
    }

    ExReleaseResourceLite(&pdode->child_lock);

    return Status;
}

static NTSTATUS pdo_query_device_relations(PDEVICE_OBJECT pdo, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_RELATIONS device_relations;

    if (IrpSp->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
        return Irp->IoStatus.Status;

    device_relations = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), ALLOC_TAG);
    if (!device_relations) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    device_relations->Count = 1;
    device_relations->Objects[0] = pdo;

    ObReferenceObject(pdo);

    Irp->IoStatus.Information = (ULONG_PTR)device_relations;

    return STATUS_SUCCESS;
}

static NTSTATUS pdo_pnp(PDEVICE_OBJECT pdo, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    pdo_device_extension* pdode = pdo->DeviceExtension;

    switch (IrpSp->MinorFunction) {
        case IRP_MN_QUERY_ID:
            return pdo_query_id(pdode, Irp);

        case IRP_MN_START_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            return STATUS_SUCCESS;

        case IRP_MN_QUERY_REMOVE_DEVICE:
            return STATUS_UNSUCCESSFUL;

        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
            return pdo_device_usage_notification(pdode, Irp);

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            return pdo_query_device_relations(pdo, Irp);
    }

    return Irp->IoStatus.Status;
}

_Function_class_(IO_COMPLETION_ROUTINE)
static NTSTATUS __stdcall pnp_usage_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr) {
    device_extension* Vcb = conptr;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    UNUSED(DeviceObject);

    if (!NT_SUCCESS(Irp->IoStatus.Status))
        IoAdjustPagingPathCount(&Vcb->page_file_count, !IrpSp->Parameters.UsageNotification.InPath);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    return STATUS_SUCCESS;
}

static NTSTATUS pnp_device_usage_notification(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    device_extension* Vcb = DeviceObject->DeviceExtension;

    switch (IrpSp->Parameters.UsageNotification.Type) {
        case DeviceUsageTypePaging:
        case DeviceUsageTypeHibernation:
        case DeviceUsageTypeDumpFile:
            IoAdjustPagingPathCount(&Vcb->page_file_count, IrpSp->Parameters.UsageNotification.InPath);
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, pnp_usage_completion, Vcb, true, true, true);
            break;

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            break;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    return IoCallDriver(Vcb->Vpb->RealDevice, Irp);
}

_Dispatch_type_(IRP_MJ_PNP)
_Function_class_(DRIVER_DISPATCH)
NTSTATUS __stdcall drv_pnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    device_extension* Vcb = DeviceObject->DeviceExtension;
    NTSTATUS Status;
    bool top_level;

    FsRtlEnterFileSystem();

    top_level = is_top_level(Irp);

    if (Vcb && Vcb->type == VCB_TYPE_BUS) {
        Status = bus_pnp(DeviceObject->DeviceExtension, Irp);
        goto exit;
    } else if (Vcb && Vcb->type == VCB_TYPE_VOLUME) {
        volume_device_extension* vde = DeviceObject->DeviceExtension;
        IoSkipCurrentIrpStackLocation(Irp);
        Status = IoCallDriver(vde->attached_device, Irp);
        goto exit;
    } else if (Vcb && Vcb->type == VCB_TYPE_PDO) {
        Status = pdo_pnp(DeviceObject, Irp);
        goto end;
    } else if (!Vcb || Vcb->type != VCB_TYPE_FS) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    Status = STATUS_NOT_IMPLEMENTED;

    switch (IrpSp->MinorFunction) {
        case IRP_MN_CANCEL_REMOVE_DEVICE:
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE:
            Status = pnp_query_remove_device(DeviceObject, Irp);
            break;

        case IRP_MN_REMOVE_DEVICE:
            Status = pnp_remove_device(DeviceObject);
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            Status = pnp_surprise_removal(DeviceObject, Irp);
            break;

        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
            Status = pnp_device_usage_notification(DeviceObject, Irp);
            goto exit;

        default:
            TRACE("passing minor function 0x%x on\n", IrpSp->MinorFunction);

            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Vcb->Vpb->RealDevice, Irp);
            goto exit;
    }

end:
    Irp->IoStatus.Status = Status;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

exit:
    TRACE("returning %08lx\n", Status);

    if (top_level)
        IoSetTopLevelIrp(NULL);

    FsRtlExitFileSystem();

    return Status;
}
