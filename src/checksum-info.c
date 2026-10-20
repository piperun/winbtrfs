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

/* Checksum query output for logical file sectors, including sparse placeholders. */
#include "checksum-info.h"

static NTSTATUS add_csum_sparse_extents(device_extension* Vcb, uint64_t sparse_extents, uint8_t** ptr, bool found, void* hash_ptr) {
    if (!found) {
        uint8_t* sector = ExAllocatePoolWithTag(PagedPool, Vcb->superblock.sectorsize, ALLOC_TAG);

        if (!sector) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        memset(sector, 0, Vcb->superblock.sectorsize);

        get_sector_csum(Vcb, sector, hash_ptr);

        ExFreePool(sector);
    }

    switch (Vcb->superblock.csum_type) {
        case BTRFS_CSUM_TYPE_CRC32: {
            uint32_t* csum = (uint32_t*)*ptr;
            uint32_t sparse_hash = *(uint32_t*)hash_ptr;

            for (uint64_t i = 0; i < sparse_extents; i++) {
                csum[i] = sparse_hash;
            }

            break;
        }

        case BTRFS_CSUM_TYPE_XXHASH: {
            uint64_t* csum = (uint64_t*)*ptr;
            uint64_t sparse_hash = *(uint64_t*)hash_ptr;

            for (uint64_t i = 0; i < sparse_extents; i++) {
                csum[i] = sparse_hash;
            }

            break;
        }

        case BTRFS_CSUM_TYPE_SHA256:
        case BTRFS_CSUM_TYPE_BLAKE2: {
            uint8_t* csum = (uint8_t*)*ptr;

            for (uint64_t i = 0; i < sparse_extents; i++) {
                memcpy(csum, hash_ptr, 32);
                csum += 32;
            }

            break;
        }

        default:
            ERR("unrecognized hash type %x\n", Vcb->superblock.csum_type);
            return STATUS_INTERNAL_ERROR;
    }

    *ptr += sparse_extents * Vcb->csum_size;

    return STATUS_SUCCESS;
}

NTSTATUS get_csum_info(device_extension* Vcb, PFILE_OBJECT FileObject, btrfs_csum_info* buf, ULONG buflen,
                              ULONG_PTR* retlen, KPROCESSOR_MODE processor_mode) {
    NTSTATUS Status;
    fcb* fcb;
    ccb* ccb;

    TRACE("get_csum_info(%p, %p, %p, %lx, %p, %x)\n", Vcb, FileObject, buf, buflen, retlen, processor_mode);

    *retlen = 0;

    if (!FileObject)
        return STATUS_INVALID_PARAMETER;

    fcb = FileObject->FsContext;
    ccb = FileObject->FsContext2;

    if (!fcb || !ccb)
        return STATUS_INVALID_PARAMETER;

    if (!buf)
        return STATUS_INVALID_PARAMETER;

    if (buflen < offsetof(btrfs_csum_info, data[0]))
        return STATUS_BUFFER_TOO_SMALL;


    if (processor_mode == UserMode && !(ccb->access & (FILE_READ_DATA | FILE_WRITE_DATA))) {
        WARN("insufficient privileges\n");
        return STATUS_ACCESS_DENIED;
    }

    ExAcquireResourceSharedLite(fcb->Header.Resource, true);

    try {
        LIST_ENTRY* le;
        uint8_t* ptr;
        uint64_t last_sector;
        uint8_t sparse_hash[MAX_HASH_SIZE];
        bool sparse_hash_found = false;

        if (fcb->ads) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            goto end;
        }

        if (fcb->type == BTRFS_FT_DIR) {
            Status = STATUS_FILE_IS_A_DIRECTORY;
            goto end;
        }

        if (fcb->inode_item.flags & BTRFS_INODE_NODATASUM) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            goto end;
        }

        buf->csum_type = Vcb->superblock.csum_type;
        buf->csum_length = Vcb->csum_size;

        le = fcb->extents.Flink;
        while (le != &fcb->extents) {
            extent* ext = CONTAINING_RECORD(le, extent, list_entry);

            if (ext->ignore) {
                le = le->Flink;
                continue;
            }

            if (ext->extent_data.type == BTRFS_FILE_EXTENT_INLINE) {
                buf->num_sectors = 0;
                *retlen = offsetof(btrfs_csum_info, data[0]);
                Status = STATUS_SUCCESS;
                goto end;
            }

            le = le->Flink;
        }

        buf->num_sectors = (fcb->inode_item.size >> Vcb->sector_shift) +
                           ((fcb->inode_item.size & (Vcb->superblock.sectorsize - 1)) != 0);

        if (buf->num_sectors > (buflen - offsetof(btrfs_csum_info, data[0])) / buf->csum_length) {
            Status = STATUS_BUFFER_OVERFLOW;
            *retlen = offsetof(btrfs_csum_info, data[0]);
            goto end;
        }

        ptr = buf->data;
        last_sector = 0;

        le = fcb->extents.Flink;
        while (le != &fcb->extents) {
            extent* ext = CONTAINING_RECORD(le, extent, list_entry);
            uint64_t first_sector, num_sectors;

            if (ext->ignore || ext->extent_data.type == BTRFS_FILE_EXTENT_INLINE) {
                le = le->Flink;
                continue;
            }

            first_sector = ext->offset >> Vcb->sector_shift;
            if (first_sector >= buf->num_sectors)
                break;

            if (first_sector > last_sector) {
                Status = add_csum_sparse_extents(Vcb, first_sector - last_sector, &ptr, sparse_hash_found, sparse_hash);
                if (!NT_SUCCESS(Status))
                    goto end;
                sparse_hash_found = true;
            }

            num_sectors = min(ext->extent_data.num_bytes >> Vcb->sector_shift, buf->num_sectors - first_sector);

            if (ext->extent_data.compression != BTRFS_COMPRESS_NONE)
                memset(ptr, 0, num_sectors * Vcb->csum_size); // dummy value for compressed extents
            else {
                if (ext->csum)
                    memcpy(ptr, ext->csum, num_sectors * Vcb->csum_size);
                else
                    memset(ptr, 0, num_sectors * Vcb->csum_size);
            }

            ptr += num_sectors * Vcb->csum_size;
            last_sector = first_sector + num_sectors;

            le = le->Flink;
        }

        if (buf->num_sectors > last_sector) {
            Status = add_csum_sparse_extents(Vcb, buf->num_sectors - last_sector, &ptr, sparse_hash_found, sparse_hash);
            if (!NT_SUCCESS(Status))
                goto end;
        }

        *retlen = offsetof(btrfs_csum_info, data[0]) + (buf->csum_length * buf->num_sectors);
        Status = STATUS_SUCCESS;

end:
        ;
    } finally {
        ExReleaseResourceLite(fcb->Header.Resource);
    }

    return Status;
}
