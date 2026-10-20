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

/* Maintain directory enumeration state and select matching entries. */
#include "btrfs_drv.h"
#include "crc32c.h"
#include "directory-query.h"

static NTSTATUS next_dir_entry(file_ref* fileref, uint64_t* offset, dir_entry* de, dir_child** pdc) {
    LIST_ENTRY* le;
    dir_child* dc;

    if (*pdc) {
        dir_child* dc2 = *pdc;

        if (dc2->list_entry_index.Flink != &fileref->fcb->dir_children_index)
            dc = CONTAINING_RECORD(dc2->list_entry_index.Flink, dir_child, list_entry_index);
        else
            dc = NULL;

        goto next;
    }

    if (fileref->parent) { // don't return . and .. if root directory
        if (*offset == 0) {
            de->key.objectid = fileref->fcb->inode;
            de->key.type = BTRFS_INODE_ITEM_KEY;
            de->key.offset = 0;
            de->dir_entry_type = DirEntryType_Self;
            de->name.Buffer = L".";
            de->name.Length = de->name.MaximumLength = sizeof(WCHAR);
            de->type = BTRFS_FT_DIR;

            *offset = 1;
            *pdc = NULL;

            return STATUS_SUCCESS;
        } else if (*offset == 1) {
            de->key.objectid = fileref->parent->fcb->inode;
            de->key.type = BTRFS_INODE_ITEM_KEY;
            de->key.offset = 0;
            de->dir_entry_type = DirEntryType_Parent;
            de->name.Buffer = L"..";
            de->name.Length = de->name.MaximumLength = sizeof(WCHAR) * 2;
            de->type = BTRFS_FT_DIR;

            *offset = 2;
            *pdc = NULL;

            return STATUS_SUCCESS;
        }
    }

    if (*offset < 2)
        *offset = 2;

    dc = NULL;
    le = fileref->fcb->dir_children_index.Flink;

    // skip entries before offset
    while (le != &fileref->fcb->dir_children_index) {
        dir_child* dc2 = CONTAINING_RECORD(le, dir_child, list_entry_index);

        if (dc2->index >= *offset) {
            dc = dc2;
            break;
        }

        le = le->Flink;
    }

next:
    if (!dc)
        return STATUS_NO_MORE_FILES;

    if (dc->root_dir && fileref->parent) { // hide $Root dir unless in apparent root, to avoid recursion
        if (dc->list_entry_index.Flink == &fileref->fcb->dir_children_index)
            return STATUS_NO_MORE_FILES;

        dc = CONTAINING_RECORD(dc->list_entry_index.Flink, dir_child, list_entry_index);
    }

    de->key = dc->key;
    de->name = dc->name;
    de->type = dc->type;
    de->dir_entry_type = DirEntryType_File;
    de->dc = dc;

    *offset = dc->index + 1;
    *pdc = dc;

    return STATUS_SUCCESS;
}

