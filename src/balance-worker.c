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

// Balance chunk selection and background worker.
#include "btrfs_drv.h"
#include "balance-worker.h"
#include "trim.h"

static __inline uint64_t get_chunk_dup_type(chunk* c) {
    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID0)
        return BTRFS_BLOCK_GROUP_RAID0;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1)
        return BTRFS_BLOCK_GROUP_RAID1;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DUP)
        return BTRFS_BLOCK_GROUP_DUP;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID10)
        return BTRFS_BLOCK_GROUP_RAID10;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5)
        return BTRFS_BLOCK_GROUP_RAID5;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)
        return BTRFS_BLOCK_GROUP_RAID6;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C3)
        return BTRFS_BLOCK_GROUP_RAID1C3;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C4)
        return BTRFS_BLOCK_GROUP_RAID1C4;
    else
        return BTRFS_AVAIL_ALLOC_BIT_SINGLE;
}

static uint16_t get_chunk_size_factor(chunk* c) {
    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID0)
        return c->chunk_item->num_stripes;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID10)
        return c->chunk_item->num_stripes / c->chunk_item->sub_stripes;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5)
        return c->chunk_item->num_stripes - 1;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)
        return c->chunk_item->num_stripes - 2;
    else // SINGLE, DUPLICATE, RAID1, RAID1C3, RAID1C4
        return 1;
}

static bool should_balance_chunk(device_extension* Vcb, uint8_t sort, chunk* c) {
    btrfs_balance_opts* opts;

    if (trim_chunk_pending(c))
        return false;

    opts = &Vcb->balance.opts[sort];

    if (!(opts->flags & BTRFS_BALANCE_OPTS_ENABLED))
        return false;

    if (opts->flags & BTRFS_BALANCE_OPTS_PROFILES) {
        uint64_t type = get_chunk_dup_type(c);

        if (!(type & opts->profiles))
            return false;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_DEVID) {
        uint16_t i;
        bool b = false;

        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (c->chunk_item->stripe[i].devid == opts->devid) {
                b = true;
                break;
            }
        }

        if (!b)
            return false;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_DRANGE) {
        uint16_t i;
        uint64_t physsize;
        bool b = false;

        physsize = c->chunk_item->length / get_chunk_size_factor(c);

        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (c->chunk_item->stripe[i].offset < opts->drange_end && c->chunk_item->stripe[i].offset + physsize >= opts->drange_start &&
                (!(opts->flags & BTRFS_BALANCE_OPTS_DEVID) || c->chunk_item->stripe[i].devid == opts->devid)) {
                b = true;
                break;
            }
        }

        if (!b)
            return false;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_VRANGE) {
        if (c->offset + c->chunk_item->length <= opts->vrange_start || c->offset > opts->vrange_end)
            return false;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_STRIPES) {
        if (c->chunk_item->num_stripes < opts->stripes_start || c->chunk_item->num_stripes < opts->stripes_end)
            return false;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_USAGE) {
        uint64_t usage = c->used * 100 / c->chunk_item->length;

        // usage == 0 should mean completely empty, not just that usage rounds to 0%
        if (c->used > 0 && usage == 0)
            usage = 1;

        if (usage < opts->usage_start || usage > opts->usage_end)
            return false;
    }

    if (opts->flags & BTRFS_BALANCE_OPTS_CONVERT && opts->flags & BTRFS_BALANCE_OPTS_SOFT) {
        uint64_t type = get_chunk_dup_type(c);

        if (type == opts->convert)
            return false;
    }

    return true;
}

