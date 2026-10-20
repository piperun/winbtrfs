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

#include "hardlink-info.h"

NTSTATUS fill_in_hard_link_information(FILE_LINKS_INFORMATION* fli, file_ref* fileref, PIRP Irp, LONG* length) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    LONG bytes_needed;
    FILE_LINK_ENTRY_INFORMATION* feli;
    bool overflow = false;
    fcb* fcb = fileref->fcb;
    ULONG len;

    if (fcb->ads)
        return STATUS_INVALID_PARAMETER;

    if (*length < (LONG)offsetof(FILE_LINKS_INFORMATION, Entry))
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(fli, *length);

    bytes_needed = offsetof(FILE_LINKS_INFORMATION, Entry);
    len = bytes_needed;
    feli = NULL;

    ExAcquireResourceExclusiveLite(&fcb->Vcb->fileref_lock, true);
    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    if (fcb->inode == BTRFS_FIRST_FREE_OBJECTID) {
        ULONG namelen;

        if (fcb == fcb->Vcb->root_fileref->fcb)
            namelen = sizeof(WCHAR);
        else
            namelen = fileref->dc->name.Length;

        bytes_needed += sizeof(FILE_LINK_ENTRY_INFORMATION) - sizeof(WCHAR) + namelen;

        if (bytes_needed > *length)
            overflow = true;

        if (!overflow) {
            feli = &fli->Entry;

            feli->NextEntryOffset = 0;
            feli->ParentFileId = 0; // we use an inode of 0 to mean the parent of a subvolume

            if (fcb == fcb->Vcb->root_fileref->fcb) {
                feli->FileNameLength = 1;
                feli->FileName[0] = '.';
            } else {
                feli->FileNameLength = fileref->dc->name.Length / sizeof(WCHAR);
                RtlCopyMemory(feli->FileName, fileref->dc->name.Buffer, fileref->dc->name.Length);
            }

            fli->EntriesReturned++;

            len = bytes_needed;
        }
    } else {
        if (IsListEmpty(&fcb->hardlinks)) {
            if (!fileref->dc) {
                Status = STATUS_INVALID_PARAMETER;
                goto end;
            }

            bytes_needed += sizeof(FILE_LINK_ENTRY_INFORMATION) + fileref->dc->name.Length - sizeof(WCHAR);

            if (bytes_needed > *length)
                overflow = true;

            if (!overflow) {
                feli = &fli->Entry;

                feli->NextEntryOffset = 0;
                feli->ParentFileId = fileref->parent->fcb->inode;
                feli->FileNameLength = fileref->dc->name.Length / sizeof(WCHAR);
                RtlCopyMemory(feli->FileName, fileref->dc->name.Buffer, fileref->dc->name.Length);

                fli->EntriesReturned++;

                len = bytes_needed;
            }
        } else {
            le = fcb->hardlinks.Flink;
            while (le != &fcb->hardlinks) {
                hardlink* hl = CONTAINING_RECORD(le, hardlink, list_entry);
                file_ref* parfr;

                TRACE("parent %I64x, index %I64x, name %.*S\n", hl->parent, hl->index, (int)(hl->name.Length / sizeof(WCHAR)), hl->name.Buffer);

                Status = open_fileref_by_inode(fcb->Vcb, fcb->subvol, hl->parent, &parfr, Irp);

                if (!NT_SUCCESS(Status)) {
                    ERR("open_fileref_by_inode returned %08lx\n", Status);
                } else if (!parfr->deleted) {
                    LIST_ENTRY* le2;
                    bool found = false, deleted = false;
                    UNICODE_STRING* fn = NULL;

                    le2 = parfr->children.Flink;
                    while (le2 != &parfr->children) {
                        file_ref* fr2 = CONTAINING_RECORD(le2, file_ref, list_entry);

                        if (fr2->dc && fr2->dc->index == hl->index) {
                            found = true;
                            deleted = fr2->deleted;

                            if (!deleted)
                                fn = &fr2->dc->name;

                            break;
                        }

                        le2 = le2->Flink;
                    }

                    if (!found)
                        fn = &hl->name;

                    if (!deleted) {
                        TRACE("fn = %.*S (found = %u)\n", (int)(fn->Length / sizeof(WCHAR)), fn->Buffer, found);

                        if (bytes_needed > (LONG)offsetof(FILE_LINKS_INFORMATION, Entry))
                            bytes_needed = (LONG)sector_align(bytes_needed, 8);

                        bytes_needed += sizeof(FILE_LINK_ENTRY_INFORMATION) + fn->Length - sizeof(WCHAR);

                        if (bytes_needed > *length)
                            overflow = true;

                        if (!overflow) {
                            if (feli) {
                                feli->NextEntryOffset = (ULONG)sector_align(sizeof(FILE_LINK_ENTRY_INFORMATION) + ((feli->FileNameLength - 1) * sizeof(WCHAR)), 8);
                                feli = (FILE_LINK_ENTRY_INFORMATION*)((uint8_t*)feli + feli->NextEntryOffset);
                            } else
                                feli = &fli->Entry;

                            feli->NextEntryOffset = 0;
                            feli->ParentFileId = parfr->fcb->inode;
                            feli->FileNameLength = fn->Length / sizeof(WCHAR);
                            RtlCopyMemory(feli->FileName, fn->Buffer, fn->Length);

                            fli->EntriesReturned++;

                            len = bytes_needed;
                        }
                    }
                }

                if (NT_SUCCESS(Status))
                    free_fileref(parfr);

                le = le->Flink;
            }
        }
    }

    fli->BytesNeeded = bytes_needed;

    *length -= len;

    Status = overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;

