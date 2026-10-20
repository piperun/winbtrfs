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

/* File and stream reads, cache dispatch and completed decompression jobs. */
#include "btrfs_drv.h"

extern bool diskacc;
extern tPsUpdateDiskCounters fPsUpdateDiskCounters;
extern tCcCopyReadEx fCcCopyReadEx;

#define LZO_PAGE_SIZE 4096

__attribute__((nonnull(1, 2)))
NTSTATUS read_stream(fcb* fcb, uint8_t* data, uint64_t start, ULONG length, ULONG* pbr) {
    ULONG readlen;

    TRACE("(%p, %p, %I64x, %lx, %p)\n", fcb, data, start, length, pbr);

    if (pbr) *pbr = 0;

    if (start >= fcb->adsdata.Length) {
        TRACE("tried to read beyond end of stream\n");
        return STATUS_END_OF_FILE;
    }

    if (length == 0) {
        WARN("tried to read zero bytes\n");
        return STATUS_SUCCESS;
    }

    if (start + length < fcb->adsdata.Length)
        readlen = length;
    else
        readlen = fcb->adsdata.Length - (ULONG)start;

    if (readlen > 0)
        RtlCopyMemory(data, fcb->adsdata.Buffer + start, readlen);

    if (pbr) *pbr = readlen;

    return STATUS_SUCCESS;
}

typedef struct {
    uint64_t off;
    uint64_t ed_size;
    uint64_t ed_offset;
    uint64_t ed_num_bytes;
} read_part_extent;

typedef struct {
    LIST_ENTRY list_entry;
    uint64_t addr;
    chunk* c;
    uint32_t read;
    uint32_t to_read;
    void* csum;
    bool csum_free;
    uint8_t* buf;
    bool buf_free;
    uint32_t bumpoff;
    bool mdl;
    void* data;
    uint8_t compression;
    unsigned int num_extents;
    read_part_extent extents[1];
} read_part;

typedef struct {
    LIST_ENTRY list_entry;
    calc_job* cj;
    void* decomp;
    void* data;
    unsigned int offset;
    size_t length;
} comp_calc_job;