static NTSTATUS try_consolidation(device_extension* Vcb, uint64_t flags, chunk** newchunk) {
    NTSTATUS Status;
    bool changed;
    LIST_ENTRY* le;
    chunk* rc;

    // FIXME - allow with metadata chunks?

    while (true) {
        rc = NULL;

        ExAcquireResourceSharedLite(&Vcb->tree_lock, true);

        ExAcquireResourceSharedLite(&Vcb->chunk_lock, true);

        // choose the least-used chunk we haven't looked at yet
        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

            // FIXME - skip full-size chunks over e.g. 90% full?
            if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA && !c->readonly && !trim_chunk_pending(c) && c->balance_num != Vcb->balance.balance_num && (!rc || c->used < rc->used))
                rc = c;

            le = le->Flink;
        }

        ExReleaseResourceLite(&Vcb->chunk_lock);

        if (!rc) {
            ExReleaseResourceLite(&Vcb->tree_lock);
            break;
        }

        if (rc->list_entry_balance.Flink) {
            RemoveEntryList(&rc->list_entry_balance);
            Vcb->balance.chunks_left--;
        }

        rc->list_entry_balance.Flink = (LIST_ENTRY*)1; // so it doesn't get dropped
        rc->reloc = true;

        ExReleaseResourceLite(&Vcb->tree_lock);

        do {
            changed = false;

            Status = balance_data_chunk(Vcb, rc, &changed);
            if (!NT_SUCCESS(Status)) {
                ERR("balance_data_chunk returned %08lx\n", Status);
                Vcb->balance.status = Status;
                rc->list_entry_balance.Flink = NULL;
                rc->reloc = false;
                return Status;
            }

            KeWaitForSingleObject(&Vcb->balance.event, Executive, KernelMode, false, NULL);

            if (Vcb->readonly)
                Vcb->balance.stopping = true;

            if (Vcb->balance.stopping)
                return STATUS_SUCCESS;
        } while (changed);

        rc->list_entry_balance.Flink = NULL;

        rc->changed = true;
        rc->space_changed = true;
        rc->balance_num = Vcb->balance.balance_num;

        ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

        Status = do_write(Vcb, NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("do_write returned %08lx\n", Status);
            ExReleaseResourceLite(&Vcb->tree_lock);
            return Status;
        }

        free_trees(Vcb);

        ExReleaseResourceLite(&Vcb->tree_lock);
    }

    ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);

    Status = alloc_chunk(Vcb, flags, &rc, true);

    ExReleaseResourceLite(&Vcb->chunk_lock);

    if (NT_SUCCESS(Status)) {
        *newchunk = rc;
        return Status;
    } else {
        ERR("alloc_chunk returned %08lx\n", Status);
        return Status;
    }
}

static NTSTATUS regenerate_space_list(device_extension* Vcb, device* dev) {
    NTSTATUS Status;
    LIST_ENTRY* le;

    while (!IsListEmpty(&dev->space)) {
        space* s = CONTAINING_RECORD(RemoveHeadList(&dev->space), space, list_entry);

        ExFreePool(s);
    }

    // The Linux driver doesn't like to allocate chunks within the first megabyte of a device.

    Status = space_list_add2(&dev->space, NULL, 0x100000,
                             dev->devitem.total_bytes - 0x100000, NULL, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        uint16_t n;
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

        for (n = 0; n < c->chunk_item->num_stripes; n++) {
            uint64_t stripe_size = 0;

            if (c->chunk_item->stripe[n].devid == dev->devitem.devid) {
                if (stripe_size == 0) {
                    uint16_t factor;

                    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID0)
                        factor = c->chunk_item->num_stripes;
                    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID10)
                        factor = c->chunk_item->num_stripes / c->chunk_item->sub_stripes;
                    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5)
                        factor = c->chunk_item->num_stripes - 1;
                    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)
                        factor = c->chunk_item->num_stripes - 2;
                    else // SINGLE, DUP, RAID1, RAID1C3, RAID1C4
                        factor = 1;

                    stripe_size = c->chunk_item->length / factor;
                }

                Status = space_list_subtract2(&dev->space, NULL,
                                              c->chunk_item->stripe[n].offset,
                                              stripe_size, NULL, NULL);
                if (!NT_SUCCESS(Status))
                    return Status;
            }
        }

        le = le->Flink;
    }

    return trim_subtract_device_pending(Vcb, dev);
}

