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

/* Internal interfaces for volume mounting and metadata discovery. */
#pragma once

#include "btrfs_drv.h"

NTSTATUS read_superblock(_In_ device_extension* Vcb, _In_ PDEVICE_OBJECT device, _In_ PFILE_OBJECT fileobj, _In_ uint64_t length);
_Requires_exclusive_lock_held_(Vcb->tree_lock)
NTSTATUS add_root(_Inout_ device_extension* Vcb, _In_ uint64_t id, _In_ uint64_t addr,
                         _In_ uint64_t generation, _In_opt_ traverse_ptr* tp);
NTSTATUS look_for_roots(_Requires_exclusive_lock_held_(_Curr_->tree_lock) _In_ device_extension* Vcb, _In_opt_ PIRP Irp);
NTSTATUS find_disk_holes(_In_ _Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_ device* dev, _In_opt_ PIRP Irp);
NTSTATUS load_chunk_root(_In_ _Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_opt_ PIRP Irp);
NTSTATUS load_sys_chunks(_In_ device_extension* Vcb);
NTSTATUS mount_vol(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
void add_device_to_list(_In_ device_extension* Vcb, _In_ device* dev);
