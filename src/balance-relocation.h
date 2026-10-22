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

// Shared records and existing entry points for chunk relocation.
typedef struct {
    uint64_t address;
    uint64_t new_address;
    struct btrfs_header* data;
    struct btrfs_extent_item* ei;
    tree* t;
    bool system;
    LIST_ENTRY refs;
    LIST_ENTRY list_entry;
} metadata_reloc;

typedef struct {
    uint8_t type;
    uint64_t hash;
    uint64_t offset;
    metadata_reloc* parent;
    bool top;
    LIST_ENTRY list_entry;
} metadata_reloc_ref;

typedef struct {
    uint64_t address;
    uint64_t size;
    uint64_t new_address;
    chunk* newchunk;
    struct btrfs_extent_item* ei;
    LIST_ENTRY refs;
    LIST_ENTRY list_entry;
} data_reloc;

typedef struct {
    uint8_t type;
    uint64_t hash;

    union {
        struct btrfs_extent_data_ref edr;

        struct {
            uint64_t offset;
            struct btrfs_shared_data_ref sdr;
        };
    };

    metadata_reloc* parent;
    LIST_ENTRY list_entry;
} data_reloc_ref;

#define BALANCE_UNIT 0x100000 // only read 1 MB at a time

NTSTATUS add_data_reloc(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* items, LIST_ENTRY* metadata_items,
                               traverse_ptr* tp, chunk* c, LIST_ENTRY* rollback);

NTSTATUS add_data_reloc_extent_item(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, data_reloc* dr);

NTSTATUS balance_data_chunk(device_extension* Vcb, chunk* c, bool* changed);

NTSTATUS add_metadata_reloc_parent(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* items,
                                          uint64_t address, metadata_reloc** mr2, LIST_ENTRY* rollback);

NTSTATUS write_metadata_items(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* items,
                                     LIST_ENTRY* data_items, chunk* c, LIST_ENTRY* rollback);
