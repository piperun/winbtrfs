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
#include "directory-query.h"


ULONG get_reparse_tag_fcb(fcb* fcb) {
    ULONG tag;

    if (fcb->type == BTRFS_FT_SYMLINK)
        return IO_REPARSE_TAG_SYMLINK;
    else if (fcb->type == BTRFS_FT_DIR) {
        if (!fcb->reparse_xattr.Buffer || fcb->reparse_xattr.Length < sizeof(ULONG))
            return 0;

        RtlCopyMemory(&tag, fcb->reparse_xattr.Buffer, sizeof(ULONG));
    } else {
        NTSTATUS Status;
        ULONG br;

        Status = read_file(fcb, (uint8_t*)&tag, 0, sizeof(ULONG), &br, NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("read_file returned %08lx\n", Status);
            return 0;
        }
    }

    return tag;
}

ULONG get_reparse_tag(device_extension* Vcb, root* subvol, uint64_t inode, uint8_t type, ULONG atts, bool lxss, PIRP Irp) {
    fcb* fcb;
    ULONG tag = 0;
    NTSTATUS Status;

    if (type == BTRFS_FT_SYMLINK)
        return IO_REPARSE_TAG_SYMLINK;
    else if (lxss) {
        if (type == BTRFS_FT_SOCK)
            return IO_REPARSE_TAG_AF_UNIX;
        else if (type == BTRFS_FT_FIFO)
            return IO_REPARSE_TAG_LX_FIFO;
        else if (type == BTRFS_FT_CHRDEV)
            return IO_REPARSE_TAG_LX_CHR;
        else if (type == BTRFS_FT_BLKDEV)
            return IO_REPARSE_TAG_LX_BLK;
    }

    if (type != BTRFS_FT_REG_FILE && type != BTRFS_FT_DIR)
        return 0;

    if (!(atts & FILE_ATTRIBUTE_REPARSE_POINT))
        return 0;

    Status = open_fcb(Vcb, subvol, inode, type, NULL, false, NULL, &fcb, PagedPool, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("open_fcb returned %08lx\n", Status);
        return 0;
    }

    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    tag = get_reparse_tag_fcb(fcb);

    ExReleaseResourceLite(fcb->Header.Resource);

    free_fcb(fcb);

    return tag;
}


static NTSTATUS notify_change_directory(device_extension* Vcb, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    fcb* fcb = FileObject->FsContext;
    ccb* ccb = FileObject->FsContext2;
    file_ref* fileref = ccb ? ccb->fileref : NULL;
    NTSTATUS Status;

    TRACE("IRP_MN_NOTIFY_CHANGE_DIRECTORY\n");

    if (!ccb) {
        ERR("ccb was NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (!fileref) {
        ERR("no fileref\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (Irp->RequestorMode == UserMode && !(ccb->access & FILE_LIST_DIRECTORY)) {
        WARN("insufficient privileges\n");
        return STATUS_ACCESS_DENIED;
    }

    ExAcquireResourceSharedLite(&fcb->Vcb->tree_lock, true);
    ExAcquireResourceExclusiveLite(fcb->Header.Resource, true);

    if (fcb->type != BTRFS_FT_DIR) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    // FIXME - raise exception if FCB marked for deletion?

    TRACE("FileObject %p\n", FileObject);

    if (ccb->filename.Length == 0) {
        ULONG reqlen;

        ccb->filename.MaximumLength = ccb->filename.Length = 0;

        Status = fileref_get_filename(fileref, &ccb->filename, NULL, &reqlen);
        if (Status == STATUS_BUFFER_OVERFLOW) {
            ccb->filename.Buffer = ExAllocatePoolWithTag(PagedPool, reqlen, ALLOC_TAG);
            if (!ccb->filename.Buffer) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            ccb->filename.MaximumLength = (uint16_t)reqlen;

            Status = fileref_get_filename(fileref, &ccb->filename, NULL, &reqlen);
            if (!NT_SUCCESS(Status)) {
                ERR("fileref_get_filename returned %08lx\n", Status);
                goto end;
            }
        } else {
            ERR("fileref_get_filename returned %08lx\n", Status);
            goto end;
        }
    }

    FsRtlNotifyFilterChangeDirectory(Vcb->NotifySync, &Vcb->DirNotifyList, FileObject->FsContext2, (PSTRING)&ccb->filename,
                                     IrpSp->Flags & SL_WATCH_TREE, false, IrpSp->Parameters.NotifyDirectory.CompletionFilter, Irp,
                                     NULL, NULL, NULL);

    Status = STATUS_PENDING;

end:
    ExReleaseResourceLite(fcb->Header.Resource);
    ExReleaseResourceLite(&fcb->Vcb->tree_lock);

    return Status;
}

_Dispatch_type_(IRP_MJ_DIRECTORY_CONTROL)
_Function_class_(DRIVER_DISPATCH)
NTSTATUS __stdcall drv_directory_control(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    PIO_STACK_LOCATION IrpSp;
    NTSTATUS Status;
    ULONG func;
    bool top_level;
    device_extension* Vcb = DeviceObject->DeviceExtension;

    FsRtlEnterFileSystem();

    TRACE("directory control\n");

    top_level = is_top_level(Irp);

    if (Vcb && Vcb->type == VCB_TYPE_VOLUME) {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto end;
    } else if (!Vcb || Vcb->type != VCB_TYPE_FS) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    Irp->IoStatus.Information = 0;

    func = IrpSp->MinorFunction;

    switch (func) {
        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
            Status = notify_change_directory(Vcb, Irp);
            break;

        case IRP_MN_QUERY_DIRECTORY:
            Status = query_directory(Irp);
            break;

        default:
            WARN("unknown minor %lu\n", func);
            Status = STATUS_NOT_IMPLEMENTED;
            Irp->IoStatus.Status = Status;
            break;
    }

    if (Status == STATUS_PENDING)
        goto exit;

end:
    Irp->IoStatus.Status = Status;

    IoCompleteRequest(Irp, IO_DISK_INCREMENT);

exit:
    TRACE("returning %08lx\n", Status);

    if (top_level)
        IoSetTopLevelIrp(NULL);

    FsRtlExitFileSystem();

    return Status;
}
