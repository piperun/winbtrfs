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

// Mutations of published chunks hold changed_extents_lock or exclusive
// Vcb->tree_lock. Initialization and terminal reset require private ownership.
// These operations allocate nothing and preserve list append order.
void initialize_changed_extents(chunk* c);
void insert_changed_extent(chunk* c, changed_extent* ce);
void remove_changed_extent(chunk* c, changed_extent* ce);
void move_changed_extent(chunk* source, chunk* destination, changed_extent* ce, uint64_t address);

changed_extent* get_changed_extent_item(chunk* c, uint64_t address, uint64_t size,
                                        bool no_csum);

NTSTATUS update_changed_extent_ref(device_extension* Vcb, chunk* c, uint64_t address,
                                   uint64_t size, uint64_t root, uint64_t objid,
                                   uint64_t offset, int32_t count, bool no_csum,
                                   bool superseded, PIRP Irp, LIST_ENTRY* rollback);

NTSTATUS add_changed_extent_ref(chunk* c, uint64_t address, uint64_t size,
                                uint64_t root, uint64_t objid, uint64_t offset,
                                uint32_t count, bool no_csum, LIST_ENTRY* rollback);

NTSTATUS flush_changed_extent(device_extension* Vcb, chunk* c, changed_extent* ce,
                                     PIRP Irp);
