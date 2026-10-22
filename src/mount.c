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

/* Volume mount initialization and device checks; failure ownership is in mount-cleanup.c. */
#ifdef _DEBUG
#define DEBUG
#endif

#include "mount.h"
#include "mount-cleanup.h"
#include "notify.h"
#include "crc32c.h"
#include <mountdev.h>
#include <ntdddisk.h>

extern PDRIVER_OBJECT drvobj;
extern PDEVICE_OBJECT master_devobj;
extern LIST_ENTRY VcbList;
extern ERESOURCE global_loading_lock;
extern ERESOURCE pdo_list_lock;
extern LIST_ENTRY pdo_list;
extern bool finished_probing;
extern bool degraded_wait;
extern uint64_t boot_subvol;

#define INCOMPAT_SUPPORTED (BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF | BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL | BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS | \
                            BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO | BTRFS_FEATURE_INCOMPAT_BIG_METADATA | BTRFS_FEATURE_INCOMPAT_RAID56 | \
                            BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF | BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA | BTRFS_FEATURE_INCOMPAT_NO_HOLES | \
                            BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD | BTRFS_FEATURE_INCOMPAT_METADATA_UUID | BTRFS_FEATURE_INCOMPAT_RAID1C34)
#define COMPAT_RO_SUPPORTED (BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE | BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID | \
                             BTRFS_FEATURE_COMPAT_RO_VERITY | BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE)

static bool is_btrfs_volume(_In_ PDEVICE_OBJECT DeviceObject) {
    NTSTATUS Status;
    MOUNTDEV_NAME mdn, *mdn2;
    ULONG mdnsize;

    Status = dev_ioctl(DeviceObject, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, &mdn, sizeof(MOUNTDEV_NAME), true, NULL);
    if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW) {
        ERR("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME returned %08lx\n", Status);
        return false;
    }

    mdnsize = (ULONG)offsetof(MOUNTDEV_NAME, Name[0]) + mdn.NameLength;

    mdn2 = ExAllocatePoolWithTag(PagedPool, mdnsize, ALLOC_TAG);
    if (!mdn2) {
        ERR("out of memory\n");
        return false;
    }

    Status = dev_ioctl(DeviceObject, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, mdn2, mdnsize, true, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME returned %08lx\n", Status);
        ExFreePool(mdn2);
        return false;
    }

    if (mdn2->NameLength > (sizeof(BTRFS_VOLUME_PREFIX) - sizeof(WCHAR)) &&
        RtlCompareMemory(mdn2->Name, BTRFS_VOLUME_PREFIX, sizeof(BTRFS_VOLUME_PREFIX) - sizeof(WCHAR)) == sizeof(BTRFS_VOLUME_PREFIX) - sizeof(WCHAR)) {
        ExFreePool(mdn2);
        return true;
    }

    ExFreePool(mdn2);

    return false;
}

_Success_(return>=0)
static NTSTATUS check_mount_device(_In_ PDEVICE_OBJECT DeviceObject,
                                   _Out_ bool* direct_mount) {
    NTSTATUS Status;
    ULONG to_read;
    struct btrfs_super_block* sb;
    UNICODE_STRING pnp_name;
    const GUID* guid;

    to_read = DeviceObject->SectorSize == 0 ? sizeof(struct btrfs_super_block) : (ULONG)sector_align(sizeof(struct btrfs_super_block), DeviceObject->SectorSize);

    sb = ExAllocatePoolWithTag(NonPagedPool, to_read, ALLOC_TAG);
    if (!sb) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = sync_read_phys(DeviceObject, NULL, superblock_addrs[0], to_read, (PUCHAR)sb, true);
    if (!NT_SUCCESS(Status)) {
        ERR("sync_read_phys returned %08lx\n", Status);
        goto end;
    }

    if (sb->magic != BTRFS_MAGIC) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    if (!check_superblock_checksum(sb)) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    DeviceObject->Flags &= ~DO_VERIFY_VOLUME;

    if (should_ignore_volume_arrival(DeviceObject)) {
        *direct_mount = true;
        Status = STATUS_SUCCESS;
        goto end;
    }

    pnp_name.Buffer = NULL;

    Status = get_device_pnp_name(DeviceObject, &pnp_name, &guid);
    if (!NT_SUCCESS(Status)) {
        WARN("get_device_pnp_name returned %08lx\n", Status);
        pnp_name.Length = 0;
    }

    *direct_mount = pnp_name.Length == 0;

    if (pnp_name.Buffer)
        ExFreePool(pnp_name.Buffer);

    Status = STATUS_SUCCESS;

end:
    ExFreePool(sb);

    return Status;
}

