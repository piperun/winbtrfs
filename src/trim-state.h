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

#pragma once
#include "trim.h"
#include <ntddstor.h>

#define TRIM_QUEUE_LIMIT (8 * 1024 * 1024)

typedef struct trim_batch trim_batch;
typedef struct {
    LIST_ENTRY list_entry;
    LIST_ENTRY ranges;
    PDEVICE_OBJECT devobj;
    PFILE_OBJECT fileobj;
    device* owner;
    DEVICE_MANAGE_DATA_SET_ATTRIBUTES* data;
    PIRP Irp;
    IO_STATUS_BLOCK iosb;
    ULONG count;
    ULONG length;
} trim_target;

typedef struct {
    LIST_ENTRY list_entry;
    LIST_ENTRY chunk_entry;
    LIST_ENTRY ranges;
    chunk* owner;
} trim_chunk;

typedef struct {
    LIST_ENTRY list_entry;
    LIST_ENTRY ranges;
    LIST_ENTRY replacement;
    device* owner;
    size_t replacement_bytes;
} trim_physical;

struct trim_batch {
    LIST_ENTRY list_entry;
    LIST_ENTRY chunks;
    LIST_ENTRY targets;
    LIST_ENTRY physical;
    struct trim_state* state;
    size_t bytes;
    uint64_t range_count;
    volatile LONG left;
    bool submitted;
    bool published;
};

typedef struct trim_state {
    KEVENT event;
    KEVENT idle;
    KSPIN_LOCK completion_lock;
    LIST_ENTRY batches;
    size_t bytes;
    volatile LONG pauses;
    bool stopping;
} trim_state;

void* trim_batch_alloc(trim_batch* batch, size_t size, POOL_TYPE pool);
bool trim_charge(trim_batch* batch, size_t size);
void trim_free_ranges(LIST_ENTRY* head);
void trim_move_ranges(LIST_ENTRY* dest, LIST_ENTRY* src);
bool trim_add_range(trim_batch* batch, device* dev, uint64_t address, uint64_t length);
bool trim_map_chunk(device_extension* Vcb, chunk* c, trim_batch* batch);
bool trim_prepare_chunk(device_extension* Vcb, chunk* c, trim_batch* batch);
bool trim_prepare_physical(trim_batch* batch, device* dev);
bool trim_prepare_requests(trim_batch* batch);
bool trim_publish_batch(trim_batch* batch);
bool trim_reclaim_batch(device_extension* Vcb, trim_batch* batch);
void trim_release_batch(trim_batch* batch);
