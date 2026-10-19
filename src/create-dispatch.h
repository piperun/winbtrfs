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

// LXSS programs can be distinguished by the fact they have a NULL PEB.
#ifdef _AMD64_
static __inline bool called_from_lxss() {
    NTSTATUS Status;
    PROCESS_BASIC_INFORMATION pbi;
    ULONG retlen;

    Status = ZwQueryInformationProcess(NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi), &retlen);

    if (!NT_SUCCESS(Status)) {
        ERR("ZwQueryInformationProcess returned %08lx\n", Status);
        return false;
    }

    return !pbi.PebBaseAddress;
}
#else
#define called_from_lxss() false
#endif

NTSTATUS file_create(PIRP Irp, _Requires_lock_held_(_Curr_->tree_lock) _Requires_exclusive_lock_held_(_Curr_->fcb_lock) device_extension* Vcb,
                            PFILE_OBJECT FileObject, file_ref* related, bool loaded_related, PUNICODE_STRING fnus, ULONG disposition, ULONG options,
                            file_ref** existing_fileref, LIST_ENTRY* rollback);
