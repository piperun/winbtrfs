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

// Free ranges stay reserved until their immutable discard requests complete.
#include "trim-state.h"

static bool trim_append_range(trim_batch* batch, LIST_ENTRY* head, uint64_t address, uint64_t size) {
    space* s;
    if (!size)
        return true;
    if (address > UINT64_MAX - size)
        return false;
    s = trim_batch_alloc(batch, sizeof(*s), PagedPool);
    if (!s)
        return false;
    s->address = address;
    s->size = size;
    InsertTailList(head, &s->list_entry);
    return true;
}

bool trim_add_range(trim_batch* batch, device* dev, uint64_t address, uint64_t length) {
    LIST_ENTRY* le;
    trim_target* target = NULL;
    if (!dev || !dev->devobj || !dev->trim || dev->readonly || !length)
        return true;
    if (address > INT64_MAX || length > INT64_MAX - address)
        return false;
    for (le = batch->targets.Flink; le != &batch->targets; le = le->Flink) {
        trim_target* t = CONTAINING_RECORD(le, trim_target, list_entry);
        if (t->owner == dev) {
            target = t;
            break;
        }
    }
    if (!target) {
        target = trim_batch_alloc(batch, sizeof(*target), NonPagedPool);
        if (!target)
            return false;
        InitializeListHead(&target->ranges);
        target->owner = dev;
        target->devobj = dev->devobj;
        target->fileobj = dev->fileobj;
        ObReferenceObject(target->devobj);
        if (target->fileobj)
            ObReferenceObject(target->fileobj);
        InsertTailList(&batch->targets, &target->list_entry);
    }
    if (target->count == (UINT32_MAX - 32) / sizeof(DEVICE_DATA_SET_RANGE))
        return false;
    if (!trim_append_range(batch, &target->ranges, address, length))
        return false;
    target->count++;
    batch->range_count++;
    return true;
}

bool trim_prepare_chunk(device_extension* Vcb, chunk* c, trim_batch* batch) {
    uint64_t before = batch->range_count;
    LIST_ENTRY* le;
    trim_chunk* entry;
    if (!trim_map_chunk(Vcb, c, batch))
        return false;
    if (before == batch->range_count)
        return true;
    entry = trim_batch_alloc(batch, sizeof(*entry), PagedPool);
    if (!entry)
        return false;
    entry->owner = c;
    InitializeListHead(&entry->ranges);
    InsertTailList(&batch->chunks, &entry->list_entry);
    for (le = c->deleting.Flink; le != &c->deleting; le = le->Flink) {
        if (!trim_charge(batch, sizeof(space)))
            return false;
    }
    return true;
}

bool trim_prepare_physical(trim_batch* batch, device* dev) {
    trim_physical* physical = trim_batch_alloc(batch, sizeof(*physical), PagedPool);
    LIST_ENTRY *le, *le2, *cursor;
    NTSTATUS Status;
    if (!physical)
        return false;
    physical->owner = dev;
    InitializeListHead(&physical->ranges);
    InitializeListHead(&physical->replacement);
    InsertTailList(&batch->physical, &physical->list_entry);

    // Only the current free intersections are eligible: a retired chunk's
    // physical address may already back a new chunk in this transaction.
    for (le = dev->space.Flink; le != &dev->space; le = le->Flink) {
        space* free = CONTAINING_RECORD(le, space, list_entry);
        if (free->address > UINT64_MAX - free->size)
            return false;
        for (le2 = dev->trim_list.Flink; le2 != &dev->trim_list; le2 = le2->Flink) {
            space* candidate = CONTAINING_RECORD(le2, space, list_entry);
            uint64_t start, end;
            if (candidate->address > UINT64_MAX - candidate->size)
                return false;
            start = max(free->address, candidate->address);
            end = min(free->address + free->size, candidate->address + candidate->size);
            if (start >= end)
                continue;
            // An insertion needs at most one new record. Conservative charges
            // include records removed by coalescing until this batch retires.
            if (!trim_charge(batch, sizeof(space)))
                return false;
            Status = space_list_add2(&physical->ranges, NULL, start, end - start, NULL, NULL);
            if (!NT_SUCCESS(Status))
                return false;
        }
    }
    if (IsListEmpty(&physical->ranges))
        return true;

    // Build the replacement map without mutating the allocator. No allocation
    // or fallible operation remains when all maps are published together.
    cursor = physical->ranges.Flink;
    for (le = dev->space.Flink; le != &dev->space; le = le->Flink) {
        space* free = CONTAINING_RECORD(le, space, list_entry);
        uint64_t start = free->address, end = start + free->size;
        while (cursor != &physical->ranges) {
            space* reserved = CONTAINING_RECORD(cursor, space, list_entry);
            if (reserved->address >= end)
                break;
            if (reserved->address > start) {
                if (!trim_append_range(batch, &physical->replacement, start, reserved->address - start))
                    return false;
                physical->replacement_bytes += sizeof(space);
            }
            start = reserved->address + reserved->size;
            cursor = cursor->Flink;
        }
        if (start < end) {
            if (!trim_append_range(batch, &physical->replacement, start, end - start))
                return false;
            physical->replacement_bytes += sizeof(space);
        }
    }
    for (le = physical->ranges.Flink; le != &physical->ranges; le = le->Flink) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        if (!trim_add_range(batch, dev, s->address, s->size))
            return false;
    }
    return true;
}