end:
    ExReleaseResourceLite(fcb->Header.Resource);
    ExReleaseResourceLite(&fcb->Vcb->fileref_lock);

    return Status;
}

NTSTATUS fill_in_hard_link_full_id_information(FILE_LINKS_FULL_ID_INFORMATION* flfii, file_ref* fileref, PIRP Irp, LONG* length) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    LONG bytes_needed;
    FILE_LINK_ENTRY_FULL_ID_INFORMATION* flefii;
    bool overflow = false;
    fcb* fcb = fileref->fcb;
    ULONG len;

    if (fcb->ads)
        return STATUS_INVALID_PARAMETER;

    if (*length < (LONG)offsetof(FILE_LINKS_FULL_ID_INFORMATION, Entry))
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(flfii, *length);

    bytes_needed = offsetof(FILE_LINKS_FULL_ID_INFORMATION, Entry);
    len = bytes_needed;
    flefii = NULL;

    ExAcquireResourceExclusiveLite(&fcb->Vcb->fileref_lock, true);
    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    if (fcb->inode == BTRFS_FIRST_FREE_OBJECTID) {
        ULONG namelen;

        if (fcb == fcb->Vcb->root_fileref->fcb)
            namelen = sizeof(WCHAR);
        else
            namelen = fileref->dc->name.Length;

        bytes_needed += offsetof(FILE_LINK_ENTRY_FULL_ID_INFORMATION, FileName[0]) + namelen;

        if (bytes_needed > *length)
            overflow = true;

        if (!overflow) {
            flefii = &flfii->Entry;

            flefii->NextEntryOffset = 0;

            if (fcb == fcb->Vcb->root_fileref->fcb) {
                RtlZeroMemory(&flefii->ParentFileId.Identifier[0], sizeof(FILE_ID_128));
                flefii->FileNameLength = 1;
                flefii->FileName[0] = '.';
            } else {
                RtlCopyMemory(&flefii->ParentFileId.Identifier[0], &fileref->parent->fcb->inode, sizeof(uint64_t));
                RtlCopyMemory(&flefii->ParentFileId.Identifier[sizeof(uint64_t)], &fileref->parent->fcb->subvol->id, sizeof(uint64_t));

                flefii->FileNameLength = fileref->dc->name.Length / sizeof(WCHAR);
                RtlCopyMemory(flefii->FileName, fileref->dc->name.Buffer, fileref->dc->name.Length);
            }

            flfii->EntriesReturned++;

            len = bytes_needed;
        }
    } else {
        if (IsListEmpty(&fcb->hardlinks)) {
            if (!fileref->dc) {
                Status = STATUS_INVALID_PARAMETER;
                goto end;
            }

            bytes_needed += offsetof(FILE_LINK_ENTRY_FULL_ID_INFORMATION, FileName[0]) + fileref->dc->name.Length;

            if (bytes_needed > *length)
                overflow = true;

            if (!overflow) {
                flefii = &flfii->Entry;

                flefii->NextEntryOffset = 0;

                RtlCopyMemory(&flefii->ParentFileId.Identifier[0], &fileref->parent->fcb->inode, sizeof(uint64_t));
                RtlCopyMemory(&flefii->ParentFileId.Identifier[sizeof(uint64_t)], &fileref->parent->fcb->subvol->id, sizeof(uint64_t));

                flefii->FileNameLength = fileref->dc->name.Length / sizeof(WCHAR);
                RtlCopyMemory(flefii->FileName, fileref->dc->name.Buffer, fileref->dc->name.Length);

                flfii->EntriesReturned++;

                len = bytes_needed;
            }
        } else {
            le = fcb->hardlinks.Flink;
            while (le != &fcb->hardlinks) {
                hardlink* hl = CONTAINING_RECORD(le, hardlink, list_entry);
                file_ref* parfr;

                TRACE("parent %I64x, index %I64x, name %.*S\n", hl->parent, hl->index, (int)(hl->name.Length / sizeof(WCHAR)), hl->name.Buffer);

                Status = open_fileref_by_inode(fcb->Vcb, fcb->subvol, hl->parent, &parfr, Irp);

                if (!NT_SUCCESS(Status)) {
                    ERR("open_fileref_by_inode returned %08lx\n", Status);
                } else if (!parfr->deleted) {
                    LIST_ENTRY* le2;
                    bool found = false, deleted = false;
                    UNICODE_STRING* fn = NULL;

                    le2 = parfr->children.Flink;
                    while (le2 != &parfr->children) {
                        file_ref* fr2 = CONTAINING_RECORD(le2, file_ref, list_entry);

                        if (fr2->dc && fr2->dc->index == hl->index) {
                            found = true;
                            deleted = fr2->deleted;

                            if (!deleted)
                                fn = &fr2->dc->name;

                            break;
                        }

                        le2 = le2->Flink;
                    }

                    if (!found)
                        fn = &hl->name;

                    if (!deleted) {
                        TRACE("fn = %.*S (found = %u)\n", (int)(fn->Length / sizeof(WCHAR)), fn->Buffer, found);

                        if (bytes_needed > (LONG)offsetof(FILE_LINKS_FULL_ID_INFORMATION, Entry))
                            bytes_needed = (LONG)sector_align(bytes_needed, 8);

                        bytes_needed += offsetof(FILE_LINK_ENTRY_FULL_ID_INFORMATION, FileName[0]) + fn->Length;

                        if (bytes_needed > *length)
                            overflow = true;

                        if (!overflow) {
                            if (flefii) {
                                flefii->NextEntryOffset = (ULONG)sector_align(offsetof(FILE_LINK_ENTRY_FULL_ID_INFORMATION, FileName[0]) + (flefii->FileNameLength * sizeof(WCHAR)), 8);
                                flefii = (FILE_LINK_ENTRY_FULL_ID_INFORMATION*)((uint8_t*)flefii + flefii->NextEntryOffset);
                            } else
                                flefii = &flfii->Entry;

                            flefii->NextEntryOffset = 0;

                            RtlCopyMemory(&flefii->ParentFileId.Identifier[0], &parfr->fcb->inode, sizeof(uint64_t));
                            RtlCopyMemory(&flefii->ParentFileId.Identifier[sizeof(uint64_t)], &parfr->fcb->subvol->id, sizeof(uint64_t));

                            flefii->FileNameLength = fn->Length / sizeof(WCHAR);
                            RtlCopyMemory(flefii->FileName, fn->Buffer, fn->Length);

                            flfii->EntriesReturned++;

                            len = bytes_needed;
                        }
                    }
                }

                if (NT_SUCCESS(Status))
                    free_fileref(parfr);

                le = le->Flink;
            }
        }
    }

    flfii->BytesNeeded = bytes_needed;

    *length -= len;

    Status = overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;

end:
    ExReleaseResourceLite(fcb->Header.Resource);
    ExReleaseResourceLite(&fcb->Vcb->fileref_lock);

    return Status;
}
