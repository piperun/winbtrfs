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

/* File extent mappings returned by FSCTL_GET_RETRIEVAL_POINTERS. */
#include "retrieval.h"

NTSTATUS get_retrieval_pointers(device_extension* Vcb, PFILE_OBJECT FileObject, STARTING_VCN_INPUT_BUFFER* in,
                                ULONG inlen, RETRIEVAL_POINTERS_BUFFER* out, ULONG outlen, ULONG_PTR* retlen) {
    NTSTATUS Status;
    fcb* fcb;

    *retlen = 0;

    if (!FileObject || !FileObject->FsContext || !in || inlen < sizeof(*in) || in->StartingVcn.QuadPart < 0 || !out)
        return STATUS_INVALID_PARAMETER;

    if (outlen < sizeof(RETRIEVAL_POINTERS_BUFFER))
        return STATUS_BUFFER_TOO_SMALL;

    fcb = FileObject->FsContext;
    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    try {
        LIST_ENTRY* le = fcb->extents.Flink;
        uint64_t num_sectors = fcb->Header.AllocationSize.QuadPart >> Vcb->sector_shift;
        uint64_t last_off = 0;

        if ((uint64_t)in->StartingVcn.QuadPart >= num_sectors) {
            Status = STATUS_END_OF_FILE;
            leave;
        }

        out->ExtentCount = 0;
        outlen -= offsetof(RETRIEVAL_POINTERS_BUFFER, Extents[0]);

        while (last_off < num_sectors) {
            extent* ext = le == &fcb->extents ? NULL : CONTAINING_RECORD(le, extent, list_entry);
            uint64_t run_end, extent_start = ext ? ext->offset >> Vcb->sector_shift : num_sectors;
            int64_t lcn = -1;

            if (ext && ext->ignore) {
                le = le->Flink;
                continue;
            }

            if (ext && ext->extent_data.type == BTRFS_FILE_EXTENT_INLINE) {
                Status = STATUS_END_OF_FILE;
                leave;
            }

            if (last_off < extent_start)
                run_end = min(extent_start, num_sectors);
            else {
                run_end = min((ext->offset + ext->extent_data.num_bytes) >> Vcb->sector_shift, num_sectors);
                if (ext->extent_data.disk_bytenr != 0 && ext->extent_data.compression == BTRFS_COMPRESS_NONE)
                    lcn = ((ext->extent_data.disk_bytenr + ext->extent_data.offset) >> Vcb->sector_shift) + last_off - extent_start;
                le = le->Flink;
            }

            if (run_end <= last_off)
                continue;

            if (run_end > (uint64_t)in->StartingVcn.QuadPart) {
                if (out->ExtentCount == 0) {
                    out->StartingVcn.QuadPart = last_off;
                    *retlen = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents[0]);
                }

                if (outlen < sizeof(out->Extents[0])) {
                    Status = STATUS_BUFFER_OVERFLOW;
                    leave;
                }

                out->Extents[out->ExtentCount].NextVcn.QuadPart = run_end;
                out->Extents[out->ExtentCount].Lcn.QuadPart = lcn;
                out->ExtentCount++;
                outlen -= sizeof(out->Extents[0]);
                *retlen += sizeof(out->Extents[0]);
            }

            last_off = run_end;
        }

        Status = STATUS_SUCCESS;
    } finally {
        ExReleaseResourceLite(fcb->Header.Resource);
    }

    return Status;
}
