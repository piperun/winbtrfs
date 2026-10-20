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

    TRACE("get_retrieval_pointers(%p, %p, %p, %lx, %p, %lx, %p)\n", Vcb, FileObject, in, inlen,
                                                                    out, outlen, retlen);

    if (!FileObject)
        return STATUS_INVALID_PARAMETER;

    fcb = FileObject->FsContext;

    if (!fcb)
        return STATUS_INVALID_PARAMETER;

    if (inlen < sizeof(STARTING_VCN_INPUT_BUFFER) || in->StartingVcn.QuadPart < 0)
        return STATUS_INVALID_PARAMETER;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    if (outlen < offsetof(RETRIEVAL_POINTERS_BUFFER, Extents[0]))
        return STATUS_BUFFER_TOO_SMALL;

    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    try {
        LIST_ENTRY* le = fcb->extents.Flink;
        extent* first_ext = NULL;
        unsigned int num_extents = 0, first_extent_num = 0, i;
        uint64_t num_sectors, last_off = 0;

        num_sectors = (fcb->inode_item.size + Vcb->superblock.sectorsize - 1) >> Vcb->sector_shift;

        while (le != &fcb->extents) {
            extent* ext = CONTAINING_RECORD(le, extent, list_entry);

            if (ext->ignore || ext->extent_data.type == BTRFS_FILE_EXTENT_INLINE) {
                le = le->Flink;
                continue;
            }

            if (ext->offset > last_off)
                num_extents++;

            if ((ext->offset >> Vcb->sector_shift) <= (uint64_t)in->StartingVcn.QuadPart &&
                (ext->offset + ext->extent_data.ram_bytes) >> Vcb->sector_shift > (uint64_t)in->StartingVcn.QuadPart) {
                first_ext = ext;
                first_extent_num = num_extents;
            }

            num_extents++;

            last_off = ext->offset + ext->extent_data.ram_bytes;

            le = le->Flink;
        }

        if (num_sectors > last_off >> Vcb->sector_shift)
            num_extents++;

        if (!first_ext) {
            Status = STATUS_END_OF_FILE;
            leave;
        }

        out->ExtentCount = num_extents - first_extent_num;
        out->StartingVcn.QuadPart = first_ext->offset >> Vcb->sector_shift;
        outlen -= offsetof(RETRIEVAL_POINTERS_BUFFER, Extents[0]);
        *retlen = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents[0]);

        le = &first_ext->list_entry;
        i = 0;
        last_off = 0;

        while (le != &fcb->extents) {
            extent* ext = CONTAINING_RECORD(le, extent, list_entry);

            if (ext->ignore || ext->extent_data.type == BTRFS_FILE_EXTENT_INLINE) {
                le = le->Flink;
                continue;
            }

            if (ext->offset > last_off) {
                if (outlen < sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER)) {
                    Status = STATUS_BUFFER_OVERFLOW;
                    leave;
                }

                out->Extents[i].NextVcn.QuadPart = ext->offset >> Vcb->sector_shift;
                out->Extents[i].Lcn.QuadPart = -1;

                outlen -= sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER);
                *retlen += sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER);
                i++;
            }

            if (outlen < sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER)) {
                Status = STATUS_BUFFER_OVERFLOW;
                leave;
            }

            out->Extents[i].NextVcn.QuadPart = (ext->offset + ext->extent_data.ram_bytes) >> Vcb->sector_shift;

            if (ext->extent_data.compression == BTRFS_COMPRESS_NONE)
                out->Extents[i].Lcn.QuadPart = (ext->extent_data.disk_bytenr + ext->extent_data.offset) >> Vcb->sector_shift;
            else
                out->Extents[i].Lcn.QuadPart = -1;

            outlen -= sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER);
            *retlen += sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER);
            i++;

            le = le->Flink;
        }

        if (num_sectors << Vcb->sector_shift > last_off) {
            if (outlen < sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER)) {
                Status = STATUS_BUFFER_OVERFLOW;
                leave;
            }

            out->Extents[i].NextVcn.QuadPart = num_sectors;
            out->Extents[i].Lcn.QuadPart = -1;

            outlen -= sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER);
            *retlen += sizeof(LARGE_INTEGER) + sizeof(LARGE_INTEGER);
        }

        Status = STATUS_SUCCESS;
    } finally {
        ExReleaseResourceLite(fcb->Header.Resource);
    }

    return Status;
}
