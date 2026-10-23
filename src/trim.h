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

#include "btrfs_drv.h"

// Synchronous discard for explicitly collected unallocated device ranges.
NTSTATUS trim_devices(device_extension* Vcb);

// Automatic discard queue: commit is serialized by tree_lock; the worker owns
// submission, reclamation and final draining outside volume resource waits.
void trim_initialize(device_extension* Vcb);
void trim_destroy(device_extension* Vcb);
KEVENT* trim_event(device_extension* Vcb);
void trim_commit(device_extension* Vcb);
void trim_process(device_extension* Vcb);
void trim_drain(device_extension* Vcb);
bool trim_chunk_pending(const chunk* c);
NTSTATUS trim_merge_pending(chunk* c, LIST_ENTRY* list, LIST_ENTRY* sizes);
uint64_t trim_pending_count(chunk* c);
void trim_forget_device(device_extension* Vcb, device* dev);
NTSTATUS trim_subtract_device_pending(device_extension* Vcb, device* dev, LIST_ENTRY* spaces);

// Pause while holding tree_lock exclusively, then wait with no volume resources.
// The matching resume is safe from handle cleanup's shared-resource scope.
void trim_pause_locked(device_extension* Vcb);
void trim_wait_idle(device_extension* Vcb);
void trim_resume(device_extension* Vcb);

// Pause and reclaim before changing chunk/device mappings. Always resume once,
// including on failure; callers hold no volume resources across this call.
NTSTATUS trim_quiesce(device_extension* Vcb);
