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

// Dismount, invalidation and PnP volume ownership transitions.
#include "btrfs_drv.h"
#include "volume-lock.h"
#include "volume-dismount.h"

extern LIST_ENTRY VcbList;
extern ERESOURCE global_loading_lock;

NTSTATUS invalidate_volumes(PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    LUID TcbPrivilege = {SE_TCB_PRIVILEGE, 0};
    NTSTATUS Status;
    HANDLE h;
    PFILE_OBJECT fileobj;
    PDEVICE_OBJECT devobj;
    LIST_ENTRY* le;

    TRACE("FSCTL_INVALIDATE_VOLUMES\n");

    if (!SeSinglePrivilegeCheck(TcbPrivilege, Irp->RequestorMode))
        return STATUS_PRIVILEGE_NOT_HELD;

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

    devobj = fileobj->DeviceObject;

    ExAcquireResourceSharedLite(&global_loading_lock, true);

    le = VcbList.Flink;

    while (le != &VcbList) {
        device_extension* Vcb = CONTAINING_RECORD(le, device_extension, list_entry);

        if (Vcb->Vpb && Vcb->Vpb->RealDevice == devobj) {
            if (Vcb->Vpb == devobj->Vpb) {
                KIRQL irql;
                PVPB newvpb;
                bool free_newvpb = false;

                newvpb = ExAllocatePoolWithTag(NonPagedPool, sizeof(VPB), ALLOC_TAG);
                if (!newvpb) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }

                RtlZeroMemory(newvpb, sizeof(VPB));

                ObReferenceObject(devobj);

                ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

                Vcb->removing = true;

                ExReleaseResourceLite(&Vcb->tree_lock);

                CcWaitForCurrentLazyWriterActivity();

                ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

                flush_fcb_caches(Vcb);

                if (Vcb->need_write && !Vcb->readonly)
                    Status = do_write(Vcb, Irp);
                else
                    Status = STATUS_SUCCESS;

                free_trees(Vcb);

                if (!NT_SUCCESS(Status)) {
                    ERR("do_write returned %08lx\n", Status);
                    ExReleaseResourceLite(&Vcb->tree_lock);
                    ExFreePool(newvpb);
                    ObDereferenceObject(devobj);
                    goto end;
                }

                flush_fcb_caches(Vcb);

                ExReleaseResourceLite(&Vcb->tree_lock);

                IoAcquireVpbSpinLock(&irql);

                if (devobj->Vpb->Flags & VPB_MOUNTED) {
                    newvpb->Type = IO_TYPE_VPB;
                    newvpb->Size = sizeof(VPB);
                    newvpb->RealDevice = devobj;
                    newvpb->Flags = devobj->Vpb->Flags & VPB_REMOVE_PENDING;

                    devobj->Vpb = newvpb;
                } else
                    free_newvpb = true;

                IoReleaseVpbSpinLock(irql);

                if (free_newvpb)
                    ExFreePool(newvpb);

                if (Vcb->open_files == 0)
                    uninit(Vcb);

                ObDereferenceObject(devobj);
            }

            break;
        }

        le = le->Flink;
    }

    Status = STATUS_SUCCESS;

end:
    ExReleaseResourceLite(&global_loading_lock);

    ObDereferenceObject(fileobj);

    return Status;
}

static void update_volumes(device_extension* Vcb) {
    LIST_ENTRY* le;
    volume_device_extension* vde = Vcb->vde;
    pdo_device_extension* pdode = vde->pdode;

    ExAcquireResourceSharedLite(&Vcb->tree_lock, true);

    ExAcquireResourceExclusiveLite(&pdode->child_lock, true);

    le = pdode->children.Flink;
    while (le != &pdode->children) {
        volume_child* vc = CONTAINING_RECORD(le, volume_child, list_entry);

        vc->generation = Vcb->superblock.generation - 1;

        le = le->Flink;
    }

    ExReleaseResourceLite(&pdode->child_lock);

    ExReleaseResourceLite(&Vcb->tree_lock);
}

NTSTATUS dismount_volume(device_extension* Vcb, bool shutdown, PIRP Irp) {
    NTSTATUS Status;
    bool open_files;

    TRACE("FSCTL_DISMOUNT_VOLUME\n");

    if (!(Vcb->Vpb->Flags & VPB_MOUNTED))
        return STATUS_SUCCESS;

    if (!shutdown) {
        if (Vcb->disallow_dismount || Vcb->page_file_count != 0) {
            WARN("attempting to dismount boot volume or one containing a pagefile\n");
            return STATUS_ACCESS_DENIED;
        }

        Status = FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_DISMOUNT);
        if (!NT_SUCCESS(Status)) {
            WARN("FsRtlNotifyVolumeEvent returned %08lx\n", Status);
        }
    }

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    if (!Vcb->locked) {
        flush_fcb_caches(Vcb);

        if (Vcb->need_write && !Vcb->readonly) {
            Status = do_write(Vcb, Irp);

            if (!NT_SUCCESS(Status))
                ERR("do_write returned %08lx\n", Status);
        }
    }

    free_trees(Vcb);

    Vcb->removing = true;

    open_files = Vcb->open_files > 0;

    if (Vcb->vde) {
        update_volumes(Vcb);
        Vcb->vde->mounted_device = NULL;
    }

    ExReleaseResourceLite(&Vcb->tree_lock);

    if (!open_files)
        uninit(Vcb);

    return STATUS_SUCCESS;
}

NTSTATUS pnp_remove_device(PDEVICE_OBJECT DeviceObject) {
    device_extension* Vcb = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    if (DeviceObject->Vpb->Flags & VPB_MOUNTED) {
        Status = FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_DISMOUNT);
        if (!NT_SUCCESS(Status)) {
            WARN("FsRtlNotifyVolumeEvent returned %08lx\n", Status);
        }

        if (Vcb->vde)
            Vcb->vde->mounted_device = NULL;

        ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
        Vcb->removing = true;
        ExReleaseResourceLite(&Vcb->tree_lock);

        if (Vcb->open_files == 0)
            uninit(Vcb);
    }

    return STATUS_SUCCESS;
}

NTSTATUS pnp_surprise_removal(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    device_extension* Vcb = DeviceObject->DeviceExtension;

    TRACE("(%p, %p)\n", DeviceObject, Irp);

    UNUSED(Irp);

    if (DeviceObject->Vpb->Flags & VPB_MOUNTED) {
        ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

        if (Vcb->vde)
            Vcb->vde->mounted_device = NULL;

        Vcb->removing = true;

        ExReleaseResourceLite(&Vcb->tree_lock);

        if (Vcb->open_files == 0)
            uninit(Vcb);
    }

    return STATUS_SUCCESS;
}
