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

#include "extent-ranges.h"

typedef struct {
    uint64_t address;
    uint64_t length;
    uint64_t offset;
    bool changed;
    chunk* chunk;
    uint64_t skip_start;
    uint64_t skip_end;
    LIST_ENTRY list_entry;
} extent_range;

NTSTATUS rationalize_extents(fcb* fcb, PIRP Irp) {
    LIST_ENTRY* le;
    LIST_ENTRY extent_ranges;
    extent_range* er;
    bool changed = false, truncating = false;
    uint32_t num_extents = 0;
    NTSTATUS Status;

    InitializeListHead(&extent_ranges);

    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if ((ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC) && ext->extent_data.compression == BTRFS_COMPRESS_NONE && ext->unique) {
            if (ext->extent_data.disk_num_bytes != 0) {
                LIST_ENTRY* le2;

                le2 = extent_ranges.Flink;
                while (le2 != &extent_ranges) {
                    extent_range* er2 = CONTAINING_RECORD(le2, extent_range, list_entry);

                    if (er2->address == ext->extent_data.disk_bytenr) {
                        er2->skip_start = min(er2->skip_start, ext->extent_data.offset);
                        er2->skip_end = min(er2->skip_end, ext->extent_data.disk_num_bytes - ext->extent_data.offset - ext->extent_data.num_bytes);
                        goto cont;
                    } else if (er2->address > ext->extent_data.disk_bytenr)
                        break;

                    le2 = le2->Flink;
                }

                er = ExAllocatePoolWithTag(PagedPool, sizeof(extent_range), ALLOC_TAG); // FIXME - should be from lookaside?
                if (!er) {
                    ERR("out of memory\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end;
                }

                er->address = ext->extent_data.disk_bytenr;
                er->length = ext->extent_data.disk_num_bytes;
                er->offset = ext->offset - ext->extent_data.offset;
                er->changed = false;
                er->chunk = NULL;
                er->skip_start = ext->extent_data.offset;
                er->skip_end = ext->extent_data.disk_num_bytes - ext->extent_data.offset - ext->extent_data.num_bytes;

                if (er->skip_start != 0 || er->skip_end != 0)
                    truncating = true;

                InsertHeadList(le2->Blink, &er->list_entry);
                num_extents++;
            }
        }

cont:
        le = le->Flink;
    }

    if (num_extents == 0 || (num_extents == 1 && !truncating)) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    le = extent_ranges.Flink;
    while (le != &extent_ranges) {
        er = CONTAINING_RECORD(le, extent_range, list_entry);

        if (!er->chunk) {
            LIST_ENTRY* le2;

            er->chunk = get_chunk_from_address(fcb->Vcb, er->address);

            if (!er->chunk) {
                ERR("get_chunk_from_address(%I64x) failed\n", er->address);
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }

            le2 = le->Flink;
            while (le2 != &extent_ranges) {
                extent_range* er2 = CONTAINING_RECORD(le2, extent_range, list_entry);

                if (!er2->chunk && er2->address >= er->chunk->offset && er2->address < er->chunk->offset + er->chunk->chunk_item->length)
                    er2->chunk = er->chunk;

                le2 = le2->Flink;
            }
        }

        le = le->Flink;
    }

    if (truncating) {
        // truncate beginning or end of extent if unused

        le = extent_ranges.Flink;
        while (le != &extent_ranges) {
            er = CONTAINING_RECORD(le, extent_range, list_entry);

            if (er->skip_start > 0) {
                LIST_ENTRY* le2 = fcb->extents.Flink;
                while (le2 != &fcb->extents) {
                    extent* ext = CONTAINING_RECORD(le2, extent, list_entry);

                    if ((ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC) && ext->extent_data.compression == BTRFS_COMPRESS_NONE && ext->unique) {
                        if (ext->extent_data.disk_num_bytes != 0 && ext->extent_data.disk_bytenr == er->address) {
                            Status = update_changed_extent_ref(fcb->Vcb, er->chunk, ext->extent_data.disk_bytenr,
                                                               ext->extent_data.disk_num_bytes, fcb->subvol->id,
                                                               fcb->inode, ext->offset - ext->extent_data.offset,
                                                               -1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                                               true, Irp, NULL);
                            if (!NT_SUCCESS(Status)) {
                                ERR("update_changed_extent_ref returned %08lx\n", Status);
                                goto end;
                            }

                            ext->extent_data.ram_bytes -= er->skip_start;
                            ext->extent_data.disk_num_bytes -= er->skip_start;
                            ext->extent_data.disk_bytenr += er->skip_start;
                            ext->extent_data.offset -= er->skip_start;

                            Status = add_changed_extent_ref(er->chunk, ext->extent_data.disk_bytenr,
                                                            ext->extent_data.disk_num_bytes,
                                                            fcb->subvol->id, fcb->inode,
                                                            ext->offset - ext->extent_data.offset,
                                                            1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                                            NULL);
                            if (!NT_SUCCESS(Status))
                                goto end;
                        }
                    }

                    le2 = le2->Flink;
                }

                if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
                    Status = add_checksum_entry(fcb->Vcb, er->address,
                                                (ULONG)(er->skip_start >> fcb->Vcb->sector_shift),
                                                NULL, NULL);
                    if (!NT_SUCCESS(Status))
                        goto end;
                }

                acquire_chunk_lock(er->chunk, fcb->Vcb);

                if (!er->chunk->cache_loaded) {
                    Status = load_cache_chunk(fcb->Vcb, er->chunk, NULL);

                    if (!NT_SUCCESS(Status)) {
                        ERR("load_cache_chunk returned %08lx\n", Status);
                        release_chunk_lock(er->chunk, fcb->Vcb);
                        goto end;
                    }
                }

                er->chunk->used -= er->skip_start;

                Status = space_list_add(er->chunk, er->address, er->skip_start,
                                        NULL);
                if (!NT_SUCCESS(Status)) {
                    release_chunk_lock(er->chunk, fcb->Vcb);
                    goto end;
                }

                release_chunk_lock(er->chunk, fcb->Vcb);

                er->address += er->skip_start;
                er->length -= er->skip_start;
            }

            if (er->skip_end > 0) {
                LIST_ENTRY* le2 = fcb->extents.Flink;
                while (le2 != &fcb->extents) {
                    extent* ext = CONTAINING_RECORD(le2, extent, list_entry);

                    if ((ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC) && ext->extent_data.compression == BTRFS_COMPRESS_NONE && ext->unique) {
                        if (ext->extent_data.disk_num_bytes != 0 && ext->extent_data.disk_bytenr == er->address) {
                            Status = update_changed_extent_ref(fcb->Vcb, er->chunk, ext->extent_data.disk_bytenr,
                                                               ext->extent_data.disk_num_bytes, fcb->subvol->id,
                                                               fcb->inode, ext->offset - ext->extent_data.offset,
                                                               -1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                                               true, Irp, NULL);
                            if (!NT_SUCCESS(Status)) {
                                ERR("update_changed_extent_ref returned %08lx\n", Status);
                                goto end;
                            }

                            ext->extent_data.ram_bytes -= er->skip_end;
                            ext->extent_data.disk_num_bytes -= er->skip_end;

                            Status = add_changed_extent_ref(er->chunk, ext->extent_data.disk_bytenr,
                                                            ext->extent_data.disk_num_bytes,
                                                            fcb->subvol->id, fcb->inode,
                                                            ext->offset - ext->extent_data.offset,
                                                            1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                                            NULL);
                            if (!NT_SUCCESS(Status))
                                goto end;
                        }
                    }

                    le2 = le2->Flink;
                }

                if (!(fcb->inode_item.flags & BTRFS_INODE_NODATASUM)) {
                    Status = add_checksum_entry(fcb->Vcb, er->address + er->length - er->skip_end,
                                                (ULONG)(er->skip_end >> fcb->Vcb->sector_shift),
                                                NULL, NULL);
                    if (!NT_SUCCESS(Status))
                        goto end;
                }

                acquire_chunk_lock(er->chunk, fcb->Vcb);

                if (!er->chunk->cache_loaded) {
                    Status = load_cache_chunk(fcb->Vcb, er->chunk, NULL);

                    if (!NT_SUCCESS(Status)) {
                        ERR("load_cache_chunk returned %08lx\n", Status);
                        release_chunk_lock(er->chunk, fcb->Vcb);
                        goto end;
                    }
                }

                er->chunk->used -= er->skip_end;

                Status = space_list_add(er->chunk, er->address + er->length - er->skip_end,
                                        er->skip_end, NULL);
                if (!NT_SUCCESS(Status)) {
                    release_chunk_lock(er->chunk, fcb->Vcb);
                    goto end;
                }

                release_chunk_lock(er->chunk, fcb->Vcb);

                er->length -= er->skip_end;
            }

            le = le->Flink;
        }
    }

    if (num_extents < 2) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    // merge together adjacent extents
    le = extent_ranges.Flink;
    while (le != &extent_ranges) {
        er = CONTAINING_RECORD(le, extent_range, list_entry);

        if (le->Flink != &extent_ranges && er->length < MAX_EXTENT_SIZE) {
            extent_range* er2 = CONTAINING_RECORD(le->Flink, extent_range, list_entry);

            if (er->chunk == er2->chunk) {
                if (er2->address == er->address + er->length && er2->offset >= er->offset + er->length) {
                    if (er->length + er2->length <= MAX_EXTENT_SIZE) {
                        er->length += er2->length;
                        er->changed = true;

                        RemoveEntryList(&er2->list_entry);
                        ExFreePool(er2);

                        changed = true;
                        continue;
                    }
                }
            }
        }

        le = le->Flink;
    }

    if (!changed) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if ((ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC) && ext->extent_data.compression == BTRFS_COMPRESS_NONE && ext->unique) {
            if (ext->extent_data.disk_num_bytes != 0) {
                LIST_ENTRY* le2;

                le2 = extent_ranges.Flink;
                while (le2 != &extent_ranges) {
                    extent_range* er2 = CONTAINING_RECORD(le2, extent_range, list_entry);

                    if (ext->extent_data.disk_bytenr >= er2->address && ext->extent_data.disk_bytenr + ext->extent_data.disk_num_bytes <= er2->address + er2->length && er2->changed) {
                        NTSTATUS Status;

                        Status = update_changed_extent_ref(fcb->Vcb, er2->chunk, ext->extent_data.disk_bytenr,
                                                           ext->extent_data.disk_num_bytes, fcb->subvol->id, fcb->inode,
                                                           ext->offset - ext->extent_data.offset, -1,
                                                           fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                                           true, Irp, NULL);
                        if (!NT_SUCCESS(Status)) {
                            ERR("update_changed_extent_ref returned %08lx\n", Status);
                            goto end;
                        }

                        ext->extent_data.offset += ext->extent_data.disk_bytenr - er2->address;
                        ext->extent_data.disk_bytenr = er2->address;
                        ext->extent_data.disk_num_bytes = er2->length;
                        ext->extent_data.ram_bytes = ext->extent_data.disk_num_bytes;

                        Status = add_changed_extent_ref(er2->chunk, ext->extent_data.disk_bytenr,
                                                        ext->extent_data.disk_num_bytes,
                                                        fcb->subvol->id, fcb->inode,
                                                        ext->offset - ext->extent_data.offset,
                                                        1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM,
                                                        NULL);
                        if (!NT_SUCCESS(Status))
                            goto end;

                        break;
                    }

                    le2 = le2->Flink;
                }
            }
        }

        le = le->Flink;
    }

    Status = STATUS_SUCCESS;

end:
    while (!IsListEmpty(&extent_ranges)) {
        le = RemoveHeadList(&extent_ranges);
        er = CONTAINING_RECORD(le, extent_range, list_entry);

        ExFreePool(er);
    }

    return Status;
}