bool trim_prepare_requests(trim_batch* batch) {
    LIST_ENTRY* le;
    const ULONG offset = (ULONG)sector_align(sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES), sizeof(uint64_t));
    for (le = batch->targets.Flink; le != &batch->targets; le = le->Flink) {
        trim_target* target = CONTAINING_RECORD(le, trim_target, list_entry);
        DEVICE_DATA_SET_RANGE* ranges;
        PIO_STACK_LOCATION IrpSp;
        LIST_ENTRY* le2;
        ULONG i = 0;
        if (target->count > (UINT32_MAX - offset) / sizeof(*ranges))
            return false;
        target->length = offset + target->count * sizeof(*ranges);
        target->data = trim_batch_alloc(batch, target->length, NonPagedPool);
        if (!target->data)
            return false;
        target->data->Size = sizeof(*target->data);
        target->data->Action = DeviceDsmAction_Trim;
        target->data->Flags = DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED;
        target->data->DataSetRangesOffset = offset;
        target->data->DataSetRangesLength = target->count * sizeof(*ranges);
        ranges = (DEVICE_DATA_SET_RANGE*)((uint8_t*)target->data + offset);
        for (le2 = target->ranges.Flink; le2 != &target->ranges; le2 = le2->Flink) {
            space* s = CONTAINING_RECORD(le2, space, list_entry);
            ranges[i].StartingOffset = s->address;
            ranges[i++].LengthInBytes = s->size;
        }
        if (!trim_charge(batch, IoSizeOfIrp(target->devobj->StackSize)))
            return false;
        target->Irp = IoAllocateIrp(target->devobj->StackSize, false);
        if (!target->Irp)
            return false;
        IrpSp = IoGetNextIrpStackLocation(target->Irp);
        IrpSp->MajorFunction = IRP_MJ_DEVICE_CONTROL;
        IrpSp->FileObject = target->fileobj;
        IrpSp->Parameters.DeviceIoControl.IoControlCode = IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES;
        IrpSp->Parameters.DeviceIoControl.InputBufferLength = target->length;
        IrpSp->Parameters.DeviceIoControl.OutputBufferLength = 0;
        target->Irp->AssociatedIrp.SystemBuffer = target->data;
        target->Irp->Flags |= IRP_BUFFERED_IO;
        target->Irp->UserBuffer = NULL;
        target->Irp->UserIosb = &target->iosb;
    }
    return true;
}

bool trim_publish_batch(trim_batch* batch) {
    LIST_ENTRY* le;
    // The caller holds chunk_lock exclusively to exclude balance selection
    // between this validation and publishing any reservation.
    for (le = batch->chunks.Flink; le != &batch->chunks; le = le->Flink) {
        trim_chunk* entry = CONTAINING_RECORD(le, trim_chunk, list_entry);
        if (entry->owner->reloc || entry->owner->list_entry_balance.Flink)
            return false;
    }
    for (le = batch->chunks.Flink; le != &batch->chunks; le = le->Flink) {
        trim_chunk* entry = CONTAINING_RECORD(le, trim_chunk, list_entry);
        trim_move_ranges(&entry->ranges, &entry->owner->deleting);
        InsertTailList(&entry->owner->pending_trims, &entry->chunk_entry);
    }
    for (le = batch->physical.Flink; le != &batch->physical; le = le->Flink) {
        trim_physical* physical = CONTAINING_RECORD(le, trim_physical, list_entry);
        if (!IsListEmpty(&physical->ranges)) {
            trim_free_ranges(&physical->owner->space);
            trim_move_ranges(&physical->owner->space, &physical->replacement);
            batch->bytes -= physical->replacement_bytes;
            batch->state->bytes -= physical->replacement_bytes;
        }
    }
    batch->published = true;
    return true;
}

