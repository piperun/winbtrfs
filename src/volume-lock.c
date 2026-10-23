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

// Volume cache flushing and exclusive lock ownership.
#include "btrfs_drv.h"
#include "volume-lock.h"
#include "trim.h"

void flush_fcb_caches(device_extension* Vcb) {
    LIST_ENTRY* le;

    le = Vcb->all_fcbs.Flink;
    while (le != &Vcb->all_fcbs) {
        struct _fcb* fcb = CONTAINING_RECORD(le, struct _fcb, list_entry_all);
        IO_STATUS_BLOCK iosb;

        if (fcb->type != BTRFS_FT_DIR && !fcb->deleted)
            CcFlushCache(&fcb->nonpaged->segment_object, NULL, 0, &iosb);

        le = le->Flink;
    }
}

NTSTATUS lock_volume(device_extension* Vcb, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;
    KIRQL irql;
    bool lock_paused_balance = false;
    bool trim_paused = false;

    TRACE("FSCTL_LOCK_VOLUME\n");

    if (Vcb->scrub.thread) {
        WARN("cannot lock while scrub running\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (Vcb->balance.thread) {
        WARN("cannot lock while balance running\n");
        return STATUS_DEVICE_NOT_READY;
    }

    TRACE("locking volume\n");

    FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_LOCK);

    if (Vcb->locked)
        return STATUS_SUCCESS;

    ExAcquireResourceExclusiveLite(&Vcb->fileref_lock, true);

    if (Vcb->root_fileref && Vcb->root_fileref->fcb && (Vcb->root_fileref->open_count > 0 || has_open_children(Vcb->root_fileref))) {
        Status = STATUS_ACCESS_DENIED;
        ExReleaseResourceLite(&Vcb->fileref_lock);
        goto end;
    }

    ExReleaseResourceLite(&Vcb->fileref_lock);

    if (Vcb->balance.thread && KeReadStateEvent(&Vcb->balance.event)) {
        ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);
        KeClearEvent(&Vcb->balance.event);
        ExReleaseResourceLite(&Vcb->tree_lock);

        lock_paused_balance = true;
    }

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    flush_fcb_caches(Vcb);

    if (Vcb->need_write && !Vcb->readonly)
        Status = do_write(Vcb, Irp);
    else
        Status = STATUS_SUCCESS;

    free_trees(Vcb);

    if (NT_SUCCESS(Status)) {
        trim_pause_locked(Vcb);
        trim_paused = true;
    }

    ExReleaseResourceLite(&Vcb->tree_lock);

    if (!NT_SUCCESS(Status)) {
        ERR("do_write returned %08lx\n", Status);
        goto end;
    }

    trim_wait_idle(Vcb);

    IoAcquireVpbSpinLock(&irql);

    if (!(Vcb->Vpb->Flags & VPB_LOCKED)) {
        Vcb->Vpb->Flags |= VPB_LOCKED;
        Vcb->locked = true;
        Vcb->locked_fileobj = IrpSp->FileObject;
        Vcb->lock_paused_balance = lock_paused_balance;
    } else {
        Status = STATUS_ACCESS_DENIED;
        IoReleaseVpbSpinLock(irql);

        if (lock_paused_balance)
            KeSetEvent(&Vcb->balance.event, 0, false);

        goto end;
    }

    IoReleaseVpbSpinLock(irql);

    Status = STATUS_SUCCESS;

end:
    if (!NT_SUCCESS(Status) && trim_paused)
        trim_resume(Vcb);
    if (!NT_SUCCESS(Status))
        FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_LOCK_FAILED);

    return Status;
}

void do_unlock_volume(device_extension* Vcb) {
    KIRQL irql;

    IoAcquireVpbSpinLock(&irql);

    Vcb->locked = false;
    Vcb->Vpb->Flags &= ~VPB_LOCKED;
    Vcb->locked_fileobj = NULL;

    IoReleaseVpbSpinLock(irql);

    trim_resume(Vcb);

    if (Vcb->lock_paused_balance)
        KeSetEvent(&Vcb->balance.event, 0, false);
}

NTSTATUS unlock_volume(device_extension* Vcb, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    TRACE("FSCTL_UNLOCK_VOLUME\n");

    if (!Vcb->locked || IrpSp->FileObject != Vcb->locked_fileobj)
        return STATUS_NOT_LOCKED;

    TRACE("unlocking volume\n");

    do_unlock_volume(Vcb);

    FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_UNLOCK);

    return STATUS_SUCCESS;
}
