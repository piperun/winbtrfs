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

/* Serialize one directory entry and read the metadata needed for it. */
#include "btrfs_drv.h"
#include "directory-query.h"

static ULONG get_ea_len(device_extension* Vcb, root* subvol, uint64_t inode, PIRP Irp) {
    uint8_t* eadata;
    uint16_t len;

    if (get_xattr(Vcb, subvol, inode, EA_EA, EA_EA_HASH, &eadata, &len, Irp)) {
        ULONG offset;
        NTSTATUS Status;

        Status = IoCheckEaBufferValidity((FILE_FULL_EA_INFORMATION*)eadata, len, &offset);

        if (!NT_SUCCESS(Status)) {
            WARN("IoCheckEaBufferValidity returned %08lx (error at offset %lu)\n", Status, offset);
            ExFreePool(eadata);
            return 0;
        } else {
            FILE_FULL_EA_INFORMATION* eainfo;
            ULONG ealen;

            ealen = 4;
            eainfo = (FILE_FULL_EA_INFORMATION*)eadata;
            do {
                ealen += 5 + eainfo->EaNameLength + eainfo->EaValueLength;

                if (eainfo->NextEntryOffset == 0)
                    break;

                eainfo = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)eainfo) + eainfo->NextEntryOffset);
            } while (true);

            ExFreePool(eadata);

            return ealen;
        }
    } else
        return 0;
}

