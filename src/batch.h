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

/* Internal collision handling for batched tree updates. */
#pragma once

#include "btrfs_drv.h"

__attribute__((nonnull(1,2,3)))
NTSTATUS add_delete_inode_extref(device_extension* Vcb, batch_item* bi, LIST_ENTRY* listhead);
__attribute__((nonnull(1,2,3,4,6,7)))
NTSTATUS handle_batch_collision(device_extension* Vcb, batch_item* bi, tree* t, tree_data* td, tree_data* newtd, LIST_ENTRY* listhead, bool* ignore);
