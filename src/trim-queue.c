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

// Bounded automatic discard ownership; only the flush worker submits requests.
#include "trim-state.h"

bool trim_charge(trim_batch* batch, size_t size) {
    if (size > TRIM_QUEUE_LIMIT - batch->state->bytes)
        return false;
    batch->bytes += size;
    batch->state->bytes += size;
    return true;
}

void* trim_batch_alloc(trim_batch* batch, size_t size, POOL_TYPE pool) {
    void* p;
    if (!trim_charge(batch, size))
        return NULL;
    p = ExAllocatePoolWithTag(pool, size, ALLOC_TAG);
    if (!p) {
        batch->bytes -= size;
        batch->state->bytes -= size;
    } else
        RtlZeroMemory(p, size);
    return p;
}

void trim_free_ranges(LIST_ENTRY* head) {
    while (!IsListEmpty(head))
        ExFreePool(CONTAINING_RECORD(RemoveHeadList(head), space, list_entry));
}

void trim_move_ranges(LIST_ENTRY* dest, LIST_ENTRY* src) {
    if (IsListEmpty(src)) {
        InitializeListHead(dest);
        return;
    }
    *dest = *src;
    dest->Flink->Blink = dest;
    dest->Blink->Flink = dest;
    InitializeListHead(src);
}

void trim_initialize(device_extension* Vcb) {
    trim_state* state = ExAllocatePoolWithTag(NonPagedPool, sizeof(*state), ALLOC_TAG);
    if (!state)
        return;
    RtlZeroMemory(state, sizeof(*state));
    InitializeListHead(&state->batches);
    KeInitializeEvent(&state->event, NotificationEvent, false);
    KeInitializeSpinLock(&state->completion_lock);
    KeInitializeEvent(&state->idle, NotificationEvent, true);
    state->bytes = sizeof(*state);
    Vcb->trim_state = state;
}

KEVENT* trim_event(device_extension* Vcb) {
    return Vcb->trim_state ? &Vcb->trim_state->event : NULL;
}

static void trim_clear_candidates(device_extension* Vcb) {
    LIST_ENTRY* le;
    for (le = Vcb->devices.Flink; le != &Vcb->devices; le = le->Flink) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);
        trim_free_ranges(&dev->trim_list);
        dev->num_trim_entries = 0;
    }
}

void trim_commit(device_extension* Vcb) {
    trim_state* state = Vcb->trim_state;
    trim_batch* batch = NULL;
    LIST_ENTRY* le;
    bool ready = true;
    bool chunks_locked = false;

    if (!state || state->stopping || state->pauses || Vcb->removing || !Vcb->trim || Vcb->options.no_trim)
        goto done;
    if (sizeof(*batch) > TRIM_QUEUE_LIMIT - state->bytes)
        goto done;
    batch = ExAllocatePoolWithTag(NonPagedPool, sizeof(*batch), ALLOC_TAG);
    if (!batch)
        goto done;
    RtlZeroMemory(batch, sizeof(*batch));
    batch->state = state;
    batch->bytes = sizeof(*batch);
    state->bytes += sizeof(*batch);
    InitializeListHead(&batch->chunks);
    InitializeListHead(&batch->targets);
    InitializeListHead(&batch->physical);

    // Balance allocates physical space without tree_lock. Keep its allocator
    // excluded from the first snapshot through publication of every map.
    ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);
    chunks_locked = true;
    for (le = Vcb->chunks.Flink; le != &Vcb->chunks; le = le->Flink) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);
        acquire_chunk_lock(c, Vcb);
        if (c->space_changed && !c->reloc && !c->list_entry_balance.Flink && !IsListEmpty(&c->deleting))
            ready = trim_prepare_chunk(Vcb, c, batch);
        release_chunk_lock(c, Vcb);
        if (!ready)
            break;
    }
    if (!ready)
        goto done;

    for (le = Vcb->devices.Flink; le != &Vcb->devices; le = le->Flink) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);
        if (!IsListEmpty(&dev->trim_list) && dev->devobj && dev->trim && !dev->readonly &&
            !trim_prepare_physical(batch, dev))
            goto done;
    }
    if (IsListEmpty(&batch->targets) || !trim_prepare_requests(batch))
        goto done;

    ready = trim_publish_batch(batch);
    if (!ready)
        goto done;
    InsertTailList(&state->batches, &batch->list_entry);
    batch = NULL;
    KeSetEvent(&state->event, 0, false);