static bool still_has_superblock(_In_ PDEVICE_OBJECT device, _In_ PFILE_OBJECT fileobj) {
    NTSTATUS Status;
    ULONG to_read;
    struct btrfs_super_block* sb;

    if (!device)
        return false;

    to_read = device->SectorSize == 0 ? sizeof(struct btrfs_super_block) : (ULONG)sector_align(sizeof(struct btrfs_super_block), device->SectorSize);

    sb = ExAllocatePoolWithTag(NonPagedPool, to_read, ALLOC_TAG);
    if (!sb) {
        ERR("out of memory\n");
        return false;
    }

    Status = sync_read_phys(device, fileobj, superblock_addrs[0], to_read, (PUCHAR)sb, true);
    if (!NT_SUCCESS(Status)) {
        ERR("Failed to read superblock: %08lx\n", Status);
        ExFreePool(sb);
        return false;
    }

    if (sb->magic != BTRFS_MAGIC) {
        TRACE("not a BTRFS volume\n");
        ExFreePool(sb);
        return false;
    } else {
        if (!check_superblock_checksum(sb)) {
            ExFreePool(sb);
            return false;
        }
    }

    ObReferenceObject(device);

    while (device) {
        PDEVICE_OBJECT device2 = IoGetLowerDeviceObject(device);

        device->Flags &= ~DO_VERIFY_VOLUME;

        ObDereferenceObject(device);

        device = device2;
    }

    ExFreePool(sb);

    return true;
}

static void calculate_sector_shift(device_extension* Vcb) {
    uint32_t ss = Vcb->superblock.sectorsize;

    Vcb->sector_shift = 0;

    while (!(ss & 1)) {
        Vcb->sector_shift++;
        ss >>= 1;
    }
}