bool trim_chunk_pending(const chunk* c) {
    return !IsListEmpty(&c->pending_trims);
}

NTSTATUS trim_merge_pending(chunk* c, LIST_ENTRY* list, LIST_ENTRY* sizes) {
    LIST_ENTRY* le;
    NTSTATUS Status;
    for (le = c->pending_trims.Flink; le != &c->pending_trims; le = le->Flink) {
        trim_chunk* entry = CONTAINING_RECORD(le, trim_chunk, chunk_entry);
        Status = space_list_merge(list, sizes, &entry->ranges);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    return STATUS_SUCCESS;
}

uint64_t trim_pending_count(chunk* c) {
    LIST_ENTRY *le, *le2;
    uint64_t count = 0;
    for (le = c->pending_trims.Flink; le != &c->pending_trims; le = le->Flink) {
        trim_chunk* entry = CONTAINING_RECORD(le, trim_chunk, chunk_entry);
        for (le2 = entry->ranges.Flink; le2 != &entry->ranges; le2 = le2->Flink)
            count++;
    }
    return count;
}

static bool trim_reclaim_ranges(LIST_ENTRY* dest, LIST_ENTRY* sizes, LIST_ENTRY* ranges) {
    while (!IsListEmpty(ranges)) {
        space* s = CONTAINING_RECORD(ranges->Flink, space, list_entry);
        if (!NT_SUCCESS(space_list_add2(dest, sizes, s->address, s->size, NULL, NULL)))
            return false;
        // Remove each published range immediately. A later allocation failure
        // must not leave already reusable addresses in the pending cache view.
        RemoveEntryList(&s->list_entry);
        ExFreePool(s);
    }
    return true;
}

bool trim_reclaim_batch(device_extension* Vcb, trim_batch* batch) {
    LIST_ENTRY* le;
    for (le = batch->chunks.Flink; le != &batch->chunks; le = le->Flink) {
        trim_chunk* entry = CONTAINING_RECORD(le, trim_chunk, list_entry);
        bool done;
        acquire_chunk_lock(entry->owner, Vcb);
        done = trim_reclaim_ranges(&entry->owner->space, &entry->owner->space_size, &entry->ranges);
        release_chunk_lock(entry->owner, Vcb);
        if (!done)
            return false;
    }
    for (le = batch->physical.Flink; le != &batch->physical; le = le->Flink) {
        trim_physical* physical = CONTAINING_RECORD(le, trim_physical, list_entry);
        if (!physical->owner)
            trim_free_ranges(&physical->ranges);
        else if (!trim_reclaim_ranges(&physical->owner->space, NULL, &physical->ranges))
            return false;
    }
    return true;
}

void trim_release_batch(trim_batch* batch) {
    while (!IsListEmpty(&batch->targets)) {
        trim_target* target = CONTAINING_RECORD(RemoveHeadList(&batch->targets), trim_target, list_entry);
        if (target->Irp)
            IoFreeIrp(target->Irp);
        if (target->data)
            ExFreePool(target->data);
        trim_free_ranges(&target->ranges);
        if (target->fileobj)
            ObDereferenceObject(target->fileobj);
        ObDereferenceObject(target->devobj);
        ExFreePool(target);
    }
    while (!IsListEmpty(&batch->chunks)) {
        trim_chunk* entry = CONTAINING_RECORD(RemoveHeadList(&batch->chunks), trim_chunk, list_entry);
        if (batch->published)
            RemoveEntryList(&entry->chunk_entry);
        trim_free_ranges(&entry->ranges);
        ExFreePool(entry);
    }
    while (!IsListEmpty(&batch->physical)) {
        trim_physical* physical = CONTAINING_RECORD(RemoveHeadList(&batch->physical), trim_physical, list_entry);
        trim_free_ranges(&physical->ranges);
        trim_free_ranges(&physical->replacement);
        ExFreePool(physical);
    }
    batch->state->bytes -= batch->bytes;
    ExFreePool(batch);
}