done:
    if (chunks_locked)
        ExReleaseResourceLite(&Vcb->chunk_lock);
    if (batch)
        trim_release_batch(batch);
    trim_clear_candidates(Vcb);
}

_Function_class_(IO_COMPLETION_ROUTINE)
static NTSTATUS __stdcall trim_batch_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID context) {
    trim_batch* batch = context;
    trim_state* state = batch->state;
    KIRQL irql;
    UNUSED(DeviceObject);
    UNUSED(Irp);
    // A poller cannot observe final completion until all callback accesses to
    // the context and its wake event have finished.
    KeAcquireSpinLock(&state->completion_lock, &irql);
    if (InterlockedDecrement(&batch->left) == 0) {
        KeSetEvent(&state->idle, 0, false);
        KeSetEvent(&state->event, 0, false);
    }
    KeReleaseSpinLock(&state->completion_lock, irql);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static bool trim_batch_finished(trim_batch* batch) {
    KIRQL irql;
    bool done;
    KeAcquireSpinLock(&batch->state->completion_lock, &irql);
    done = batch->left == 0;
    KeReleaseSpinLock(&batch->state->completion_lock, irql);
    return done;
}

static void trim_submit_batch(trim_batch* batch) {
    LIST_ENTRY* le;
    LONG count = 1; // The submission guard also covers inline final completion.
    for (le = batch->targets.Flink; le != &batch->targets; le = le->Flink)
        count++;
    batch->left = count;
    for (le = batch->targets.Flink; le != &batch->targets; le = le->Flink) {
        trim_target* target = CONTAINING_RECORD(le, trim_target, list_entry);
        if (!target->owner) {
            trim_batch_completion(NULL, NULL, batch);
            continue;
        }
        IoSetCompletionRoutine(target->Irp, trim_batch_completion, batch, true, true, true);
        IoCallDriver(target->devobj, target->Irp);
    }
    trim_batch_completion(NULL, NULL, batch);
}

void trim_process(device_extension* Vcb) {
    trim_state* state = Vcb->trim_state;
    trim_batch* submit = NULL;
    if (!state)
        return;
    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
    KeClearEvent(&state->event);
    if (!IsListEmpty(&state->batches)) {
        trim_batch* batch = CONTAINING_RECORD(state->batches.Flink, trim_batch, list_entry);
        if (batch->submitted && trim_batch_finished(batch)) {
            ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);
            if (trim_reclaim_batch(Vcb, batch)) {
                RemoveEntryList(&batch->list_entry);
                trim_release_batch(batch);
            }
            ExReleaseResourceLite(&Vcb->chunk_lock);
        }
        if (!IsListEmpty(&state->batches) && !state->stopping && !state->pauses && !Vcb->removing) {
            batch = CONTAINING_RECORD(state->batches.Flink, trim_batch, list_entry);
            if (!batch->submitted) {
                batch->submitted = true;
                batch->left = 1;
                KeClearEvent(&state->idle);
                submit = batch;
            }
        }
    }
    ExReleaseResourceLite(&Vcb->tree_lock);
    if (submit)
        trim_submit_batch(submit);
}

void trim_pause_locked(device_extension* Vcb) {
    LIST_ENTRY* le;
    trim_state* state = Vcb->trim_state;
    if (!state)
        return;
    InterlockedIncrement(&state->pauses);
    // Queued addresses must never be discarded after a raw volume writer has
    // acquired the lock. Keep their free-range records solely for reclamation.
    for (le = state->batches.Flink; le != &state->batches; le = le->Flink) {
        trim_batch* batch = CONTAINING_RECORD(le, trim_batch, list_entry);
        if (!batch->submitted) {
            batch->submitted = true;
            batch->left = 0;
        }
    }
    KeSetEvent(&state->event, 0, false);
}

void trim_wait_idle(device_extension* Vcb) {
    if (Vcb->trim_state)
        KeWaitForSingleObject(&Vcb->trim_state->idle, Executive, KernelMode, false, NULL);
}

void trim_resume(device_extension* Vcb) {
    trim_state* state = Vcb->trim_state;
    if (!state)
        return;
    // Handle cleanup may already own tree_lock shared; never upgrade it here.
    InterlockedDecrement(&state->pauses);
    KeSetEvent(&state->event, 0, false);
}