NTSTATUS query_directory(PIRP Irp) {
    PIO_STACK_LOCATION IrpSp;
    NTSTATUS Status, status2;
    fcb* fcb;
    ccb* ccb;
    file_ref* fileref;
    device_extension* Vcb;
    void* buf;
    uint8_t *curitem, *lastitem;
    LONG length;
    ULONG count;
    bool has_wildcard = false, specific_file = false, initial;
    dir_entry de;
    uint64_t newoffset;
    dir_child* dc = NULL;

    TRACE("query directory\n");

    IrpSp = IoGetCurrentIrpStackLocation(Irp);
    fcb = IrpSp->FileObject->FsContext;
    ccb = IrpSp->FileObject->FsContext2;
    fileref = ccb ? ccb->fileref : NULL;

    if (!fileref)
        return STATUS_INVALID_PARAMETER;

    if (!ccb) {
        ERR("ccb was NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (!fcb) {
        ERR("fcb was NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (Irp->RequestorMode == UserMode && !(ccb->access & FILE_LIST_DIRECTORY)) {
        WARN("insufficient privileges\n");
        return STATUS_ACCESS_DENIED;
    }

    Vcb = fcb->Vcb;

    if (!Vcb) {
        ERR("Vcb was NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (fileref->fcb == Vcb->dummy_fcb)
        return STATUS_NO_MORE_FILES;

    if (IrpSp->Flags == 0) {
        TRACE("QD flags: (none)\n");
    } else {
        ULONG flags = IrpSp->Flags;

        TRACE("QD flags:\n");

        if (flags & SL_INDEX_SPECIFIED) {
            TRACE("    SL_INDEX_SPECIFIED\n");
            flags &= ~SL_INDEX_SPECIFIED;
        }

        if (flags & SL_RESTART_SCAN) {
            TRACE("    SL_RESTART_SCAN\n");
            flags &= ~SL_RESTART_SCAN;
        }

        if (flags & SL_RETURN_SINGLE_ENTRY) {
            TRACE("    SL_RETURN_SINGLE_ENTRY\n");
            flags &= ~SL_RETURN_SINGLE_ENTRY;
        }

        if (flags != 0)
            TRACE("    unknown flags: %lu\n", flags);
    }

    initial = !ccb->query_string.Buffer;

    if (IrpSp->Flags & SL_RESTART_SCAN)
        ccb->query_dir_offset = 0;

    if (initial) {
        UNICODE_STRING all_files = {sizeof(WCHAR), sizeof(WCHAR), L"*"};
        UNICODE_STRING* expression = IrpSp->Parameters.QueryDirectory.FileName;

        if (!expression || expression->Length == 0)
            expression = &all_files;

        has_wildcard = FsRtlDoesNameContainWildCards(expression);
        specific_file = !has_wildcard;

        if (has_wildcard && !ccb->case_sensitive) {
            Status = RtlUpcaseUnicodeString(&ccb->query_string, expression, true);
            if (!NT_SUCCESS(Status))
                return Status;
        } else {
            ccb->query_string.Buffer = ExAllocatePoolWithTag(PagedPool, expression->Length, ALLOC_TAG);
            if (!ccb->query_string.Buffer)
                return STATUS_INSUFFICIENT_RESOURCES;

            ccb->query_string.Length = ccb->query_string.MaximumLength = expression->Length;
            RtlCopyMemory(ccb->query_string.Buffer, expression->Buffer, expression->Length);
        }

        ccb->has_wildcard = has_wildcard;
        ccb->specific_file = specific_file;
    } else {
        has_wildcard = ccb->has_wildcard;
        specific_file = ccb->specific_file;

        if (specific_file && ccb->query_dir_offset != 0 && !(IrpSp->Flags & SL_RESTART_SCAN))
            return STATUS_NO_MORE_FILES;
    }

    if (ccb->query_string.Buffer) {
        TRACE("query string = %.*S\n", (int)(ccb->query_string.Length / sizeof(WCHAR)), ccb->query_string.Buffer);
    }

    newoffset = ccb->query_dir_offset;

    ExAcquireResourceSharedLite(&Vcb->tree_lock, true);

    ExAcquireResourceSharedLite(&fileref->fcb->nonpaged->dir_children_lock, true);

    Status = next_dir_entry(fileref, &newoffset, &de, &dc);

    if (!NT_SUCCESS(Status)) {
        if (Status == STATUS_NO_MORE_FILES && initial)
            Status = STATUS_NO_SUCH_FILE;
        goto end;
    }

    buf = map_user_buffer(Irp, NormalPagePriority);

    if (Irp->MdlAddress && !buf) {
        ERR("MmGetSystemAddressForMdlSafe returned NULL\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    length = IrpSp->Parameters.QueryDirectory.Length;

    if (specific_file) {
        bool found = false;
        UNICODE_STRING us;
        LIST_ENTRY* le;
        uint32_t hash;
        uint8_t c;

        us.Buffer = NULL;

        if (!ccb->case_sensitive) {
            Status = RtlUpcaseUnicodeString(&us, &ccb->query_string, true);
            if (!NT_SUCCESS(Status)) {
                ERR("RtlUpcaseUnicodeString returned %08lx\n", Status);
                goto end;
            }

            hash = calc_crc32c(0xffffffff, (uint8_t*)us.Buffer, us.Length);
        } else
            hash = calc_crc32c(0xffffffff, (uint8_t*)ccb->query_string.Buffer, ccb->query_string.Length);

        c = hash >> 24;

        if (ccb->case_sensitive) {
            if (fileref->fcb->hash_ptrs[c]) {
                le = fileref->fcb->hash_ptrs[c];
                while (le != &fileref->fcb->dir_children_hash) {
                    dir_child* dc2 = CONTAINING_RECORD(le, dir_child, list_entry_hash);

                    if (dc2->hash == hash) {
                        if (dc2->name.Length == ccb->query_string.Length && RtlCompareMemory(dc2->name.Buffer, ccb->query_string.Buffer, ccb->query_string.Length) == ccb->query_string.Length) {
                            found = true;

                            de.key = dc2->key;
                            de.name = dc2->name;
                            de.type = dc2->type;
                            de.dir_entry_type = DirEntryType_File;
                            de.dc = dc2;

                            break;
                        }
                    } else if (dc2->hash > hash)
                        break;

                    le = le->Flink;
                }
            }
        } else {
            if (fileref->fcb->hash_ptrs_uc[c]) {
                le = fileref->fcb->hash_ptrs_uc[c];
                while (le != &fileref->fcb->dir_children_hash_uc) {
                    dir_child* dc2 = CONTAINING_RECORD(le, dir_child, list_entry_hash_uc);

                    if (dc2->hash_uc == hash) {
                        if (dc2->name_uc.Length == us.Length && RtlCompareMemory(dc2->name_uc.Buffer, us.Buffer, us.Length) == us.Length) {
                            found = true;

                            de.key = dc2->key;
                            de.name = dc2->name;
                            de.type = dc2->type;
                            de.dir_entry_type = DirEntryType_File;
                            de.dc = dc2;

                            break;
                        }
                    } else if (dc2->hash_uc > hash)
                        break;

                    le = le->Flink;
                }
            }
        }

        if (us.Buffer)
            ExFreePool(us.Buffer);

        if (!found) {
            Status = STATUS_NO_SUCH_FILE;
            goto end;
        }
    } else if (has_wildcard) {
        while (!FsRtlIsNameInExpression(&ccb->query_string, &de.name, !ccb->case_sensitive, NULL)) {
            ccb->query_dir_offset = newoffset;
            Status = next_dir_entry(fileref, &newoffset, &de, &dc);
            if (!NT_SUCCESS(Status)) {
                if (Status == STATUS_NO_MORE_FILES && initial)
                    Status = STATUS_NO_SUCH_FILE;

                goto end;
            }
        }
    }

    TRACE("file(0) = %.*S\n", (int)(de.name.Length / sizeof(WCHAR)), de.name.Buffer);
    TRACE("offset = %I64u\n", ccb->query_dir_offset - 1);

    Status = query_dir_item(fcb, ccb, buf, &length, Irp, &de, fcb->subvol, initial);
    if (NT_SUCCESS(Status))
        ccb->query_dir_offset = newoffset;
    else if (Status == STATUS_BUFFER_OVERFLOW && !initial)
        Status = STATUS_SUCCESS;

    count = 0;
    if (NT_SUCCESS(Status) && length != (LONG)IrpSp->Parameters.QueryDirectory.Length &&
        !(IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) && !specific_file) {
        lastitem = (uint8_t*)buf;

        while (length > 0) {
            LONG item_length;
            ULONG used, padding, alignment;

            newoffset = ccb->query_dir_offset;
            Status = next_dir_entry(fileref, &newoffset, &de, &dc);
            if (!NT_SUCCESS(Status)) {
                if (Status == STATUS_NO_MORE_FILES)
                    Status = STATUS_SUCCESS;
                break;
            }

            if (has_wildcard && !FsRtlIsNameInExpression(&ccb->query_string, &de.name, !ccb->case_sensitive, NULL)) {
                ccb->query_dir_offset = newoffset;
                continue;
            }

            alignment = IrpSp->Parameters.QueryDirectory.FileInformationClass == FileNamesInformation ? 4 : 8;
            used = IrpSp->Parameters.QueryDirectory.Length - length;
            padding = (alignment - used % alignment) % alignment;
            if ((ULONG)length <= padding)
                break;

            item_length = length - padding;
            curitem = (uint8_t*)buf + used + padding;
            count++;

            TRACE("file(%lu) %Iu = %.*S\n", count, curitem - (uint8_t*)buf, (int)(de.name.Length / sizeof(WCHAR)), de.name.Buffer);

            status2 = query_dir_item(fcb, ccb, curitem, &item_length, Irp, &de, fcb->subvol, false);
            if (!NT_SUCCESS(status2)) {
                if (status2 != STATUS_BUFFER_OVERFLOW)
                    Status = status2;
                break;
            }

            *(ULONG*)lastitem = (ULONG)(curitem - lastitem);
            ccb->query_dir_offset = newoffset;
            length = item_length;
            lastitem = curitem;
        }
    }

    Irp->IoStatus.Information = IrpSp->Parameters.QueryDirectory.Length - length;

end:
    ExReleaseResourceLite(&fileref->fcb->nonpaged->dir_children_lock);

    ExReleaseResourceLite(&Vcb->tree_lock);

    TRACE("returning %08lx\n", Status);

    return Status;
}
