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
    extent** exts; // the extents using this range, in list order; not updated when ranges merge
    size_t num_exts;
} extent_range;

static bool can_trim_or_merge(extent* ext) {
    return (ext->extent_data.type == BTRFS_FILE_EXTENT_REG || ext->extent_data.type == BTRFS_FILE_EXTENT_PREALLOC) &&
           ext->extent_data.compression == BTRFS_COMPRESS_NONE && ext->unique && ext->extent_data.disk_num_bytes != 0;
}

// bottom-up merge sort by disk address, which keeps extents at the same address in list order;
// returns whichever of the two arrays holds the result
static extent** sort_extents(extent** exts, extent** tmp, size_t num) {
    size_t width;

    for (width = 1; width < num; width *= 2) {
        size_t start;
        extent** swap;

        for (start = 0; start < num; start += 2 * width) {
            size_t mid = min(start + width, num), end = min(start + 2 * width, num);
            size_t i = start, j = mid, k = start;

            while (i < mid && j < end) {
                if (exts[j]->extent_data.disk_bytenr < exts[i]->extent_data.disk_bytenr)
                    tmp[k++] = exts[j++];
                else
                    tmp[k++] = exts[i++];
            }

            while (i < mid)
                tmp[k++] = exts[i++];

            while (j < end)
                tmp[k++] = exts[j++];
        }

        swap = exts;
        exts = tmp;
        tmp = swap;
    }

    return exts;
}

// the last range that starts at or before address, or NULL if there is none
static extent_range* find_extent_range(extent_range* ranges, size_t num_ranges, uint64_t address) {
    size_t lo = 0, hi = num_ranges;

    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);

        if (ranges[mid].address <= address)
            lo = mid + 1;
        else
            hi = mid;
    }

    return lo > 0 ? &ranges[lo - 1] : NULL;
}

NTSTATUS rationalize_extents(fcb* fcb, PIRP Irp) {
    LIST_ENTRY* le;
    extent** exts;
    extent** sorted;
    extent_range* ranges = NULL;
    extent_range* er;
    bool changed = false, truncating = false;
    size_t num_exts = 0, num_ranges = 0, i, j;
    NTSTATUS Status;

    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if (can_trim_or_merge(ext))
            num_exts++;

        le = le->Flink;
    }

    if (num_exts == 0)
        return STATUS_SUCCESS;

    // the second half of the array is the merge sort's buffer
    exts = ExAllocatePoolWithTag(PagedPool, sizeof(extent*) * num_exts * 2, ALLOC_TAG);
    if (!exts) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    i = 0;
    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if (can_trim_or_merge(ext))
            exts[i++] = ext;

        le = le->Flink;
    }

    sorted = sort_extents(exts, &exts[num_exts], num_exts);

    for (i = 0; i < num_exts; i++) {
        if (i == 0 || sorted[i]->extent_data.disk_bytenr != sorted[i - 1]->extent_data.disk_bytenr)
            num_ranges++;
    }

    ranges = ExAllocatePoolWithTag(PagedPool, sizeof(extent_range) * num_ranges, ALLOC_TAG);
    if (!ranges) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    // one range per disk extent, with its length and file offset taken from the first extent in the list that uses it
    er = ranges;
    for (i = 0; i < num_exts; i++) {
        extent* ext = sorted[i];

        if (i > 0 && ext->extent_data.disk_bytenr == sorted[i - 1]->extent_data.disk_bytenr) {
            er->skip_start = min(er->skip_start, ext->extent_data.offset);
            er->skip_end = min(er->skip_end, ext->extent_data.disk_num_bytes - ext->extent_data.offset - ext->extent_data.num_bytes);
            er->num_exts++;
            continue;
        }

        if (i > 0)
            er++;

        er->address = ext->extent_data.disk_bytenr;
        er->length = ext->extent_data.disk_num_bytes;
        er->offset = ext->offset - ext->extent_data.offset;
        er->changed = false;
        er->skip_start = ext->extent_data.offset;
        er->skip_end = ext->extent_data.disk_num_bytes - ext->extent_data.offset - ext->extent_data.num_bytes;
        er->exts = &sorted[i];
        er->num_exts = 1;

        if (er->skip_start != 0 || er->skip_end != 0)
            truncating = true;
    }

    if (num_ranges == 1 && !truncating) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    for (i = 0; i < num_ranges; i++) {
        er = &ranges[i];

        // the ranges are sorted by address, so a chunk's ranges are consecutive
        if (i > 0 && er->address < ranges[i - 1].chunk->offset + ranges[i - 1].chunk->chunk_item->length) {
            er->chunk = ranges[i - 1].chunk;
            continue;
        }

        er->chunk = get_chunk_from_address(fcb->Vcb, er->address);

        if (!er->chunk) {
            ERR("get_chunk_from_address(%I64x) failed\n", er->address);
            Status = STATUS_INTERNAL_ERROR;
            goto end;
        }
    }

    if (truncating) {
        // truncate beginning or end of extent if unused

        for (i = 0; i < num_ranges; i++) {
            er = &ranges[i];

            if (er->skip_start > 0) {
                for (j = 0; j < er->num_exts; j++) {
                    extent* ext = er->exts[j];

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
                for (j = 0; j < er->num_exts; j++) {
                    extent* ext = er->exts[j];

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
        }
    }

    if (num_ranges < 2) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    // merge together adjacent extents, moving the remaining ranges down over the merged ones
    er = ranges;
    for (i = 1; i < num_ranges; i++) {
        extent_range* er2 = &ranges[i];

        if (er->length < MAX_EXTENT_SIZE && er->chunk == er2->chunk) {
            if (er2->address == er->address + er->length && er2->offset >= er->offset + er->length) {
                if (er->length + er2->length <= MAX_EXTENT_SIZE) {
                    er->length += er2->length;
                    er->changed = true;

                    changed = true;
                    continue;
                }
            }
        }

        er++;

        if (er != er2)
            *er = *er2;
    }

    num_ranges = (size_t)(er - ranges) + 1;

    if (!changed) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);

        if (can_trim_or_merge(ext)) {
            extent_range* er2 = find_extent_range(ranges, num_ranges, ext->extent_data.disk_bytenr);

            if (er2 && er2->changed && ext->extent_data.disk_bytenr + ext->extent_data.disk_num_bytes <= er2->address + er2->length) {
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
            }
        }

        le = le->Flink;
    }

    Status = STATUS_SUCCESS;

end:
    if (ranges)
        ExFreePool(ranges);

    ExFreePool(exts);

    return Status;
}
