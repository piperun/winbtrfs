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

// Private interfaces shared by tree transaction topics.
#pragma once

typedef struct {
    struct btrfs_extent_item extent_item;
    struct btrfs_tree_block_info tbi;
    struct btrfs_extent_inline_ref eir;
} EXTENT_ITEM_TREE2;

static_assert(sizeof(EXTENT_ITEM_TREE2) == 51, "EXTENT_ITEM_TREE2 has wrong size");

typedef struct {
    struct btrfs_extent_item ei;
    struct btrfs_extent_inline_ref eir;
} EXTENT_ITEM_SKINNY_METADATA;

static_assert(sizeof(EXTENT_ITEM_SKINNY_METADATA) == 33, "EXTENT_ITEM_SKINNY_METADATA has wrong size");

NTSTATUS update_tree_extents(device_extension* Vcb, tree* t, PIRP Irp);
NTSTATUS reduce_tree_extent(device_extension* Vcb, uint64_t address, tree* t,
                                   uint64_t parent_root, PIRP Irp);
NTSTATUS do_splits(device_extension* Vcb, PIRP Irp);
NTSTATUS write_trees(device_extension* Vcb, PIRP Irp);
