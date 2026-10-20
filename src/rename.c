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

#include "rename.h"
#include "crc32c.h"

static NTSTATUS rename_in_same_directory(file_ref* fileref, ccb* ccb,
                                         file_ref* related, ANSI_STRING* utf8,
                                         UNICODE_STRING* fnus, PIRP Irp) {
    NTSTATUS Status;
    UNICODE_STRING oldfn, newfn, name, name_uc;
    ANSI_STRING new_utf8;
    USHORT name_offset;
    ULONG reqlen, oldutf8len;
    LARGE_INTEGER time;
    struct btrfs_timespec now;
    fcb* fcb = fileref->fcb;

    oldfn.Length = oldfn.MaximumLength = 0;

    Status = fileref_get_filename(fileref, &oldfn, &name_offset, &reqlen);
    if (Status != STATUS_BUFFER_OVERFLOW) {
        ERR("fileref_get_filename returned %08lx\n", Status);
        return Status;
    }

    oldfn.Buffer = ExAllocatePoolWithTag(PagedPool, reqlen, ALLOC_TAG);
    if (!oldfn.Buffer) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    oldfn.MaximumLength = (uint16_t)reqlen;

    Status = fileref_get_filename(fileref, &oldfn, &name_offset, &reqlen);
    if (!NT_SUCCESS(Status)) {
        ERR("fileref_get_filename returned %08lx\n", Status);
        ExFreePool(oldfn.Buffer);
        return Status;
    }

    oldutf8len = fileref->dc->utf8.Length;

    if (!fileref->created && !fileref->oldutf8.Buffer) {
        fileref->oldutf8.Buffer = ExAllocatePoolWithTag(PagedPool, fileref->dc->utf8.Length,
                                                        ALLOC_TAG);
        if (!fileref->oldutf8.Buffer) {
            ERR("out of memory\n");
            ExFreePool(oldfn.Buffer);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        fileref->oldutf8.Length = fileref->oldutf8.MaximumLength = fileref->dc->utf8.Length;
        RtlCopyMemory(fileref->oldutf8.Buffer, fileref->dc->utf8.Buffer,
                      fileref->dc->utf8.Length);
    }

    TRACE("renaming %.*S to %.*S\n", (int)(fileref->dc->name.Length / sizeof(WCHAR)),
          fileref->dc->name.Buffer, (int)(fnus->Length / sizeof(WCHAR)), fnus->Buffer);

    mark_fileref_dirty(fileref);

    // update fileref->dc

    ExAcquireResourceExclusiveLite(&fileref->parent->fcb->nonpaged->dir_children_lock,
                                    true);

    new_utf8.Buffer = ExAllocatePoolWithTag(PagedPool, utf8->Length, ALLOC_TAG);
    if (!new_utf8.Buffer) {
        ERR("out of memory\n");
        ExReleaseResourceLite(&fileref->parent->fcb->nonpaged->dir_children_lock);
        ExFreePool(oldfn.Buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    new_utf8.Length = new_utf8.MaximumLength = utf8->Length;
    RtlCopyMemory(new_utf8.Buffer, utf8->Buffer, utf8->Length);

    name.Buffer = ExAllocatePoolWithTag(PagedPool, fnus->Length, ALLOC_TAG);
    if (!name.Buffer) {
        ERR("out of memory\n");
        ExReleaseResourceLite(&fileref->parent->fcb->nonpaged->dir_children_lock);
        ExFreePool(oldfn.Buffer);
        ExFreePool(new_utf8.Buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    name.Length = name.MaximumLength = fnus->Length;
    RtlCopyMemory(name.Buffer, fnus->Buffer, fnus->Length);

    Status = RtlUpcaseUnicodeString(&name_uc, &name, true);
    if (!NT_SUCCESS(Status)) {
        ERR("RtlUpcaseUnicodeString returned %08lx\n", Status);
        ExReleaseResourceLite(&fileref->parent->fcb->nonpaged->dir_children_lock);
        ExFreePool(oldfn.Buffer);
        ExFreePool(name.Buffer);
        ExFreePool(new_utf8.Buffer);
        return Status;
    }

    ExFreePool(fileref->dc->utf8.Buffer);
    fileref->dc->utf8 = new_utf8;

    ExFreePool(fileref->dc->name.Buffer);
    fileref->dc->name = name;

    ExFreePool(fileref->dc->name_uc.Buffer);
    fileref->dc->name_uc = name_uc;

    remove_dir_child_from_hash_lists(fileref->parent->fcb, fileref->dc);

    fileref->dc->hash = calc_crc32c(0xffffffff, (uint8_t*)fileref->dc->name.Buffer,
                                    fileref->dc->name.Length);
    fileref->dc->hash_uc = calc_crc32c(0xffffffff, (uint8_t*)fileref->dc->name_uc.Buffer,
                                        fileref->dc->name_uc.Length);

    insert_dir_child_into_hash_lists(fileref->parent->fcb, fileref->dc);

    ExReleaseResourceLite(&fileref->parent->fcb->nonpaged->dir_children_lock);

    // get new filename, for notification

    newfn.Length = newfn.MaximumLength = 0;

    Status = fileref_get_filename(fileref, &newfn, &name_offset, &reqlen);
    if (Status != STATUS_BUFFER_OVERFLOW) {
        ERR("fileref_get_filename returned %08lx\n", Status);
        ExFreePool(oldfn.Buffer);
        return Status;
    }

    newfn.Buffer = ExAllocatePoolWithTag(PagedPool, reqlen, ALLOC_TAG);
    if (!newfn.Buffer) {
        ERR("out of memory\n");
        ExFreePool(oldfn.Buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    newfn.MaximumLength = (uint16_t)reqlen;

    Status = fileref_get_filename(fileref, &newfn, &name_offset, &reqlen);
    if (!NT_SUCCESS(Status)) {
        ERR("fileref_get_filename returned %08lx\n", Status);
        ExFreePool(oldfn.Buffer);
        ExFreePool(newfn.Buffer);
        return Status;
    }

    // update INODE_ITEM

    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);

    if (fcb != fcb->Vcb->dummy_fcb && (fileref->parent->fcb->subvol == fcb->subvol || !is_subvol_readonly(fcb->subvol, Irp))) {
        fcb->inode_item.transid = fcb->Vcb->superblock.generation;
        fcb->inode_item.sequence++;

        if (!ccb->user_set_change_time)
            fcb->inode_item.ctime = now;

        fcb->inode_item_changed = true;
        mark_fcb_dirty(fcb);
    }

    // update parent's INODE_ITEM

    related->fcb->inode_item.transid = fcb->Vcb->superblock.generation;

    TRACE("related->fcb->inode_item.size (inode %I64x) was %I64x\n",
          related->fcb->inode, related->fcb->inode_item.size);

    related->fcb->inode_item.size = related->fcb->inode_item.size + (2 * utf8->Length) - (2 * oldutf8len);

    TRACE("related->fcb->inode_item.size (inode %I64x) now %I64x\n",
          related->fcb->inode, related->fcb->inode_item.size);

    related->fcb->inode_item.sequence++;
    related->fcb->inode_item.ctime = now;
    related->fcb->inode_item.mtime = now;

    related->fcb->inode_item_changed = true;
    mark_fcb_dirty(related->fcb);
    send_notification_fileref(related, FILE_NOTIFY_CHANGE_LAST_WRITE, FILE_ACTION_MODIFIED, NULL);

    FsRtlNotifyFilterReportChange(fcb->Vcb->NotifySync, &fcb->Vcb->DirNotifyList,
                                  (PSTRING)&oldfn, name_offset, NULL, NULL,
                                  fcb->type == BTRFS_FT_DIR ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                  FILE_ACTION_RENAMED_OLD_NAME, NULL, NULL);
    FsRtlNotifyFilterReportChange(fcb->Vcb->NotifySync, &fcb->Vcb->DirNotifyList,
                                  (PSTRING)&newfn, name_offset, NULL, NULL,
                                  fcb->type == BTRFS_FT_DIR ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                  FILE_ACTION_RENAMED_NEW_NAME, NULL, NULL);

    ExFreePool(oldfn.Buffer);
    ExFreePool(newfn.Buffer);

    return STATUS_SUCCESS;
}

NTSTATUS set_rename_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject, PFILE_OBJECT tfo, bool ex) {
    FILE_RENAME_INFORMATION_EX* fri = Irp->AssociatedIrp.SystemBuffer;
    fcb* fcb = FileObject->FsContext;
    ccb* ccb = FileObject->FsContext2;
    file_ref *fileref = ccb ? ccb->fileref : NULL, *oldfileref = NULL, *related = NULL;
    WCHAR* fn;
    ULONG fnlen, utf8len;
    UNICODE_STRING fnus;
    ANSI_STRING utf8;
    NTSTATUS Status;
    LIST_ENTRY rollback;
    SECURITY_SUBJECT_CONTEXT subjcont;
    ACCESS_MASK access;
    ULONG flags;

    InitializeListHead(&rollback);

    if (ex)
        flags = fri->Flags;
    else
        flags = fri->ReplaceIfExists ? FILE_RENAME_REPLACE_IF_EXISTS : 0;

    TRACE("tfo = %p\n", tfo);
    TRACE("Flags = %lx\n", flags);
    TRACE("RootDirectory = %p\n", fri->RootDirectory);
    TRACE("FileName = %.*S\n", (int)(fri->FileNameLength / sizeof(WCHAR)), fri->FileName);

    fn = fri->FileName;
    fnlen = fri->FileNameLength / sizeof(WCHAR);

    if (!tfo) {
        if (!fileref || !fileref->parent) {
            ERR("no fileref set and no directory given\n");
            return STATUS_INVALID_PARAMETER;
        }
    } else {
        LONG i;

        while (fnlen > 0 && (fri->FileName[fnlen - 1] == '/' || fri->FileName[fnlen - 1] == '\\')) {
            fnlen--;
        }

        if (fnlen == 0)
            return STATUS_INVALID_PARAMETER;

        for (i = fnlen - 1; i >= 0; i--) {
            if (fri->FileName[i] == '\\' || fri->FileName[i] == '/') {
                fn = &fri->FileName[i+1];
                fnlen -= i + 1;
                break;
            }
        }
    }

    ExAcquireResourceSharedLite(&Vcb->tree_lock, true);
    ExAcquireResourceExclusiveLite(&Vcb->fileref_lock, true);
    ExAcquireResourceExclusiveLite(fcb->Header.Resource, true);

    if (fcb->inode == BTRFS_FIRST_FREE_OBJECTID && fcb->subvol->id == BTRFS_FS_TREE_OBJECTID) {
        WARN("not allowing \\$Root to be renamed\n");
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }

    if (fcb->ads) {
        if (FileObject->SectionObjectPointer && FileObject->SectionObjectPointer->DataSectionObject) {
            IO_STATUS_BLOCK iosb;

            CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &iosb);
            if (!NT_SUCCESS(iosb.Status)) {
                ERR("CcFlushCache returned %08lx\n", iosb.Status);
                Status = iosb.Status;
                goto end;
            }
        }

        Status = rename_stream(Vcb, fileref, ccb, fri, flags, Irp, &rollback);
        goto end;
    } else if (fnlen >= 1 && fn[0] == ':') {
        if (FileObject->SectionObjectPointer && FileObject->SectionObjectPointer->DataSectionObject) {
            IO_STATUS_BLOCK iosb;

            CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &iosb);
            if (!NT_SUCCESS(iosb.Status)) {
                ERR("CcFlushCache returned %08lx\n", iosb.Status);
                Status = iosb.Status;
                goto end;
            }
        }

        Status = rename_file_to_stream(Vcb, fileref, ccb, fri, flags, Irp, &rollback);
        goto end;
    }

    fnus.Buffer = fn;
    fnus.Length = fnus.MaximumLength = (uint16_t)(fnlen * sizeof(WCHAR));

    TRACE("fnus = %.*S\n", (int)(fnus.Length / sizeof(WCHAR)), fnus.Buffer);

    Status = check_file_name_valid(&fnus, false, false);
    if (!NT_SUCCESS(Status))
        goto end;

    Status = utf16_to_utf8(NULL, 0, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
        goto end;

    utf8.MaximumLength = utf8.Length = (uint16_t)utf8len;
    utf8.Buffer = ExAllocatePoolWithTag(PagedPool, utf8.MaximumLength, ALLOC_TAG);
    if (!utf8.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    Status = utf16_to_utf8(utf8.Buffer, utf8len, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
        goto end;

    if (tfo && tfo->FsContext2) {
        struct _ccb* relatedccb = tfo->FsContext2;

        related = relatedccb->fileref;
        increase_fileref_refcount(related);
    } else if (fnus.Length >= sizeof(WCHAR) && fnus.Buffer[0] != '\\') {
        related = fileref->parent;
        increase_fileref_refcount(related);
    }

    Status = open_fileref(Vcb, &oldfileref, &fnus, related, false, NULL, NULL, PagedPool, ccb->case_sensitive,  Irp);

    if (NT_SUCCESS(Status)) {
        TRACE("destination file already exists\n");

        if (fileref != oldfileref && !oldfileref->deleted) {
            if (!(flags & FILE_RENAME_REPLACE_IF_EXISTS)) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                goto end;
            } else if (fileref == oldfileref) {
                Status = STATUS_ACCESS_DENIED;
                goto end;
            } else if (!(flags & FILE_RENAME_POSIX_SEMANTICS) && (oldfileref->open_count > 0 || has_open_children(oldfileref)) && !oldfileref->deleted) {
                WARN("trying to overwrite open file\n");
                Status = STATUS_ACCESS_DENIED;
                goto end;
            } else if (!(flags & FILE_RENAME_IGNORE_READONLY_ATTRIBUTE) && oldfileref->fcb->atts & FILE_ATTRIBUTE_READONLY) {
                WARN("trying to overwrite readonly file\n");
                Status = STATUS_ACCESS_DENIED;
                goto end;
            } else if (oldfileref->fcb->type == BTRFS_FT_DIR) {
                WARN("trying to overwrite directory\n");
                Status = STATUS_ACCESS_DENIED;
                goto end;
            }
        }

        if (fileref == oldfileref || oldfileref->deleted) {
            free_fileref(oldfileref);
            oldfileref = NULL;
        }
    }

    if (!related) {
        Status = open_fileref(Vcb, &related, &fnus, NULL, true, NULL, NULL, PagedPool, ccb->case_sensitive, Irp);

        if (Status == STATUS_REPARSE) {
            ERR("open_fileref returned %08lx\n", Status);
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
            goto end;
        } else if (!NT_SUCCESS(Status)) {
            ERR("open_fileref returned %08lx\n", Status);
            goto end;
        }
    }

    if (related->fcb == Vcb->dummy_fcb) {
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }

    SeCaptureSubjectContext(&subjcont);

    ExAcquireResourceSharedLite(related->fcb->Header.Resource, true);

    if (!SeAccessCheck(related->fcb->sd, &subjcont, false, fcb->type == BTRFS_FT_DIR ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE, 0, NULL,
        IoGetFileObjectGenericMapping(), Irp->RequestorMode, &access, &Status)) {
        ExReleaseResourceLite(related->fcb->Header.Resource);
        SeReleaseSubjectContext(&subjcont);
        TRACE("SeAccessCheck failed, returning %08lx\n", Status);
        goto end;
    }

    ExReleaseResourceLite(related->fcb->Header.Resource);

    SeReleaseSubjectContext(&subjcont);

    if (has_open_children(fileref)) {
        WARN("trying to rename file with open children\n");
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }

    if (oldfileref) {
        SeCaptureSubjectContext(&subjcont);

        ExAcquireResourceSharedLite(oldfileref->fcb->Header.Resource, true);

        if (!SeAccessCheck(oldfileref->fcb->sd, &subjcont, false, DELETE, 0, NULL,
                           IoGetFileObjectGenericMapping(), Irp->RequestorMode, &access, &Status)) {
            ExReleaseResourceLite(oldfileref->fcb->Header.Resource);
            SeReleaseSubjectContext(&subjcont);
            TRACE("SeAccessCheck failed, returning %08lx\n", Status);
            goto end;
        }

        ExReleaseResourceLite(oldfileref->fcb->Header.Resource);

        SeReleaseSubjectContext(&subjcont);

        if (oldfileref->open_count > 0 && flags & FILE_RENAME_POSIX_SEMANTICS) {
            oldfileref->delete_on_close = true;
            oldfileref->posix_delete = true;
        }

        Status = delete_fileref(oldfileref, NULL, oldfileref->open_count > 0 && flags & FILE_RENAME_POSIX_SEMANTICS, Irp, &rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_fileref returned %08lx\n", Status);
            goto end;
        }
    }

    if (fileref->parent->fcb->subvol != related->fcb->subvol && (fileref->fcb->subvol == fileref->parent->fcb->subvol || fileref->fcb == Vcb->dummy_fcb))
        Status = move_across_subvols(fileref, ccb, related, &utf8, &fnus, Irp);
    else if (related == fileref->parent) // keeping file in same directory
        Status = rename_in_same_directory(fileref, ccb, related, &utf8, &fnus, Irp);
    else
        Status = move_across_directories(fileref, ccb, related, &utf8, &fnus, Irp);

end:
    if (oldfileref)
        free_fileref(oldfileref);

    if (!NT_SUCCESS(Status) && related)
        free_fileref(related);

    if (NT_SUCCESS(Status))
        clear_rollback(&rollback);
    else
        do_rollback(Vcb, &rollback);

    ExReleaseResourceLite(fcb->Header.Resource);
    ExReleaseResourceLite(&Vcb->fileref_lock);
    ExReleaseResourceLite(&Vcb->tree_lock);

    return Status;
}