_Function_class_(KSTART_ROUTINE)
void __stdcall balance_thread(void* context) {
    device_extension* Vcb = (device_extension*)context;
    LIST_ENTRY chunks;
    LIST_ENTRY* le;
    uint64_t num_chunks[3], okay_metadata_chunks = 0, okay_data_chunks = 0, okay_system_chunks = 0;
    uint64_t old_data_flags = 0, old_metadata_flags = 0, old_system_flags = 0;
    NTSTATUS Status;

    Vcb->balance.balance_num++;

    if (Vcb->balance.opts[BALANCE_OPTS_DATA].flags & BTRFS_BALANCE_OPTS_ENABLED && Vcb->balance.opts[BALANCE_OPTS_DATA].flags & BTRFS_BALANCE_OPTS_CONVERT) {
        old_data_flags = Vcb->data_flags;
        Vcb->data_flags = BTRFS_BLOCK_GROUP_DATA | (Vcb->balance.opts[BALANCE_OPTS_DATA].convert == BTRFS_AVAIL_ALLOC_BIT_SINGLE ? 0 : Vcb->balance.opts[BALANCE_OPTS_DATA].convert);

        FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_CHANGE_SIZE);
    }

    if (Vcb->balance.opts[BALANCE_OPTS_METADATA].flags & BTRFS_BALANCE_OPTS_ENABLED && Vcb->balance.opts[BALANCE_OPTS_METADATA].flags & BTRFS_BALANCE_OPTS_CONVERT) {
        old_metadata_flags = Vcb->metadata_flags;
        Vcb->metadata_flags = BTRFS_BLOCK_GROUP_METADATA | (Vcb->balance.opts[BALANCE_OPTS_METADATA].convert == BTRFS_AVAIL_ALLOC_BIT_SINGLE ? 0 : Vcb->balance.opts[BALANCE_OPTS_METADATA].convert);
    }

    if (Vcb->balance.opts[BALANCE_OPTS_SYSTEM].flags & BTRFS_BALANCE_OPTS_ENABLED && Vcb->balance.opts[BALANCE_OPTS_SYSTEM].flags & BTRFS_BALANCE_OPTS_CONVERT) {
        old_system_flags = Vcb->system_flags;
        Vcb->system_flags = BTRFS_BLOCK_GROUP_SYSTEM | (Vcb->balance.opts[BALANCE_OPTS_SYSTEM].convert == BTRFS_AVAIL_ALLOC_BIT_SINGLE ? 0 : Vcb->balance.opts[BALANCE_OPTS_SYSTEM].convert);
    }

    if (Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS) {
        if (Vcb->balance.opts[BALANCE_OPTS_DATA].flags & BTRFS_BALANCE_OPTS_ENABLED)
            RtlCopyMemory(&Vcb->balance.opts[BALANCE_OPTS_METADATA], &Vcb->balance.opts[BALANCE_OPTS_DATA], sizeof(btrfs_balance_opts));
        else if (Vcb->balance.opts[BALANCE_OPTS_METADATA].flags & BTRFS_BALANCE_OPTS_ENABLED)
            RtlCopyMemory(&Vcb->balance.opts[BALANCE_OPTS_DATA], &Vcb->balance.opts[BALANCE_OPTS_METADATA], sizeof(btrfs_balance_opts));
    }

    num_chunks[0] = num_chunks[1] = num_chunks[2] = 0;
    Vcb->balance.total_chunks = Vcb->balance.chunks_left = 0;

    InitializeListHead(&chunks);

    // FIXME - what are we supposed to do with limit_start?

    if (!Vcb->readonly) {
        if (!Vcb->balance.removing && !Vcb->balance.shrinking) {
            Status = add_balance_item(Vcb);
            if (!NT_SUCCESS(Status)) {
                ERR("add_balance_item returned %08lx\n", Status);
                Vcb->balance.status = Status;
                goto end;
            }
        } else {
            ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

            if (Vcb->need_write) {
                Status = do_write(Vcb, NULL);

                free_trees(Vcb);

                if (!NT_SUCCESS(Status)) {
                    ERR("do_write returned %08lx\n", Status);
                    Vcb->balance.status = Status;
                    ExReleaseResourceLite(&Vcb->tree_lock);
                    goto end;
                }
            }

            ExReleaseResourceLite(&Vcb->tree_lock);
        }
    }

    KeWaitForSingleObject(&Vcb->balance.event, Executive, KernelMode, false, NULL);

    if (Vcb->balance.stopping)
        goto end;

    ExAcquireResourceSharedLite(&Vcb->chunk_lock, true);

    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);
        uint8_t sort;

        acquire_chunk_lock(c, Vcb);

        if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA)
            sort = BALANCE_OPTS_DATA;
        else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_METADATA)
            sort = BALANCE_OPTS_METADATA;
        else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_SYSTEM)
            sort = BALANCE_OPTS_SYSTEM;
        else {
            ERR("unexpected chunk type %I64x\n", c->chunk_item->type);
            release_chunk_lock(c, Vcb);
            break;
        }

        if ((!(Vcb->balance.opts[sort].flags & BTRFS_BALANCE_OPTS_LIMIT) || num_chunks[sort] < Vcb->balance.opts[sort].limit_end) &&
            should_balance_chunk(Vcb, sort, c)) {
            InsertTailList(&chunks, &c->list_entry_balance);

            num_chunks[sort]++;
            Vcb->balance.total_chunks++;
            Vcb->balance.chunks_left++;
        } else if (sort == BALANCE_OPTS_METADATA)
            okay_metadata_chunks++;
        else if (sort == BALANCE_OPTS_DATA)
            okay_data_chunks++;
        else if (sort == BALANCE_OPTS_SYSTEM)
            okay_system_chunks++;

        if (!c->cache_loaded) {
            Status = load_cache_chunk(Vcb, c, NULL);

            if (!NT_SUCCESS(Status)) {
                ERR("load_cache_chunk returned %08lx\n", Status);
                Vcb->balance.status = Status;
                release_chunk_lock(c, Vcb);
                ExReleaseResourceLite(&Vcb->chunk_lock);
                goto end;
            }
        }

        release_chunk_lock(c, Vcb);

        le = le->Flink;
    }

    ExReleaseResourceLite(&Vcb->chunk_lock);

    // If we're doing a full balance, try and allocate a new chunk now, before we mess things up
    if (okay_metadata_chunks == 0 || okay_data_chunks == 0 || okay_system_chunks == 0) {
        bool consolidated = false;
        chunk* c;

        if (okay_metadata_chunks == 0) {
            ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);

            Status = alloc_chunk(Vcb, Vcb->metadata_flags, &c, true);
            if (NT_SUCCESS(Status))
                c->balance_num = Vcb->balance.balance_num;
            else if (Status != STATUS_DISK_FULL || consolidated) {
                ERR("alloc_chunk returned %08lx\n", Status);
                ExReleaseResourceLite(&Vcb->chunk_lock);
                Vcb->balance.status = Status;
                goto end;
            }

            ExReleaseResourceLite(&Vcb->chunk_lock);

            if (Status == STATUS_DISK_FULL) {
                Status = try_consolidation(Vcb, Vcb->metadata_flags, &c);
                if (!NT_SUCCESS(Status)) {
                    ERR("try_consolidation returned %08lx\n", Status);
                    Vcb->balance.status = Status;
                    goto end;
                } else
                    c->balance_num = Vcb->balance.balance_num;

                consolidated = true;

                if (Vcb->balance.stopping)
                    goto end;
            }
        }

        if (okay_data_chunks == 0) {
            ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);

            Status = alloc_chunk(Vcb, Vcb->data_flags, &c, true);
            if (NT_SUCCESS(Status))
                c->balance_num = Vcb->balance.balance_num;
            else if (Status != STATUS_DISK_FULL || consolidated) {
                ERR("alloc_chunk returned %08lx\n", Status);
                ExReleaseResourceLite(&Vcb->chunk_lock);
                Vcb->balance.status = Status;
                goto end;
            }

            ExReleaseResourceLite(&Vcb->chunk_lock);

            if (Status == STATUS_DISK_FULL) {
                Status = try_consolidation(Vcb, Vcb->data_flags, &c);
                if (!NT_SUCCESS(Status)) {
                    ERR("try_consolidation returned %08lx\n", Status);
                    Vcb->balance.status = Status;
                    goto end;
                } else
                    c->balance_num = Vcb->balance.balance_num;

                consolidated = true;

                if (Vcb->balance.stopping)
                    goto end;
            }
        }

        if (okay_system_chunks == 0) {
            ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, true);

            Status = alloc_chunk(Vcb, Vcb->system_flags, &c, true);
            if (NT_SUCCESS(Status))
                c->balance_num = Vcb->balance.balance_num;
            else if (Status != STATUS_DISK_FULL || consolidated) {
                ERR("alloc_chunk returned %08lx\n", Status);
                ExReleaseResourceLite(&Vcb->chunk_lock);
                Vcb->balance.status = Status;
                goto end;
            }

            ExReleaseResourceLite(&Vcb->chunk_lock);

            if (Status == STATUS_DISK_FULL) {
                Status = try_consolidation(Vcb, Vcb->system_flags, &c);
                if (!NT_SUCCESS(Status)) {
                    ERR("try_consolidation returned %08lx\n", Status);
                    Vcb->balance.status = Status;
                    goto end;
                } else
                    c->balance_num = Vcb->balance.balance_num;

                consolidated = true;

                if (Vcb->balance.stopping)
                    goto end;
            }
        }
    }

    ExAcquireResourceSharedLite(&Vcb->chunk_lock, true);

    le = chunks.Flink;
    while (le != &chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry_balance);

        c->reloc = true;

        le = le->Flink;
    }

    ExReleaseResourceLite(&Vcb->chunk_lock);

    // do data chunks before metadata
    le = chunks.Flink;
    while (le != &chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry_balance);
        LIST_ENTRY* le2 = le->Flink;

        if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA) {
            bool changed;

            do {
                changed = false;

                Status = balance_data_chunk(Vcb, c, &changed);
                if (!NT_SUCCESS(Status)) {
                    ERR("balance_data_chunk returned %08lx\n", Status);
                    Vcb->balance.status = Status;
                    goto end;
                }

                KeWaitForSingleObject(&Vcb->balance.event, Executive, KernelMode, false, NULL);

                if (Vcb->readonly)
                    Vcb->balance.stopping = true;

                if (Vcb->balance.stopping)
                    break;
            } while (changed);

            c->changed = true;
            c->space_changed = true;
        }

        if (Vcb->balance.stopping)
            goto end;

        if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA &&
            (!(Vcb->balance.opts[BALANCE_OPTS_METADATA].flags & BTRFS_BALANCE_OPTS_ENABLED) || !(c->chunk_item->type & BTRFS_BLOCK_GROUP_METADATA))) {
            RemoveEntryList(&c->list_entry_balance);
            c->list_entry_balance.Flink = NULL;

            Vcb->balance.chunks_left--;
        }

        le = le2;
    }

    // do metadata chunks
    while (!IsListEmpty(&chunks)) {
        chunk* c;
        bool changed;

        le = RemoveHeadList(&chunks);
        c = CONTAINING_RECORD(le, chunk, list_entry_balance);

        if (c->chunk_item->type & BTRFS_BLOCK_GROUP_METADATA || c->chunk_item->type & BTRFS_BLOCK_GROUP_SYSTEM) {
            do {
                Status = balance_metadata_chunk(Vcb, c, &changed);
                if (!NT_SUCCESS(Status)) {
                    ERR("balance_metadata_chunk returned %08lx\n", Status);
                    Vcb->balance.status = Status;
                    goto end;
                }

                KeWaitForSingleObject(&Vcb->balance.event, Executive, KernelMode, false, NULL);

                if (Vcb->readonly)
                    Vcb->balance.stopping = true;

                if (Vcb->balance.stopping)
                    break;
            } while (changed);

            c->changed = true;
            c->space_changed = true;
        }

        if (Vcb->balance.stopping)
            break;

        c->list_entry_balance.Flink = NULL;

        Vcb->balance.chunks_left--;
    }

