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

NTSTATUS add_insert_extent_rollback(LIST_ENTRY* rollback, fcb* fcb, extent* ext);
NTSTATUS remove_fcb_extent(fcb* fcb, extent* ext, LIST_ENTRY* rollback);
NTSTATUS insert_extent(device_extension* Vcb, fcb* fcb, uint64_t start_data, uint64_t length, void* data,
                              PIRP Irp, bool file_write, uint64_t irp_offset, LIST_ENTRY* rollback);