NTSTATUS query_dir_item(fcb* fcb, ccb* ccb, void* buf, LONG* len, PIRP Irp, dir_entry* de, root* r) {
    PIO_STACK_LOCATION IrpSp;
    LONG needed;
    uint64_t inode;
    struct btrfs_inode_item ii;
    NTSTATUS Status;
    ULONG atts = 0, ealen = 0;
    file_ref* fileref = ccb->fileref;

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (de->key.type == BTRFS_ROOT_ITEM_KEY) { // subvol
        LIST_ENTRY* le;

        r = NULL;

        le = fcb->Vcb->roots.Flink;
        while (le != &fcb->Vcb->roots) {
            root* subvol = CONTAINING_RECORD(le, root, list_entry);

            if (subvol->id == de->key.objectid) {
                r = subvol;
                break;
            }

            le = le->Flink;
        }

        if (r && r->parent != fcb->subvol->id && (!de->dc || !de->dc->root_dir))
            r = NULL;

        inode = BTRFS_FIRST_FREE_OBJECTID;
    } else {
        inode = de->key.objectid;
    }

    if (IrpSp->Parameters.QueryDirectory.FileInformationClass != FileNamesInformation) { // FIXME - object ID and reparse point classes too?
        switch (de->dir_entry_type) {
            case DirEntryType_File:
            {
                if (!r) {
                    LARGE_INTEGER time;

                    ii = fcb->Vcb->dummy_fcb->inode_item;
                    atts = fcb->Vcb->dummy_fcb->atts;
                    ealen = fcb->Vcb->dummy_fcb->ealen;

                    KeQuerySystemTime(&time);
                    win_time_to_unix(time, &ii.otime);
                    ii.atime = ii.mtime = ii.ctime = ii.otime;
                } else {
                    bool found = false;

                    if (de->dc && de->dc->fileref && de->dc->fileref->fcb) {
                        ii = de->dc->fileref->fcb->inode_item;
                        atts = de->dc->fileref->fcb->atts;
                        ealen = de->dc->fileref->fcb->ealen;
                        found = true;
                    }

                    if (!found) {
                        struct btrfs_key searchkey;
                        traverse_ptr tp;

                        searchkey.objectid = inode;
                        searchkey.type = BTRFS_INODE_ITEM_KEY;
                        searchkey.offset = 0xffffffffffffffff;

                        Status = find_item(fcb->Vcb, r, &tp, &searchkey, false, Irp);
                        if (!NT_SUCCESS(Status)) {
                            ERR("error - find_item returned %08lx\n", Status);
                            return Status;
                        }

                        if (tp.item->key.objectid != searchkey.objectid || tp.item->key.type != searchkey.type) {
                            ERR("could not find inode item for inode %I64x in root %I64x\n", inode, r->id);
                            return STATUS_INTERNAL_ERROR;
                        }

                        RtlZeroMemory(&ii, sizeof(struct btrfs_inode_item));

                        if (tp.item->size > 0)
                            RtlCopyMemory(&ii, tp.item->data, min(sizeof(struct btrfs_inode_item), tp.item->size));

                        if (IrpSp->Parameters.QueryDirectory.FileInformationClass == FileBothDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileFullDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdBothDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdFullDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdExtdDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdExtdBothDirectoryInformation) {

                            bool dotfile = de->name.Length > sizeof(WCHAR) && de->name.Buffer[0] == '.';

                            atts = get_file_attributes(fcb->Vcb, r, inode, de->type, dotfile, false, Irp);
                        }

                        if (IrpSp->Parameters.QueryDirectory.FileInformationClass == FileBothDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileFullDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdBothDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdFullDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdExtdDirectoryInformation ||
                            IrpSp->Parameters.QueryDirectory.FileInformationClass == FileIdExtdBothDirectoryInformation) {
                            ealen = get_ea_len(fcb->Vcb, r, inode, Irp);
                        }
                    }
                }

                break;
            }

            case DirEntryType_Self:
                ii = fcb->inode_item;
                r = fcb->subvol;
                inode = fcb->inode;
                atts = fcb->atts;
                ealen = fcb->ealen;
                break;

            case DirEntryType_Parent:
                if (fileref && fileref->parent) {
                    ii = fileref->parent->fcb->inode_item;
                    r = fileref->parent->fcb->subvol;
                    inode = fileref->parent->fcb->inode;
                    atts = fileref->parent->fcb->atts;
                    ealen = fileref->parent->fcb->ealen;
                } else {
                    ERR("no fileref\n");
                    return STATUS_INTERNAL_ERROR;
                }
                break;
        }

        if (atts == 0)
            atts = FILE_ATTRIBUTE_NORMAL;
    }

    switch (IrpSp->Parameters.QueryDirectory.FileInformationClass) {
        case FileBothDirectoryInformation:
        {
            FILE_BOTH_DIR_INFORMATION* fbdi = buf;

            TRACE("FileBothDirectoryInformation\n");

            needed = offsetof(FILE_BOTH_DIR_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            fbdi->NextEntryOffset = 0;
            fbdi->FileIndex = 0;
            fbdi->CreationTime.QuadPart = unix_time_to_win(&ii.otime);
            fbdi->LastAccessTime.QuadPart = unix_time_to_win(&ii.atime);
            fbdi->LastWriteTime.QuadPart = unix_time_to_win(&ii.mtime);
            fbdi->ChangeTime.QuadPart = unix_time_to_win(&ii.ctime);
            fbdi->EndOfFile.QuadPart = de->type == BTRFS_FT_SYMLINK ? 0 : ii.size;

            if (de->type == BTRFS_FT_SYMLINK)
                fbdi->AllocationSize.QuadPart = 0;
            else if (atts & FILE_ATTRIBUTE_SPARSE_FILE)
                fbdi->AllocationSize.QuadPart = ii.nbytes;
            else
                fbdi->AllocationSize.QuadPart = sector_align(ii.size, fcb->Vcb->superblock.sectorsize);

            fbdi->FileAttributes = atts;
            fbdi->FileNameLength = de->name.Length;
            fbdi->EaSize = (r && atts & FILE_ATTRIBUTE_REPARSE_POINT) ? get_reparse_tag(fcb->Vcb, r, inode, de->type, atts, ccb->lxss, Irp) : ealen;
            fbdi->ShortNameLength = 0;

            RtlCopyMemory(fbdi->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

        case FileDirectoryInformation:
        {
            FILE_DIRECTORY_INFORMATION* fdi = buf;

            TRACE("FileDirectoryInformation\n");

            needed = offsetof(FILE_DIRECTORY_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            fdi->NextEntryOffset = 0;
            fdi->FileIndex = 0;
            fdi->CreationTime.QuadPart = unix_time_to_win(&ii.otime);
            fdi->LastAccessTime.QuadPart = unix_time_to_win(&ii.atime);
            fdi->LastWriteTime.QuadPart = unix_time_to_win(&ii.mtime);
            fdi->ChangeTime.QuadPart = unix_time_to_win(&ii.ctime);
            fdi->EndOfFile.QuadPart = de->type == BTRFS_FT_SYMLINK ? 0 : ii.size;

            if (de->type == BTRFS_FT_SYMLINK)
                fdi->AllocationSize.QuadPart = 0;
            else if (atts & FILE_ATTRIBUTE_SPARSE_FILE)
                fdi->AllocationSize.QuadPart = ii.nbytes;
            else
                fdi->AllocationSize.QuadPart = sector_align(ii.size, fcb->Vcb->superblock.sectorsize);

            fdi->FileAttributes = atts;
            fdi->FileNameLength = de->name.Length;

            RtlCopyMemory(fdi->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

        case FileFullDirectoryInformation:
        {
            FILE_FULL_DIR_INFORMATION* ffdi = buf;

            TRACE("FileFullDirectoryInformation\n");

            needed = offsetof(FILE_FULL_DIR_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            ffdi->NextEntryOffset = 0;
            ffdi->FileIndex = 0;
            ffdi->CreationTime.QuadPart = unix_time_to_win(&ii.otime);
            ffdi->LastAccessTime.QuadPart = unix_time_to_win(&ii.atime);
            ffdi->LastWriteTime.QuadPart = unix_time_to_win(&ii.mtime);
            ffdi->ChangeTime.QuadPart = unix_time_to_win(&ii.ctime);
            ffdi->EndOfFile.QuadPart = de->type == BTRFS_FT_SYMLINK ? 0 : ii.size;

            if (de->type == BTRFS_FT_SYMLINK)
                ffdi->AllocationSize.QuadPart = 0;
            else if (atts & FILE_ATTRIBUTE_SPARSE_FILE)
                ffdi->AllocationSize.QuadPart = ii.nbytes;
            else
                ffdi->AllocationSize.QuadPart = sector_align(ii.size, fcb->Vcb->superblock.sectorsize);

            ffdi->FileAttributes = atts;
            ffdi->FileNameLength = de->name.Length;
            ffdi->EaSize = (r && atts & FILE_ATTRIBUTE_REPARSE_POINT) ? get_reparse_tag(fcb->Vcb, r, inode, de->type, atts, ccb->lxss, Irp) : ealen;

            RtlCopyMemory(ffdi->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

        case FileIdBothDirectoryInformation:
        {
            FILE_ID_BOTH_DIR_INFORMATION* fibdi = buf;

            TRACE("FileIdBothDirectoryInformation\n");

            needed = offsetof(FILE_ID_BOTH_DIR_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            fibdi->NextEntryOffset = 0;
            fibdi->FileIndex = 0;
            fibdi->CreationTime.QuadPart = unix_time_to_win(&ii.otime);
            fibdi->LastAccessTime.QuadPart = unix_time_to_win(&ii.atime);
            fibdi->LastWriteTime.QuadPart = unix_time_to_win(&ii.mtime);
            fibdi->ChangeTime.QuadPart = unix_time_to_win(&ii.ctime);
            fibdi->EndOfFile.QuadPart = de->type == BTRFS_FT_SYMLINK ? 0 : ii.size;

            if (de->type == BTRFS_FT_SYMLINK)
                fibdi->AllocationSize.QuadPart = 0;
            else if (atts & FILE_ATTRIBUTE_SPARSE_FILE)
                fibdi->AllocationSize.QuadPart = ii.nbytes;
            else
                fibdi->AllocationSize.QuadPart = sector_align(ii.size, fcb->Vcb->superblock.sectorsize);

            fibdi->FileAttributes = atts;
            fibdi->FileNameLength = de->name.Length;
            fibdi->EaSize = (r && atts & FILE_ATTRIBUTE_REPARSE_POINT) ? get_reparse_tag(fcb->Vcb, r, inode, de->type, atts, ccb->lxss, Irp) : ealen;
            fibdi->ShortNameLength = 0;
            fibdi->FileId.QuadPart = r ? make_file_id(r, inode) : make_file_id(fcb->Vcb->dummy_fcb->subvol, fcb->Vcb->dummy_fcb->inode);

            RtlCopyMemory(fibdi->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

        case FileIdFullDirectoryInformation:
        {
            FILE_ID_FULL_DIR_INFORMATION* fifdi = buf;

            TRACE("FileIdFullDirectoryInformation\n");

            needed = offsetof(FILE_ID_FULL_DIR_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            fifdi->NextEntryOffset = 0;
            fifdi->FileIndex = 0;
            fifdi->CreationTime.QuadPart = unix_time_to_win(&ii.otime);
            fifdi->LastAccessTime.QuadPart = unix_time_to_win(&ii.atime);
            fifdi->LastWriteTime.QuadPart = unix_time_to_win(&ii.mtime);
            fifdi->ChangeTime.QuadPart = unix_time_to_win(&ii.ctime);
            fifdi->EndOfFile.QuadPart = de->type == BTRFS_FT_SYMLINK ? 0 : ii.size;

            if (de->type == BTRFS_FT_SYMLINK)
                fifdi->AllocationSize.QuadPart = 0;
            else if (atts & FILE_ATTRIBUTE_SPARSE_FILE)
                fifdi->AllocationSize.QuadPart = ii.nbytes;
            else
                fifdi->AllocationSize.QuadPart = sector_align(ii.size, fcb->Vcb->superblock.sectorsize);

            fifdi->FileAttributes = atts;
            fifdi->FileNameLength = de->name.Length;
            fifdi->EaSize = (r && atts & FILE_ATTRIBUTE_REPARSE_POINT) ? get_reparse_tag(fcb->Vcb, r, inode, de->type, atts, ccb->lxss, Irp) : ealen;
            fifdi->FileId.QuadPart = r ? make_file_id(r, inode) : make_file_id(fcb->Vcb->dummy_fcb->subvol, fcb->Vcb->dummy_fcb->inode);

            RtlCopyMemory(fifdi->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
#endif
        case FileIdExtdDirectoryInformation:
        {
            FILE_ID_EXTD_DIR_INFORMATION* fiedi = buf;

            TRACE("FileIdExtdDirectoryInformation\n");

            needed = offsetof(FILE_ID_EXTD_DIR_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            fiedi->NextEntryOffset = 0;
            fiedi->FileIndex = 0;
            fiedi->CreationTime.QuadPart = unix_time_to_win(&ii.otime);
            fiedi->LastAccessTime.QuadPart = unix_time_to_win(&ii.atime);
            fiedi->LastWriteTime.QuadPart = unix_time_to_win(&ii.mtime);
            fiedi->ChangeTime.QuadPart = unix_time_to_win(&ii.ctime);
            fiedi->EndOfFile.QuadPart = de->type == BTRFS_FT_SYMLINK ? 0 : ii.size;

            if (de->type == BTRFS_FT_SYMLINK)
                fiedi->AllocationSize.QuadPart = 0;
            else if (atts & FILE_ATTRIBUTE_SPARSE_FILE)
                fiedi->AllocationSize.QuadPart = ii.nbytes;
            else
                fiedi->AllocationSize.QuadPart = sector_align(ii.size, fcb->Vcb->superblock.sectorsize);

            fiedi->FileAttributes = atts;
            fiedi->FileNameLength = de->name.Length;
            fiedi->EaSize = ealen;
            fiedi->ReparsePointTag = get_reparse_tag(fcb->Vcb, r, inode, de->type, atts, ccb->lxss, Irp);

            RtlCopyMemory(&fiedi->FileId.Identifier[0], &fcb->inode, sizeof(uint64_t));
            RtlCopyMemory(&fiedi->FileId.Identifier[sizeof(uint64_t)], &fcb->subvol->id, sizeof(uint64_t));

            RtlCopyMemory(fiedi->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

        case FileIdExtdBothDirectoryInformation:
        {
            FILE_ID_EXTD_BOTH_DIR_INFORMATION* fiebdi = buf;

            TRACE("FileIdExtdBothDirectoryInformation\n");

            needed = offsetof(FILE_ID_EXTD_BOTH_DIR_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            fiebdi->NextEntryOffset = 0;
            fiebdi->FileIndex = 0;
            fiebdi->CreationTime.QuadPart = unix_time_to_win(&ii.otime);
            fiebdi->LastAccessTime.QuadPart = unix_time_to_win(&ii.atime);
            fiebdi->LastWriteTime.QuadPart = unix_time_to_win(&ii.mtime);
            fiebdi->ChangeTime.QuadPart = unix_time_to_win(&ii.ctime);
            fiebdi->EndOfFile.QuadPart = de->type == BTRFS_FT_SYMLINK ? 0 : ii.size;

            if (de->type == BTRFS_FT_SYMLINK)
                fiebdi->AllocationSize.QuadPart = 0;
            else if (atts & FILE_ATTRIBUTE_SPARSE_FILE)
                fiebdi->AllocationSize.QuadPart = ii.nbytes;
            else
                fiebdi->AllocationSize.QuadPart = sector_align(ii.size, fcb->Vcb->superblock.sectorsize);

            fiebdi->FileAttributes = atts;
            fiebdi->FileNameLength = de->name.Length;
            fiebdi->EaSize = ealen;
            fiebdi->ReparsePointTag = get_reparse_tag(fcb->Vcb, r, inode, de->type, atts, ccb->lxss, Irp);

            RtlCopyMemory(&fiebdi->FileId.Identifier[0], &fcb->inode, sizeof(uint64_t));
            RtlCopyMemory(&fiebdi->FileId.Identifier[sizeof(uint64_t)], &fcb->subvol->id, sizeof(uint64_t));

            fiebdi->ShortNameLength = 0;

            RtlCopyMemory(fiebdi->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

        case FileNamesInformation:
        {
            FILE_NAMES_INFORMATION* fni = buf;

            TRACE("FileNamesInformation\n");

            needed = offsetof(FILE_NAMES_INFORMATION, FileName) + de->name.Length;

            if (needed > *len) {
                TRACE("buffer overflow - %li > %lu\n", needed, *len);
                return STATUS_BUFFER_OVERFLOW;
            }

            fni->NextEntryOffset = 0;
            fni->FileIndex = 0;
            fni->FileNameLength = de->name.Length;

            RtlCopyMemory(fni->FileName, de->name.Buffer, de->name.Length);

            *len -= needed;

            return STATUS_SUCCESS;
        }

        default:
            WARN("Unknown FileInformationClass %u\n", IrpSp->Parameters.QueryDirectory.FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_NO_MORE_FILES;
}
