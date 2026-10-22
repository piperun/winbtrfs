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

// Chunk ownership and pending extent reference records.
struct changed_extent;

typedef struct {
    struct btrfs_chunk* chunk_item;
    uint16_t size;
    uint64_t offset;
    uint64_t used;
    uint64_t oldused;
    device** devices;
    fcb* cache;
    fcb* old_cache;
    LIST_ENTRY space;
    LIST_ENTRY space_size;
    LIST_ENTRY deleting;
    LIST_ENTRY changed_extents;
    struct changed_extent* changed_extents_root;
    LIST_ENTRY range_locks;
    ERESOURCE range_locks_lock;
    KEVENT range_locks_event;
    ERESOURCE lock;
    ERESOURCE changed_extents_lock;
    bool created;
    bool readonly;
    bool reloc;
    bool last_alloc_set;
    bool cache_loaded;
    bool changed;
    bool space_changed;
    bool using_fst_bitmaps;
    uint64_t last_alloc;
    uint16_t last_stripe;
    LIST_ENTRY partial_stripes;
    ERESOURCE partial_stripes_lock;
    ULONG balance_num;

    LIST_ENTRY list_entry;
    LIST_ENTRY list_entry_balance;
} chunk;

typedef struct changed_extent {
    uint64_t address;
    uint64_t size;
    uint64_t old_size;
    uint64_t count;
    uint64_t old_count;
    bool no_csum;
    bool superseded;
    LIST_ENTRY refs;
    LIST_ENTRY old_refs;
    LIST_ENTRY list_entry;
    struct changed_extent* left;
    struct changed_extent* right;
    struct changed_extent* parent;
    uint8_t height;
} changed_extent;

typedef struct {
    uint8_t type;

    union {
        struct btrfs_extent_data_ref edr;

        struct {
            uint64_t offset;
            struct btrfs_shared_data_ref sdr;
        };
    };

    LIST_ENTRY list_entry;
} changed_extent_ref;
