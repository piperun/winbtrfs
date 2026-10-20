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

// Build file and stream names for file-information queries.

#include "file-name.h"

NTSTATUS fileref_get_filename(file_ref* fileref, PUNICODE_STRING fn, USHORT* name_offset, ULONG* preqlen) {
    file_ref* fr;
    NTSTATUS Status;
    ULONG reqlen = 0;
    USHORT offset;
    bool overflow = false;

    // FIXME - we need a lock on filerefs' filepart

    if (fileref == fileref->fcb->Vcb->root_fileref) {
        if (fn->MaximumLength >= sizeof(WCHAR)) {
            fn->Buffer[0] = '\\';
            fn->Length = sizeof(WCHAR);

            if (name_offset)
                *name_offset = 0;

            return STATUS_SUCCESS;
        } else {
            if (preqlen)
                *preqlen = sizeof(WCHAR);
            fn->Length = 0;

            return STATUS_BUFFER_OVERFLOW;
        }
    }

    fr = fileref;
    offset = 0;

    while (fr->parent) {
        USHORT movelen;

        if (!fr->dc)
            return STATUS_INTERNAL_ERROR;

        if (!overflow) {
            if (fr->dc->name.Length + sizeof(WCHAR) + fn->Length > fn->MaximumLength)
                overflow = true;
        }

        if (overflow)
            movelen = fn->MaximumLength - fr->dc->name.Length - sizeof(WCHAR);
        else
            movelen = fn->Length;

        if ((!overflow || fn->MaximumLength > fr->dc->name.Length + sizeof(WCHAR)) && movelen > 0) {
            RtlMoveMemory(&fn->Buffer[(fr->dc->name.Length / sizeof(WCHAR)) + 1], fn->Buffer, movelen);
            offset += fr->dc->name.Length + sizeof(WCHAR);
        }

        if (fn->MaximumLength >= sizeof(WCHAR)) {
            fn->Buffer[0] = fr->fcb->ads ? ':' : '\\';
            fn->Length += sizeof(WCHAR);

            if (fn->MaximumLength > sizeof(WCHAR)) {
                RtlCopyMemory(&fn->Buffer[1], fr->dc->name.Buffer, min(fr->dc->name.Length, fn->MaximumLength - sizeof(WCHAR)));
                fn->Length += fr->dc->name.Length;
            }

            if (fn->Length > fn->MaximumLength) {
                fn->Length = fn->MaximumLength;
                overflow = true;
            }
        }

        reqlen += sizeof(WCHAR) + fr->dc->name.Length;

        fr = fr->parent;
    }

    offset += sizeof(WCHAR);

    if (overflow) {
        if (preqlen)
            *preqlen = reqlen;
        Status = STATUS_BUFFER_OVERFLOW;
    } else {
        if (name_offset)
            *name_offset = offset;

        Status = STATUS_SUCCESS;
    }

    return Status;
}

NTSTATUS fill_in_file_name_information(FILE_NAME_INFORMATION* fni, fcb* fcb, file_ref* fileref, LONG* length) {
    ULONG reqlen;
    UNICODE_STRING fn;
    NTSTATUS Status;
    static const WCHAR datasuf[] = {':','$','D','A','T','A',0};
    uint16_t datasuflen = sizeof(datasuf) - sizeof(WCHAR);

    if (!fileref) {
        ERR("called without fileref\n");
        return STATUS_INVALID_PARAMETER;
    }

    *length -= (LONG)offsetof(FILE_NAME_INFORMATION, FileName[0]);

    TRACE("maximum length is %li\n", *length);
    fni->FileNameLength = 0;

    fni->FileName[0] = 0;

    fn.Buffer = fni->FileName;
    fn.Length = 0;
    fn.MaximumLength = (uint16_t)*length;

    Status = fileref_get_filename(fileref, &fn, NULL, &reqlen);
    if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW) {
        ERR("fileref_get_filename returned %08lx\n", Status);
        return Status;
    }

    if (fcb->ads) {
        if (Status == STATUS_BUFFER_OVERFLOW)
            reqlen += datasuflen;
        else {
            if (fn.Length + datasuflen > fn.MaximumLength) {
                RtlCopyMemory(&fn.Buffer[fn.Length / sizeof(WCHAR)], datasuf, fn.MaximumLength - fn.Length);
                reqlen = fn.Length + datasuflen;
                Status = STATUS_BUFFER_OVERFLOW;
            } else {
                RtlCopyMemory(&fn.Buffer[fn.Length / sizeof(WCHAR)], datasuf, datasuflen);
                fn.Length += datasuflen;
            }
        }
    }

    if (Status == STATUS_BUFFER_OVERFLOW) {
        *length = -1;
        fni->FileNameLength = reqlen;
        TRACE("%.*S (truncated)\n", (int)(fn.Length / sizeof(WCHAR)), fn.Buffer);
    } else {
        *length -= fn.Length;
        fni->FileNameLength = fn.Length;
        TRACE("%.*S\n", (int)(fn.Length / sizeof(WCHAR)), fn.Buffer);
    }

    return Status;
}