__attribute__((nonnull(1, 2)))
NTSTATUS read_file(fcb* fcb, uint8_t* data, uint64_t start, uint64_t length, ULONG* pbr, PIRP Irp) {
    NTSTATUS Status;
    uint32_t bytes_read = 0;
    uint64_t last_end;
    LIST_ENTRY* le;
    POOL_TYPE pool_type;
    LIST_ENTRY read_parts, calc_jobs;

    TRACE("(%p, %p, %I64x, %I64x, %p)\n", fcb, data, start, length, pbr);

    if (pbr)
        *pbr = 0;

    if (start >= fcb->inode_item.size) {
        WARN("Tried to read beyond end of file\n");
        return STATUS_END_OF_FILE;
    }

    InitializeListHead(&read_parts);
    InitializeListHead(&calc_jobs);

    pool_type = fcb->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE ? NonPagedPool : PagedPool;

    le = fcb->extents.Flink;

    last_end = start;

    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if (!ext->ignore) {
            struct btrfs_file_extent_item* ed = &ext->extent_data;
            uint64_t len;

            if (ed->type == BTRFS_FILE_EXTENT_REG || ed->type == BTRFS_FILE_EXTENT_PREALLOC)
                len = ed->num_bytes;
            else
                len = ed->ram_bytes;

            if (ext->offset + len <= start) {
                last_end = ext->offset + len;
                goto nextitem;
            }

            if (ext->offset > last_end && ext->offset > start + bytes_read) {
                uint32_t read = (uint32_t)min(length, ext->offset - max(start, last_end));

                RtlZeroMemory(data + bytes_read, read);
                bytes_read += read;
                length -= read;
            }

            if (length == 0 || ext->offset > start + bytes_read + length)
                break;

            if (ed->encryption != 0) {
                WARN("Encryption not supported\n");
                Status = STATUS_NOT_IMPLEMENTED;
                goto exit;
            }

            if (ed->other_encoding != 0) {
                WARN("Other encodings not supported\n");
                Status = STATUS_NOT_IMPLEMENTED;
                goto exit;
            }

            switch (ed->type) {
                case BTRFS_FILE_EXTENT_INLINE:
                {
                    uint64_t off = start + bytes_read - ext->offset;
                    uint32_t read;

                    if (ed->compression == BTRFS_COMPRESS_NONE) {
                        read = (uint32_t)min(min(len, ext->datalen) - off, length);

                        RtlCopyMemory(data + bytes_read, (uint8_t*)&ed->disk_bytenr + off, read);
                    } else if (ed->compression == BTRFS_COMPRESS_ZLIB || ed->compression == BTRFS_COMPRESS_LZO || ed->compression == BTRFS_COMPRESS_ZSTD) {
                        uint8_t* decomp;
                        bool decomp_alloc;
                        uint16_t inlen = ext->datalen - (uint16_t)offsetof(struct btrfs_file_extent_item, disk_bytenr);

                        if (ed->ram_bytes == 0 || ed->ram_bytes > 0xffffffff) {
                            ERR("ed->ram_bytes was invalid (%I64x)\n", ed->ram_bytes);
                            Status = STATUS_INTERNAL_ERROR;
                            goto exit;
                        }

                        read = (uint32_t)min(ed->ram_bytes - off, length);

                        if (off > 0) {
                            decomp = ExAllocatePoolWithTag(NonPagedPool, (uint32_t)ed->ram_bytes, ALLOC_TAG);
                            if (!decomp) {
                                ERR("out of memory\n");
                                Status = STATUS_INSUFFICIENT_RESOURCES;
                                goto exit;
                            }

                            decomp_alloc = true;
                        } else {
                            decomp = data + bytes_read;
                            decomp_alloc = false;
                        }

                        if (ed->compression == BTRFS_COMPRESS_ZLIB) {
                            Status = zlib_decompress((uint8_t*)&ed->disk_bytenr, inlen, decomp, (uint32_t)(read + off));
                            if (!NT_SUCCESS(Status)) {
                                ERR("zlib_decompress returned %08lx\n", Status);
                                if (decomp_alloc) ExFreePool(decomp);
                                goto exit;
                            }
                        } else if (ed->compression == BTRFS_COMPRESS_LZO) {
                            if (inlen < sizeof(uint32_t)) {
                                ERR("extent data was truncated\n");
                                Status = STATUS_INTERNAL_ERROR;
                                if (decomp_alloc) ExFreePool(decomp);
                                goto exit;
                            } else
                                inlen -= sizeof(uint32_t);

                            Status = lzo_decompress((uint8_t*)&ed->disk_bytenr + sizeof(uint32_t), inlen, decomp, (uint32_t)(read + off), sizeof(uint32_t));
                            if (!NT_SUCCESS(Status)) {
                                ERR("lzo_decompress returned %08lx\n", Status);
                                if (decomp_alloc) ExFreePool(decomp);
                                goto exit;
                            }
                        } else if (ed->compression == BTRFS_COMPRESS_ZSTD) {
                            Status = zstd_decompress((uint8_t*)&ed->disk_bytenr, inlen, decomp, (uint32_t)(read + off));
                            if (!NT_SUCCESS(Status)) {
                                ERR("zstd_decompress returned %08lx\n", Status);
                                if (decomp_alloc) ExFreePool(decomp);
                                goto exit;
                            }
                        }

                        if (decomp_alloc) {
                            RtlCopyMemory(data + bytes_read, decomp + off, read);
                            ExFreePool(decomp);
                        }
                    } else {
                        ERR("unhandled compression type %x\n", ed->compression);
                        Status = STATUS_NOT_IMPLEMENTED;
                        goto exit;
                    }

                    bytes_read += read;
                    length -= read;

                    break;
                }

                case BTRFS_FILE_EXTENT_REG:
                {
                    read_part* rp;

                    rp = ExAllocatePoolWithTag(pool_type, sizeof(read_part), ALLOC_TAG);
                    if (!rp) {
                        ERR("out of memory\n");
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        goto exit;
                    }

                    rp->mdl = (Irp && Irp->MdlAddress) ? true : false;
                    rp->extents[0].off = start + bytes_read - ext->offset;
                    rp->bumpoff = 0;
                    rp->num_extents = 1;
                    rp->csum_free = false;

                    rp->read = (uint32_t)(len - rp->extents[0].off);
                    if (rp->read > length) rp->read = (uint32_t)length;

                    if (ed->compression == BTRFS_COMPRESS_NONE) {
                        rp->addr = ed->disk_bytenr + ed->offset + rp->extents[0].off;
                        rp->to_read = (uint32_t)sector_align(rp->read, fcb->Vcb->superblock.sectorsize);

                        if (rp->addr & (fcb->Vcb->superblock.sectorsize - 1)) {
                            rp->bumpoff = rp->addr & (fcb->Vcb->superblock.sectorsize - 1);
                            rp->addr -= rp->bumpoff;
                            rp->to_read = (uint32_t)sector_align(rp->read + rp->bumpoff, fcb->Vcb->superblock.sectorsize);
                        }
                    } else {
                        rp->addr = ed->disk_bytenr;
                        rp->to_read = (uint32_t)sector_align(ed->disk_num_bytes, fcb->Vcb->superblock.sectorsize);
                    }

                    if (ed->compression == BTRFS_COMPRESS_NONE && (start & (fcb->Vcb->superblock.sectorsize - 1)) == 0 &&
                        (length & (fcb->Vcb->superblock.sectorsize - 1)) == 0) {
                        rp->buf = data + bytes_read;
                        rp->buf_free = false;
                    } else {
                        rp->buf = ExAllocatePoolWithTag(pool_type, rp->to_read, ALLOC_TAG);
                        rp->buf_free = true;

                        if (!rp->buf) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(rp);
                            goto exit;
                        }

                        rp->mdl = false;
                    }

                    rp->c = get_chunk_from_address(fcb->Vcb, rp->addr);

                    if (!rp->c) {
                        ERR("get_chunk_from_address(%I64x) failed\n", rp->addr);

                        if (rp->buf_free)
                            ExFreePool(rp->buf);

                        ExFreePool(rp);

                        Status = STATUS_INTERNAL_ERROR;
                        goto exit;
                    }

                    if (ext->csum) {
                        if (ed->compression == BTRFS_COMPRESS_NONE) {
                            rp->csum = (uint8_t*)ext->csum + (fcb->Vcb->csum_size * (rp->extents[0].off >> fcb->Vcb->sector_shift));
                        } else
                            rp->csum = ext->csum;
                    } else
                        rp->csum = NULL;

                    rp->data = data + bytes_read;
                    rp->compression = ed->compression;
                    rp->extents[0].ed_offset = ed->offset;
                    rp->extents[0].ed_size = ed->disk_num_bytes;
                    rp->extents[0].ed_num_bytes = ed->num_bytes;

                    InsertTailList(&read_parts, &rp->list_entry);

                    bytes_read += rp->read;
                    length -= rp->read;

                    break;
                }

                case BTRFS_FILE_EXTENT_PREALLOC:
                {
                    uint64_t off = start + bytes_read - ext->offset;
                    uint32_t read = (uint32_t)(len - off);

                    if (read > length) read = (uint32_t)length;

                    RtlZeroMemory(data + bytes_read, read);

                    bytes_read += read;
                    length -= read;

                    break;
                }

                default:
                    WARN("Unsupported extent data type %u\n", ed->type);
                    Status = STATUS_NOT_IMPLEMENTED;
                    goto exit;
            }

            last_end = ext->offset + len;

            if (length == 0)
                break;
        }