NTSTATUS mount_vol(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp) {
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT NewDeviceObject = NULL;
    PDEVICE_OBJECT DeviceToMount, readobj;
    PFILE_OBJECT fileobj;
    NTSTATUS Status;
    device_extension* Vcb = NULL;
    LIST_ENTRY *le, batchlist;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    fcb* root_fcb = NULL;
    ccb* root_ccb = NULL;
    bool init_lookaside = false;
    device* dev;
    volume_device_extension* vde = NULL;
    pdo_device_extension* pdode = NULL;
    volume_child* vc;
    uint64_t readobjsize;
    OBJECT_ATTRIBUTES oa;
    device_extension* real_devext;
    KIRQL irql;
    bool direct_mount = false;
    LARGE_INTEGER due_time;

    TRACE("(%p, %p)\n", DeviceObject, Irp);

    if (DeviceObject != master_devobj)
        return STATUS_INVALID_DEVICE_REQUEST;

    IrpSp = IoGetCurrentIrpStackLocation(Irp);
    DeviceToMount = IrpSp->Parameters.MountVolume.DeviceObject;

    real_devext = IrpSp->Parameters.MountVolume.Vpb->RealDevice->DeviceExtension;

    // Make sure we're not trying to mount the PDO
    if (IrpSp->Parameters.MountVolume.Vpb->RealDevice->DriverObject == drvobj && real_devext->type == VCB_TYPE_PDO)
        return STATUS_UNRECOGNIZED_VOLUME;

    if (!is_btrfs_volume(DeviceToMount)) {
        Status = check_mount_device(DeviceToMount, &direct_mount);
        if (!NT_SUCCESS(Status))
            WARN("check_mount_device returned %08lx\n", Status);

        if (!direct_mount) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            goto exit;
        }
    } else {
        PDEVICE_OBJECT pdo;

        pdo = DeviceToMount;

        ObReferenceObject(pdo);

        while (true) {
            PDEVICE_OBJECT pdo2 = IoGetLowerDeviceObject(pdo);

            ObDereferenceObject(pdo);

            if (!pdo2)
                break;
            else
                pdo = pdo2;
        }

        ExAcquireResourceSharedLite(&pdo_list_lock, true);

        le = pdo_list.Flink;
        while (le != &pdo_list) {
            pdo_device_extension* pdode2 = CONTAINING_RECORD(le, pdo_device_extension, list_entry);

            if (pdode2->pdo == pdo) {
                vde = pdode2->vde;
                break;
            }

            le = le->Flink;
        }

        ExReleaseResourceLite(&pdo_list_lock);

        if (!vde || vde->type != VCB_TYPE_VOLUME) {
            vde = NULL;
            Status = STATUS_UNRECOGNIZED_VOLUME;
            goto exit;
        }
    }

    if (vde) {
        pdode = vde->pdode;

        ExAcquireResourceExclusiveLite(&pdode->child_lock, true);

        le = pdode->children.Flink;
        while (le != &pdode->children) {
            LIST_ENTRY* le2 = le->Flink;

            vc = CONTAINING_RECORD(le, volume_child, list_entry);

            if (!still_has_superblock(vc->devobj, vc->fileobj)) {
                remove_volume_child(vde, vc, false);

                if (pdode->num_children == 0) {
                    ERR("error - number of devices is zero\n");
                    Status = STATUS_INTERNAL_ERROR;
                    goto exit;
                }

                Status = STATUS_DEVICE_NOT_READY;
                goto exit;
            }

            le = le2;
        }

        if (pdode->num_children == 0 || pdode->children_loaded == 0) {
            ERR("error - number of devices is zero\n");
            Status = STATUS_INTERNAL_ERROR;
            ExReleaseResourceLite(&pdode->child_lock);
            goto exit;
        }

        ExConvertExclusiveToSharedLite(&pdode->child_lock);

        vc = CONTAINING_RECORD(pdode->children.Flink, volume_child, list_entry);

        readobj = vc->devobj;
        fileobj = vc->fileobj;
        readobjsize = vc->size;

        vde->device->Characteristics &= ~FILE_DEVICE_SECURE_OPEN;
    } else {
        GET_LENGTH_INFORMATION gli;

        vc = NULL;
        readobj = DeviceToMount;
        fileobj = NULL;

        Status = dev_ioctl(readobj, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                           &gli, sizeof(gli), true, NULL);

        if (!NT_SUCCESS(Status)) {
            ERR("error reading length information: %08lx\n", Status);
            goto exit;
        }

        readobjsize = gli.Length.QuadPart;
    }

    Status = IoCreateDevice(drvobj, sizeof(device_extension), NULL, FILE_DEVICE_DISK_FILE_SYSTEM, 0, false, &NewDeviceObject);
    if (!NT_SUCCESS(Status)) {
        ERR("IoCreateDevice returned %08lx\n", Status);
        Status = STATUS_UNRECOGNIZED_VOLUME;

        if (pdode)
            ExReleaseResourceLite(&pdode->child_lock);

        goto exit;
    }

    NewDeviceObject->Flags |= DO_DIRECT_IO;

    // Some programs seem to expect that the sector size will be 512, for
    // FILE_NO_INTERMEDIATE_BUFFERING and the like.
    NewDeviceObject->SectorSize = min(DeviceToMount->SectorSize, 512);

    Vcb = (PVOID)NewDeviceObject->DeviceExtension;
    RtlZeroMemory(Vcb, sizeof(device_extension));
    Vcb->type = VCB_TYPE_FS;
    Vcb->vde = vde;

    ExInitializeResourceLite(&Vcb->tree_lock);
    Vcb->need_write = false;

    ExInitializeResourceLite(&Vcb->fcb_lock);
    ExInitializeResourceLite(&Vcb->fileref_lock);
    ExInitializeResourceLite(&Vcb->chunk_lock);
    ExInitializeResourceLite(&Vcb->dirty_fcbs_lock);
    ExInitializeResourceLite(&Vcb->dirty_filerefs_lock);
    ExInitializeResourceLite(&Vcb->dirty_subvols_lock);
    ExInitializeResourceLite(&Vcb->scrub.stats_lock);

    ExInitializeResourceLite(&Vcb->load_lock);
    ExAcquireResourceExclusiveLite(&Vcb->load_lock, true);

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    DeviceToMount->Flags |= DO_DIRECT_IO;

    Status = read_superblock(Vcb, readobj, fileobj, readobjsize);
    if (!NT_SUCCESS(Status)) {
        if (!IoIsErrorUserInduced(Status))
            Status = STATUS_UNRECOGNIZED_VOLUME;
        else if (Irp->Tail.Overlay.Thread)
            IoSetHardErrorOrVerifyDevice(Irp, readobj);

        if (pdode)
            ExReleaseResourceLite(&pdode->child_lock);

        goto exit;
    }

    if (direct_mount && Vcb->superblock.num_devices > 1) {
        ERR("cannot mount multi-device FS when doing direct mount\n");
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto exit;
    }

    Status = registry_load_volume_options(Vcb);
    if (!NT_SUCCESS(Status)) {
        ERR("registry_load_volume_options returned %08lx\n", Status);

        if (pdode)
            ExReleaseResourceLite(&pdode->child_lock);

        goto exit;
    }

    if (pdode) {
        if (RtlCompareMemory(&boot_uuid, &pdode->uuid, BTRFS_UUID_SIZE) == BTRFS_UUID_SIZE && boot_subvol != 0)
            Vcb->options.subvol_id = boot_subvol;

        if (pdode->children_loaded < pdode->num_children && (!Vcb->options.allow_degraded || !finished_probing || degraded_wait)) {
            ERR("could not mount as %I64u device(s) missing\n", pdode->num_children - pdode->children_loaded);
            Status = STATUS_DEVICE_NOT_READY;
            ExReleaseResourceLite(&pdode->child_lock);
            goto exit;
        }

        // Windows holds DeviceObject->DeviceLock, guaranteeing that mount_vol is serialized
        ExReleaseResourceLite(&pdode->child_lock);
    }

    if (Vcb->options.ignore) {
        TRACE("ignoring volume\n");
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto exit;
    }

    if (Vcb->superblock.incompat_flags & ~INCOMPAT_SUPPORTED) {
        WARN("cannot mount because of unsupported incompat flags (%I64x)\n", Vcb->superblock.incompat_flags & ~INCOMPAT_SUPPORTED);
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto exit;
    }

    if (!(Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_METADATA_UUID)) {
        memcpy(Vcb->superblock.metadata_uuid, Vcb->superblock.fsid,
               BTRFS_UUID_SIZE);
    }

    Vcb->readonly = false;
    if (Vcb->superblock.compat_ro_flags & ~COMPAT_RO_SUPPORTED) {
        WARN("mounting read-only because of unsupported flags (%I64x)\n", Vcb->superblock.compat_ro_flags & ~COMPAT_RO_SUPPORTED);
        Vcb->readonly = true;
    }

    if (Vcb->options.readonly)
        Vcb->readonly = true;

    calculate_sector_shift(Vcb);

    Vcb->superblock.generation++;
    Vcb->superblock.incompat_flags |= BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF;

    if (Vcb->superblock.log_root != 0) {
        FIXME("FIXME - replay transaction log (clearing for now)\n");
        Vcb->superblock.log_root = 0;
    }

    switch (Vcb->superblock.csum_type) {
        case BTRFS_CSUM_TYPE_CRC32:
            Vcb->csum_size = sizeof(uint32_t);
            break;

        case BTRFS_CSUM_TYPE_XXHASH:
            Vcb->csum_size = sizeof(uint64_t);
            break;

        case BTRFS_CSUM_TYPE_SHA256:
            Vcb->csum_size = SHA256_HASH_SIZE;
            break;

        case BTRFS_CSUM_TYPE_BLAKE2:
            Vcb->csum_size = BLAKE2_HASH_SIZE;
            break;

        default:
            ERR("unrecognized csum type %x\n", Vcb->superblock.csum_type);
            break;
    }

    InitializeListHead(&Vcb->devices);
    dev = ExAllocatePoolWithTag(NonPagedPool, sizeof(device), ALLOC_TAG);
    if (!dev) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    dev->devobj = readobj;
    dev->fileobj = fileobj;
    RtlCopyMemory(&dev->devitem, &Vcb->superblock.dev_item, sizeof(struct btrfs_dev_item));

    if (dev->devitem.total_bytes > readobjsize) {
        WARN("device %I64x: btrfs_dev_item says %I64x bytes, but Windows only reports %I64x\n", dev->devitem.devid,
                dev->devitem.total_bytes, readobjsize);

        dev->devitem.total_bytes = readobjsize;
    }

    dev->seeding = Vcb->superblock.flags & BTRFS_SUPER_FLAG_SEEDING ? true : false;

    init_device(Vcb, dev, true);

    InsertTailList(&Vcb->devices, &dev->list_entry);
    Vcb->devices_loaded = 1;

    Vcb->direct_mount = direct_mount;

    if (DeviceToMount->Flags & DO_SYSTEM_BOOT_PARTITION)
        Vcb->disallow_dismount = true;

    TRACE("DeviceToMount = %p\n", DeviceToMount);
    TRACE("IrpSp->Parameters.MountVolume.Vpb = %p\n", IrpSp->Parameters.MountVolume.Vpb);

    NewDeviceObject->StackSize = DeviceToMount->StackSize + 1;
    NewDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    InitializeListHead(&Vcb->roots);
    InitializeListHead(&Vcb->drop_roots);

    Vcb->log_to_phys_loaded = false;

    add_root(Vcb, BTRFS_CHUNK_TREE_OBJECTID, Vcb->superblock.chunk_root, Vcb->superblock.chunk_root_generation, NULL);

    if (!Vcb->chunk_root) {
        ERR("Could not load chunk root.\n");
        Status = STATUS_INTERNAL_ERROR;
        goto exit;
    }

    InitializeListHead(&Vcb->sys_chunks);
    Status = load_sys_chunks(Vcb);
    if (!NT_SUCCESS(Status)) {
        ERR("load_sys_chunks returned %08lx\n", Status);
        goto exit;
    }

    InitializeListHead(&Vcb->chunks);
    InitializeListHead(&Vcb->trees);
    InitializeListHead(&Vcb->trees_hash);
    InitializeListHead(&Vcb->all_fcbs);
    InitializeListHead(&Vcb->dirty_fcbs);
    InitializeListHead(&Vcb->dirty_filerefs);
    InitializeListHead(&Vcb->dirty_subvols);
    InitializeListHead(&Vcb->send_ops);

    ExInitializeFastMutex(&Vcb->trees_list_mutex);

    InitializeListHead(&Vcb->DirNotifyList);
    InitializeListHead(&Vcb->scrub.errors);

    FsRtlNotifyInitializeSync(&Vcb->NotifySync);

    ExInitializePagedLookasideList(&Vcb->tree_data_lookaside, NULL, NULL, 0, sizeof(tree_data), ALLOC_TAG, 0);
    ExInitializePagedLookasideList(&Vcb->traverse_ptr_lookaside, NULL, NULL, 0, sizeof(traverse_ptr), ALLOC_TAG, 0);
    ExInitializePagedLookasideList(&Vcb->batch_item_lookaside, NULL, NULL, 0, sizeof(batch_item), ALLOC_TAG, 0);
    ExInitializePagedLookasideList(&Vcb->fileref_lookaside, NULL, NULL, 0, sizeof(file_ref), ALLOC_TAG, 0);
    ExInitializePagedLookasideList(&Vcb->fcb_lookaside, NULL, NULL, 0, sizeof(fcb), ALLOC_TAG, 0);
    ExInitializePagedLookasideList(&Vcb->name_bit_lookaside, NULL, NULL, 0, sizeof(name_bit), ALLOC_TAG, 0);
    ExInitializeNPagedLookasideList(&Vcb->range_lock_lookaside, NULL, NULL, 0, sizeof(range_lock), ALLOC_TAG, 0);
    ExInitializeNPagedLookasideList(&Vcb->fcb_np_lookaside, NULL, NULL, 0, sizeof(fcb_nonpaged), ALLOC_TAG, 0);
    init_lookaside = true;

    Vcb->Vpb = IrpSp->Parameters.MountVolume.Vpb;

    Status = load_chunk_root(Vcb, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("load_chunk_root returned %08lx\n", Status);
        goto exit;
    }

    if (Vcb->superblock.num_devices > 1) {
        if (Vcb->devices_loaded < Vcb->superblock.num_devices && (!Vcb->options.allow_degraded || !finished_probing)) {
            ERR("could not mount as %I64u device(s) missing\n", Vcb->superblock.num_devices - Vcb->devices_loaded);

            IoRaiseInformationalHardError(IO_ERR_INTERNAL_ERROR, NULL, NULL);

            Status = STATUS_INTERNAL_ERROR;
            goto exit;
        }

        if (dev->readonly && !Vcb->readonly) {
            Vcb->readonly = true;

            le = Vcb->devices.Flink;
            while (le != &Vcb->devices) {
                device* dev2 = CONTAINING_RECORD(le, device, list_entry);

                if (dev2->readonly && !dev2->seeding)
                    break;

                if (!dev2->readonly) {
                    Vcb->readonly = false;
                    break;
                }

                le = le->Flink;
            }

            if (Vcb->readonly)
                WARN("setting volume to readonly\n");
        }
    } else {
        if (dev->readonly) {
            WARN("setting volume to readonly as device is readonly\n");
            Vcb->readonly = true;
        }
    }

    add_root(Vcb, BTRFS_ROOT_TREE_OBJECTID, Vcb->superblock.root, Vcb->superblock.generation - 1, NULL);

    if (!Vcb->root_root) {
        ERR("Could not load root of roots.\n");
        Status = STATUS_INTERNAL_ERROR;
        goto exit;
    }

    Status = look_for_roots(Vcb, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("look_for_roots returned %08lx\n", Status);
        goto exit;
    }

    if (!Vcb->readonly) {
        Status = find_chunk_usage(Vcb, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("find_chunk_usage returned %08lx\n", Status);
            goto exit;
        }
    }

    InitializeListHead(&batchlist);

    // We've already increased the generation by one
    if (!Vcb->readonly && (
        Vcb->options.clear_cache ||
        (!(Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE) && Vcb->superblock.generation - 1 != Vcb->superblock.cache_generation) ||
        (Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE && !(Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID)))) {
        if (Vcb->options.clear_cache)
            WARN("ClearCache option was set, clearing cache...\n");
        else if (Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE && !(Vcb->superblock.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID))
            WARN("clearing free-space tree created by buggy Linux driver\n");
        else
            WARN("generation was %I64x, free-space cache generation was %I64x; clearing cache...\n", Vcb->superblock.generation - 1, Vcb->superblock.cache_generation);

        Status = clear_free_space_cache(Vcb, &batchlist, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("clear_free_space_cache returned %08lx\n", Status);
            clear_batch_list(Vcb, &batchlist);
            goto exit;
        }
    }

    Status = commit_batch_list(Vcb, &batchlist, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("commit_batch_list returned %08lx\n", Status);
        goto exit;
    }

    Vcb->volume_fcb = create_fcb(Vcb, NonPagedPool);
    if (!Vcb->volume_fcb) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    Vcb->volume_fcb->Vcb = Vcb;
    Vcb->volume_fcb->sd = NULL;

    Vcb->dummy_fcb = create_fcb(Vcb, NonPagedPool);
    if (!Vcb->dummy_fcb) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    Vcb->dummy_fcb->Vcb = Vcb;
    Vcb->dummy_fcb->type = BTRFS_FT_DIR;
    Vcb->dummy_fcb->inode = 2;
    Vcb->dummy_fcb->subvol = Vcb->root_root;
    Vcb->dummy_fcb->atts = FILE_ATTRIBUTE_DIRECTORY;
    Vcb->dummy_fcb->inode_item.nlink = 1;
    Vcb->dummy_fcb->inode_item.mode = __S_IFDIR;

    Vcb->dummy_fcb->hash_ptrs = ExAllocatePoolWithTag(PagedPool, sizeof(LIST_ENTRY*) * 256, ALLOC_TAG);
    if (!Vcb->dummy_fcb->hash_ptrs) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    RtlZeroMemory(Vcb->dummy_fcb->hash_ptrs, sizeof(LIST_ENTRY*) * 256);

    Vcb->dummy_fcb->hash_ptrs_uc = ExAllocatePoolWithTag(PagedPool, sizeof(LIST_ENTRY*) * 256, ALLOC_TAG);
    if (!Vcb->dummy_fcb->hash_ptrs_uc) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    RtlZeroMemory(Vcb->dummy_fcb->hash_ptrs_uc, sizeof(LIST_ENTRY*) * 256);

    root_fcb = create_fcb(Vcb, NonPagedPool);
    if (!root_fcb) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    root_fcb->Vcb = Vcb;
    root_fcb->inode = BTRFS_FIRST_FREE_OBJECTID;
    root_fcb->hash = calc_crc32c(0xffffffff, (uint8_t*)&root_fcb->inode, sizeof(uint64_t));
    root_fcb->type = BTRFS_FT_DIR;