NTSTATUS trim_quiesce(device_extension* Vcb) {
    trim_state* state = Vcb->trim_state;
    NTSTATUS Status = STATUS_SUCCESS;
    if (!state)
        return Status;
    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
    trim_pause_locked(Vcb);
    ExReleaseResourceLite(&Vcb->tree_lock);
    trim_wait_idle(Vcb);

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
    ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);
    while (!IsListEmpty(&state->batches)) {
        trim_batch* batch = CONTAINING_RECORD(state->batches.Flink, trim_batch, list_entry);
        if (!trim_reclaim_batch(Vcb, batch)) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RemoveEntryList(&batch->list_entry);
        trim_release_batch(batch);
    }
    ExReleaseResourceLite(&Vcb->chunk_lock);
    ExReleaseResourceLite(&Vcb->tree_lock);
    return Status;
}

void trim_forget_device(device_extension* Vcb, device* dev) {
    LIST_ENTRY *le, *le2;
    if (!Vcb->trim_state)
        return;
    for (le = Vcb->trim_state->batches.Flink; le != &Vcb->trim_state->batches; le = le->Flink) {
        trim_batch* batch = CONTAINING_RECORD(le, trim_batch, list_entry);
        for (le2 = batch->targets.Flink; le2 != &batch->targets; le2 = le2->Flink) {
            trim_target* target = CONTAINING_RECORD(le2, trim_target, list_entry);
            if (target->owner == dev)
                target->owner = NULL;
        }
        for (le2 = batch->physical.Flink; le2 != &batch->physical; le2 = le2->Flink) {
            trim_physical* physical = CONTAINING_RECORD(le2, trim_physical, list_entry);
            if (physical->owner == dev)
                physical->owner = NULL;
        }
    }
}

NTSTATUS trim_subtract_device_pending(device_extension* Vcb, device* dev, LIST_ENTRY* spaces) {
    LIST_ENTRY *le, *le2, *le3;
    NTSTATUS Status;
    if (!Vcb->trim_state)
        return STATUS_SUCCESS;
    for (le = Vcb->trim_state->batches.Flink; le != &Vcb->trim_state->batches; le = le->Flink) {
        trim_batch* batch = CONTAINING_RECORD(le, trim_batch, list_entry);
        for (le2 = batch->physical.Flink; le2 != &batch->physical; le2 = le2->Flink) {
            trim_physical* physical = CONTAINING_RECORD(le2, trim_physical, list_entry);
            if (physical->owner != dev)
                continue;
            for (le3 = physical->ranges.Flink; le3 != &physical->ranges; le3 = le3->Flink) {
                space* s = CONTAINING_RECORD(le3, space, list_entry);
                Status = space_list_subtract2(spaces, NULL, s->address, s->size, NULL, NULL);
                if (!NT_SUCCESS(Status))
                    return Status;
            }
        }
    }
    return STATUS_SUCCESS;
}

void trim_drain(device_extension* Vcb) {
    trim_state* state = Vcb->trim_state;
    if (!state)
        return;
    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
    state->stopping = true;
    while (!IsListEmpty(&state->batches)) {
        trim_batch* batch;
        KeClearEvent(&state->event);
        batch = CONTAINING_RECORD(state->batches.Flink, trim_batch, list_entry);
        if (batch->submitted && !trim_batch_finished(batch)) {
            ExReleaseResourceLite(&Vcb->tree_lock);
            // Another drainer or the worker can consume the work event before
            // this wait starts. Once stopping, idle stays set after the active
            // batch completes because no new request can clear it.
            KeWaitForSingleObject(&state->idle, Executive, KernelMode, false, NULL);
            ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
            continue;
        }
        RemoveEntryList(&batch->list_entry);
        ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);
        trim_release_batch(batch);
        ExReleaseResourceLite(&Vcb->chunk_lock);
    }
    trim_clear_candidates(Vcb);
    ExReleaseResourceLite(&Vcb->tree_lock);
}

void trim_destroy(device_extension* Vcb) {
    if (!Vcb->trim_state)
        return;
    trim_drain(Vcb);
    ExFreePool(Vcb->trim_state);
    Vcb->trim_state = NULL;
}