nextitem:
        le = le->Flink;
    }

    if (!IsListEmpty(&read_parts) && read_parts.Flink->Flink != &read_parts) { // at least two entries in list
        read_part* last_rp = CONTAINING_RECORD(read_parts.Flink, read_part, list_entry);

        le = read_parts.Flink->Flink;
        while (le != &read_parts) {
            LIST_ENTRY* le2 = le->Flink;
            read_part* rp = CONTAINING_RECORD(le, read_part, list_entry);

            // merge together runs
            if (rp->compression != BTRFS_COMPRESS_NONE && rp->compression == last_rp->compression && rp->addr == last_rp->addr + last_rp->to_read &&
                rp->data == (uint8_t*)last_rp->data + last_rp->read && rp->c == last_rp->c && ((rp->csum && last_rp->csum) || (!rp->csum && !last_rp->csum))) {
                read_part* rp2;

                rp2 = ExAllocatePoolWithTag(pool_type, offsetof(read_part, extents) + (sizeof(read_part_extent) * (last_rp->num_extents + 1)), ALLOC_TAG);
                if (!rp2) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto exit;
                }

                rp2->addr = last_rp->addr;
                rp2->c = last_rp->c;
                rp2->read = last_rp->read + rp->read;
                rp2->to_read = last_rp->to_read + rp->to_read;
                rp2->csum_free = false;

                if (last_rp->csum) {
                    uint32_t sectors = (last_rp->to_read + rp->to_read) >> fcb->Vcb->sector_shift;

                    rp2->csum = ExAllocatePoolWithTag(pool_type, sectors * fcb->Vcb->csum_size, ALLOC_TAG);
                    if (!rp2->csum) {
                        ERR("out of memory\n");
                        ExFreePool(rp2);
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        goto exit;
                    }

                    RtlCopyMemory(rp2->csum, last_rp->csum, (last_rp->to_read * fcb->Vcb->csum_size) >> fcb->Vcb->sector_shift);
                    RtlCopyMemory((uint8_t*)rp2->csum + ((last_rp->to_read * fcb->Vcb->csum_size) >> fcb->Vcb->sector_shift), rp->csum,
                                  (rp->to_read * fcb->Vcb->csum_size) >> fcb->Vcb->sector_shift);

                    rp2->csum_free = true;
                } else
                    rp2->csum = NULL;

                rp2->buf = ExAllocatePoolWithTag(pool_type, rp2->to_read, ALLOC_TAG);
                if (!rp2->buf) {
                    ERR("out of memory\n");

                    if (rp2->csum)
                        ExFreePool(rp2->csum);

                    ExFreePool(rp2);
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto exit;
                }

                rp2->buf_free = true;
                rp2->bumpoff = 0;
                rp2->mdl = false;
                rp2->data = last_rp->data;
                rp2->compression = last_rp->compression;
                rp2->num_extents = last_rp->num_extents + 1;

                RtlCopyMemory(rp2->extents, last_rp->extents, last_rp->num_extents * sizeof(read_part_extent));
                RtlCopyMemory(&rp2->extents[last_rp->num_extents], rp->extents, sizeof(read_part_extent));

                InsertHeadList(le->Blink, &rp2->list_entry);

                if (rp->buf_free)
                    ExFreePool(rp->buf);

                if (rp->csum_free)
                    ExFreePool(rp->csum);

                RemoveEntryList(&rp->list_entry);

                ExFreePool(rp);

                if (last_rp->buf_free)
                    ExFreePool(last_rp->buf);

                if (last_rp->csum_free)
                    ExFreePool(last_rp->csum);

                RemoveEntryList(&last_rp->list_entry);

                ExFreePool(last_rp);

                last_rp = rp2;
            } else
                last_rp = rp;

            le = le2;
        }
    }

    le = read_parts.Flink;
    while (le != &read_parts) {
        read_part* rp = CONTAINING_RECORD(le, read_part, list_entry);

        Status = read_data(fcb->Vcb, rp->addr, rp->to_read, rp->csum, false, rp->buf, rp->c, NULL, Irp, 0, rp->mdl,
                           fcb->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE ? HighPagePriority : NormalPagePriority);
        if (!NT_SUCCESS(Status)) {
            ERR("read_data returned %08lx\n", Status);
            goto exit;
        }

        if (rp->compression == BTRFS_COMPRESS_NONE) {
            if (rp->buf_free)
                RtlCopyMemory(rp->data, rp->buf + rp->bumpoff, rp->read);
        } else {
            uint8_t* buf = rp->buf;

            for (unsigned int i = 0; i < rp->num_extents; i++) {
                uint8_t *decomp = NULL, *buf2;
                ULONG outlen, inlen, off2;
                uint32_t inpageoff = 0;
                comp_calc_job* ccj;

                off2 = (ULONG)(rp->extents[i].ed_offset + rp->extents[i].off);
                buf2 = buf;
                inlen = (ULONG)rp->extents[i].ed_size;

                if (rp->compression == BTRFS_COMPRESS_LZO) {
                    ULONG inoff = sizeof(uint32_t);

                    inlen -= sizeof(uint32_t);

                    // If reading a few sectors in, skip to the interesting bit
                    while (off2 > LZO_PAGE_SIZE) {
                        uint32_t partlen;

                        if (inlen < sizeof(uint32_t))
                            break;

                        partlen = *(uint32_t*)(buf2 + inoff);

                        if (partlen < inlen) {
                            off2 -= LZO_PAGE_SIZE;
                            inoff += partlen + sizeof(uint32_t);
                            inlen -= partlen + sizeof(uint32_t);

                            if (LZO_PAGE_SIZE - (inoff % LZO_PAGE_SIZE) < sizeof(uint32_t))
                                inoff = ((inoff / LZO_PAGE_SIZE) + 1) * LZO_PAGE_SIZE;
                        } else
                            break;
                    }

                    buf2 = &buf2[inoff];
                    inpageoff = inoff % LZO_PAGE_SIZE;
                }

                /* Previous versions of this code decompressed directly into the destination buffer,
                 * but unfortunately that can't be relied on - Windows likes to use dummy pages sometimes
                 * when mmap-ing, which breaks the backtracking used by e.g. zstd. */

                if (off2 != 0)
                    outlen = off2 + min(rp->read, (uint32_t)(rp->extents[i].ed_num_bytes - rp->extents[i].off));
                else
                    outlen = min(rp->read, (uint32_t)(rp->extents[i].ed_num_bytes - rp->extents[i].off));

                decomp = ExAllocatePoolWithTag(pool_type, outlen, ALLOC_TAG);
                if (!decomp) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto exit;
                }

                ccj = (comp_calc_job*)ExAllocatePoolWithTag(pool_type, sizeof(comp_calc_job), ALLOC_TAG);
                if (!ccj) {
                    ERR("out of memory\n");

                    ExFreePool(decomp);

                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto exit;
                }

                ccj->data = rp->data;
                ccj->decomp = decomp;

                ccj->offset = off2;
                ccj->length = (size_t)min(rp->read, rp->extents[i].ed_num_bytes - rp->extents[i].off);

                Status = add_calc_job_decomp(fcb->Vcb, rp->compression, buf2, inlen, decomp, outlen,
                                             inpageoff, &ccj->cj);
                if (!NT_SUCCESS(Status)) {
                    ERR("add_calc_job_decomp returned %08lx\n", Status);

                    ExFreePool(decomp);
                    ExFreePool(ccj);

                    goto exit;
                }

                InsertTailList(&calc_jobs, &ccj->list_entry);

                buf += rp->extents[i].ed_size;
                rp->data = (uint8_t*)rp->data + rp->extents[i].ed_num_bytes - rp->extents[i].off;
                rp->read -= (uint32_t)(rp->extents[i].ed_num_bytes - rp->extents[i].off);
            }
        }

        le = le->Flink;
    }

    if (length > 0 && start + bytes_read < fcb->inode_item.size) {
        uint32_t read = (uint32_t)min(fcb->inode_item.size - start - bytes_read, length);

        RtlZeroMemory(data + bytes_read, read);

        bytes_read += read;
        length -= read;
    }

    Status = STATUS_SUCCESS;

    while (!IsListEmpty(&calc_jobs)) {
        comp_calc_job* ccj = CONTAINING_RECORD(RemoveTailList(&calc_jobs), comp_calc_job, list_entry);

        calc_thread_main(fcb->Vcb, ccj->cj);

        KeWaitForSingleObject(&ccj->cj->event, Executive, KernelMode, false, NULL);

        if (!NT_SUCCESS(ccj->cj->Status))
            Status = ccj->cj->Status;

        RtlCopyMemory(ccj->data, (uint8_t*)ccj->decomp + ccj->offset, ccj->length);
        ExFreePool(ccj->decomp);
        ExFreePool(ccj->cj);

        ExFreePool(ccj);
    }

    if (pbr)
        *pbr = bytes_read;

