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

#include "stream-create.h"
#include "file-create.h"
#include "crc32c.h"

NTSTATUS create_stream(_Requires_lock_held_(_Curr_->tree_lock) _Requires_exclusive_lock_held_(_Curr_->fcb_lock) device_extension* Vcb,
                              file_ref** pfileref, file_ref** pparfileref, PUNICODE_STRING fpus, PUNICODE_STRING stream, PIRP Irp,
                              ULONG options, POOL_TYPE pool_type, bool case_sensitive, LIST_ENTRY* rollback) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    file_ref *fileref, *newpar, *parfileref;
    fcb* fcb;
    static const char xapref[] = "user.";
    static const WCHAR DOSATTRIB[] = L"DOSATTRIB";
    static const WCHAR EA[] = L"EA";
    static const WCHAR reparse[] = L"reparse";
    static const WCHAR casesensitive_str[] = L"casesensitive";
    LARGE_INTEGER time;
    struct btrfs_timespec now;
    ULONG utf8len, overhead;
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    dir_child* dc;
    dir_child* existing_dc = NULL;
    ACCESS_MASK granted_access;

    TRACE("fpus = %.*S\n", (int)(fpus->Length / sizeof(WCHAR)), fpus->Buffer);
    TRACE("stream = %.*S\n", (int)(stream->Length / sizeof(WCHAR)), stream->Buffer);

    parfileref = *pparfileref;

    if (parfileref->fcb == Vcb->dummy_fcb)
        return STATUS_ACCESS_DENIED;

    Status = check_file_name_valid(stream, false, true);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = open_fileref(Vcb, &newpar, fpus, parfileref, false, NULL, NULL, PagedPool, case_sensitive, Irp);

    if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
        UNICODE_STRING fpus2;

        Status = check_file_name_valid(fpus, false, false);
        if (!NT_SUCCESS(Status))
            return Status;

        fpus2.Length = fpus2.MaximumLength = fpus->Length;
        fpus2.Buffer = ExAllocatePoolWithTag(pool_type, fpus2.MaximumLength, ALLOC_TAG);

        if (!fpus2.Buffer) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(fpus2.Buffer, fpus->Buffer, fpus2.Length);

        SeLockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);

        ExAcquireResourceSharedLite(parfileref->fcb->Header.Resource, true);

        if (!SeAccessCheck(parfileref->fcb->sd, &IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext,
                           true, options & FILE_DIRECTORY_FILE ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE, 0, NULL,
                           IoGetFileObjectGenericMapping(), IrpSp->Flags & SL_FORCE_ACCESS_CHECK ? UserMode : Irp->RequestorMode,
                           &granted_access, &Status)) {
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);
            SeUnlockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);
            return Status;
        }

        ExReleaseResourceLite(parfileref->fcb->Header.Resource);

        SeUnlockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);

        Status = file_create2(Irp, Vcb, &fpus2, parfileref, options, NULL, 0, &newpar, case_sensitive, rollback);

        if (!NT_SUCCESS(Status)) {
            ERR("file_create2 returned %08lx\n", Status);
            ExFreePool(fpus2.Buffer);
            return Status;
        } else if (Status != STATUS_OBJECT_NAME_COLLISION) {
            send_notification_fileref(newpar, options & FILE_DIRECTORY_FILE ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED, NULL);
            queue_notification_fcb(newpar->parent, FILE_NOTIFY_CHANGE_LAST_WRITE, FILE_ACTION_MODIFIED, NULL);
        }

        ExFreePool(fpus2.Buffer);
    } else if (!NT_SUCCESS(Status)) {
        ERR("open_fileref returned %08lx\n", Status);
        return Status;
    }

    parfileref = newpar;
    *pparfileref = parfileref;

    if (parfileref->fcb->type != BTRFS_FT_REG_FILE && parfileref->fcb->type != BTRFS_FT_SYMLINK && parfileref->fcb->type != BTRFS_FT_DIR) {
        WARN("parent not file, directory, or symlink\n");
        free_fileref(parfileref);
        return STATUS_INVALID_PARAMETER;
    }

    if (options & FILE_DIRECTORY_FILE) {
        WARN("tried to create directory as stream\n");
        free_fileref(parfileref);
        return STATUS_INVALID_PARAMETER;
    }

    if (parfileref->fcb->atts & FILE_ATTRIBUTE_READONLY && !(IrpSp->Flags & SL_IGNORE_READONLY_ATTRIBUTE)) {
        free_fileref(parfileref);
        return STATUS_ACCESS_DENIED;
    }

    SeLockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);

    ExAcquireResourceSharedLite(parfileref->fcb->Header.Resource, true);

    if (!SeAccessCheck(parfileref->fcb->sd, &IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext,
                       true, FILE_WRITE_DATA, 0, NULL, IoGetFileObjectGenericMapping(), IrpSp->Flags & SL_FORCE_ACCESS_CHECK ? UserMode : Irp->RequestorMode,
                       &granted_access, &Status)) {
        ExReleaseResourceLite(parfileref->fcb->Header.Resource);
        SeUnlockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);
        free_fileref(parfileref);
        return Status;
    }

    ExReleaseResourceLite(parfileref->fcb->Header.Resource);

    SeUnlockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);

    if ((stream->Length == sizeof(DOSATTRIB) - sizeof(WCHAR) && RtlCompareMemory(stream->Buffer, DOSATTRIB, stream->Length) == stream->Length) ||
        (stream->Length == sizeof(EA) - sizeof(WCHAR) && RtlCompareMemory(stream->Buffer, EA, stream->Length) == stream->Length) ||
        (stream->Length == sizeof(reparse) - sizeof(WCHAR) && RtlCompareMemory(stream->Buffer, reparse, stream->Length) == stream->Length) ||
        (stream->Length == sizeof(casesensitive_str) - sizeof(WCHAR) && RtlCompareMemory(stream->Buffer, casesensitive_str, stream->Length) == stream->Length)) {
        free_fileref(parfileref);
        return STATUS_OBJECT_NAME_INVALID;
    }

    fcb = create_fcb(Vcb, pool_type);
    if (!fcb) {
        ERR("out of memory\n");
        free_fileref(parfileref);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fcb->Vcb = Vcb;

    fcb->Header.IsFastIoPossible = fast_io_possible(fcb);
    fcb->Header.AllocationSize.QuadPart = 0;
    fcb->Header.FileSize.QuadPart = 0;
    fcb->Header.ValidDataLength.QuadPart = 0;

    fcb->subvol = parfileref->fcb->subvol;
    fcb->inode = parfileref->fcb->inode;
    fcb->hash = parfileref->fcb->hash;
    fcb->type = parfileref->fcb->type;

    fcb->ads = true;

    Status = utf16_to_utf8(NULL, 0, &utf8len, stream->Buffer, stream->Length);
    if (!NT_SUCCESS(Status)) {
        ERR("utf16_to_utf8 1 returned %08lx\n", Status);
        reap_fcb(fcb);
        free_fileref(parfileref);
        return Status;
    }

    fcb->adsxattr.Length = (uint16_t)utf8len + sizeof(xapref) - 1;
    fcb->adsxattr.MaximumLength = fcb->adsxattr.Length + 1;
    fcb->adsxattr.Buffer = ExAllocatePoolWithTag(pool_type, fcb->adsxattr.MaximumLength, ALLOC_TAG);
    if (!fcb->adsxattr.Buffer) {
        ERR("out of memory\n");
        reap_fcb(fcb);
        free_fileref(parfileref);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(fcb->adsxattr.Buffer, xapref, sizeof(xapref) - 1);

    Status = utf16_to_utf8(&fcb->adsxattr.Buffer[sizeof(xapref) - 1], utf8len, &utf8len, stream->Buffer, stream->Length);
    if (!NT_SUCCESS(Status)) {
        ERR("utf16_to_utf8 2 returned %08lx\n", Status);
        reap_fcb(fcb);
        free_fileref(parfileref);
        return Status;
    }

    fcb->adsxattr.Buffer[fcb->adsxattr.Length] = 0;

    TRACE("adsxattr = %s\n", fcb->adsxattr.Buffer);

    fcb->adshash = calc_crc32c(0xfffffffe, (uint8_t*)fcb->adsxattr.Buffer, fcb->adsxattr.Length);
    TRACE("adshash = %08x\n", fcb->adshash);

    searchkey.objectid = parfileref->fcb->inode;
    searchkey.type = BTRFS_XATTR_ITEM_KEY;
    searchkey.offset = fcb->adshash;

    Status = find_item(Vcb, parfileref->fcb->subvol, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        reap_fcb(fcb);
        free_fileref(parfileref);
        return Status;
    }

    if (!keycmp(tp.item->key, searchkey))
        overhead = tp.item->size;
    else
        overhead = 0;

    fcb->adsmaxlen = Vcb->superblock.nodesize - sizeof(struct btrfs_header) - sizeof(struct btrfs_item) - sizeof(struct btrfs_dir_item);

    if (utf8len + sizeof(xapref) - 1 + overhead > fcb->adsmaxlen) {
        WARN("not enough room for new DIR_ITEM (%Iu + %lu > %lu)\n", utf8len + sizeof(xapref) - 1, overhead, fcb->adsmaxlen);
        reap_fcb(fcb);
        free_fileref(parfileref);
        return STATUS_DISK_FULL;
    } else
        fcb->adsmaxlen -= overhead + utf8len + sizeof(xapref) - 1;

    fcb->created = true;
    fcb->deleted = true;

    acquire_fcb_lock_exclusive(Vcb);
    InsertHeadList(&parfileref->fcb->list_entry, &fcb->list_entry); // insert in list after parent fcb
    InsertTailList(&Vcb->all_fcbs, &fcb->list_entry_all);
    parfileref->fcb->subvol->fcbs_version++;
    release_fcb_lock(Vcb);

    fileref = create_fileref(Vcb);
    if (!fileref) {
        ERR("out of memory\n");
        free_fcb(fcb);
        free_fileref(parfileref);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fileref->fcb = fcb;

    dc = ExAllocatePoolWithTag(PagedPool, sizeof(dir_child), ALLOC_TAG);
    if (!dc) {
        ERR("out of memory\n");
        reap_fileref(Vcb, fileref);
        free_fileref(parfileref);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(dc, sizeof(dir_child));

    dc->utf8.MaximumLength = dc->utf8.Length = fcb->adsxattr.Length + 1 - sizeof(xapref);
    dc->utf8.Buffer = ExAllocatePoolWithTag(PagedPool, dc->utf8.MaximumLength, ALLOC_TAG);
    if (!dc->utf8.Buffer) {
        ERR("out of memory\n");
        ExFreePool(dc);
        reap_fileref(Vcb, fileref);
        free_fileref(parfileref);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(dc->utf8.Buffer, &fcb->adsxattr.Buffer[sizeof(xapref) - 1], fcb->adsxattr.Length + 1 - sizeof(xapref));

    dc->name.MaximumLength = dc->name.Length = stream->Length;
    dc->name.Buffer = ExAllocatePoolWithTag(pool_type, dc->name.MaximumLength, ALLOC_TAG);
    if (!dc->name.Buffer) {
        ERR("out of memory\n");
        ExFreePool(dc->utf8.Buffer);
        ExFreePool(dc);
        reap_fileref(Vcb, fileref);
        free_fileref(parfileref);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(dc->name.Buffer, stream->Buffer, stream->Length);

    Status = RtlUpcaseUnicodeString(&dc->name_uc, &dc->name, true);
    if (!NT_SUCCESS(Status)) {
        ERR("RtlUpcaseUnicodeString returned %08lx\n", Status);
        ExFreePool(dc->utf8.Buffer);
        ExFreePool(dc->name.Buffer);
        ExFreePool(dc);
        reap_fileref(Vcb, fileref);
        free_fileref(parfileref);
        return Status;
    }

    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);

    ExAcquireResourceExclusiveLite(&parfileref->fcb->nonpaged->dir_children_lock, true);

    LIST_ENTRY* le = parfileref->fcb->dir_children_index.Flink;
    while (le != &parfileref->fcb->dir_children_index) {
        dir_child* dc2 = CONTAINING_RECORD(le, dir_child, list_entry_index);

        if (dc2->index == 0) {
            if ((case_sensitive && dc2->name.Length == dc->name.Length && RtlCompareMemory(dc2->name.Buffer, dc->name.Buffer, dc2->name.Length) == dc2->name.Length) ||
                (!case_sensitive && dc2->name_uc.Length == dc->name_uc.Length && RtlCompareMemory(dc2->name_uc.Buffer, dc->name_uc.Buffer, dc2->name_uc.Length) == dc2->name_uc.Length)
            ) {
                existing_dc = dc2;
                break;
            }
        } else
            break;

        le = le->Flink;
    }

    if (existing_dc) {
        file_ref* existing_fileref = existing_dc->fileref;

        increase_fileref_refcount(existing_fileref);
        ExReleaseResourceLite(&parfileref->fcb->nonpaged->dir_children_lock);

        ExFreePool(dc->utf8.Buffer);
        ExFreePool(dc->name.Buffer);
        ExFreePool(dc->name_uc.Buffer);
        ExFreePool(dc);
        reap_fileref(Vcb, fileref);
        free_fileref(parfileref);

        *pfileref = existing_fileref;

        return STATUS_OBJECT_NAME_COLLISION;
    }

    dc->fileref = fileref;
    fileref->dc = dc;
    fileref->parent = (struct _file_ref*)parfileref;
    fcb->deleted = false;

    InsertHeadList(&parfileref->fcb->dir_children_index, &dc->list_entry_index);

    InsertTailList(&parfileref->children, &fileref->list_entry);

    ExReleaseResourceLite(&parfileref->fcb->nonpaged->dir_children_lock);

    mark_fcb_dirty(fcb);
    mark_fileref_dirty(fileref);

    ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
    parfileref->fcb->inode_item.transid = Vcb->superblock.generation;
    parfileref->fcb->inode_item.sequence++;
    parfileref->fcb->inode_item.ctime = now;
    parfileref->fcb->inode_item_changed = true;
    ExReleaseResourceLite(parfileref->fcb->Header.Resource);

    mark_fcb_dirty(parfileref->fcb);

    parfileref->fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
    parfileref->fcb->subvol->root_item.ctime = now;

    increase_fileref_refcount(parfileref);

    *pfileref = fileref;

    send_notification_fileref(parfileref, FILE_NOTIFY_CHANGE_STREAM_NAME, FILE_ACTION_ADDED_STREAM, &fileref->dc->name);

    return STATUS_SUCCESS;
}
