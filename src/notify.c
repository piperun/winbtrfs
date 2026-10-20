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

#include "notify.h"

extern bool shutting_down;
extern PDEVICE_OBJECT master_devobj;

void send_notification_fileref(_In_ file_ref* fileref, _In_ ULONG filter_match, _In_ ULONG action, _In_opt_ PUNICODE_STRING stream) {
    UNICODE_STRING fn;
    NTSTATUS Status;
    ULONG reqlen;
    USHORT name_offset;
    fcb* fcb = fileref->fcb;

    fn.Length = fn.MaximumLength = 0;
    Status = fileref_get_filename(fileref, &fn, NULL, &reqlen);
    if (Status != STATUS_BUFFER_OVERFLOW) {
        ERR("fileref_get_filename returned %08lx\n", Status);
        return;
    }

    if (reqlen > 0xffff) {
        WARN("reqlen was too long for FsRtlNotifyFilterReportChange\n");
        return;
    }

    fn.Buffer = ExAllocatePoolWithTag(PagedPool, reqlen, ALLOC_TAG);
    if (!fn.Buffer) {
        ERR("out of memory\n");
        return;
    }

    fn.MaximumLength = (USHORT)reqlen;
    fn.Length = 0;

    Status = fileref_get_filename(fileref, &fn, &name_offset, &reqlen);
    if (!NT_SUCCESS(Status)) {
        ERR("fileref_get_filename returned %08lx\n", Status);
        ExFreePool(fn.Buffer);
        return;
    }

    FsRtlNotifyFilterReportChange(fcb->Vcb->NotifySync, &fcb->Vcb->DirNotifyList, (PSTRING)&fn, name_offset,
                                  (PSTRING)stream, NULL, filter_match, action, NULL, NULL);
    ExFreePool(fn.Buffer);
}

void send_notification_fcb(_In_ file_ref* fileref, _In_ ULONG filter_match, _In_ ULONG action, _In_opt_ PUNICODE_STRING stream) {
    fcb* fcb = fileref->fcb;
    LIST_ENTRY* le;
    NTSTATUS Status;

    // no point looking for hardlinks if nlink == 1
    if (fileref->fcb->inode_item.nlink == 1) {
        ExAcquireResourceExclusiveLite(&fcb->Vcb->fileref_lock, true);
        send_notification_fileref(fileref, filter_match, action, stream);
        ExReleaseResourceLite(&fcb->Vcb->fileref_lock);
        return;
    }

    ExAcquireResourceExclusiveLite(&fcb->Vcb->fileref_lock, true);

    for (le = fcb->hardlinks.Flink; le != &fcb->hardlinks; le = le->Flink) {
        hardlink* hl = CONTAINING_RECORD(le, hardlink, list_entry);
        file_ref* parfr;

        Status = open_fileref_by_inode(fcb->Vcb, fcb->subvol, hl->parent, &parfr, NULL);

        if (!NT_SUCCESS(Status)) {
            ERR("open_fileref_by_inode returned %08lx\n", Status);
            continue;
        }

        if (!parfr->deleted) {
            UNICODE_STRING fn;
            ULONG pathlen;

            fn.Length = fn.MaximumLength = 0;
            Status = fileref_get_filename(parfr, &fn, NULL, &pathlen);
            if (Status != STATUS_BUFFER_OVERFLOW) {
                ERR("fileref_get_filename returned %08lx\n", Status);
                free_fileref(parfr);
                break;
            }

            if (parfr != fcb->Vcb->root_fileref)
                pathlen += sizeof(WCHAR);

            if (pathlen + hl->name.Length > 0xffff) {
                WARN("pathlen + hl->name.Length was too long for FsRtlNotifyFilterReportChange\n");
                free_fileref(parfr);
                break;
            }

            fn.MaximumLength = (USHORT)(pathlen + hl->name.Length);
            fn.Buffer = ExAllocatePoolWithTag(PagedPool, fn.MaximumLength, ALLOC_TAG);
            if (!fn.Buffer) {
                ERR("out of memory\n");
                free_fileref(parfr);
                break;
            }

            Status = fileref_get_filename(parfr, &fn, NULL, NULL);
            if (!NT_SUCCESS(Status)) {
                ERR("fileref_get_filename returned %08lx\n", Status);
                free_fileref(parfr);
                ExFreePool(fn.Buffer);
                break;
            }

            if (parfr != fcb->Vcb->root_fileref) {
                fn.Buffer[(pathlen / sizeof(WCHAR)) - 1] = '\\';
                fn.Length += sizeof(WCHAR);
            }

            RtlCopyMemory(&fn.Buffer[pathlen / sizeof(WCHAR)], hl->name.Buffer, hl->name.Length);
            fn.Length += hl->name.Length;

            FsRtlNotifyFilterReportChange(fcb->Vcb->NotifySync, &fcb->Vcb->DirNotifyList, (PSTRING)&fn, (USHORT)pathlen,
                                          (PSTRING)stream, NULL, filter_match, action, NULL, NULL);

            ExFreePool(fn.Buffer);
        }

        free_fileref(parfr);
    }

    ExReleaseResourceLite(&fcb->Vcb->fileref_lock);
}

typedef struct {
    file_ref* fileref;
    ULONG filter_match;
    ULONG action;
    PUNICODE_STRING stream;
    PIO_WORKITEM work_item;
} notification_fcb;

_Function_class_(IO_WORKITEM_ROUTINE)
static void __stdcall notification_work_item(PDEVICE_OBJECT DeviceObject, PVOID con) {
    notification_fcb* nf = con;

    UNUSED(DeviceObject);

    ExAcquireResourceSharedLite(&nf->fileref->fcb->Vcb->tree_lock, TRUE); // protect us from fileref being reaped

    send_notification_fcb(nf->fileref, nf->filter_match, nf->action, nf->stream);

    free_fileref(nf->fileref);

    ExReleaseResourceLite(&nf->fileref->fcb->Vcb->tree_lock);

    IoFreeWorkItem(nf->work_item);

    ExFreePool(nf);
}

void queue_notification_fcb(_In_ file_ref* fileref, _In_ ULONG filter_match, _In_ ULONG action, _In_opt_ PUNICODE_STRING stream) {
    notification_fcb* nf;
    PIO_WORKITEM work_item;

    if (shutting_down)
        return;

    nf = ExAllocatePoolWithTag(PagedPool, sizeof(notification_fcb) + (stream ? sizeof(UNICODE_STRING) + stream->Length : 0), ALLOC_TAG);
    if (!nf) {
        ERR("out of memory\n");
        return;
    }

    work_item = IoAllocateWorkItem(master_devobj);
    if (!work_item) {
        ERR("out of memory\n");
        ExFreePool(nf);
        return;
    }

    InterlockedIncrement(&fileref->refcount);

    nf->fileref = fileref;
    nf->filter_match = filter_match;
    nf->action = action;
    nf->stream = stream ? (PUNICODE_STRING)(nf + 1) : NULL;
    if (stream) {
        nf->stream->Length = nf->stream->MaximumLength = stream->Length;
        nf->stream->Buffer = (WCHAR*)(nf->stream + 1);

        if (stream->Length > 0)
            RtlCopyMemory(nf->stream->Buffer, stream->Buffer, stream->Length);
    }
    nf->work_item = work_item;

    IoQueueWorkItem(work_item, notification_work_item, DelayedWorkQueue, nf);
}
