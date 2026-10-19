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

#include "btrfs_drv.h"

uint32_t get_num_of_processors() {
    KAFFINITY p = KeQueryActiveProcessors();
    uint32_t r = 0;

    while (p != 0) {
        if (p & 1)
            r++;

        p >>= 1;
    }

    return r;
}

NTSTATUS create_calc_threads(_In_ PDEVICE_OBJECT DeviceObject) {
    device_extension* Vcb = DeviceObject->DeviceExtension;
    OBJECT_ATTRIBUTES oa;
    ULONG i, num_threads = get_num_of_processors();

    Vcb->calcthreads.num_threads = 0;

    Vcb->calcthreads.threads = ExAllocatePoolWithTag(NonPagedPool, sizeof(drv_calc_thread) * num_threads, ALLOC_TAG);
    if (!Vcb->calcthreads.threads) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InitializeListHead(&Vcb->calcthreads.job_list);
    KeInitializeSpinLock(&Vcb->calcthreads.spinlock);
    KeInitializeEvent(&Vcb->calcthreads.event, NotificationEvent, false);

    RtlZeroMemory(Vcb->calcthreads.threads, sizeof(drv_calc_thread) * num_threads);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    for (i = 0; i < num_threads; i++) {
        NTSTATUS Status;

        Vcb->calcthreads.threads[i].DeviceObject = DeviceObject;
        Vcb->calcthreads.threads[i].number = i;
        KeInitializeEvent(&Vcb->calcthreads.threads[i].finished, NotificationEvent, false);

        Status = PsCreateSystemThread(&Vcb->calcthreads.threads[i].handle, SYNCHRONIZE, &oa, NULL, NULL, calc_thread, &Vcb->calcthreads.threads[i]);
        if (!NT_SUCCESS(Status)) {
            ERR("PsCreateSystemThread returned %08lx\n", Status);
            stop_calc_threads(Vcb);
            return Status;
        }

        Vcb->calcthreads.num_threads++;
    }

    return STATUS_SUCCESS;
}

void stop_calc_threads(_In_ device_extension* Vcb) {
    ULONG i;

    if (!Vcb->calcthreads.threads)
        return;

    for (i = 0; i < Vcb->calcthreads.num_threads; i++) {
        Vcb->calcthreads.threads[i].quit = true;
    }

    KeSetEvent(&Vcb->calcthreads.event, 0, false);

    // The finished event is signaled before the thread has terminated.
    for (i = 0; i < Vcb->calcthreads.num_threads; i++) {
        ZwWaitForSingleObject(Vcb->calcthreads.threads[i].handle, false, NULL);
        ZwClose(Vcb->calcthreads.threads[i].handle);
    }

    ExFreePool(Vcb->calcthreads.threads);
    Vcb->calcthreads.threads = NULL;
    Vcb->calcthreads.num_threads = 0;
}

void stop_flush_thread(_In_ device_extension* Vcb) {
    LARGE_INTEGER time;

    if (!Vcb->flush_thread_handle)
        return;

    // The caller marks the volume removing before waking this thread.
    time.QuadPart = 0;
    KeSetTimer(&Vcb->flush_thread_timer, time, NULL); // trigger the timer early
    ZwWaitForSingleObject(Vcb->flush_thread_handle, false, NULL);
    ZwClose(Vcb->flush_thread_handle);
    Vcb->flush_thread_handle = NULL;
}

static void do_flush(device_extension* Vcb) {
    NTSTATUS Status;

    ExAcquireResourceExclusiveLite(&Vcb->tree_lock, true);

    if (Vcb->need_write && !Vcb->readonly)
        Status = do_write(Vcb, NULL);
    else
        Status = STATUS_SUCCESS;

    free_trees(Vcb);

    if (!NT_SUCCESS(Status))
        ERR("do_write returned %08lx\n", Status);

    ExReleaseResourceLite(&Vcb->tree_lock);
}

_Function_class_(KSTART_ROUTINE)
void __stdcall flush_thread(void* context) {
    DEVICE_OBJECT* devobj = context;
    device_extension* Vcb = devobj->DeviceExtension;
    LARGE_INTEGER due_time;

    ObReferenceObject(devobj);

    due_time.QuadPart = (uint64_t)Vcb->options.flush_interval * -10000000;

    // Rearming after do_flush can overwrite the shutdown timer wakeup.
    while (!Vcb->removing) {
        KeWaitForSingleObject(&Vcb->flush_thread_timer, Executive, KernelMode, false, NULL);

        if (!(devobj->Vpb->Flags & VPB_MOUNTED) || Vcb->removing)
            break;

        if (!Vcb->locked)
            do_flush(Vcb);

        KeSetTimer(&Vcb->flush_thread_timer, due_time, NULL);
    }

    ObDereferenceObject(devobj);
    KeCancelTimer(&Vcb->flush_thread_timer);

    KeSetEvent(&Vcb->flush_thread_finished, 0, false);

    PsTerminateSystemThread(STATUS_SUCCESS);
}
