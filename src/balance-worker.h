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

// Internal interfaces used by the volume balance worker.
NTSTATUS balance_metadata_chunk(device_extension* Vcb, chunk* c, bool* changed);
NTSTATUS add_balance_item(device_extension* Vcb);
NTSTATUS remove_balance_item(device_extension* Vcb);
NTSTATUS finish_removing_device(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, device* dev);
void trim_unalloc_space(_Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, device* dev);