exit:
    // Decompression jobs borrow read-part buffers until they complete.
    while (!IsListEmpty(&calc_jobs)) {
        comp_calc_job* ccj = CONTAINING_RECORD(RemoveHeadList(&calc_jobs), comp_calc_job, list_entry);

        calc_thread_main(fcb->Vcb, ccj->cj);

        KeWaitForSingleObject(&ccj->cj->event, Executive, KernelMode, false, NULL);

        if (ccj->decomp)
            ExFreePool(ccj->decomp);

        ExFreePool(ccj->cj);

        ExFreePool(ccj);
    }

    while (!IsListEmpty(&read_parts)) {
        read_part* rp = CONTAINING_RECORD(RemoveHeadList(&read_parts), read_part, list_entry);

        if (rp->buf_free)
            ExFreePool(rp->buf);

        if (rp->csum_free)
            ExFreePool(rp->csum);

        ExFreePool(rp);
    }

    return Status;
}

NTSTATUS do_read(PIRP Irp, bool wait, ULONG* bytes_read) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    fcb* fcb = FileObject->FsContext;
    uint8_t* data = NULL;
    ULONG length = IrpSp->Parameters.Read.Length, addon = 0;
    uint64_t start = IrpSp->Parameters.Read.ByteOffset.QuadPart;

    *bytes_read = 0;

    if (!fcb || !fcb->Vcb || !fcb->subvol)
        return STATUS_INTERNAL_ERROR;

    TRACE("fcb = %p\n", fcb);
    TRACE("offset = %I64x, length = %lx\n", start, length);
    TRACE("paging_io = %s, no cache = %s\n", Irp->Flags & IRP_PAGING_IO ? "true" : "false", Irp->Flags & IRP_NOCACHE ? "true" : "false");

    if (!fcb->ads && fcb->type == BTRFS_FT_DIR)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (!(Irp->Flags & IRP_PAGING_IO) && !FsRtlCheckLockForReadAccess(&fcb->lock, Irp)) {
        WARN("tried to read locked region\n");
        return STATUS_FILE_LOCK_CONFLICT;
    }

    if (length == 0) {
        TRACE("tried to read zero bytes\n");
        return STATUS_SUCCESS;
    }

    if (start >= (uint64_t)fcb->Header.FileSize.QuadPart) {
        TRACE("tried to read with offset after file end (%I64x >= %I64x)\n", start, fcb->Header.FileSize.QuadPart);
        return STATUS_END_OF_FILE;
    }

    TRACE("FileObject %p fcb %p FileSize = %I64x size = %I64x (%p)\n", FileObject, fcb, fcb->Header.FileSize.QuadPart, fcb->inode_item.size, &fcb->inode_item.size);

    if (!(Irp->Flags & IRP_NOCACHE) && IrpSp->MinorFunction & IRP_MN_MDL) {
        NTSTATUS Status = STATUS_SUCCESS;

        try {
            if (!FileObject->PrivateCacheMap) {
                CC_FILE_SIZES ccfs;

                ccfs.AllocationSize = fcb->Header.AllocationSize;
                ccfs.FileSize = fcb->Header.FileSize;
                ccfs.ValidDataLength = fcb->Header.ValidDataLength;

                init_file_cache(FileObject, &ccfs);
            }

            CcMdlRead(FileObject, &IrpSp->Parameters.Read.ByteOffset, length, &Irp->MdlAddress, &Irp->IoStatus);
        } except (EXCEPTION_EXECUTE_HANDLER) {
            Status = GetExceptionCode();
        }

        if (NT_SUCCESS(Status)) {
            Status = Irp->IoStatus.Status;
            Irp->IoStatus.Information += addon;
            *bytes_read = (ULONG)Irp->IoStatus.Information;
        } else
            ERR("EXCEPTION - %08lx\n", Status);

        return Status;
    }

    data = map_user_buffer(Irp, fcb->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE ? HighPagePriority : NormalPagePriority);

    if (Irp->MdlAddress && !data) {
        ERR("MmGetSystemAddressForMdlSafe returned NULL\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (start >= (uint64_t)fcb->Header.ValidDataLength.QuadPart) {
        length = (ULONG)min(length, (uint64_t)fcb->Header.FileSize.QuadPart - start);
        RtlZeroMemory(data, length);
        Irp->IoStatus.Information = *bytes_read = length;
        return STATUS_SUCCESS;
    }

    if (length + start > (uint64_t)fcb->Header.ValidDataLength.QuadPart) {
        addon = (ULONG)(min(start + length, (uint64_t)fcb->Header.FileSize.QuadPart) - fcb->Header.ValidDataLength.QuadPart);
        RtlZeroMemory(data + (fcb->Header.ValidDataLength.QuadPart - start), addon);
        length = (ULONG)(fcb->Header.ValidDataLength.QuadPart - start);
    }

    if (!(Irp->Flags & IRP_NOCACHE)) {
        NTSTATUS Status = STATUS_SUCCESS;

        try {
            if (!FileObject->PrivateCacheMap) {
                CC_FILE_SIZES ccfs;

                ccfs.AllocationSize = fcb->Header.AllocationSize;
                ccfs.FileSize = fcb->Header.FileSize;
                ccfs.ValidDataLength = fcb->Header.ValidDataLength;

                init_file_cache(FileObject, &ccfs);
            }

            if (fCcCopyReadEx) {
                TRACE("CcCopyReadEx(%p, %I64x, %lx, %u, %p, %p, %p)\n", FileObject, IrpSp->Parameters.Read.ByteOffset.QuadPart,
                        length, wait, data, &Irp->IoStatus, Irp->Tail.Overlay.Thread);
                TRACE("sizes = %I64x, %I64x, %I64x\n", fcb->Header.AllocationSize.QuadPart, fcb->Header.FileSize.QuadPart, fcb->Header.ValidDataLength.QuadPart);
                if (!fCcCopyReadEx(FileObject, &IrpSp->Parameters.Read.ByteOffset, length, wait, data, &Irp->IoStatus, Irp->Tail.Overlay.Thread)) {
                    TRACE("CcCopyReadEx could not wait\n");

                    IoMarkIrpPending(Irp);
                    return STATUS_PENDING;
                }
                TRACE("CcCopyReadEx finished\n");
            } else {
                TRACE("CcCopyRead(%p, %I64x, %lx, %u, %p, %p)\n", FileObject, IrpSp->Parameters.Read.ByteOffset.QuadPart, length, wait, data, &Irp->IoStatus);
                TRACE("sizes = %I64x, %I64x, %I64x\n", fcb->Header.AllocationSize.QuadPart, fcb->Header.FileSize.QuadPart, fcb->Header.ValidDataLength.QuadPart);
                if (!CcCopyRead(FileObject, &IrpSp->Parameters.Read.ByteOffset, length, wait, data, &Irp->IoStatus)) {
                    TRACE("CcCopyRead could not wait\n");

                    IoMarkIrpPending(Irp);
                    return STATUS_PENDING;
                }
                TRACE("CcCopyRead finished\n");
            }
        } except (EXCEPTION_EXECUTE_HANDLER) {
            Status = GetExceptionCode();
        }

        if (NT_SUCCESS(Status)) {
            Status = Irp->IoStatus.Status;
            Irp->IoStatus.Information += addon;
            *bytes_read = (ULONG)Irp->IoStatus.Information;
        } else
            ERR("EXCEPTION - %08lx\n", Status);

        return Status;
    } else {
        NTSTATUS Status;

        if (!wait) {
            IoMarkIrpPending(Irp);
            return STATUS_PENDING;
        }

        if (fcb->ads) {
            Status = read_stream(fcb, data, start, length, bytes_read);

            if (!NT_SUCCESS(Status))
                ERR("read_stream returned %08lx\n", Status);
        } else {
            Status = read_file(fcb, data, start, length, bytes_read, Irp);

            if (!NT_SUCCESS(Status))
                ERR("read_file returned %08lx\n", Status);
        }

        *bytes_read += addon;
        TRACE("read %lu bytes\n", *bytes_read);

        Irp->IoStatus.Information = *bytes_read;

        if (diskacc && Status != STATUS_PENDING) {
            PETHREAD thread = NULL;

            if (Irp->Tail.Overlay.Thread && !IoIsSystemThread(Irp->Tail.Overlay.Thread))
                thread = Irp->Tail.Overlay.Thread;
            else if (!IoIsSystemThread(PsGetCurrentThread()))
                thread = PsGetCurrentThread();
            else if (IoIsSystemThread(PsGetCurrentThread()) && IoGetTopLevelIrp() == Irp)
                thread = PsGetCurrentThread();

            if (thread)
                fPsUpdateDiskCounters(PsGetThreadProcess(thread), *bytes_read, 0, 1, 0, 0);
        }

        return Status;
    }
}

