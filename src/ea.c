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

#include <sys/stat.h>
#include "btrfs_drv.h"

_Dispatch_type_(IRP_MJ_QUERY_EA)
_Function_class_(DRIVER_DISPATCH)
NTSTATUS __stdcall drv_query_ea(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    NTSTATUS Status;
    bool top_level;
    device_extension* Vcb = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    fcb* fcb;
    ccb* ccb;
    FILE_FULL_EA_INFORMATION* ffei;
    ULONG retlen = 0;

    FsRtlEnterFileSystem();

    TRACE("(%p, %p)\n", DeviceObject, Irp);

    top_level = is_top_level(Irp);

    if (Vcb && Vcb->type == VCB_TYPE_VOLUME) {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto end;
    } else if (!Vcb || Vcb->type != VCB_TYPE_FS) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    ffei = map_user_buffer(Irp, NormalPagePriority);
    if (!ffei) {
        ERR("could not get output buffer\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    if (!FileObject) {
        ERR("no file object\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    fcb = FileObject->FsContext;

    if (!fcb) {
        ERR("no fcb\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    if (fcb == fcb->Vcb->volume_fcb) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    ccb = FileObject->FsContext2;

    if (!ccb) {
        ERR("no ccb\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    if (Irp->RequestorMode == UserMode && !(ccb->access & (FILE_READ_EA | FILE_WRITE_EA))) {
        WARN("insufficient privileges\n");
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }

    if (fcb->ads)
        fcb = ccb->fileref->parent->fcb;

    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    if (fcb->ea_xattr.Length == 0) {
        Status = STATUS_NO_EAS_ON_FILE;
        goto end2;
    }

    Status = STATUS_SUCCESS;

    if (IrpSp->Parameters.QueryEa.EaList) {
        FILE_FULL_EA_INFORMATION *ea, *out;
        FILE_GET_EA_INFORMATION* in;
        uint8_t padding = retlen % 4 > 0 ? (4 - (retlen % 4)) : 0;

        in = IrpSp->Parameters.QueryEa.EaList;
        out = NULL;

        do {
            bool found = false;
            STRING s;

            s.Length = s.MaximumLength = in->EaNameLength;
            s.Buffer = in->EaName;

            RtlUpperString(&s, &s);

            ea = (FILE_FULL_EA_INFORMATION*)fcb->ea_xattr.Buffer;

            do {
                if (ea->EaNameLength == in->EaNameLength &&
                    RtlCompareMemory(ea->EaName, in->EaName, ea->EaNameLength) == ea->EaNameLength) {
                    found = true;
                    break;
                }

                if (ea->NextEntryOffset == 0)
                    break;

                ea = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)ea) + ea->NextEntryOffset);
            } while (true);

            if (offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + ea->EaNameLength + 1 + ea->EaValueLength > IrpSp->Parameters.QueryEa.Length - retlen - padding) {
                Status = STATUS_BUFFER_OVERFLOW;
                retlen = 0;
                goto end2;
            }

            retlen += padding;

            if (out) {
                out->NextEntryOffset = (ULONG)offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + out->EaNameLength + 1 + out->EaValueLength + padding;
                out = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)out) + out->NextEntryOffset);
            } else
                out = ffei;

            out->NextEntryOffset = 0;

            if (found) {
                out->Flags = ea->Flags;
                out->EaNameLength = ea->EaNameLength;
                out->EaValueLength = ea->EaValueLength;
                RtlCopyMemory(out->EaName, ea->EaName, ea->EaNameLength + ea->EaValueLength + 1);
            } else {
                out->Flags = 0;
                out->EaNameLength = in->EaNameLength;
                out->EaValueLength = 0;
                RtlCopyMemory(out->EaName, in->EaName, in->EaNameLength);
                out->EaName[in->EaNameLength] = 0;
            }

            retlen += (ULONG)offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + out->EaNameLength + 1 + out->EaValueLength;

            if (IrpSp->Flags & SL_RETURN_SINGLE_ENTRY)
                break;

            if (in->NextEntryOffset == 0)
                break;

            in = (FILE_GET_EA_INFORMATION*)(((uint8_t*)in) + in->NextEntryOffset);
        } while (true);
    } else {
        FILE_FULL_EA_INFORMATION *ea, *out;
        ULONG index;

        if (IrpSp->Flags & SL_INDEX_SPECIFIED) {
            // The index is 1-based
            if (IrpSp->Parameters.QueryEa.EaIndex == 0) {
                Status = STATUS_NONEXISTENT_EA_ENTRY;
                goto end2;
            } else
                index = IrpSp->Parameters.QueryEa.EaIndex - 1;
        } else if (IrpSp->Flags & SL_RESTART_SCAN)
            index = ccb->ea_index = 0;
        else
            index = ccb->ea_index;

        ea = (FILE_FULL_EA_INFORMATION*)fcb->ea_xattr.Buffer;

        if (index > 0) {
            ULONG i;

            for (i = 0; i < index; i++) {
                if (ea->NextEntryOffset == 0) { // last item
                    Status = STATUS_NO_MORE_EAS;
                    goto end2;
                }

                ea = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)ea) + ea->NextEntryOffset);
            }
        }

        out = NULL;

        do {
            uint8_t padding = retlen % 4 > 0 ? (4 - (retlen % 4)) : 0;

            if (offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + ea->EaNameLength + 1 + ea->EaValueLength > IrpSp->Parameters.QueryEa.Length - retlen - padding) {
                Status = retlen == 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;
                goto end2;
            }

            retlen += padding;

            if (out) {
                out->NextEntryOffset = (ULONG)offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + out->EaNameLength + 1 + out->EaValueLength + padding;
                out = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)out) + out->NextEntryOffset);
            } else
                out = ffei;

            out->NextEntryOffset = 0;
            out->Flags = ea->Flags;
            out->EaNameLength = ea->EaNameLength;
            out->EaValueLength = ea->EaValueLength;
            RtlCopyMemory(out->EaName, ea->EaName, ea->EaNameLength + ea->EaValueLength + 1);

            retlen += (ULONG)offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + ea->EaNameLength + 1 + ea->EaValueLength;

            if (!(IrpSp->Flags & SL_INDEX_SPECIFIED))
                ccb->ea_index++;

            if (ea->NextEntryOffset == 0 || IrpSp->Flags & SL_RETURN_SINGLE_ENTRY)
                break;

            ea = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)ea) + ea->NextEntryOffset);
        } while (true);
    }

