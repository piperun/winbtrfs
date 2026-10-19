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

NTSTATUS file_create2(_In_ PIRP Irp, _Requires_exclusive_lock_held_(_Curr_->fcb_lock) _In_ device_extension* Vcb, _In_ PUNICODE_STRING fpus,
                             _In_ file_ref* parfileref, _In_ ULONG options, _In_reads_bytes_opt_(ealen) FILE_FULL_EA_INFORMATION* ea, _In_ ULONG ealen,
                             _Out_ file_ref** pfr, bool case_sensitive, _In_ LIST_ENTRY* rollback);