end:
    if (!Vcb->readonly) {
        if (Vcb->balance.stopping || !NT_SUCCESS(Vcb->balance.status)) {
            le = chunks.Flink;
            while (le != &chunks) {
                chunk* c = CONTAINING_RECORD(le, chunk, list_entry_balance);
                c->reloc = false;

                le = le->Flink;
                c->list_entry_balance.Flink = NULL;
            }

            if (old_data_flags != 0)
                Vcb->data_flags = old_data_flags;

            if (old_metadata_flags != 0)
                Vcb->metadata_flags = old_metadata_flags;

            if (old_system_flags != 0)
                Vcb->system_flags = old_system_flags;
        }

        if (Vcb->balance.removing) {
            device* dev = NULL;

            ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

            le = Vcb->devices.Flink;
            while (le != &Vcb->devices) {
                device* dev2 = CONTAINING_RECORD(le, device, list_entry);

                if (dev2->devitem.devid == Vcb->balance.opts[0].devid) {
                    dev = dev2;
                    break;
                }

                le = le->Flink;
            }

            if (dev) {
                if (Vcb->balance.chunks_left == 0) {
                    Status = finish_removing_device(Vcb, dev);

                    if (!NT_SUCCESS(Status)) {
                        ERR("finish_removing_device returned %08lx\n", Status);
                        dev->reloc = false;
                    }
                } else
                    dev->reloc = false;
            }

            ExReleaseResourceLite(&Vcb->tree_lock);
        } else if (Vcb->balance.shrinking) {
            device* dev = NULL;

            ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

            le = Vcb->devices.Flink;
            while (le != &Vcb->devices) {
                device* dev2 = CONTAINING_RECORD(le, device, list_entry);

                if (dev2->devitem.devid == Vcb->balance.opts[0].devid) {
                    dev = dev2;
                    break;
                }

                le = le->Flink;
            }

            if (!dev) {
                ERR("could not find device %I64x\n", Vcb->balance.opts[0].devid);
                Vcb->balance.status = STATUS_INTERNAL_ERROR;
            }

            if (Vcb->balance.stopping || !NT_SUCCESS(Vcb->balance.status)) {
                if (dev) {
                    Status = regenerate_space_list(Vcb, dev);
                    if (!NT_SUCCESS(Status))
                        WARN("regenerate_space_list returned %08lx\n", Status);
                }
            } else {
                uint64_t old_size;
                bool failed = false;

                ExAcquireResourceSharedLite(&Vcb->chunk_lock, true);

                for (LIST_ENTRY* le = Vcb->chunks.Flink; le != &Vcb->chunks; le = le->Flink) {
                    chunk* c = CONTAINING_RECORD(le, chunk, list_entry);

                    uint16_t factor = get_chunk_size_factor(c);

                    for (uint16_t i = 0; i < c->chunk_item->num_stripes; i++) {
                        if (c->chunk_item->stripe[i].devid != dev->devitem.devid)
                            continue;

                        if (c->chunk_item->stripe[i].offset + (c->chunk_item->length / factor) > Vcb->balance.opts[0].drange_start) {
                            failed = true;
                            break;
                        }
                    }

                    if (failed)
                        break;
                }

                ExReleaseResourceLite(&Vcb->chunk_lock);

                if (failed) {
                    Vcb->balance.status = STATUS_DISK_FULL;

                    Status = regenerate_space_list(Vcb, dev);
                    if (!NT_SUCCESS(Status))
                        WARN("regenerate_space_list returned %08lx\n", Status);
                } else {
                    old_size = dev->devitem.total_bytes;
                    dev->devitem.total_bytes = Vcb->balance.opts[0].drange_start;

                    Status = update_dev_item(Vcb, dev, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("update_dev_item returned %08lx\n", Status);
                        dev->devitem.total_bytes = old_size;
                        Vcb->balance.status = Status;

                        Status = regenerate_space_list(Vcb, dev);
                        if (!NT_SUCCESS(Status))
                            WARN("regenerate_space_list returned %08lx\n", Status);
                    } else {
                        Vcb->superblock.total_bytes -= old_size - dev->devitem.total_bytes;

                        Status = do_write(Vcb, NULL);
                        if (!NT_SUCCESS(Status))
                            ERR("do_write returned %08lx\n", Status);

                        free_trees(Vcb);
                    }
                }
            }

            ExReleaseResourceLite(&Vcb->tree_lock);

            if (!Vcb->balance.stopping && NT_SUCCESS(Vcb->balance.status))
                FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_CHANGE_SIZE);
        } else {
            Status = remove_balance_item(Vcb);
            if (!NT_SUCCESS(Status)) {
                ERR("remove_balance_item returned %08lx\n", Status);
                goto end;
            }
        }

        if (Vcb->trim && !Vcb->options.no_trim) {
            ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

            le = Vcb->devices.Flink;
            while (le != &Vcb->devices) {
                device* dev2 = CONTAINING_RECORD(le, device, list_entry);

                if (dev2->devobj && !dev2->readonly && dev2->trim)
                    trim_unalloc_space(Vcb, dev2);

                le = le->Flink;
            }

            ExReleaseResourceLite(&Vcb->tree_lock);
        }
    }

    ZwClose(Vcb->balance.thread);
    Vcb->balance.thread = NULL;

    KeSetEvent(&Vcb->balance.finished, 0, false);
}