end2:
    ExReleaseResourceLite(fcb->Header.Resource);

end:
    TRACE("returning %08lx\n", Status);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW ? retlen : 0;

    IoCompleteRequest( Irp, IO_NO_INCREMENT );

    if (top_level)
        IoSetTopLevelIrp(NULL);

    FsRtlExitFileSystem();

    return Status;
}

_Dispatch_type_(IRP_MJ_SET_EA)
_Function_class_(DRIVER_DISPATCH)
NTSTATUS __stdcall drv_set_ea(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    device_extension* Vcb = DeviceObject->DeviceExtension;
    NTSTATUS Status;
    bool top_level;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    fcb* fcb;
    ccb* ccb;
    file_ref* fileref;
    FILE_FULL_EA_INFORMATION* ffei;
    ULONG offset;
    LIST_ENTRY ealist;
    ea_item* item;
    FILE_FULL_EA_INFORMATION* ea;
    LIST_ENTRY* le;
    LARGE_INTEGER time;
    struct btrfs_timespec now;
    uint32_t uid, gid, mode;
    bool uid_set = false;

    FsRtlEnterFileSystem();

    TRACE("(%p, %p)\n", DeviceObject, Irp);

    top_level = is_top_level(Irp);

    if (Vcb && Vcb->type == VCB_TYPE_VOLUME) {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto end;
    } else if (!Vcb || Vcb->type != VCB_TYPE_FS) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    if (Vcb->readonly) {
        Status = STATUS_MEDIA_WRITE_PROTECTED;
        goto end;
    }

    ffei = map_user_buffer(Irp, NormalPagePriority);
    if (!ffei) {
        ERR("could not get output buffer\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    Status = IoCheckEaBufferValidity(ffei, IrpSp->Parameters.SetEa.Length, &offset);
    if (!NT_SUCCESS(Status)) {
        ERR("IoCheckEaBufferValidity returned %08lx (error at offset %lu)\n", Status, offset);
        goto end;
    }

    if (!FileObject) {
        ERR("no file object\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    fcb = FileObject->FsContext;

    if (!fcb) {
        ERR("no fcb\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    if (fcb == fcb->Vcb->volume_fcb) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    ccb = FileObject->FsContext2;

    if (!ccb) {
        ERR("no ccb\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    if (Irp->RequestorMode == UserMode && !(ccb->access & FILE_WRITE_EA)) {
        WARN("insufficient privileges\n");
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }

    if (fcb->ads) {
        fileref = ccb->fileref->parent;
        fcb = fileref->fcb;
    } else
        fileref = ccb->fileref;

    InitializeListHead(&ealist);

    ExAcquireResourceExclusiveLite(fcb->Header.Resource, true);

    uid = fcb->inode_item.uid;
    gid = fcb->inode_item.gid;
    mode = fcb->inode_item.mode;

    if (fcb->ea_xattr.Length > 0) {
        ea = (FILE_FULL_EA_INFORMATION*)fcb->ea_xattr.Buffer;

        do {
            item = ExAllocatePoolWithTag(PagedPool, sizeof(ea_item), ALLOC_TAG);
            if (!item) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end2;
            }

            item->name.Length = item->name.MaximumLength = ea->EaNameLength;
            item->name.Buffer = ea->EaName;

            item->value.Length = item->value.MaximumLength = ea->EaValueLength;
            item->value.Buffer = &ea->EaName[ea->EaNameLength + 1];

            item->flags = ea->Flags;

            InsertTailList(&ealist, &item->list_entry);

            if (ea->NextEntryOffset == 0)
                break;

            ea = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)ea) + ea->NextEntryOffset);
        } while (true);
    }

    ea = ffei;

    do {
        STRING s;
        bool found = false;

        s.Length = s.MaximumLength = ea->EaNameLength;
        s.Buffer = ea->EaName;

        RtlUpperString(&s, &s);

        le = ealist.Flink;
        while (le != &ealist) {
            item = CONTAINING_RECORD(le, ea_item, list_entry);

            if (item->name.Length == s.Length &&
                RtlCompareMemory(item->name.Buffer, s.Buffer, s.Length) == s.Length) {
                item->flags = ea->Flags;
                item->value.Length = item->value.MaximumLength = ea->EaValueLength;
                item->value.Buffer = &ea->EaName[ea->EaNameLength + 1];
                found = true;
                break;
            }

            le = le->Flink;
        }

        if (!found) {
            item = ExAllocatePoolWithTag(PagedPool, sizeof(ea_item), ALLOC_TAG);
            if (!item) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end2;
            }

            item->name.Length = item->name.MaximumLength = ea->EaNameLength;
            item->name.Buffer = ea->EaName;

            item->value.Length = item->value.MaximumLength = ea->EaValueLength;
            item->value.Buffer = &ea->EaName[ea->EaNameLength + 1];

            item->flags = ea->Flags;

            InsertTailList(&ealist, &item->list_entry);
        }

        if (ea->NextEntryOffset == 0)
            break;

        ea = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)ea) + ea->NextEntryOffset);
    } while (true);

    // remove entries with zero-length value
    le = ealist.Flink;
    while (le != &ealist) {
        LIST_ENTRY* le2 = le->Flink;

        item = CONTAINING_RECORD(le, ea_item, list_entry);

        if (item->value.Length == 0) {
            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        }

        le = le2;
    }

    // handle LXSS values
    le = ealist.Flink;
    while (le != &ealist) {
        LIST_ENTRY* le2 = le->Flink;

        item = CONTAINING_RECORD(le, ea_item, list_entry);

        if (item->name.Length == sizeof(lxuid) - 1 && RtlCompareMemory(item->name.Buffer, lxuid, item->name.Length) == item->name.Length) {
            if (item->value.Length < sizeof(uint32_t)) {
                ERR("uid value was shorter than expected\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end2;
            }

            if (Irp->RequestorMode == KernelMode || ccb->access & FILE_WRITE_ATTRIBUTES) {
                RtlCopyMemory(&uid, item->value.Buffer, sizeof(uid));
                uid_set = true;
            }

            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        } else if (item->name.Length == sizeof(lxgid) - 1 && RtlCompareMemory(item->name.Buffer, lxgid, item->name.Length) == item->name.Length) {
            if (item->value.Length < sizeof(uint32_t)) {
                ERR("gid value was shorter than expected\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end2;
            }

            if (Irp->RequestorMode == KernelMode || ccb->access & FILE_WRITE_ATTRIBUTES)
                RtlCopyMemory(&gid, item->value.Buffer, sizeof(gid));

            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        } else if (item->name.Length == sizeof(lxmod) - 1 && RtlCompareMemory(item->name.Buffer, lxmod, item->name.Length) == item->name.Length) {
            if (item->value.Length < sizeof(uint32_t)) {
                ERR("mode value was shorter than expected\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end2;
            }

            if (Irp->RequestorMode == KernelMode || ccb->access & FILE_WRITE_ATTRIBUTES) {
                uint32_t allowed = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH | S_ISGID | S_ISVTX | S_ISUID;
                uint32_t val;

                RtlCopyMemory(&val, item->value.Buffer, sizeof(uint32_t));

                mode &= ~allowed;
                mode |= val & allowed;
            }

            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        }

        le = le2;
    }

    if (IsListEmpty(&ealist)) {
        fcb->ealen = 0;

        if (fcb->ea_xattr.Buffer)
            ExFreePool(fcb->ea_xattr.Buffer);

        fcb->ea_xattr.Length = fcb->ea_xattr.MaximumLength = 0;
        fcb->ea_xattr.Buffer = NULL;
    } else {
        ULONG size = 0;
        char *buf, *oldbuf;

        le = ealist.Flink;
        while (le != &ealist) {
            item = CONTAINING_RECORD(le, ea_item, list_entry);

            if (size % 4 > 0)
                size += 4 - (size % 4);

            size += (ULONG)offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + item->name.Length + 1 + item->value.Length;

            if (size > UINT16_MAX || size + sizeof(struct btrfs_header) + sizeof(struct btrfs_item) +
                sizeof(struct btrfs_dir_item) + sizeof(EA_EA) - 1 > Vcb->superblock.nodesize) {
                Status = STATUS_EA_TOO_LARGE;
                goto end2;
            }

            le = le->Flink;
        }

        buf = ExAllocatePoolWithTag(PagedPool, size, ALLOC_TAG);
        if (!buf) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end2;
        }

        oldbuf = fcb->ea_xattr.Buffer;

        fcb->ea_xattr.Length = fcb->ea_xattr.MaximumLength = size;
        fcb->ea_xattr.Buffer = buf;

        fcb->ealen = 4;
        ea = NULL;

        le = ealist.Flink;
        while (le != &ealist) {
            item = CONTAINING_RECORD(le, ea_item, list_entry);

            if (ea) {
                ea->NextEntryOffset = (ULONG)offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + ea->EaNameLength + 1 + ea->EaValueLength;

                if (ea->NextEntryOffset % 4 > 0)
                    ea->NextEntryOffset += 4 - (ea->NextEntryOffset % 4);

                ea = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)ea) + ea->NextEntryOffset);
            } else
                ea = (FILE_FULL_EA_INFORMATION*)fcb->ea_xattr.Buffer;

            ea->NextEntryOffset = 0;
            ea->Flags = item->flags;
            ea->EaNameLength = (UCHAR)item->name.Length;
            ea->EaValueLength = item->value.Length;

            RtlCopyMemory(ea->EaName, item->name.Buffer, item->name.Length);
            ea->EaName[item->name.Length] = 0;
            RtlCopyMemory(&ea->EaName[item->name.Length + 1], item->value.Buffer, item->value.Length);

            fcb->ealen += 5 + item->name.Length + item->value.Length;

            le = le->Flink;
        }

        if (oldbuf)
            ExFreePool(oldbuf);
    }

    fcb->inode_item.uid = uid;
    fcb->inode_item.gid = gid;
    fcb->inode_item.mode = mode;
    if (uid_set) {
        fcb->sd_dirty = true;
        fcb->sd_deleted = false;
    }

    fcb->ea_changed = true;

    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);

    fcb->inode_item.transid = Vcb->superblock.generation;
    fcb->inode_item.sequence++;

    if (!ccb->user_set_change_time)
        fcb->inode_item.ctime = now;

    fcb->inode_item_changed = true;
    mark_fcb_dirty(fcb);

    send_notification_fileref(fileref, FILE_NOTIFY_CHANGE_EA, FILE_ACTION_MODIFIED, NULL);

    Status = STATUS_SUCCESS;

end2:
    ExReleaseResourceLite(fcb->Header.Resource);

    while (!IsListEmpty(&ealist)) {
        le = RemoveHeadList(&ealist);

        item = CONTAINING_RECORD(le, ea_item, list_entry);

        ExFreePool(item);
    }

end:
    TRACE("returning %08lx\n", Status);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (top_level)
        IoSetTopLevelIrp(NULL);

    FsRtlExitFileSystem();

    return Status;
}
