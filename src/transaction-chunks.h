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

// Internal transaction helpers; callers serialize volume tree changes.
NTSTATUS create_chunk(device_extension* Vcb, chunk* c, PIRP Irp);
NTSTATUS update_chunks(device_extension* Vcb, LIST_ENTRY* batchlist, PIRP Irp);
NTSTATUS clean_space_cache(device_extension* Vcb);
void add_trim_entry(device* dev, uint64_t address, uint64_t size);
NTSTATUS insert_tree_item_batch(LIST_ENTRY* batchlist, device_extension* Vcb, root* r, uint64_t objid,
                               uint8_t objtype, uint64_t offset, void* data, uint16_t datalen,
                               enum batch_operation operation);
