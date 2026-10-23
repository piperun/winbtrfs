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

// Validate backing devices after sleep and invalidate stale mounts.
#include "btrfs_drv.h"
#include "volume-wakeup.h"

extern ERESOURCE global_loading_lock;

static bool device_still_valid(device* dev, uint64_t expected_generation) {
    NTSTATUS Status;
    unsigned int to_read;
    struct btrfs_super_block* sb;

    to_read = (unsigned int)(dev->devobj->SectorSize == 0 ? sizeof(struct btrfs_super_block) : sector_align(sizeof(struct btrfs_super_block), dev->devobj->SectorSize));

    sb = ExAllocatePoolWithTag(NonPagedPool, to_read, ALLOC_TAG);
    if (!sb) {
        ERR("out of memory\n");
        return false;
    }

    Status = sync_read_phys(dev->devobj, dev->fileobj, superblock_addrs[0], to_read, (PUCHAR)sb, false);
    if (!NT_SUCCESS(Status)) {
        ERR("sync_read_phys returned %08lx\n", Status);
        ExFreePool(sb);
        return false;
    }

    if (sb->magic != BTRFS_MAGIC) {
        ERR("magic not found\n");
        ExFreePool(sb);
        return false;
    }

    if (!check_superblock_checksum(sb)) {
        ExFreePool(sb);
        return false;
    }

    if (sb->generation > expected_generation) {
        ERR("generation was %I64x, expected %I64x\n", sb->generation, expected_generation);
        ExFreePool(sb);
        return false;
    }

    ExFreePool(sb);

    return true;
}

_Function_class_(IO_WORKITEM_ROUTINE)
void __stdcall check_after_wakeup(PDEVICE_OBJECT DeviceObject, PVOID con) {
    device_extension* Vcb = (device_extension*)con;
    LIST_ENTRY* le;

    UNUSED(DeviceObject);

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    le = Vcb->devices.Flink;

    // FIXME - do reads in parallel?

    while (le != &Vcb->devices) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);

        if (dev->devobj) {
            if (!device_still_valid(dev, Vcb->superblock.generation - 1)) {
                PDEVICE_OBJECT voldev = Vcb->Vpb->RealDevice;
                KIRQL irql;
                PVPB newvpb;

                WARN("forcing remount\n");

                newvpb = ExAllocatePoolWithTag(NonPagedPool, sizeof(VPB), ALLOC_TAG);
                if (!newvpb) {
                    ERR("out of memory in check_after_wakeup, going readonly rather than unmounting\n");

                    Vcb->readonly = true;
                    FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_FORCED_CLOSED);

                    ExReleaseResourceLite(&Vcb->tree_lock);
                    return;
                }

                RtlZeroMemory(newvpb, sizeof(VPB));

                newvpb->Type = IO_TYPE_VPB;
                newvpb->Size = sizeof(VPB);
                newvpb->RealDevice = voldev;
                newvpb->Flags = VPB_DIRECT_WRITES_ALLOWED;

                Vcb->removing = true;

                IoAcquireVpbSpinLock(&irql);
                voldev->Vpb = newvpb;
                IoReleaseVpbSpinLock(irql);

                Vcb->vde = NULL;

                ExReleaseResourceLite(&Vcb->tree_lock);

                if (Vcb->open_files == 0)
                    uninit(Vcb);
                else { // remove from VcbList
                    ExAcquireResourceExclusiveLite(&global_loading_lock, true);
                    RemoveEntryList(&Vcb->list_entry);
                    Vcb->list_entry.Flink = NULL;
                    ExReleaseResourceLite(&global_loading_lock);
                }

                return;
            }
        }

        le = le->Flink;
    }

    ExReleaseResourceLite(&Vcb->tree_lock);
}
