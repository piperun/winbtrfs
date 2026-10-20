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

/* Writes to existing extents and allocation of the remaining range. */
#include "write-extents.h"

__attribute__((nonnull(1,2,5,6)))
static NTSTATUS do_write_file_prealloc(fcb* fcb, extent* ext, uint64_t start_data, uint64_t end_data, void* data, uint64_t* written,
                                       PIRP Irp, bool file_write, uint64_t irp_offset, ULONG priority, LIST_ENTRY* rollback) {
    struct btrfs_file_extent_item* ed = &ext->extent_data;
    NTSTATUS Status;
    chunk* c = NULL;

    if (start_data <= ext->offset && end_data >= ext->offset + ed->num_bytes) { // replace all
        extent* newext;

        newext = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(&newext->extent_data, &ext->extent_data, ext->datalen);

        newext->extent_data.type = BTRFS_FILE_EXTENT_REG;

        Status = write_data_complete(fcb->Vcb, ed->disk_bytenr + ed->offset, (uint8_t*)data + ext->offset - start_data, (uint32_t)ed->num_bytes, Irp,
                                     NULL, file_write, irp_offset + ext->offset - start_data, priority);
        if (!NT_SUCCESS(Status)) {
            ERR("write_data_complete returned %08lx\n", Status);
            return Status;
        }

        if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
            ULONG sl = (ULONG)(ed->num_bytes >> fcb->Vcb->sector_shift);
            void* csum = ExAllocatePoolWithTag(PagedPool, sl * fcb->Vcb->csum_size, ALLOC_TAG);

            if (!csum) {
                ERR("out of memory\n");
                ExFreePool(newext);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            do_calc_job(fcb->Vcb, (uint8_t*)data + ext->offset - start_data, sl, csum);

            newext->csum = csum;
        } else
            newext->csum = NULL;

        *written = ed->num_bytes;

        newext->offset = ext->offset;
        newext->datalen = ext->datalen;
        newext->unique = ext->unique;
        newext->ignore = false;
        newext->inserted = true;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext);
            if (!NT_SUCCESS(Status)) {
                if (newext->csum)
                    ExFreePool(newext->csum);

                ExFreePool(newext);
                return Status;
            }
        }

        InsertHeadList(&ext->list_entry, &newext->list_entry);

        Status = remove_fcb_extent(fcb, ext, rollback);
        if (!NT_SUCCESS(Status))
            return Status;

        c = get_chunk_from_address(fcb->Vcb, ed->disk_bytenr);
    } else if (start_data <= ext->offset && end_data < ext->offset + ed->num_bytes) { // replace beginning
        extent *newext1, *newext2;

        newext1 = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext1) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        newext2 = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext2) {
            ERR("out of memory\n");
            ExFreePool(newext1);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(&newext1->extent_data, &ext->extent_data, ext->datalen);
        newext1->extent_data.type = BTRFS_FILE_EXTENT_REG;
        newext1->extent_data.num_bytes = end_data - ext->offset;

        RtlCopyMemory(&newext2->extent_data, &ext->extent_data, ext->datalen);
        newext2->extent_data.offset += end_data - ext->offset;
        newext2->extent_data.num_bytes -= end_data - ext->offset;

        Status = write_data_complete(fcb->Vcb, ed->disk_bytenr + ed->offset, (uint8_t*)data + ext->offset - start_data, (uint32_t)(end_data - ext->offset),
                                     Irp, NULL, file_write, irp_offset + ext->offset - start_data, priority);
        if (!NT_SUCCESS(Status)) {
            ERR("write_data_complete returned %08lx\n", Status);
            ExFreePool(newext1);
            ExFreePool(newext2);
            return Status;
        }

        if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
            ULONG sl = (ULONG)((end_data - ext->offset) >> fcb->Vcb->sector_shift);
            void* csum = ExAllocatePoolWithTag(PagedPool, sl * fcb->Vcb->csum_size, ALLOC_TAG);

            if (!csum) {
                ERR("out of memory\n");
                ExFreePool(newext1);
                ExFreePool(newext2);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            do_calc_job(fcb->Vcb, (uint8_t*)data + ext->offset - start_data, sl, csum);

            newext1->csum = csum;
        } else
            newext1->csum = NULL;

        *written = end_data - ext->offset;

        newext1->offset = ext->offset;
        newext1->datalen = ext->datalen;
        newext1->unique = ext->unique;
        newext1->ignore = false;
        newext1->inserted = true;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext1);
            if (!NT_SUCCESS(Status)) {
                if (newext1->csum)
                    ExFreePool(newext1->csum);

                ExFreePool(newext1);
                ExFreePool(newext2);
                return Status;
            }
        }

        InsertHeadList(&ext->list_entry, &newext1->list_entry);

        newext2->offset = end_data;
        newext2->datalen = ext->datalen;
        newext2->unique = ext->unique;
        newext2->ignore = false;
        newext2->inserted = true;
        newext2->csum = NULL;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext2);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(newext2);
                return Status;
            }
        }

        add_extent(fcb, &newext1->list_entry, newext2);

        c = get_chunk_from_address(fcb->Vcb, ed->disk_bytenr);

        if (!c)
            ERR("get_chunk_from_address(%I64x) failed\n", ed->disk_bytenr);
        else {
            Status = update_changed_extent_ref(fcb->Vcb, c, ed->disk_bytenr, ed->disk_num_bytes,
                                               fcb->subvol->id, fcb->inode, ext->offset - ed->offset,
                                               1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                               false, Irp, rollback);

            if (!NT_SUCCESS(Status)) {
                ERR("update_changed_extent_ref returned %08lx\n", Status);
                return Status;
            }
        }

        Status = remove_fcb_extent(fcb, ext, rollback);
        if (!NT_SUCCESS(Status))
            return Status;
    } else if (start_data > ext->offset && end_data >= ext->offset + ed->num_bytes) { // replace end
        extent *newext1, *newext2;

        newext1 = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext1) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        newext2 = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext2) {
            ERR("out of memory\n");
            ExFreePool(newext1);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(&newext1->extent_data, &ext->extent_data, ext->datalen);

        newext1->extent_data.num_bytes = start_data - ext->offset;

        RtlCopyMemory(&newext2->extent_data, &ext->extent_data, ext->datalen);

        newext2->extent_data.type = BTRFS_FILE_EXTENT_REG;
        newext2->extent_data.offset += start_data - ext->offset;
        newext2->extent_data.num_bytes = ext->offset + ed->num_bytes - start_data;

        Status = write_data_complete(fcb->Vcb, ed->disk_bytenr + newext2->extent_data.offset, data, (uint32_t)newext2->extent_data.num_bytes, Irp, NULL, file_write, irp_offset, priority);
        if (!NT_SUCCESS(Status)) {
            ERR("write_data_complete returned %08lx\n", Status);
            ExFreePool(newext1);
            ExFreePool(newext2);
            return Status;
        }

        if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
            ULONG sl = (ULONG)(newext2->extent_data.num_bytes >> fcb->Vcb->sector_shift);
            void* csum = ExAllocatePoolWithTag(PagedPool, sl * fcb->Vcb->csum_size, ALLOC_TAG);

            if (!csum) {
                ERR("out of memory\n");
                ExFreePool(newext1);
                ExFreePool(newext2);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            do_calc_job(fcb->Vcb, data, sl, csum);

            newext2->csum = csum;
        } else
            newext2->csum = NULL;

        *written = newext2->extent_data.num_bytes;

        newext1->offset = ext->offset;
        newext1->datalen = ext->datalen;
        newext1->unique = ext->unique;
        newext1->ignore = false;
        newext1->inserted = true;
        newext1->csum = NULL;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext1);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(newext1);

                if (newext2->csum)
                    ExFreePool(newext2->csum);

                ExFreePool(newext2);
                return Status;
            }
        }

        InsertHeadList(&ext->list_entry, &newext1->list_entry);

        newext2->offset = start_data;
        newext2->datalen = ext->datalen;
        newext2->unique = ext->unique;
        newext2->ignore = false;
        newext2->inserted = true;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext2);
            if (!NT_SUCCESS(Status)) {
                if (newext2->csum)
                    ExFreePool(newext2->csum);

                ExFreePool(newext2);
                return Status;
            }
        }

        add_extent(fcb, &newext1->list_entry, newext2);

        c = get_chunk_from_address(fcb->Vcb, ed->disk_bytenr);

        if (!c)
            ERR("get_chunk_from_address(%I64x) failed\n", ed->disk_bytenr);
        else {
            Status = update_changed_extent_ref(fcb->Vcb, c, ed->disk_bytenr, ed->disk_num_bytes,
                                               fcb->subvol->id, fcb->inode, ext->offset - ed->offset,
                                               1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                               false, Irp, rollback);
            if (!NT_SUCCESS(Status)) {
                ERR("update_changed_extent_ref returned %08lx\n", Status);
                return Status;
            }
        }

        Status = remove_fcb_extent(fcb, ext, rollback);
        if (!NT_SUCCESS(Status))
            return Status;
    } else if (start_data > ext->offset && end_data < ext->offset + ed->num_bytes) { // replace middle
        extent *newext1, *newext2, *newext3;

        newext1 = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext1) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        newext2 = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext2) {
            ERR("out of memory\n");
            ExFreePool(newext1);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        newext3 = ExAllocatePoolWithTag(PagedPool, offsetof(extent, extent_data) + ext->datalen, ALLOC_TAG);
        if (!newext3) {
            ERR("out of memory\n");
            ExFreePool(newext1);
            ExFreePool(newext2);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(&newext1->extent_data, &ext->extent_data, ext->datalen);
        RtlCopyMemory(&newext2->extent_data, &ext->extent_data, ext->datalen);
        RtlCopyMemory(&newext3->extent_data, &ext->extent_data, ext->datalen);

        newext1->extent_data.num_bytes = start_data - ext->offset;

        newext2->extent_data.type = BTRFS_FILE_EXTENT_REG;
        newext2->extent_data.offset += start_data - ext->offset;
        newext2->extent_data.num_bytes = end_data - start_data;

        newext3->extent_data.offset += end_data - ext->offset;
        newext3->extent_data.num_bytes -= end_data - ext->offset;

        Status = write_data_complete(fcb->Vcb, ed->disk_bytenr + newext2->extent_data.offset, data, (uint32_t)(end_data - start_data), Irp, NULL, file_write, irp_offset, priority);
        if (!NT_SUCCESS(Status)) {
            ERR("write_data_complete returned %08lx\n", Status);
            ExFreePool(newext1);
            ExFreePool(newext2);
            ExFreePool(newext3);
            return Status;
        }

        if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
            ULONG sl = (ULONG)((end_data - start_data) >> fcb->Vcb->sector_shift);
            void* csum = ExAllocatePoolWithTag(PagedPool, sl * fcb->Vcb->csum_size, ALLOC_TAG);

            if (!csum) {
                ERR("out of memory\n");
                ExFreePool(newext1);
                ExFreePool(newext2);
                ExFreePool(newext3);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            do_calc_job(fcb->Vcb, data, sl, csum);

            newext2->csum = csum;
        } else
            newext2->csum = NULL;

        *written = end_data - start_data;

        newext1->offset = ext->offset;
        newext1->datalen = ext->datalen;
        newext1->unique = ext->unique;
        newext1->ignore = false;
        newext1->inserted = true;
        newext1->csum = NULL;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext1);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(newext1);

                if (newext2->csum)
                    ExFreePool(newext2->csum);

                ExFreePool(newext2);
                ExFreePool(newext3);
                return Status;
            }
        }

        InsertHeadList(&ext->list_entry, &newext1->list_entry);

        newext2->offset = start_data;
        newext2->datalen = ext->datalen;
        newext2->unique = ext->unique;
        newext2->ignore = false;
        newext2->inserted = true;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext2);
            if (!NT_SUCCESS(Status)) {
                if (newext2->csum)
                    ExFreePool(newext2->csum);

                ExFreePool(newext2);
                ExFreePool(newext3);
                return Status;
            }
        }

        add_extent(fcb, &newext1->list_entry, newext2);

        newext3->offset = end_data;
        newext3->datalen = ext->datalen;
        newext3->unique = ext->unique;
        newext3->ignore = false;
        newext3->inserted = true;
        newext3->csum = NULL;

        if (rollback) {
            Status = add_insert_extent_rollback(rollback, fcb, newext3);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(newext3);
                return Status;
            }
        }

        add_extent(fcb, &newext2->list_entry, newext3);

        c = get_chunk_from_address(fcb->Vcb, ed->disk_bytenr);

        if (!c)
            ERR("get_chunk_from_address(%I64x) failed\n", ed->disk_bytenr);
        else {
            Status = update_changed_extent_ref(fcb->Vcb, c, ed->disk_bytenr, ed->disk_num_bytes,
                                               fcb->subvol->id, fcb->inode, ext->offset - ed->offset,
                                               2, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                               false, Irp, rollback);
            if (!NT_SUCCESS(Status)) {
                ERR("update_changed_extent_ref returned %08lx\n", Status);
                return Status;
            }
        }

        Status = remove_fcb_extent(fcb, ext, rollback);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (c)
        c->changed = true;

    return STATUS_SUCCESS;
}

