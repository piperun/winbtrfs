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

NTSTATUS query_ranges(PFILE_OBJECT FileObject, FILE_ALLOCATED_RANGE_BUFFER* inbuf, ULONG inbuflen, void* outbuf, ULONG outbuflen, ULONG_PTR* retlen) {
    NTSTATUS Status;
    fcb* fcb;
    LIST_ENTRY* le;
    FILE_ALLOCATED_RANGE_BUFFER* ranges = outbuf;
    ULONG i = 0;
    uint64_t last_start, last_end;

    TRACE("FSCTL_QUERY_ALLOCATED_RANGES\n");

    if (!FileObject) {
        ERR("FileObject was NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (!inbuf || inbuflen < sizeof(FILE_ALLOCATED_RANGE_BUFFER) || !outbuf)
        return STATUS_INVALID_PARAMETER;

    fcb = FileObject->FsContext;

    if (!fcb) {
        ERR("FCB was NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    // If file is not marked as sparse, claim the whole thing as an allocated range

    if (!(fcb->atts & FILE_ATTRIBUTE_SPARSE_FILE)) {
        if (fcb->inode_item.size == 0)
            Status = STATUS_SUCCESS;
        else if (outbuflen < sizeof(FILE_ALLOCATED_RANGE_BUFFER))
            Status = STATUS_BUFFER_TOO_SMALL;
        else {
            ranges[i].FileOffset.QuadPart = 0;
            ranges[i].Length.QuadPart = fcb->inode_item.size;
            i++;
            Status = STATUS_SUCCESS;
        }

        goto end;

    }

    le = fcb->extents.Flink;

    last_start = 0;
    last_end = 0;

    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if (!ext->ignore) {
            uint64_t len = ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC ? ext->extent_data.num_bytes : ext->extent_data.ram_bytes;

            if (ext->offset > last_end) { // first extent after a hole
                if (last_end > last_start) {
                    if ((i + 1) * sizeof(FILE_ALLOCATED_RANGE_BUFFER) <= outbuflen) {
                        ranges[i].FileOffset.QuadPart = last_start;
                        ranges[i].Length.QuadPart = min(fcb->inode_item.size, last_end) - last_start;
                        i++;
                    } else {
                        Status = STATUS_BUFFER_TOO_SMALL;
                        goto end;
                    }
                }

                last_start = ext->offset;
            }

            last_end = ext->offset + len;
        }

        le = le->Flink;
    }

    if (last_end > last_start) {
        if ((i + 1) * sizeof(FILE_ALLOCATED_RANGE_BUFFER) <= outbuflen) {
            ranges[i].FileOffset.QuadPart = last_start;
            ranges[i].Length.QuadPart = min(fcb->inode_item.size, last_end) - last_start;
            i++;
        } else {
            Status = STATUS_BUFFER_TOO_SMALL;
            goto end;
        }
    }

    Status = STATUS_SUCCESS;

end:
    *retlen = i * sizeof(FILE_ALLOCATED_RANGE_BUFFER);

    ExReleaseResourceLite(fcb->Header.Resource);

    return Status;
}
