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

#ifndef SL_IGNORE_READONLY_ATTRIBUTE
#define SL_IGNORE_READONLY_ATTRIBUTE 0x40 // introduced in Windows 10, not in mingw
#endif

NTSTATUS create_stream(_Requires_lock_held_(_Curr_->tree_lock) _Requires_exclusive_lock_held_(_Curr_->fcb_lock) device_extension* Vcb,
                              file_ref** pfileref, file_ref** pparfileref, PUNICODE_STRING fpus, PUNICODE_STRING stream, PIRP Irp,
                              ULONG options, POOL_TYPE pool_type, bool case_sensitive, LIST_ENTRY* rollback);
