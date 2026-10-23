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

#include "trim.h"
#include <ntddstor.h>

typedef struct {
    DEVICE_MANAGE_DATA_SET_ATTRIBUTES* dmdsa;
    PIRP Irp;
    IO_STATUS_BLOCK iosb;
#ifdef DEBUG_TRIM_EMULATION
    PMDL mdl;
    void* buf;
#endif
} trim_context_stripe;

typedef struct {
    KEVENT Event;
    LONG left;
    trim_context_stripe* stripes;
} trim_context;

_Function_class_(IO_COMPLETION_ROUTINE)
static NTSTATUS __stdcall trim_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr) {
    trim_context* context = (trim_context*)conptr;
    LONG left2 = InterlockedDecrement(&context->left);

    UNUSED(DeviceObject);
    UNUSED(Irp);

    if (left2 == 0)
        KeSetEvent(&context->Event, 0, false);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

#ifdef DEBUG_TRIM_EMULATION
static void trim_emulation(device* dev) {
    LIST_ENTRY* le;
    trim_context context;
    unsigned int i = 0, count = 0;

    le = dev->trim_list.Flink;
    while (le != &dev->trim_list) {
        count++;
        le = le->Flink;
    }

    context.left = count;

    KeInitializeEvent(&context.Event, NotificationEvent, false);

    context.stripes = ExAllocatePoolWithTag(NonPagedPool, sizeof(trim_context_stripe) * context.left, ALLOC_TAG);
    if (!context.stripes) {
        ERR("out of memory\n");
        return;
    }

    RtlZeroMemory(context.stripes, sizeof(trim_context_stripe) * context.left);

    i = 0;
    le = dev->trim_list.Flink;
    while (le != &dev->trim_list) {
        trim_context_stripe* stripe = &context.stripes[i];
        space* s = CONTAINING_RECORD(le, space, list_entry);

        WARN("(%I64x, %I64x)\n", s->address, s->size);

        stripe->Irp = IoAllocateIrp(dev->devobj->StackSize, false);

        if (!stripe->Irp) {
            ERR("IoAllocateIrp failed\n");
        } else {
            PIO_STACK_LOCATION IrpSp = IoGetNextIrpStackLocation(stripe->Irp);
            IrpSp->MajorFunction = IRP_MJ_WRITE;
            IrpSp->FileObject = dev->fileobj;

            stripe->buf = ExAllocatePoolWithTag(NonPagedPool, (uint32_t)s->size, ALLOC_TAG);

            if (!stripe->buf) {
                ERR("out of memory\n");
            } else {
                RtlZeroMemory(stripe->buf, (uint32_t)s->size); // FIXME - randomize instead?

                stripe->mdl = IoAllocateMdl(stripe->buf, (uint32_t)s->size, false, false, NULL);

                if (!stripe->mdl) {
                    ERR("IoAllocateMdl failed\n");
                } else {
                    MmBuildMdlForNonPagedPool(stripe->mdl);

                    stripe->Irp->MdlAddress = stripe->mdl;

                    IrpSp->Parameters.Write.ByteOffset.QuadPart = s->address;
                    IrpSp->Parameters.Write.Length = s->size;

                    stripe->Irp->UserIosb = &stripe->iosb;

                    IoSetCompletionRoutine(stripe->Irp, trim_completion, &context, true, true, true);

                    IoCallDriver(dev->devobj, stripe->Irp);
                }
            }
        }

        i++;

        le = le->Flink;
    }

    KeWaitForSingleObject(&context.Event, Executive, KernelMode, false, NULL);

    for (i = 0; i < count; i++) {
        trim_context_stripe* stripe = &context.stripes[i];

        if (stripe->mdl)
            IoFreeMdl(stripe->mdl);

        if (stripe->buf)
            ExFreePool(stripe->buf);
    }

    ExFreePool(context.stripes);
}
#endif

NTSTATUS trim_devices(device_extension* Vcb) {
    LIST_ENTRY* le;
#ifndef DEBUG_TRIM_EMULATION
    ULONG num;
#endif

#ifndef DEBUG_TRIM_EMULATION
    trim_context context;
    ULONG total_num;

    context.left = 0;

    if (!Vcb->trim || Vcb->options.no_trim)
        return STATUS_SUCCESS;

    le = Vcb->devices.Flink;
    while (le != &Vcb->devices) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);

        if (dev->devobj && !dev->readonly && dev->trim && dev->num_trim_entries > 0)
            context.left++;

        le = le->Flink;
    }

    if (context.left == 0)
        return STATUS_SUCCESS;

    total_num = context.left;
    num = 0;

    KeInitializeEvent(&context.Event, NotificationEvent, false);

    context.stripes = ExAllocatePoolWithTag(NonPagedPool, sizeof(trim_context_stripe) * context.left, ALLOC_TAG);
    if (!context.stripes) {
        ERR("out of memory\n");
        return STATUS_SUCCESS; // no need to go readonly, just skip TRIM
    }

    RtlZeroMemory(context.stripes, sizeof(trim_context_stripe) * context.left);