#ifdef DEBUG_FCB_REFCOUNTS
    WARN("volume FCB = %p\n", Vcb->volume_fcb);
    WARN("root FCB = %p\n", root_fcb);
#endif

    root_fcb->subvol = find_default_subvol(Vcb, Irp);

    if (!root_fcb->subvol) {
        ERR("could not find top subvol\n");
        Status = STATUS_INTERNAL_ERROR;
        goto exit;
    }

    Status = load_dir_children(Vcb, root_fcb, true, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("load_dir_children returned %08lx\n", Status);
        goto exit;
    }

    searchkey.objectid = root_fcb->inode;
    searchkey.type = BTRFS_INODE_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, root_fcb->subvol, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        goto exit;
    }

    if (tp.item->key.objectid != searchkey.objectid || tp.item->key.type != searchkey.type) {
        ERR("couldn't find INODE_ITEM for root directory\n");
        Status = STATUS_INTERNAL_ERROR;
        goto exit;
    }

    if (tp.item->size > 0)
        RtlCopyMemory(&root_fcb->inode_item, tp.item->data, min(sizeof(struct btrfs_inode_item), tp.item->size));

    Status = fcb_get_sd(root_fcb, NULL, true, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("fcb_get_sd returned %08lx\n", Status);
        goto exit;
    }

    root_fcb->atts = get_file_attributes(Vcb, root_fcb->subvol, root_fcb->inode, root_fcb->type, false, false, Irp);

    if (root_fcb->subvol->id == BTRFS_FS_TREE_OBJECTID)
        root_fcb->atts &= ~FILE_ATTRIBUTE_HIDDEN;

    Vcb->root_fileref = create_fileref(Vcb);
    if (!Vcb->root_fileref) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    Vcb->root_fileref->fcb = root_fcb;
    InsertTailList(&root_fcb->subvol->fcbs, &root_fcb->list_entry);
    InsertTailList(&Vcb->all_fcbs, &root_fcb->list_entry_all);

    root_fcb->subvol->fcbs_ptrs[root_fcb->hash >> 24] = &root_fcb->list_entry;

    root_fcb->fileref = Vcb->root_fileref;

    root_ccb = ExAllocatePoolWithTag(PagedPool, sizeof(ccb), ALLOC_TAG);
    if (!root_ccb) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    Vcb->root_file = IoCreateStreamFileObject(NULL, DeviceToMount);
    Vcb->root_file->FsContext = root_fcb;
    Vcb->root_file->SectionObjectPointer = &root_fcb->nonpaged->segment_object;
    Vcb->root_file->Vpb = DeviceObject->Vpb;

    RtlZeroMemory(root_ccb, sizeof(ccb));
    root_ccb->NodeType = BTRFS_NODE_TYPE_CCB;
    root_ccb->NodeSize = sizeof(ccb);

    Vcb->root_file->FsContext2 = root_ccb;

    try {
        CcInitializeCacheMap(Vcb->root_file, (PCC_FILE_SIZES)(&root_fcb->Header.AllocationSize), false, &cache_callbacks, Vcb->root_file);
    } except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
        goto exit;
    }

    le = Vcb->devices.Flink;
    while (le != &Vcb->devices) {
        device* dev2 = CONTAINING_RECORD(le, device, list_entry);

        Status = find_disk_holes(Vcb, dev2, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("find_disk_holes returned %08lx\n", Status);
            goto exit;
        }

        le = le->Flink;
    }

    // Keep the VPB private until all volume threads have been created.
    Status = create_calc_threads(NewDeviceObject);
    if (!NT_SUCCESS(Status)) {
        ERR("create_calc_threads returned %08lx\n", Status);
        goto exit;
    }

    KeInitializeTimer(&Vcb->flush_thread_timer);
    KeInitializeEvent(&Vcb->flush_thread_finished, NotificationEvent, false);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    Status = PsCreateSystemThread(&Vcb->flush_thread_handle, SYNCHRONIZE, &oa, NULL, NULL, flush_thread, NewDeviceObject);
    if (!NT_SUCCESS(Status)) {
        ERR("PsCreateSystemThread returned %08lx\n", Status);
        stop_calc_threads(Vcb);
        goto exit;
    }

    IoAcquireVpbSpinLock(&irql);

    NewDeviceObject->Vpb = IrpSp->Parameters.MountVolume.Vpb;
    IrpSp->Parameters.MountVolume.Vpb->DeviceObject = NewDeviceObject;
    IrpSp->Parameters.MountVolume.Vpb->Flags |= VPB_MOUNTED;
    NewDeviceObject->Vpb->VolumeLabelLength = 4; // FIXME
    NewDeviceObject->Vpb->VolumeLabel[0] = '?';
    NewDeviceObject->Vpb->VolumeLabel[1] = 0;
    NewDeviceObject->Vpb->ReferenceCount++;

    IoReleaseVpbSpinLock(irql);

    due_time.QuadPart = (uint64_t)Vcb->options.flush_interval * -10000000;
    KeSetTimer(&Vcb->flush_thread_timer, due_time, NULL);

    Status = registry_mark_volume_mounted(Vcb->superblock.fsid);
    if (!NT_SUCCESS(Status))
        WARN("registry_mark_volume_mounted returned %08lx\n", Status);

    Status = look_for_balance_item(Vcb);
    if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND)
        WARN("look_for_balance_item returned %08lx\n", Status);

    Status = STATUS_SUCCESS;

    if (vde)
        vde->mounted_device = NewDeviceObject;

    Vcb->devobj = NewDeviceObject;

    ExInitializeResourceLite(&Vcb->send_load_lock);

exit:
    if (Vcb) {
        ExReleaseResourceLite(&Vcb->tree_lock);
        ExReleaseResourceLite(&Vcb->load_lock);
    }

    if (!NT_SUCCESS(Status)) {
        if (Vcb) {
            cleanup_failed_mount(Vcb, root_fcb, init_lookaside);
        }

        if (NewDeviceObject)
            IoDeleteDevice(NewDeviceObject);
    } else {
        ExAcquireResourceExclusiveLite(&global_loading_lock, true);
        InsertTailList(&VcbList, &Vcb->list_entry);
        ExReleaseResourceLite(&global_loading_lock);

        FsRtlNotifyVolumeEvent(Vcb->root_file, FSRTL_VOLUME_MOUNT);
    }

    TRACE("mount_vol done (status: %lx)\n", Status);

    return Status;
}
