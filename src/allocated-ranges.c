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

/* Allocated file ranges returned by FSCTL_QUERY_ALLOCATED_RANGES. */
#include "allocated-ranges.h"

static NTSTATUS append_range(FILE_ALLOCATED_RANGE_BUFFER* out, ULONG outlen, ULONG_PTR* retlen,
                             uint64_t start, uint64_t end) {
    FILE_ALLOCATED_RANGE_BUFFER* range;

    if (outlen - *retlen < sizeof(*out))
        return *retlen ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;

    range = &out[*retlen / sizeof(*out)];
    range->FileOffset.QuadPart = start;
    range->Length.QuadPart = end - start;
    *retlen += sizeof(*out);
    return STATUS_SUCCESS;
}

NTSTATUS query_ranges(PFILE_OBJECT FileObject, FILE_ALLOCATED_RANGE_BUFFER* inbuf, ULONG inbuflen, void* outbuf, ULONG outbuflen, ULONG_PTR* retlen) {
    NTSTATUS Status = STATUS_SUCCESS;
    fcb* fcb;
    LIST_ENTRY* le;
    uint64_t start, end, last_start = 0, last_end = 0;

    TRACE("FSCTL_QUERY_ALLOCATED_RANGES\n");

    *retlen = 0;

    if (!FileObject || !FileObject->FsContext || !inbuf || inbuflen < sizeof(*inbuf) || !outbuf)
        return STATUS_INVALID_PARAMETER;

    if (inbuf->FileOffset.QuadPart < 0 || inbuf->Length.QuadPart < 0 ||
        inbuf->Length.QuadPart > INT64_MAX - inbuf->FileOffset.QuadPart)
        return STATUS_INVALID_PARAMETER;

    start = inbuf->FileOffset.QuadPart;
    end = start + inbuf->Length.QuadPart;
    fcb = FileObject->FsContext;

    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    end = min(end, fcb->inode_item.size);
    if (start >= end)
        goto end;

    if (!(fcb->atts & FILE_ATTRIBUTE_SPARSE_FILE)) {
        Status = append_range(outbuf, outbuflen, retlen, start, end);
        goto end;
    }

    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);
        uint64_t length, extent_start, extent_end;

        le = le->Flink;
        if (ext->ignore || (ext->extent_data.type != BTRFS_FILE_EXTENT_INLINE && ext->extent_data.disk_bytenr == 0))
            continue;

        if (ext->offset >= end)
            break;

        length = ext->extent_data.type == BTRFS_FILE_EXTENT_INLINE ? ext->extent_data.ram_bytes : ext->extent_data.num_bytes;
        extent_start = ext->offset < start ? start : ext->offset;
        extent_end = ext->offset + min(length, end - ext->offset);
        if (extent_start >= extent_end)
            continue;

        if (last_end > last_start && extent_start > last_end) {
            Status = append_range(outbuf, outbuflen, retlen, last_start, last_end);
            if (!NT_SUCCESS(Status))
                goto end;
            last_start = extent_start;
        } else if (last_end == last_start)
            last_start = extent_start;

        if (extent_end > last_end)
            last_end = extent_end;
    }

    if (last_end > last_start)
        Status = append_range(outbuf, outbuflen, retlen, last_start, last_end);

end:
    ExReleaseResourceLite(fcb->Header.Resource);
    return Status;
}