#endif

    le = Vcb->devices.Flink;
    while (le != &Vcb->devices) {
        device* dev = CONTAINING_RECORD(le, device, list_entry);

        if (dev->devobj && !dev->readonly && dev->trim && dev->num_trim_entries > 0) {
#ifdef DEBUG_TRIM_EMULATION
            trim_emulation(dev);
#else
            LIST_ENTRY* le2;
            trim_context_stripe* stripe = &context.stripes[num];
            DEVICE_DATA_SET_RANGE* ranges;
            ULONG datalen = (ULONG)sector_align(sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES), sizeof(uint64_t)) + (dev->num_trim_entries * sizeof(DEVICE_DATA_SET_RANGE)), i;
            PIO_STACK_LOCATION IrpSp;

            stripe->dmdsa = ExAllocatePoolWithTag(PagedPool, datalen, ALLOC_TAG);
            if (!stripe->dmdsa) {
                ERR("out of memory\n");
                goto skipdev;
            }

            stripe->dmdsa->Size = sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES);
            stripe->dmdsa->Action = DeviceDsmAction_Trim;
            stripe->dmdsa->Flags = DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED;
            stripe->dmdsa->ParameterBlockOffset = 0;
            stripe->dmdsa->ParameterBlockLength = 0;
            stripe->dmdsa->DataSetRangesOffset = (ULONG)sector_align(sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES), sizeof(uint64_t));
            stripe->dmdsa->DataSetRangesLength = dev->num_trim_entries * sizeof(DEVICE_DATA_SET_RANGE);

            ranges = (DEVICE_DATA_SET_RANGE*)((uint8_t*)stripe->dmdsa + stripe->dmdsa->DataSetRangesOffset);

            i = 0;

            le2 = dev->trim_list.Flink;
            while (le2 != &dev->trim_list) {
                space* s = CONTAINING_RECORD(le2, space, list_entry);

                ranges[i].StartingOffset = s->address;
                ranges[i].LengthInBytes = s->size;
                i++;

                le2 = le2->Flink;
            }

            stripe->Irp = IoAllocateIrp(dev->devobj->StackSize, false);

            if (!stripe->Irp) {
                ERR("IoAllocateIrp failed\n");
                goto skipdev;
            }

            IrpSp = IoGetNextIrpStackLocation(stripe->Irp);
            IrpSp->MajorFunction = IRP_MJ_DEVICE_CONTROL;
            IrpSp->FileObject = dev->fileobj;

            IrpSp->Parameters.DeviceIoControl.IoControlCode = IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES;
            IrpSp->Parameters.DeviceIoControl.InputBufferLength = datalen;
            IrpSp->Parameters.DeviceIoControl.OutputBufferLength = 0;

            stripe->Irp->AssociatedIrp.SystemBuffer = stripe->dmdsa;
            stripe->Irp->Flags |= IRP_BUFFERED_IO;
            stripe->Irp->UserBuffer = NULL;
            stripe->Irp->UserIosb = &stripe->iosb;

            IoSetCompletionRoutine(stripe->Irp, trim_completion, &context, true, true, true);

            IoCallDriver(dev->devobj, stripe->Irp);
            goto nextdev;

skipdev:
            // This device will not produce a completion, so retire its pending slot.
            if (InterlockedDecrement(&context.left) == 0)
                KeSetEvent(&context.Event, 0, false);

nextdev:
#endif
            while (!IsListEmpty(&dev->trim_list)) {
                space* s = CONTAINING_RECORD(RemoveHeadList(&dev->trim_list), space, list_entry);
                ExFreePool(s);
            }

            dev->num_trim_entries = 0;

#ifndef DEBUG_TRIM_EMULATION
            num++;
#endif
        }

        le = le->Flink;
    }

#ifndef DEBUG_TRIM_EMULATION
    KeWaitForSingleObject(&context.Event, Executive, KernelMode, false, NULL);

    for (num = 0; num < total_num; num++) {
        if (context.stripes[num].dmdsa)
            ExFreePool(context.stripes[num].dmdsa);

        if (context.stripes[num].Irp)
            IoFreeIrp(context.stripes[num].Irp);
    }

    ExFreePool(context.stripes);
#endif

    return STATUS_SUCCESS;
}
