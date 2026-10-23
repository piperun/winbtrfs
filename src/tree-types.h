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

// In-memory tree nodes and ordered item ownership.
#pragma once

typedef struct {
    uint64_t address;
    uint64_t generation;
    struct _tree* tree;
} tree_holder;

typedef struct _tree_data {
    struct btrfs_key key;
    LIST_ENTRY list_entry;
    bool ignore;
    bool inserted;

    union {
        tree_holder treeholder;

        struct {
            uint16_t size;
            uint8_t* data;
        };
    };
} tree_data;

typedef struct {
    FAST_MUTEX mutex;
} tree_nonpaged;

typedef struct _tree {
    tree_nonpaged* nonpaged;
    struct btrfs_header header;
    uint32_t hash;
    bool has_address;
    uint32_t size;
    struct _device_extension* Vcb;
    struct _tree* parent;
    tree_data* paritem;
    struct _root* root;
    LIST_ENTRY itemlist;
    tree_data** search_items;
    uint32_t search_count;
    LIST_ENTRY list_entry;
    LIST_ENTRY list_entry_hash;
    uint64_t new_address;
    bool has_new_address;
    bool updated_extents;
    bool write;
    bool is_unique;
    bool uniqueness_determined;
    uint8_t* buf;
} tree;