_Dispatch_type_(IRP_MJ_READ)
_Function_class_(DRIVER_DISPATCH)
NTSTATUS __stdcall drv_read(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    device_extension* Vcb = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    ULONG bytes_read = 0;
    NTSTATUS Status;
    bool top_level;
    fcb* fcb;
    ccb* ccb;
    bool acquired_fcb_lock = false, wait;

    FsRtlEnterFileSystem();

    top_level = is_top_level(Irp);

    TRACE("read\n");

    if (Vcb && Vcb->type == VCB_TYPE_VOLUME) {
        Status = vol_read(DeviceObject, Irp);
        goto exit2;
    } else if (!Vcb || Vcb->type != VCB_TYPE_FS) {
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }

    Irp->IoStatus.Information = 0;

    if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
        CcMdlReadComplete(IrpSp->FileObject, Irp->MdlAddress);

        Irp->MdlAddress = NULL;
        Status = STATUS_SUCCESS;

        goto exit;
    }

    fcb = FileObject->FsContext;

    if (!fcb) {
        ERR("fcb was NULL\n");
        Status = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    ccb = FileObject->FsContext2;

    if (!ccb) {
        ERR("ccb was NULL\n");
        Status = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    if (Irp->RequestorMode == UserMode && !(ccb->access & FILE_READ_DATA)) {
        WARN("insufficient privileges\n");
        Status = STATUS_ACCESS_DENIED;
        goto exit;
    }

    if (fcb == Vcb->volume_fcb) {
        TRACE("reading volume FCB\n");

        IoSkipCurrentIrpStackLocation(Irp);

        Status = IoCallDriver(Vcb->Vpb->RealDevice, Irp);

        goto exit2;
    }

    if (!(Irp->Flags & IRP_PAGING_IO))
        FsRtlCheckOplock(fcb_oplock(fcb), Irp, NULL, NULL, NULL);

    wait = IoIsOperationSynchronous(Irp);

    // Don't offload jobs when doing paging IO - otherwise this can lead to
    // deadlocks in CcCopyRead.
    if (Irp->Flags & IRP_PAGING_IO)
        wait = true;

    if (!(Irp->Flags & IRP_PAGING_IO) && FileObject->SectionObjectPointer && FileObject->SectionObjectPointer->DataSectionObject) {
        IO_STATUS_BLOCK iosb;

        CcFlushCache(FileObject->SectionObjectPointer, &IrpSp->Parameters.Read.ByteOffset, IrpSp->Parameters.Read.Length, &iosb);
        if (!NT_SUCCESS(iosb.Status)) {
            ERR("CcFlushCache returned %08lx\n", iosb.Status);
            Status = iosb.Status;
            goto exit;
        }
    }

    if (!ExIsResourceAcquiredSharedLite(fcb->Header.Resource)) {
        if (!ExAcquireResourceSharedLite(fcb->Header.Resource, wait)) {
            Status = STATUS_PENDING;
            IoMarkIrpPending(Irp);
            goto exit;
        }

        acquired_fcb_lock = true;
    }

    Status = do_read(Irp, wait, &bytes_read);

    if (acquired_fcb_lock)
        ExReleaseResourceLite(fcb->Header.Resource);

exit:
    if (FileObject->Flags & FO_SYNCHRONOUS_IO && !(Irp->Flags & IRP_PAGING_IO))
        FileObject->CurrentByteOffset.QuadPart = IrpSp->Parameters.Read.ByteOffset.QuadPart + (NT_SUCCESS(Status) ? bytes_read : 0);

end:
    Irp->IoStatus.Status = Status;

    TRACE("Irp->IoStatus.Status = %08lx\n", Irp->IoStatus.Status);
    TRACE("Irp->IoStatus.Information = %Iu\n", Irp->IoStatus.Information);
    TRACE("returning %08lx\n", Status);

    if (Status != STATUS_PENDING)
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    else {
        if (!add_thread_job(Vcb, Irp))
            Status = do_read_job(Irp);
    }

exit2:
    if (top_level)
        IoSetTopLevelIrp(NULL);

    FsRtlExitFileSystem();

    return Status;
}
