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
#include "file-create.h"
#include "crc32c.h"

static NTSTATUS file_create_parse_ea(fcb* fcb, FILE_FULL_EA_INFORMATION* ea) {
    NTSTATUS Status;
    LIST_ENTRY ealist, *le;
    ULONG size = 0;
    char* buf;

    InitializeListHead(&ealist);

    do {
        STRING s;
        bool found = false;

        s.Length = s.MaximumLength = ea->EaNameLength;
        s.Buffer = ea->EaName;

        RtlUpperString(&s, &s);

        le = ealist.Flink;
        while (le != &ealist) {
            ea_item* item = CONTAINING_RECORD(le, ea_item, list_entry);

            if (item->name.Length == s.Length && RtlCompareMemory(item->name.Buffer, s.Buffer, s.Length) == s.Length) {
                item->flags = ea->Flags;
                item->value.Length = item->value.MaximumLength = ea->EaValueLength;
                item->value.Buffer = &ea->EaName[ea->EaNameLength + 1];
                found = true;
                break;
            }

            le = le->Flink;
        }

        if (!found) {
            ea_item* item = ExAllocatePoolWithTag(PagedPool, sizeof(ea_item), ALLOC_TAG);
            if (!item) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
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

    // handle LXSS values
    le = ealist.Flink;
    while (le != &ealist) {
        LIST_ENTRY* le2 = le->Flink;
        ea_item* item = CONTAINING_RECORD(le, ea_item, list_entry);

        if (item->name.Length == sizeof(lxuid) - 1 && RtlCompareMemory(item->name.Buffer, lxuid, item->name.Length) == item->name.Length) {
            if (item->value.Length < sizeof(uint32_t)) {
                ERR("uid value was shorter than expected\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end;
            }

            RtlCopyMemory(&fcb->inode_item.uid, item->value.Buffer, sizeof(uint32_t));
            fcb->sd_dirty = true;
            fcb->sd_deleted = false;

            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        } else if (item->name.Length == sizeof(lxgid) - 1 && RtlCompareMemory(item->name.Buffer, lxgid, item->name.Length) == item->name.Length) {
            if (item->value.Length < sizeof(uint32_t)) {
                ERR("gid value was shorter than expected\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end;
            }

            RtlCopyMemory(&fcb->inode_item.gid, item->value.Buffer, sizeof(uint32_t));

            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        } else if (item->name.Length == sizeof(lxmod) - 1 && RtlCompareMemory(item->name.Buffer, lxmod, item->name.Length) == item->name.Length) {
            uint32_t allowed = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH | S_ISGID | S_ISVTX | S_ISUID;
            uint32_t val;

            if (item->value.Length < sizeof(uint32_t)) {
                ERR("mode value was shorter than expected\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end;
            }

            RtlCopyMemory(&val, item->value.Buffer, sizeof(val));

            fcb->inode_item.mode &= ~allowed;
            fcb->inode_item.mode |= val & allowed;

            if (fcb->type != BTRFS_FT_DIR) {
                if (__S_ISTYPE(val, __S_IFCHR)) {
                    fcb->type = BTRFS_FT_CHRDEV;
                    fcb->inode_item.mode &= ~__S_IFMT;
                    fcb->inode_item.mode |= __S_IFCHR;
                } else if (__S_ISTYPE(val, __S_IFBLK)) {
                    fcb->type = BTRFS_FT_BLKDEV;
                    fcb->inode_item.mode &= ~__S_IFMT;
                    fcb->inode_item.mode |= __S_IFBLK;
                } else if (__S_ISTYPE(val, __S_IFIFO)) {
                    fcb->type = BTRFS_FT_FIFO;
                    fcb->inode_item.mode &= ~__S_IFMT;
                    fcb->inode_item.mode |= __S_IFIFO;
                } else if (__S_ISTYPE(val, __S_IFSOCK)) {
                    fcb->type = BTRFS_FT_SOCK;
                    fcb->inode_item.mode &= ~__S_IFMT;
                    fcb->inode_item.mode |= __S_IFSOCK;
                }
            }

            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        } else if (item->name.Length == sizeof(lxdev) - 1 && RtlCompareMemory(item->name.Buffer, lxdev, item->name.Length) == item->name.Length) {
            uint32_t major, minor;

            if (item->value.Length < sizeof(uint64_t)) {
                ERR("dev value was shorter than expected\n");
                Status = STATUS_INVALID_PARAMETER;
                goto end;
            }

            RtlCopyMemory(&major, item->value.Buffer, sizeof(major));
            RtlCopyMemory(&minor, item->value.Buffer + sizeof(major), sizeof(minor));

            fcb->inode_item.rdev = (minor & 0xFFFFF) | ((major & 0xFFFFFFFFFFF) << 20);

            RemoveEntryList(&item->list_entry);
            ExFreePool(item);
        }

        le = le2;
    }

    if (fcb->type != BTRFS_FT_CHRDEV && fcb->type != BTRFS_FT_BLKDEV)
        fcb->inode_item.rdev = 0;

    if (IsListEmpty(&ealist))
        return STATUS_SUCCESS;

    le = ealist.Flink;
    while (le != &ealist) {
        ea_item* item = CONTAINING_RECORD(le, ea_item, list_entry);

        if (size % 4 > 0)
            size += 4 - (size % 4);

        size += (ULONG)offsetof(FILE_FULL_EA_INFORMATION, EaName[0]) + item->name.Length + 1 + item->value.Length;

        if (size > UINT16_MAX || size + sizeof(struct btrfs_header) + sizeof(struct btrfs_item) +
            sizeof(struct btrfs_dir_item) + sizeof(EA_EA) - 1 > fcb->Vcb->superblock.nodesize) {
            Status = STATUS_EA_TOO_LARGE;
            goto end;
        }

        le = le->Flink;
    }

    buf = ExAllocatePoolWithTag(PagedPool, size, ALLOC_TAG);
    if (!buf) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    fcb->ea_xattr.Length = fcb->ea_xattr.MaximumLength = size;
    fcb->ea_xattr.Buffer = buf;

    fcb->ealen = 4;
    ea = NULL;

    le = ealist.Flink;
    while (le != &ealist) {
        ea_item* item = CONTAINING_RECORD(le, ea_item, list_entry);

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

    fcb->ea_changed = true;

    Status = STATUS_SUCCESS;

end:
    while (!IsListEmpty(&ealist)) {
        ea_item* item = CONTAINING_RECORD(RemoveHeadList(&ealist), ea_item, list_entry);

        ExFreePool(item);
    }

    return Status;
}

NTSTATUS file_create2(_In_ PIRP Irp, _Requires_exclusive_lock_held_(_Curr_->fcb_lock) _In_ device_extension* Vcb, _In_ PUNICODE_STRING fpus,
                             _In_ file_ref* parfileref, _In_ ULONG options, _In_reads_bytes_opt_(ealen) FILE_FULL_EA_INFORMATION* ea, _In_ ULONG ealen,
                             _Out_ file_ref** pfr, bool case_sensitive, _In_ LIST_ENTRY* rollback) {
    NTSTATUS Status;
    fcb* fcb;
    ULONG utf8len;
    char* utf8 = NULL;
    uint64_t inode;
    uint8_t type;
    LARGE_INTEGER time;
    struct btrfs_timespec now;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    POOL_TYPE pool_type = IrpSp->Flags & SL_OPEN_PAGING_FILE ? NonPagedPool : PagedPool;
    USHORT defda;
    file_ref* fileref;
    dir_child* dc;
    ANSI_STRING utf8as;
    LIST_ENTRY* lastle = NULL;
    file_ref* existing_fileref = NULL;
    bool name_exists = false;

    if (parfileref->fcb == Vcb->dummy_fcb)
        return STATUS_ACCESS_DENIED;

    if (options & FILE_DIRECTORY_FILE && IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_TEMPORARY)
        return STATUS_INVALID_PARAMETER;

    Status = utf16_to_utf8(NULL, 0, &utf8len, fpus->Buffer, fpus->Length);
    if (!NT_SUCCESS(Status)) {
        ERR("utf16_to_utf8 returned %08lx\n", Status);
        return Status;
    }

    utf8 = ExAllocatePoolWithTag(pool_type, utf8len + 1, ALLOC_TAG);
    if (!utf8) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = utf16_to_utf8(utf8, utf8len, &utf8len, fpus->Buffer, fpus->Length);
    if (!NT_SUCCESS(Status)) {
        ERR("utf16_to_utf8 returned %08lx\n", Status);
        ExFreePool(utf8);
        return Status;
    }

    utf8[utf8len] = 0;

    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);

    TRACE("create file %.*S\n", (int)(fpus->Length / sizeof(WCHAR)), fpus->Buffer);
    ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
    TRACE("parfileref->fcb->inode_item.size (inode %I64x) was %I64x\n", parfileref->fcb->inode, parfileref->fcb->inode_item.size);
    parfileref->fcb->inode_item.size += utf8len * 2;
    TRACE("parfileref->fcb->inode_item.size (inode %I64x) now %I64x\n", parfileref->fcb->inode, parfileref->fcb->inode_item.size);
    parfileref->fcb->inode_item.transid = Vcb->superblock.generation;
    parfileref->fcb->inode_item.sequence++;
    parfileref->fcb->inode_item.ctime = now;
    parfileref->fcb->inode_item.mtime = now;
    ExReleaseResourceLite(parfileref->fcb->Header.Resource);

    parfileref->fcb->inode_item_changed = true;
    mark_fcb_dirty(parfileref->fcb);

    inode = InterlockedIncrement64(&parfileref->fcb->subvol->lastinode);

    type = options & FILE_DIRECTORY_FILE ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE;

    // FIXME - link FILE_ATTRIBUTE_READONLY to mode

    TRACE("requested attributes = %x\n", IrpSp->Parameters.Create.FileAttributes);

    defda = 0;

    if (utf8[0] == '.')
        defda |= FILE_ATTRIBUTE_HIDDEN;

    if (options & FILE_DIRECTORY_FILE) {
        defda |= FILE_ATTRIBUTE_DIRECTORY;
        IrpSp->Parameters.Create.FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else
        IrpSp->Parameters.Create.FileAttributes &= ~FILE_ATTRIBUTE_DIRECTORY;

    if (!(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        IrpSp->Parameters.Create.FileAttributes |= FILE_ATTRIBUTE_ARCHIVE;
        defda |= FILE_ATTRIBUTE_ARCHIVE;
    }

    TRACE("defda = %x\n", defda);

    if (IrpSp->Parameters.Create.FileAttributes == FILE_ATTRIBUTE_NORMAL)
        IrpSp->Parameters.Create.FileAttributes = defda;

    fcb = create_fcb(Vcb, pool_type);
    if (!fcb) {
        ERR("out of memory\n");
        ExFreePool(utf8);

        ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
        parfileref->fcb->inode_item.size -= utf8len * 2;
        ExReleaseResourceLite(parfileref->fcb->Header.Resource);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fcb->Vcb = Vcb;

    if (IrpSp->Flags & SL_OPEN_PAGING_FILE)
        fcb->Header.Flags2 |= FSRTL_FLAG2_IS_PAGING_FILE;

    fcb->inode_item.generation = Vcb->superblock.generation;
    fcb->inode_item.transid = Vcb->superblock.generation;
    fcb->inode_item.size = 0;
    fcb->inode_item.nbytes = 0;
    fcb->inode_item.block_group = 0;
    fcb->inode_item.nlink = 1;
    fcb->inode_item.gid = GID_NOBODY; // FIXME?
    fcb->inode_item.mode = inherit_mode(parfileref->fcb, type == BTRFS_FT_DIR); // use parent's permissions by default
    fcb->inode_item.rdev = 0;
    fcb->inode_item.flags = 0;
    fcb->inode_item.sequence = 1;
    fcb->inode_item.atime = now;
    fcb->inode_item.ctime = now;
    fcb->inode_item.mtime = now;
    fcb->inode_item.otime = now;

    if (type == BTRFS_FT_DIR)
        fcb->inode_item.mode |= S_IFDIR;
    else {
        fcb->inode_item.mode |= S_IFREG;
        fcb->inode_item.mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH); // remove executable bit if not directory
    }

    if (IrpSp->Flags & SL_OPEN_PAGING_FILE) {
        fcb->inode_item.flags = BTRFS_INODE_NODATACOW | BTRFS_INODE_NODATASUM | BTRFS_INODE_NOCOMPRESS;
    } else {
        // inherit nodatacow flag from parent directory
        if (parfileref->fcb->inode_item.flags & BTRFS_INODE_NODATACOW || Vcb->options.nodatacow) {
            fcb->inode_item.flags |= BTRFS_INODE_NODATACOW;

            if (type != BTRFS_FT_DIR)
                fcb->inode_item.flags |= BTRFS_INODE_NODATASUM;
        }

        if (parfileref->fcb->inode_item.flags & BTRFS_INODE_COMPRESS &&
            !(fcb->inode_item.flags & BTRFS_INODE_NODATACOW)) {
            fcb->inode_item.flags |= BTRFS_INODE_COMPRESS;
        }
    }

    if (!(fcb->inode_item.flags & BTRFS_INODE_NODATACOW)) {
        fcb->prop_compression = parfileref->fcb->prop_compression;
        fcb->prop_compression_changed = fcb->prop_compression != PropCompression_None;
    } else
        fcb->prop_compression = PropCompression_None;

    fcb->inode_item_changed = true;

    fcb->Header.IsFastIoPossible = fast_io_possible(fcb);
    fcb->Header.AllocationSize.QuadPart = 0;
    fcb->Header.FileSize.QuadPart = 0;
    fcb->Header.ValidDataLength.QuadPart = 0;

    fcb->atts = IrpSp->Parameters.Create.FileAttributes & ~FILE_ATTRIBUTE_NORMAL;
    fcb->atts_changed = fcb->atts != defda;

    fcb->subvol = parfileref->fcb->subvol;
    fcb->inode = inode;
    fcb->type = type;
    fcb->created = true;
    fcb->deleted = true;

    fcb->hash = calc_crc32c(0xffffffff, (uint8_t*)&inode, sizeof(uint64_t));

    acquire_fcb_lock_exclusive(Vcb);

    if (fcb->subvol->fcbs_ptrs[fcb->hash >> 24]) {
        LIST_ENTRY* le = fcb->subvol->fcbs_ptrs[fcb->hash >> 24];

        while (le != &fcb->subvol->fcbs) {
            struct _fcb* fcb2 = CONTAINING_RECORD(le, struct _fcb, list_entry);

            if (fcb2->hash > fcb->hash) {
                lastle = le->Blink;
                break;
            }

            le = le->Flink;
        }
    }

    if (!lastle) {
        uint8_t c = fcb->hash >> 24;

        if (c != 0xff) {
            uint8_t d = c + 1;

            do {
                if (fcb->subvol->fcbs_ptrs[d]) {
                    lastle = fcb->subvol->fcbs_ptrs[d]->Blink;
                    break;
                }

                d++;
            } while (d != 0);
        }
    }

    if (lastle) {
        InsertHeadList(lastle, &fcb->list_entry);

        if (lastle == &fcb->subvol->fcbs || (CONTAINING_RECORD(lastle, struct _fcb, list_entry)->hash >> 24) != (fcb->hash >> 24))
            fcb->subvol->fcbs_ptrs[fcb->hash >> 24] = &fcb->list_entry;
    } else {
        InsertTailList(&fcb->subvol->fcbs, &fcb->list_entry);

        if (fcb->list_entry.Blink == &fcb->subvol->fcbs || (CONTAINING_RECORD(fcb->list_entry.Blink, struct _fcb, list_entry)->hash >> 24) != (fcb->hash >> 24))
            fcb->subvol->fcbs_ptrs[fcb->hash >> 24] = &fcb->list_entry;
    }

    InsertTailList(&Vcb->all_fcbs, &fcb->list_entry_all);

    fcb->subvol->fcbs_version++;

    release_fcb_lock(Vcb);

    mark_fcb_dirty(fcb);

    Status = fcb_get_new_sd(fcb, parfileref, IrpSp->Parameters.Create.SecurityContext->AccessState);

    if (!NT_SUCCESS(Status)) {
        ERR("fcb_get_new_sd returned %08lx\n", Status);
        free_fcb(fcb);

        ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
        parfileref->fcb->inode_item.size -= utf8len * 2;
        ExReleaseResourceLite(parfileref->fcb->Header.Resource);

        ExFreePool(utf8);

        return Status;
    }

    fcb->sd_dirty = true;

    if (ea && ealen > 0) {
        Status = file_create_parse_ea(fcb, ea);
        if (!NT_SUCCESS(Status)) {
            ERR("file_create_parse_ea returned %08lx\n", Status);
            free_fcb(fcb);

            ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
            parfileref->fcb->inode_item.size -= utf8len * 2;
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);

            ExFreePool(utf8);

            return Status;
        }
    }

    fileref = create_fileref(Vcb);
    if (!fileref) {
        ERR("out of memory\n");
        free_fcb(fcb);

        ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
        parfileref->fcb->inode_item.size -= utf8len * 2;
        ExReleaseResourceLite(parfileref->fcb->Header.Resource);

        ExFreePool(utf8);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fileref->fcb = fcb;

    if (Irp->Overlay.AllocationSize.QuadPart > 0 && !write_fcb_compressed(fcb) && fcb->type != BTRFS_FT_DIR) {
        Status = extend_file(fcb, fileref, Irp->Overlay.AllocationSize.QuadPart, true, NULL, rollback);

        if (!NT_SUCCESS(Status)) {
            ERR("extend_file returned %08lx\n", Status);
            reap_fileref(Vcb, fileref);

            ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
            parfileref->fcb->inode_item.size -= utf8len * 2;
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);

            ExFreePool(utf8);

            return Status;
        }
    }

    if (fcb->type == BTRFS_FT_DIR) {
        fcb->hash_ptrs = ExAllocatePoolWithTag(PagedPool, sizeof(LIST_ENTRY*) * 256, ALLOC_TAG);
        if (!fcb->hash_ptrs) {
            ERR("out of memory\n");
            reap_fileref(Vcb, fileref);

            ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
            parfileref->fcb->inode_item.size -= utf8len * 2;
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);

            ExFreePool(utf8);

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(fcb->hash_ptrs, sizeof(LIST_ENTRY*) * 256);

        fcb->hash_ptrs_uc = ExAllocatePoolWithTag(PagedPool, sizeof(LIST_ENTRY*) * 256, ALLOC_TAG);
        if (!fcb->hash_ptrs_uc) {
            ERR("out of memory\n");
            reap_fileref(Vcb, fileref);

            ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
            parfileref->fcb->inode_item.size -= utf8len * 2;
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);

            ExFreePool(utf8);

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(fcb->hash_ptrs_uc, sizeof(LIST_ENTRY*) * 256);
    }

    fileref->created = true;

    fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
    fcb->subvol->root_item.ctime = now;

    utf8as.Buffer = utf8;
    utf8as.Length = utf8as.MaximumLength = (uint16_t)utf8len;

    ExAcquireResourceExclusiveLite(&parfileref->fcb->nonpaged->dir_children_lock, true);

    // check again doesn't already exist
    if (case_sensitive) {
        uint32_t dc_hash = calc_crc32c(0xffffffff, (uint8_t*)fpus->Buffer, fpus->Length);

        if (parfileref->fcb->hash_ptrs[dc_hash >> 24]) {
            LIST_ENTRY* le = parfileref->fcb->hash_ptrs[dc_hash >> 24];
            while (le != &parfileref->fcb->dir_children_hash) {
                dc = CONTAINING_RECORD(le, dir_child, list_entry_hash);

                if (dc->hash == dc_hash && dc->name.Length == fpus->Length && RtlCompareMemory(dc->name.Buffer, fpus->Buffer, fpus->Length) == fpus->Length) {
                    name_exists = true;
                    existing_fileref = dc->fileref;
                    break;
                } else if (dc->hash > dc_hash)
                    break;

                le = le->Flink;
            }
        }
    } else {
        UNICODE_STRING fpusuc;

        Status = RtlUpcaseUnicodeString(&fpusuc, fpus, true);
        if (!NT_SUCCESS(Status)) {
            ExReleaseResourceLite(&parfileref->fcb->nonpaged->dir_children_lock);
            ERR("RtlUpcaseUnicodeString returned %08lx\n", Status);
            reap_fileref(Vcb, fileref);

            ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
            parfileref->fcb->inode_item.size -= utf8len * 2;
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);

            ExFreePool(utf8);

            return Status;
        }

        uint32_t dc_hash = calc_crc32c(0xffffffff, (uint8_t*)fpusuc.Buffer, fpusuc.Length);

        if (parfileref->fcb->hash_ptrs_uc[dc_hash >> 24]) {
            LIST_ENTRY* le = parfileref->fcb->hash_ptrs_uc[dc_hash >> 24];
            while (le != &parfileref->fcb->dir_children_hash_uc) {
                dc = CONTAINING_RECORD(le, dir_child, list_entry_hash_uc);

                if (dc->hash_uc == dc_hash && dc->name_uc.Length == fpusuc.Length && RtlCompareMemory(dc->name_uc.Buffer, fpusuc.Buffer, fpusuc.Length) == fpusuc.Length) {
                    name_exists = true;
                    existing_fileref = dc->fileref;
                    break;
                } else if (dc->hash_uc > dc_hash)
                    break;

                le = le->Flink;
            }
        }

        ExFreePool(fpusuc.Buffer);
    }

    if (name_exists) {
        if (existing_fileref)
            increase_fileref_refcount(existing_fileref);

        ExReleaseResourceLite(&parfileref->fcb->nonpaged->dir_children_lock);
        reap_fileref(Vcb, fileref);

        ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
        parfileref->fcb->inode_item.size -= utf8len * 2;
        ExReleaseResourceLite(parfileref->fcb->Header.Resource);

        ExFreePool(utf8);

        // A cached directory entry can outlive its fileref.
        if (!existing_fileref) {
            Status = open_fileref_child(Vcb, parfileref, fpus, case_sensitive, true, false, pool_type, &existing_fileref, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("open_fileref_child returned %08lx\n", Status);
                return Status;
            }
        }

        *pfr = existing_fileref;

        return STATUS_OBJECT_NAME_COLLISION;
    }

    Status = add_dir_child(parfileref->fcb, fcb->inode, false, &utf8as, fpus,
                           fcb->type, &dc, rollback);
    if (!NT_SUCCESS(Status)) {
        ExReleaseResourceLite(&parfileref->fcb->nonpaged->dir_children_lock);
        ERR("add_dir_child returned %08lx\n", Status);
        reap_fileref(Vcb, fileref);

        ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
        parfileref->fcb->inode_item.size -= utf8len * 2;
        ExReleaseResourceLite(parfileref->fcb->Header.Resource);

        ExFreePool(utf8);

        return Status;
    }

    fcb->deleted = false;

    fileref->parent = parfileref;
    fileref->dc = dc;
    dc->fileref = fileref;

    if (type == BTRFS_FT_DIR)
        fileref->fcb->fileref = fileref;

    InsertTailList(&parfileref->children, &fileref->list_entry);
    ExReleaseResourceLite(&parfileref->fcb->nonpaged->dir_children_lock);

    ExFreePool(utf8);

    mark_fileref_dirty(fileref);
    increase_fileref_refcount(parfileref);

    *pfr = fileref;

    TRACE("created new file in subvol %I64x, inode %I64x\n", fcb->subvol->id, fcb->inode);

    return STATUS_SUCCESS;
}
