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

// Mounted volume ownership, resources and background workers.
typedef struct _device_extension {
    uint32_t type;
    mount_options options;
    PVPB Vpb;
    PDEVICE_OBJECT devobj;
    struct _volume_device_extension* vde;
    LIST_ENTRY devices;
#ifdef DEBUG_CHUNK_LOCKS
    LONG chunk_locks_held;
#endif
    uint64_t devices_loaded;
    struct btrfs_super_block superblock;
    unsigned int sector_shift;
    unsigned int csum_size;
    bool readonly;
    bool removing;
    bool locked;
    bool lock_paused_balance;
    bool disallow_dismount;
    bool direct_mount;
    LONG page_file_count;
    bool trim;
    struct trim_state* trim_state;
    PFILE_OBJECT locked_fileobj;
    fcb* volume_fcb;
    fcb* dummy_fcb;
    file_ref* root_fileref;
    LONG open_files;
    _Has_lock_level_(fcb_lock) ERESOURCE fcb_lock;
    ERESOURCE fileref_lock;
    ERESOURCE load_lock;
    _Has_lock_level_(tree_lock) ERESOURCE tree_lock;
    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY DirNotifyList;
    bool need_write;
    bool stats_changed;
    uint64_t data_flags;
    uint64_t metadata_flags;
    uint64_t system_flags;
    LIST_ENTRY roots;
    LIST_ENTRY drop_roots;
    root* chunk_root;
    root* root_root;
    root* extent_root;
    root* checksum_root;
    root* dev_root;
    root* uuid_root;
    root* data_reloc_root;
    root* space_root;
    root* block_group_root;
    bool log_to_phys_loaded;
    bool chunk_usage_found;
    LIST_ENTRY sys_chunks;
    LIST_ENTRY chunks;
    LIST_ENTRY trees;
    LIST_ENTRY trees_hash;
    LIST_ENTRY* trees_ptrs[256];
    FAST_MUTEX trees_list_mutex;
    LIST_ENTRY all_fcbs;
    LIST_ENTRY dirty_fcbs;
    ERESOURCE dirty_fcbs_lock;
    LIST_ENTRY dirty_filerefs;
    ERESOURCE dirty_filerefs_lock;
    LIST_ENTRY dirty_subvols;
    ERESOURCE dirty_subvols_lock;
    ERESOURCE chunk_lock;
    HANDLE flush_thread_handle;
    KTIMER flush_thread_timer;
    KEVENT flush_thread_finished;
    drv_calc_threads calcthreads;
    balance_info balance;
    scrub_info scrub;
    ERESOURCE send_load_lock;
    LONG running_sends;
    LIST_ENTRY send_ops;
    PFILE_OBJECT root_file;
    PAGED_LOOKASIDE_LIST tree_data_lookaside;
    PAGED_LOOKASIDE_LIST traverse_ptr_lookaside;
    PAGED_LOOKASIDE_LIST batch_item_lookaside;
    PAGED_LOOKASIDE_LIST fileref_lookaside;
    PAGED_LOOKASIDE_LIST fcb_lookaside;
    PAGED_LOOKASIDE_LIST name_bit_lookaside;
    NPAGED_LOOKASIDE_LIST range_lock_lookaside;
    NPAGED_LOOKASIDE_LIST fcb_np_lookaside;
    LIST_ENTRY list_entry;
} device_extension;