__attribute__((nonnull(1, 4)))
NTSTATUS do_write_file(fcb* fcb, uint64_t start, uint64_t end_data, void* data, PIRP Irp, bool file_write, uint32_t irp_offset, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    LIST_ENTRY *le, *le2;
    uint64_t written = 0, length = end_data - start;
    uint64_t last_cow_start;
    ULONG priority = fcb->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE ? HighPagePriority : NormalPagePriority;
#ifdef DEBUG_PARANOID
    uint64_t last_off;
#endif
    bool extents_changed = false;

    last_cow_start = 0;

    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        le2 = le->Flink;

        if (!ext->ignore) {
            struct btrfs_file_extent_item* ed = &ext->extent_data;
            uint64_t len;

            if (ed->type == BTRFS_FILE_EXTENT_INLINE)
                len = ed->ram_bytes;
            else
                len = ed->num_bytes;

            if (ext->offset + len <= start)
                goto nextitem;

            if (ext->offset > start + written + length)
                break;

            if ((fcb->inode_item.flags & BTRFS_INODE_NODATACOW || ed->type == BTRFS_FILE_EXTENT_PREALLOC) && ext->unique && ed->compression == BTRFS_COMPRESS_NONE) {
                if (max(last_cow_start, start + written) < ext->offset) {
                    uint64_t start_write = max(last_cow_start, start + written);

                    extents_changed = true;

                    Status = excise_extents(fcb->Vcb, fcb, start_write, ext->offset, Irp, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("excise_extents returned %08lx\n", Status);
                        return Status;
                    }

                    Status = insert_extent(fcb->Vcb, fcb, start_write, ext->offset - start_write, (uint8_t*)data + written, Irp, file_write, irp_offset + written, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("insert_extent returned %08lx\n", Status);
                        return Status;
                    }

                    written += ext->offset - start_write;
                    length -= ext->offset - start_write;

                    if (length == 0)
                        break;
                }

                if (ed->type == BTRFS_FILE_EXTENT_REG) {
                    uint64_t writeaddr = ed->disk_bytenr + ed->offset + start + written - ext->offset;
                    uint64_t write_len = min(ext->offset + len - start - written, length);
                    chunk* c;

                    TRACE("doing non-COW write to %I64x\n", writeaddr);

                    Status = write_data_complete(fcb->Vcb, writeaddr, (uint8_t*)data + written, (uint32_t)write_len, Irp, NULL, file_write, irp_offset + written, priority);
                    if (!NT_SUCCESS(Status)) {
                        ERR("write_data_complete returned %08lx\n", Status);
                        return Status;
                    }

                    c = get_chunk_from_address(fcb->Vcb, writeaddr);
                    if (c)
                        c->changed = true;

                    // This shouldn't ever get called - nocow files should always also be nosum.
                    if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
                        do_calc_job(fcb->Vcb, (uint8_t*)data + written, (uint32_t)(write_len >> fcb->Vcb->sector_shift),
                                    (uint8_t*)ext->csum + (((start + written - ext->offset) * fcb->Vcb->csum_size) >> fcb->Vcb->sector_shift));

                        ext->inserted = true;
                        extents_changed = true;
                    }

                    written += write_len;
                    length -= write_len;

                    if (length == 0)
                        break;
                } else if (ed->type == BTRFS_FILE_EXTENT_PREALLOC) {
                    uint64_t write_len;

                    Status = do_write_file_prealloc(fcb, ext, start + written, end_data, (uint8_t*)data + written, &write_len,
                                                    Irp, file_write, irp_offset + written, priority, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("do_write_file_prealloc returned %08lx\n", Status);
                        return Status;
                    }

                    extents_changed = true;

                    written += write_len;
                    length -= write_len;

                    if (length == 0)
                        break;
                }

                last_cow_start = ext->offset + len;
            }
        }

nextitem:
        le = le2;
    }

    if (length > 0) {
        uint64_t start_write = max(last_cow_start, start + written);

        extents_changed = true;

        Status = excise_extents(fcb->Vcb, fcb, start_write, end_data, Irp, rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("excise_extents returned %08lx\n", Status);
            return Status;
        }

        Status = insert_extent(fcb->Vcb, fcb, start_write, end_data - start_write, (uint8_t*)data + written, Irp, file_write, irp_offset + written, rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_extent returned %08lx\n", Status);
            return Status;
        }
    }

#ifdef DEBUG_PARANOID
    last_off = 0xffffffffffffffff;

    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if (!ext->ignore) {
            if (ext->offset == last_off) {
                ERR("offset %I64x duplicated\n", ext->offset);
                int3;
            } else if (ext->offset < last_off && last_off != 0xffffffffffffffff) {
                ERR("offsets out of order\n");
                int3;
            }

            last_off = ext->offset;
        }

        le = le->Flink;
    }
#endif

    if (extents_changed) {
        fcb->extents_changed = true;
        mark_fcb_dirty(fcb);
    }

    return STATUS_SUCCESS;
}
