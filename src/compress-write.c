/* Copyright (c) Mark Harmstone 2016-17
 * Copyright (c) Reimar Doeffinger 2006
 * Copyright (c) Markus Oberhumer 1996
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

/* Write compressed file extents. Compression codecs live in compress.c. */
#include "btrfs_drv.h"

typedef struct {
    uint8_t buf[COMPRESSED_EXTENT_SIZE];
    uint8_t compression_type;
    unsigned int inlen;
    unsigned int outlen;
    calc_job* cj;
} comp_part;

NTSTATUS write_compressed(fcb* fcb, uint64_t start_data, uint64_t end_data, void* data, PIRP Irp, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    uint64_t i;
    unsigned int num_parts = (unsigned int)sector_align(end_data - start_data, COMPRESSED_EXTENT_SIZE) / COMPRESSED_EXTENT_SIZE;
    uint8_t type;
    comp_part* parts;
    unsigned int buflen = 0;
    uint8_t* buf;
    chunk* c = NULL;
    LIST_ENTRY* le;
    uint64_t address, extaddr;
    void* csum = NULL;

    if (fcb->Vcb->options.compress_type != 0 && fcb->prop_compression == PropCompression_None)
        type = fcb->Vcb->options.compress_type;
    else {
        if (!(fcb->Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD) && fcb->prop_compression == PropCompression_ZSTD)
            type = BTRFS_COMPRESS_ZSTD;
        else if (fcb->Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD && fcb->prop_compression != PropCompression_Zlib && fcb->prop_compression != PropCompression_LZO)
            type = BTRFS_COMPRESS_ZSTD;
        else if (!(fcb->Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO) && fcb->prop_compression == PropCompression_LZO)
            type = BTRFS_COMPRESS_LZO;
        else if (fcb->Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO && fcb->prop_compression != PropCompression_Zlib)
            type = BTRFS_COMPRESS_LZO;
        else
            type = BTRFS_COMPRESS_ZLIB;
    }

    Status = excise_extents(fcb->Vcb, fcb, start_data, end_data, Irp, rollback);
    if (!NT_SUCCESS(Status)) {
        ERR("excise_extents returned %08lx\n", Status);
        return Status;
    }

    parts = ExAllocatePoolWithTag(PagedPool, sizeof(comp_part) * num_parts, ALLOC_TAG);
    if (!parts) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < num_parts; i++) {
        if (i == num_parts - 1)
            parts[i].inlen = ((unsigned int)(end_data - start_data) - ((num_parts - 1) * COMPRESSED_EXTENT_SIZE));
        else
            parts[i].inlen = COMPRESSED_EXTENT_SIZE;

        Status = add_calc_job_comp(fcb->Vcb, type, (uint8_t*)data + (i * COMPRESSED_EXTENT_SIZE), parts[i].inlen,
                                   parts[i].buf, parts[i].inlen, &parts[i].cj);
        if (!NT_SUCCESS(Status)) {
            ERR("add_calc_job_comp returned %08lx\n", Status);

            for (unsigned int j = 0; j < i; j++) {
                KeWaitForSingleObject(&parts[j].cj->event, Executive, KernelMode, false, NULL);
                ExFreePool(parts[j].cj);
            }

            ExFreePool(parts);
            return Status;
        }
    }

    Status = STATUS_SUCCESS;

    for (int i = num_parts - 1; i >= 0; i--) {
        calc_thread_main(fcb->Vcb, parts[i].cj);

        KeWaitForSingleObject(&parts[i].cj->event, Executive, KernelMode, false, NULL);

        if (!NT_SUCCESS(parts[i].cj->Status))
            Status = parts[i].cj->Status;
    }

    if (!NT_SUCCESS(Status)) {
        ERR("calc job returned %08lx\n", Status);

        for (unsigned int i = 0; i < num_parts; i++) {
            ExFreePool(parts[i].cj);
        }

        ExFreePool(parts);
        return Status;
    }

    for (unsigned int i = 0; i < num_parts; i++) {
        if (parts[i].cj->space_left >= fcb->Vcb->superblock.sectorsize) {
            parts[i].compression_type = type;
            parts[i].outlen = parts[i].inlen - parts[i].cj->space_left;

            if (type == BTRFS_COMPRESS_LZO)
                fcb->Vcb->superblock.incompat_flags |= BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO;
            else if (type == BTRFS_COMPRESS_ZSTD)
                fcb->Vcb->superblock.incompat_flags |= BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD;

            if ((parts[i].outlen & (fcb->Vcb->superblock.sectorsize - 1)) != 0) {
                unsigned int newlen = (unsigned int)sector_align(parts[i].outlen, fcb->Vcb->superblock.sectorsize);

                RtlZeroMemory(parts[i].buf + parts[i].outlen, newlen - parts[i].outlen);

                parts[i].outlen = newlen;
            }
        } else {
            parts[i].compression_type = BTRFS_COMPRESS_NONE;
            parts[i].outlen = (unsigned int)sector_align(parts[i].inlen, fcb->Vcb->superblock.sectorsize);
        }

        buflen += parts[i].outlen;
        ExFreePool(parts[i].cj);
    }

    // check if first 128 KB of file is incompressible

    if (start_data == 0 && parts[0].compression_type == BTRFS_COMPRESS_NONE && !fcb->Vcb->options.compress_force) {
        TRACE("adding nocompress flag to subvol %I64x, inode %I64x\n", fcb->subvol->id, fcb->inode);

        fcb->inode_item.flags |= BTRFS_INODE_NOCOMPRESS;
        fcb->inode_item_changed = true;
        mark_fcb_dirty(fcb);
    }

    // join together into continuous buffer

    buf = ExAllocatePoolWithTag(PagedPool, buflen, ALLOC_TAG);
    if (!buf) {
        ERR("out of memory\n");
        ExFreePool(parts);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    {
        uint8_t* buf2 = buf;

        for (i = 0; i < num_parts; i++) {
            if (parts[i].compression_type == BTRFS_COMPRESS_NONE)
                RtlCopyMemory(buf2, (uint8_t*)data + (i * COMPRESSED_EXTENT_SIZE), parts[i].outlen);
            else
                RtlCopyMemory(buf2, parts[i].buf, parts[i].outlen);

            buf2 += parts[i].outlen;
        }
    }

    // find an address

    ExAcquireResourceSharedLite(&fcb->Vcb->chunk_lock, true);

    le = fcb->Vcb->chunks.Flink;
    while (le != &fcb->Vcb->chunks) {
        chunk* c2 = CONTAINING_RECORD(le, chunk, list_entry);

        if (!c2->readonly && !c2->reloc) {
            acquire_chunk_lock(c2, fcb->Vcb);

            if (c2->chunk_item->type == fcb->Vcb->data_flags && (c2->chunk_item->length - c2->used) >= buflen) {
                if (find_data_address_in_chunk(fcb->Vcb, c2, buflen, &address)) {
                    c = c2;
                    c->used += buflen;

                    Status = space_list_subtract(c, address, buflen, rollback);
                    if (!NT_SUCCESS(Status)) {
                        release_chunk_lock(c2, fcb->Vcb);
                        ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
                        ExFreePool(buf);
                        ExFreePool(parts);
                        return Status;
                    }

                    release_chunk_lock(c2, fcb->Vcb);
                    break;
                }
            }

            release_chunk_lock(c2, fcb->Vcb);
        }

        le = le->Flink;
    }

    ExReleaseResourceLite(&fcb->Vcb->chunk_lock);

    if (!c) {
        chunk* c2;

        ExAcquireResourceExclusiveLite(&fcb->Vcb->chunk_lock, true);

        Status = alloc_chunk(fcb->Vcb, fcb->Vcb->data_flags, &c2, false);

        ExReleaseResourceLite(&fcb->Vcb->chunk_lock);

        if (!NT_SUCCESS(Status)) {
            ERR("alloc_chunk returned %08lx\n", Status);
            ExFreePool(buf);
            ExFreePool(parts);
            return Status;
        }

        acquire_chunk_lock(c2, fcb->Vcb);

        if (find_data_address_in_chunk(fcb->Vcb, c2, buflen, &address)) {
            c = c2;
            c->used += buflen;

            Status = space_list_subtract(c, address, buflen, rollback);
            if (!NT_SUCCESS(Status)) {
                release_chunk_lock(c2, fcb->Vcb);
                ExFreePool(buf);
                ExFreePool(parts);
                return Status;
            }
        }

        release_chunk_lock(c2, fcb->Vcb);
    }

    if (!c) {
        WARN("couldn't find any data chunks with %x bytes free\n", buflen);
        ExFreePool(buf);
        ExFreePool(parts);
        return STATUS_DISK_FULL;
    }

    // write to disk

    TRACE("writing %x bytes to %I64x\n", buflen, address);

    Status = write_data_complete(fcb->Vcb, address, buf, buflen, Irp, NULL, false, 0,
                                 fcb->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE ? HighPagePriority : NormalPagePriority);
    if (!NT_SUCCESS(Status)) {
        ERR("write_data_complete returned %08lx\n", Status);
        ExFreePool(buf);
        ExFreePool(parts);
        return Status;
    }

    // FIXME - do rest of the function while we're waiting for I/O to finish?

    // calculate csums if necessary

    if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
        unsigned int sl = buflen >> fcb->Vcb->sector_shift;

        csum = ExAllocatePoolWithTag(PagedPool, sl * fcb->Vcb->csum_size, ALLOC_TAG);
        if (!csum) {
            ERR("out of memory\n");
            ExFreePool(buf);
            ExFreePool(parts);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        do_calc_job(fcb->Vcb, buf, sl, csum);
    }

    ExFreePool(buf);

    // add extents to fcb

    extaddr = address;

    for (i = 0; i < num_parts; i++) {
        struct btrfs_file_extent_item* ed;
        void* csum2;

        ed = ExAllocatePoolWithTag(PagedPool, sizeof(struct btrfs_file_extent_item), ALLOC_TAG);
        if (!ed) {
            ERR("out of memory\n");
            ExFreePool(parts);

            if (csum)
                ExFreePool(csum);

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ed->generation = fcb->Vcb->superblock.generation;
        ed->ram_bytes = parts[i].inlen;
        ed->compression = parts[i].compression_type;
        ed->encryption = 0;
        ed->other_encoding = 0;
        ed->type = BTRFS_FILE_EXTENT_REG;
        ed->disk_bytenr = extaddr;
        ed->disk_num_bytes = parts[i].outlen;
        ed->offset = 0;
        ed->num_bytes = parts[i].inlen;

        if (csum) {
            csum2 = ExAllocatePoolWithTag(PagedPool, (parts[i].outlen * fcb->Vcb->csum_size) >> fcb->Vcb->sector_shift, ALLOC_TAG);
            if (!csum2) {
                ERR("out of memory\n");
                ExFreePool(ed);
                ExFreePool(parts);
                ExFreePool(csum);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            RtlCopyMemory(csum2, (uint8_t*)csum + (((extaddr - address) * fcb->Vcb->csum_size) >> fcb->Vcb->sector_shift),
                          (parts[i].outlen * fcb->Vcb->csum_size) >> fcb->Vcb->sector_shift);
        } else
            csum2 = NULL;

        Status = add_extent_to_fcb(fcb, start_data + (i * COMPRESSED_EXTENT_SIZE), ed, sizeof(struct btrfs_file_extent_item),
                                   true, csum2, rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("add_extent_to_fcb returned %08lx\n", Status);
            ExFreePool(ed);
            ExFreePool(parts);

            if (csum)
                ExFreePool(csum);

            return Status;
        }

        ExFreePool(ed);

        fcb->inode_item.nbytes += parts[i].inlen;

        extaddr += parts[i].outlen;
    }

    if (csum)
        ExFreePool(csum);

    // update extent refcounts

    ExAcquireResourceExclusiveLite(&c->changed_extents_lock, true);

    extaddr = address;

    for (i = 0; i < num_parts; i++) {
        Status = add_changed_extent_ref(c, extaddr, parts[i].outlen, fcb->subvol->id,
                                        fcb->inode, start_data + (i * COMPRESSED_EXTENT_SIZE),
                                        1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                        rollback);
        if (!NT_SUCCESS(Status)) {
            ExReleaseResourceLite(&c->changed_extents_lock);
            ExFreePool(parts);
            return Status;
        }

        extaddr += parts[i].outlen;
    }

    ExReleaseResourceLite(&c->changed_extents_lock);

    fcb->extents_changed = true;
    fcb->inode_item_changed = true;
    mark_fcb_dirty(fcb);

    ExFreePool(parts);

    return STATUS_SUCCESS;
}
