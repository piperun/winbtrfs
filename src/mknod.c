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

#include "mknod.h"
#include "btrfsioctl.h"
#include "crc32c.h"

#define SEF_SACL_AUTO_INHERIT 0x02

static NTSTATUS check_inode_used(_In_ _Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb,
                                 _In_ root* subvol, _In_ uint64_t inode, _In_ uint32_t hash, _In_opt_ PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    uint8_t c = hash >> 24;

    if (subvol->fcbs_ptrs[c]) {
        LIST_ENTRY* le = subvol->fcbs_ptrs[c];

        while (le != &subvol->fcbs) {
            struct _fcb* fcb2 = CONTAINING_RECORD(le, struct _fcb, list_entry);

            if (fcb2->inode == inode)
                return STATUS_SUCCESS;
            else if (fcb2->hash > hash)
                break;

            le = le->Flink;
        }
    }

    searchkey.objectid = inode;
    searchkey.type = BTRFS_INODE_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, subvol, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    if (tp.item->key.objectid == searchkey.objectid && tp.item->key.type == searchkey.type)
        return STATUS_SUCCESS;

    return STATUS_NOT_FOUND;
}

NTSTATUS mknod(device_extension* Vcb, PFILE_OBJECT FileObject, void* data, ULONG datalen, PIRP Irp) {
    NTSTATUS Status;
    btrfs_mknod* bmn;
    fcb *parfcb, *fcb;
    ccb* parccb;
    file_ref *parfileref, *fileref;
    UNICODE_STRING name;
    root* subvol;
    uint64_t inode;
    dir_child* dc;
    LARGE_INTEGER time;
    struct btrfs_timespec now;
    ANSI_STRING utf8;
    ULONG len, i;
    SECURITY_SUBJECT_CONTEXT subjcont;
    PSID owner;
    BOOLEAN defaulted;

    TRACE("(%p, %p, %p, %lu)\n", Vcb, FileObject, data, datalen);

    if (!FileObject || !FileObject->FsContext || !FileObject->FsContext2 || FileObject->FsContext == Vcb->volume_fcb)
        return STATUS_INVALID_PARAMETER;

    if (Vcb->readonly)
        return STATUS_MEDIA_WRITE_PROTECTED;

    parfcb = FileObject->FsContext;

    if (parfcb->type != BTRFS_FT_DIR) {
        WARN("trying to create file in something other than a directory\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (is_subvol_readonly(parfcb->subvol, Irp))
        return STATUS_ACCESS_DENIED;

    parccb = FileObject->FsContext2;
    parfileref = parccb->fileref;

    if (!parfileref)
        return STATUS_INVALID_PARAMETER;

    if (datalen < sizeof(btrfs_mknod))
        return STATUS_INVALID_PARAMETER;

    bmn = (btrfs_mknod*)data;

    if (datalen < offsetof(btrfs_mknod, name[0]) + bmn->namelen || bmn->namelen < sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER;

    if (bmn->type == BTRFS_FT_UNKNOWN || bmn->type > BTRFS_FT_SYMLINK)
        return STATUS_INVALID_PARAMETER;

    if ((bmn->type == BTRFS_FT_DIR && !(parccb->access & FILE_ADD_SUBDIRECTORY)) ||
        (bmn->type != BTRFS_FT_DIR && !(parccb->access & FILE_ADD_FILE))) {
        WARN("insufficient privileges\n");
        return STATUS_ACCESS_DENIED;
    }

    if (bmn->inode != 0) {
        if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), Irp->RequestorMode))
            return STATUS_PRIVILEGE_NOT_HELD;
    }

    for (i = 0; i < bmn->namelen / sizeof(WCHAR); i++) {
        if (bmn->name[i] == 0 || bmn->name[i] == '/')
            return STATUS_OBJECT_NAME_INVALID;
    }

    // don't allow files called . or ..
    if (bmn->name[0] == '.' && (bmn->namelen == sizeof(WCHAR) || (bmn->namelen == 2 * sizeof(WCHAR) && bmn->name[1] == '.')))
        return STATUS_OBJECT_NAME_INVALID;

    Status = utf16_to_utf8(NULL, 0, &len, bmn->name, bmn->namelen);
    if (!NT_SUCCESS(Status)) {
        ERR("utf16_to_utf8 returned %08lx\n", Status);
        return Status;
    }

    if (len == 0) {
        ERR("utf16_to_utf8 returned a length of 0\n");
        return STATUS_INTERNAL_ERROR;
    }

    if (len > 0xffff) {
        ERR("len was too long (%lx)\n", len);
        return STATUS_INVALID_PARAMETER;
    }

    utf8.MaximumLength = utf8.Length = (USHORT)len;
    utf8.Buffer = ExAllocatePoolWithTag(PagedPool, utf8.MaximumLength, ALLOC_TAG);

    if (!utf8.Buffer) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = utf16_to_utf8(utf8.Buffer, len, &len, bmn->name, bmn->namelen);
    if (!NT_SUCCESS(Status)) {
        ERR("utf16_to_utf8 failed with error %08lx\n", Status);
        ExFreePool(utf8.Buffer);
        return Status;
    }

    name.Length = name.MaximumLength = bmn->namelen;
    name.Buffer = bmn->name;

    Status = find_file_in_dir(&name, parfcb, &subvol, &inode, &dc, true);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        ERR("find_file_in_dir returned %08lx\n", Status);
        goto end;
    }

    if (NT_SUCCESS(Status)) {
        WARN("filename already exists\n");
        Status = STATUS_OBJECT_NAME_COLLISION;
        goto end;
    }

    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);

    fcb = create_fcb(Vcb, PagedPool);
    if (!fcb) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    fcb->Vcb = Vcb;

    fcb->inode_item.generation = Vcb->superblock.generation;
    fcb->inode_item.transid = Vcb->superblock.generation;
    fcb->inode_item.size = 0;
    fcb->inode_item.nbytes = 0;
    fcb->inode_item.block_group = 0;
    fcb->inode_item.nlink = 1;
    fcb->inode_item.uid = UID_NOBODY;
    fcb->inode_item.gid = GID_NOBODY;
    fcb->inode_item.mode = inherit_mode(parfcb, bmn->type == BTRFS_FT_DIR);

    if (bmn->type == BTRFS_FT_BLKDEV || bmn->type == BTRFS_FT_CHRDEV)
        fcb->inode_item.rdev = (minor(bmn->rdev) & 0xFFFFF) | ((major(bmn->rdev) & 0xFFFFFFFFFFF) << 20);
    else
        fcb->inode_item.rdev = 0;

    fcb->inode_item.flags = 0;
    fcb->inode_item.sequence = 1;
    fcb->inode_item.atime = now;
    fcb->inode_item.ctime = now;
    fcb->inode_item.mtime = now;
    fcb->inode_item.otime = now;

    if (bmn->type == BTRFS_FT_DIR)
        fcb->inode_item.mode |= __S_IFDIR;
    else if (bmn->type == BTRFS_FT_CHRDEV)
        fcb->inode_item.mode |= __S_IFCHR;
    else if (bmn->type == BTRFS_FT_BLKDEV)
        fcb->inode_item.mode |= __S_IFBLK;
    else if (bmn->type == BTRFS_FT_FIFO)
        fcb->inode_item.mode |= __S_IFIFO;
    else if (bmn->type == BTRFS_FT_SOCK)
        fcb->inode_item.mode |= __S_IFSOCK;
    else if (bmn->type == BTRFS_FT_SYMLINK)
        fcb->inode_item.mode |= __S_IFLNK;
    else
        fcb->inode_item.mode |= __S_IFREG;

    if (bmn->type != BTRFS_FT_DIR)
        fcb->inode_item.mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH); // remove executable bit if not directory

    // inherit nodatacow flag from parent directory
    if (parfcb->inode_item.flags & BTRFS_INODE_NODATACOW) {
        fcb->inode_item.flags |= BTRFS_INODE_NODATACOW;

        if (bmn->type != BTRFS_FT_DIR)
            fcb->inode_item.flags |= BTRFS_INODE_NODATASUM;
    }

    if (parfcb->inode_item.flags & BTRFS_INODE_COMPRESS)
        fcb->inode_item.flags |= BTRFS_INODE_COMPRESS;

    fcb->prop_compression = parfcb->prop_compression;
    fcb->prop_compression_changed = fcb->prop_compression != PropCompression_None;

    fcb->inode_item_changed = true;

    fcb->Header.IsFastIoPossible = fast_io_possible(fcb);
    fcb->Header.AllocationSize.QuadPart = 0;
    fcb->Header.FileSize.QuadPart = 0;
    fcb->Header.ValidDataLength.QuadPart = 0;

    fcb->atts = 0;

    if (bmn->name[0] == '.')
        fcb->atts |= FILE_ATTRIBUTE_HIDDEN;

    if (bmn->type == BTRFS_FT_DIR)
        fcb->atts |= FILE_ATTRIBUTE_DIRECTORY;

    fcb->atts_changed = false;

    InterlockedIncrement(&parfcb->refcount);
    fcb->subvol = parfcb->subvol;

    SeCaptureSubjectContext(&subjcont);

    ExAcquireResourceSharedLite(parfileref->fcb->Header.Resource, true);

    Status = SeAssignSecurityEx(parfileref->fcb->sd, NULL, (void**)&fcb->sd, NULL, fcb->type == BTRFS_FT_DIR,
                                SEF_SACL_AUTO_INHERIT, &subjcont, IoGetFileObjectGenericMapping(), PagedPool);

    ExReleaseResourceLite(parfileref->fcb->Header.Resource);

    if (!NT_SUCCESS(Status)) {
        ERR("SeAssignSecurityEx returned %08lx\n", Status);
        SeReleaseSubjectContext(&subjcont);
        reap_fcb(fcb);
        goto end;
    }

    Status = RtlGetOwnerSecurityDescriptor(fcb->sd, &owner, &defaulted);
    if (!NT_SUCCESS(Status)) {
        WARN("RtlGetOwnerSecurityDescriptor returned %08lx\n", Status);
        fcb->sd_dirty = true;
    } else {
        fcb->inode_item.uid = sid_to_uid(owner);
        fcb->sd_dirty = fcb->inode_item.uid == UID_NOBODY;
    }

    find_gid(fcb, parfcb, &subjcont);

    SeReleaseSubjectContext(&subjcont);

    ExAcquireResourceExclusiveLite(&Vcb->fileref_lock, true);
    acquire_fcb_lock_exclusive(Vcb);

    if (bmn->inode == 0) {
        fcb->inode = InterlockedIncrement64(&parfcb->subvol->lastinode);
        fcb->hash = calc_crc32c(0xffffffff, (uint8_t*)&fcb->inode, sizeof(uint64_t));
    } else {
        if (bmn->inode > (uint64_t)parfcb->subvol->lastinode) {
            fcb->inode = parfcb->subvol->lastinode = bmn->inode;
            fcb->hash = calc_crc32c(0xffffffff, (uint8_t*)&fcb->inode, sizeof(uint64_t));
        } else {
            uint32_t hash = calc_crc32c(0xffffffff, (uint8_t*)&bmn->inode, sizeof(uint64_t));

            Status = check_inode_used(Vcb, subvol, bmn->inode, hash, Irp);
            if (NT_SUCCESS(Status)) { // STATUS_SUCCESS means inode found
                release_fcb_lock(Vcb);
                ExReleaseResourceLite(&Vcb->fileref_lock);

                WARN("inode collision\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end;
            } else if (Status != STATUS_NOT_FOUND) {
                ERR("check_inode_used returned %08lx\n", Status);

                release_fcb_lock(Vcb);
                ExReleaseResourceLite(&Vcb->fileref_lock);
                goto end;
            }

            fcb->inode = bmn->inode;
            fcb->hash = hash;
        }
    }

    fcb->inode = inode;
    fcb->type = bmn->type;

    if (fcb->type == BTRFS_FT_DIR) {
        fcb->hash_ptrs = ExAllocatePoolWithTag(PagedPool, sizeof(LIST_ENTRY*) * 256, ALLOC_TAG);
        if (!fcb->hash_ptrs) {
            release_fcb_lock(Vcb);
            ExReleaseResourceLite(&Vcb->fileref_lock);

            ERR("out of memory\n");
            reap_fcb(fcb);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        RtlZeroMemory(fcb->hash_ptrs, sizeof(LIST_ENTRY*) * 256);

        fcb->hash_ptrs_uc = ExAllocatePoolWithTag(PagedPool, sizeof(LIST_ENTRY*) * 256, ALLOC_TAG);
        if (!fcb->hash_ptrs_uc) {
            release_fcb_lock(Vcb);
            ExReleaseResourceLite(&Vcb->fileref_lock);

            ERR("out of memory\n");
            reap_fcb(fcb);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        RtlZeroMemory(fcb->hash_ptrs_uc, sizeof(LIST_ENTRY*) * 256);
    }

    fileref = create_fileref(Vcb);
    if (!fileref) {
        release_fcb_lock(Vcb);
        ExReleaseResourceLite(&Vcb->fileref_lock);

        ERR("out of memory\n");
        reap_fcb(fcb);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    fileref->fcb = fcb;

    fcb->created = true;
    fileref->created = true;

    fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
    fcb->subvol->root_item.ctime = now;

    // no rollback machinery, as this is the only thing that can fail
    Status = add_dir_child(parfileref->fcb, fcb->inode, false, &utf8, &name,
                           fcb->type, &dc, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("add_dir_child returned %08lx\n", Status);
        free_fileref(fileref);
        release_fcb_lock(Vcb);
        ExReleaseResourceLite(&Vcb->fileref_lock);
        goto end;
    }

    fileref->parent = parfileref;

    mark_fcb_dirty(fcb);
    mark_fileref_dirty(fileref);

    fileref->dc = dc;
    dc->fileref = fileref;

    ExAcquireResourceExclusiveLite(&parfileref->fcb->nonpaged->dir_children_lock, true);
    InsertTailList(&parfileref->children, &fileref->list_entry);
    ExReleaseResourceLite(&parfileref->fcb->nonpaged->dir_children_lock);

    increase_fileref_refcount(parfileref);

    add_fcb_to_subvol(fcb);
    InsertTailList(&Vcb->all_fcbs, &fcb->list_entry_all);

    if (bmn->type == BTRFS_FT_DIR)
        fileref->fcb->fileref = fileref;

    ExAcquireResourceExclusiveLite(parfcb->Header.Resource, true);
    parfcb->inode_item.size += utf8.Length * 2;
    parfcb->inode_item.transid = Vcb->superblock.generation;
    parfcb->inode_item.sequence++;

    if (!parccb->user_set_change_time)
        parfcb->inode_item.ctime = now;

    if (!parccb->user_set_write_time)
        parfcb->inode_item.mtime = now;

    parfcb->subvol->fcbs_version++;

    ExReleaseResourceLite(parfcb->Header.Resource);
    release_fcb_lock(Vcb);
    ExReleaseResourceLite(&Vcb->fileref_lock);

    parfcb->inode_item_changed = true;
    mark_fcb_dirty(parfcb);

    send_notification_fileref(fileref, bmn->type == BTRFS_FT_DIR ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED, NULL);

    if (!parccb->user_set_write_time)
        queue_notification_fcb(parfileref, FILE_NOTIFY_CHANGE_LAST_WRITE, FILE_ACTION_MODIFIED, NULL);

    Status = STATUS_SUCCESS;

end:
    ExFreePool(utf8.Buffer);

    return Status;
}
