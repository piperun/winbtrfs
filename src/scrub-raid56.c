/* Copyright (c) Mark Harmstone 2017
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

/* RAID5/6 batch reads, repair writes and I/O cleanup. */
#include "scrub.h"

_Function_class_(IO_COMPLETION_ROUTINE)
static NTSTATUS __stdcall scrub_read_completion_raid56(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr) {
    scrub_context_raid56_stripe* stripe = conptr;
    scrub_context_raid56* context = (scrub_context_raid56*)stripe->context;
    LONG left = InterlockedDecrement(&context->stripes_left);

    UNUSED(DeviceObject);

    stripe->iosb = Irp->IoStatus;

    if (left == 0)
        KeSetEvent(&context->Event, 0, false);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS scrub_chunk_raid56_stripe_run(device_extension* Vcb, chunk* c, uint64_t stripe_start, uint64_t stripe_end) {
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    bool b;
    uint64_t run_start, run_end, full_stripe_len, stripe;
    uint32_t max_read, num_sectors;
    ULONG arrlen, *allocarr, *csumarr = NULL, *treearr, num_parity_stripes = c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6 ? 2 : 1;
    scrub_context_raid56 context;
    uint16_t i;

    TRACE("(%p, %p, %I64x, %I64x)\n", Vcb, c, stripe_start, stripe_end);

    full_stripe_len = (c->chunk_item->num_stripes - num_parity_stripes) * c->chunk_item->stripe_len;
    run_start = c->offset + (stripe_start * full_stripe_len);
    run_end = c->offset + ((stripe_end + 1) * full_stripe_len);

    searchkey.objectid = run_start;
    searchkey.type = BTRFS_METADATA_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    num_sectors = (uint32_t)(((stripe_end - stripe_start + 1) * full_stripe_len) >> Vcb->sector_shift);
    arrlen = (ULONG)sector_align((num_sectors / 8) + 1, sizeof(ULONG));

    allocarr = ExAllocatePoolWithTag(PagedPool, arrlen, ALLOC_TAG);
    if (!allocarr) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    treearr = ExAllocatePoolWithTag(PagedPool, arrlen, ALLOC_TAG);
    if (!treearr) {
        ERR("out of memory\n");
        ExFreePool(allocarr);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitializeBitMap(&context.alloc, allocarr, num_sectors);
    RtlClearAllBits(&context.alloc);

    RtlInitializeBitMap(&context.is_tree, treearr, num_sectors);
    RtlClearAllBits(&context.is_tree);

    context.parity_scratch = ExAllocatePoolWithTag(PagedPool, (ULONG)c->chunk_item->stripe_len, ALLOC_TAG);
    if (!context.parity_scratch) {
        ERR("out of memory\n");
        ExFreePool(allocarr);
        ExFreePool(treearr);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA) {
        csumarr = ExAllocatePoolWithTag(PagedPool, arrlen, ALLOC_TAG);
        if (!csumarr) {
            ERR("out of memory\n");
            ExFreePool(allocarr);
            ExFreePool(treearr);
            ExFreePool(context.parity_scratch);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlInitializeBitMap(&context.has_csum, csumarr, num_sectors);
        RtlClearAllBits(&context.has_csum);

        context.csum = ExAllocatePoolWithTag(PagedPool, num_sectors * Vcb->csum_size, ALLOC_TAG);
        if (!context.csum) {
            ERR("out of memory\n");
            ExFreePool(allocarr);
            ExFreePool(treearr);
            ExFreePool(context.parity_scratch);
            ExFreePool(csumarr);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6) {
        context.parity_scratch2 = ExAllocatePoolWithTag(PagedPool, (ULONG)c->chunk_item->stripe_len, ALLOC_TAG);
        if (!context.parity_scratch2) {
            ERR("out of memory\n");
            ExFreePool(allocarr);
            ExFreePool(treearr);
            ExFreePool(context.parity_scratch);

            if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA) {
                ExFreePool(csumarr);
                ExFreePool(context.csum);
            }

            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    do {
        traverse_ptr next_tp;

        if (tp.item->key.objectid >= run_end)
            break;

        if (tp.item->key.type == BTRFS_EXTENT_ITEM_KEY || tp.item->key.type == BTRFS_METADATA_ITEM_KEY) {
            uint64_t size = tp.item->key.type == BTRFS_METADATA_ITEM_KEY ? Vcb->superblock.nodesize : tp.item->key.offset;

            if (tp.item->key.objectid + size > run_start) {
                uint64_t extent_start = max(run_start, tp.item->key.objectid);
                uint64_t extent_end = min(tp.item->key.objectid + size, run_end);
                bool extent_is_tree = false;

                RtlSetBits(&context.alloc, (ULONG)((extent_start - run_start) >> Vcb->sector_shift), (ULONG)((extent_end - extent_start) >> Vcb->sector_shift));

                if (tp.item->key.type == BTRFS_METADATA_ITEM_KEY)
                    extent_is_tree = true;
                else {
                    struct btrfs_extent_item* ei = (struct btrfs_extent_item*)tp.item->data;

                    if (tp.item->size < sizeof(struct btrfs_extent_item)) {
                        ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_extent_item));
                        Status = STATUS_INTERNAL_ERROR;
                        goto end;
                    }

                    if (ei->flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
                        extent_is_tree = true;
                }

                if (extent_is_tree)
                    RtlSetBits(&context.is_tree, (ULONG)((extent_start - run_start) >> Vcb->sector_shift), (ULONG)((extent_end - extent_start) >> Vcb->sector_shift));
                else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA) {
                    traverse_ptr tp2;
                    bool b2;

                    searchkey.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
                    searchkey.type = BTRFS_EXTENT_CSUM_KEY;
                    searchkey.offset = extent_start;

                    Status = find_item(Vcb, Vcb->checksum_root, &tp2, &searchkey, false, NULL);
                    if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
                        ERR("find_item returned %08lx\n", Status);
                        goto end;
                    }

                    do {
                        traverse_ptr next_tp2;

                        if (tp2.item->key.offset >= extent_end)
                            break;

                        if (tp2.item->key.offset >= extent_start) {
                            uint64_t csum_start = max(extent_start, tp2.item->key.offset);
                            uint64_t csum_end = min(extent_end, tp2.item->key.offset + (((uint64_t)tp2.item->size << Vcb->sector_shift) / Vcb->csum_size));

                            RtlSetBits(&context.has_csum, (ULONG)((csum_start - run_start) >> Vcb->sector_shift), (ULONG)((csum_end - csum_start) >> Vcb->sector_shift));

                            RtlCopyMemory((uint8_t*)context.csum + (((csum_start - run_start) * Vcb->csum_size) >> Vcb->sector_shift),
                                          tp2.item->data + (((csum_start - tp2.item->key.offset) * Vcb->csum_size) >> Vcb->sector_shift),
                                          (ULONG)(((csum_end - csum_start) * Vcb->csum_size) >> Vcb->sector_shift));
                        }

                        b2 = find_next_item(Vcb, &tp2, &next_tp2, false, NULL);

                        if (b2)
                            tp2 = next_tp2;
                    } while (b2);
                }
            }
        }

        b = find_next_item(Vcb, &tp, &next_tp, false, NULL);

        if (b)
            tp = next_tp;
    } while (b);

    context.stripes = ExAllocatePoolWithTag(PagedPool, sizeof(scrub_context_raid56_stripe) * c->chunk_item->num_stripes, ALLOC_TAG);
    if (!context.stripes) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    max_read = (uint32_t)min(1048576 / c->chunk_item->stripe_len, stripe_end - stripe_start + 1); // only process 1 MB of data at a time

    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        context.stripes[i].buf = ExAllocatePoolWithTag(PagedPool, (ULONG)(max_read * c->chunk_item->stripe_len), ALLOC_TAG);
        if (!context.stripes[i].buf) {
            uint64_t j;

            ERR("out of memory\n");

            for (j = 0; j < i; j++) {
                ExFreePool(context.stripes[j].buf);
                ExFreePool(context.stripes[j].errorarr);
            }
            ExFreePool(context.stripes);

            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        context.stripes[i].errorarr = ExAllocatePoolWithTag(PagedPool, (ULONG)sector_align(((c->chunk_item->stripe_len >> Vcb->sector_shift) / 8) + 1, sizeof(ULONG)), ALLOC_TAG);
        if (!context.stripes[i].errorarr) {
            uint64_t j;

            ERR("out of memory\n");

            ExFreePool(context.stripes[i].buf);

            for (j = 0; j < i; j++) {
                ExFreePool(context.stripes[j].buf);
                ExFreePool(context.stripes[j].errorarr);
            }
            ExFreePool(context.stripes);

            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        RtlInitializeBitMap(&context.stripes[i].error, context.stripes[i].errorarr, (ULONG)(c->chunk_item->stripe_len >> Vcb->sector_shift));

        context.stripes[i].context = &context;
        context.stripes[i].Irp = NULL;
    }

    stripe = stripe_start;

    Status = chunk_lock_range(Vcb, c, run_start, run_end - run_start);
    if (!NT_SUCCESS(Status))
        goto end2;

    do {
        ULONG read_stripes;
        uint16_t missing_devices = 0;
        bool need_wait = false;

        if (max_read < stripe_end + 1 - stripe)
            read_stripes = max_read;
        else
            read_stripes = (ULONG)(stripe_end + 1 - stripe);

        context.stripes_left = c->chunk_item->num_stripes;

        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            context.stripes[i].rewrite = false;
        }

        // read megabyte by megabyte
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (c->devices[i]->devobj) {
                PIO_STACK_LOCATION IrpSp;

                context.stripes[i].Irp = IoAllocateIrp(c->devices[i]->devobj->StackSize, false);

                if (!context.stripes[i].Irp) {
                    ERR("IoAllocateIrp failed\n");
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto end4;
                }

                context.stripes[i].Irp->MdlAddress = NULL;

                IrpSp = IoGetNextIrpStackLocation(context.stripes[i].Irp);
                IrpSp->MajorFunction = IRP_MJ_READ;
                IrpSp->FileObject = c->devices[i]->fileobj;

                if (c->devices[i]->devobj->Flags & DO_BUFFERED_IO) {
                    context.stripes[i].Irp->AssociatedIrp.SystemBuffer = ExAllocatePoolWithTag(NonPagedPool, (ULONG)(read_stripes * c->chunk_item->stripe_len), ALLOC_TAG);
                    if (!context.stripes[i].Irp->AssociatedIrp.SystemBuffer) {
                        ERR("out of memory\n");
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        goto end4;
                    }

                    context.stripes[i].Irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER | IRP_INPUT_OPERATION;

                    context.stripes[i].Irp->UserBuffer = context.stripes[i].buf;
                } else if (c->devices[i]->devobj->Flags & DO_DIRECT_IO) {
                    context.stripes[i].Irp->MdlAddress = IoAllocateMdl(context.stripes[i].buf, (ULONG)(read_stripes * c->chunk_item->stripe_len), false, false, NULL);
                    if (!context.stripes[i].Irp->MdlAddress) {
                        ERR("IoAllocateMdl failed\n");
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        goto end4;
                    }

                    Status = STATUS_SUCCESS;

                    try {
                        MmProbeAndLockPages(context.stripes[i].Irp->MdlAddress, KernelMode, IoWriteAccess);
                    } except (EXCEPTION_EXECUTE_HANDLER) {
                        Status = GetExceptionCode();
                    }

                    if (!NT_SUCCESS(Status)) {
                        ERR("MmProbeAndLockPages threw exception %08lx\n", Status);
                        IoFreeMdl(context.stripes[i].Irp->MdlAddress);
                        context.stripes[i].Irp->MdlAddress = NULL;
                        goto end4;
                    }
                } else
                    context.stripes[i].Irp->UserBuffer = context.stripes[i].buf;

                context.stripes[i].offset = stripe * c->chunk_item->stripe_len;

                IrpSp->Parameters.Read.Length = (ULONG)(read_stripes * c->chunk_item->stripe_len);
                IrpSp->Parameters.Read.ByteOffset.QuadPart = c->chunk_item->stripe[i].offset + context.stripes[i].offset;

                context.stripes[i].Irp->UserIosb = &context.stripes[i].iosb;
                context.stripes[i].missing = false;

                IoSetCompletionRoutine(context.stripes[i].Irp, scrub_read_completion_raid56, &context.stripes[i], true, true, true);

                Vcb->scrub.data_scrubbed += read_stripes * c->chunk_item->stripe_len;
                need_wait = true;
            } else {
                context.stripes[i].Irp = NULL;
                context.stripes[i].missing = true;
                missing_devices++;
                InterlockedDecrement(&context.stripes_left);
            }
        }

        if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5 && missing_devices > 1) {
            ERR("too many missing devices (%u, maximum 1)\n", missing_devices);
            Status = STATUS_UNEXPECTED_IO_ERROR;
            goto end4;
        } else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6 && missing_devices > 2) {
            ERR("too many missing devices (%u, maximum 2)\n", missing_devices);
            Status = STATUS_UNEXPECTED_IO_ERROR;
            goto end4;
        }

        if (need_wait) {
            KeInitializeEvent(&context.Event, NotificationEvent, false);

            for (i = 0; i < c->chunk_item->num_stripes; i++) {
                if (c->devices[i]->devobj)
                    IoCallDriver(c->devices[i]->devobj, context.stripes[i].Irp);
            }

            KeWaitForSingleObject(&context.Event, Executive, KernelMode, false, NULL);
        }

        // return an error if any of the stripes returned an error
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (!context.stripes[i].missing && !NT_SUCCESS(context.stripes[i].iosb.Status)) {
                Status = context.stripes[i].iosb.Status;
                log_device_error(Vcb, c->devices[i], BTRFS_DEV_STAT_READ_ERRS);
                goto end4;
            }
        }

        if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6) {
            for (i = 0; i < read_stripes; i++) {
                Status = scrub_raid6_stripe(Vcb, c, &context, stripe_start,
                                            stripe, i, missing_devices);
                if (!NT_SUCCESS(Status)) {
                    ERR("scrub_raid6_stripe returned %08lx\n", Status);
                    goto end4;
                }
            }
        } else {
            for (i = 0; i < read_stripes; i++) {
                scrub_raid5_stripe(Vcb, c, &context, stripe_start, stripe, i, missing_devices);
            }
        }
        stripe += read_stripes;

end4:
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (context.stripes[i].Irp) {
                if (c->devices[i]->devobj->Flags & DO_DIRECT_IO && context.stripes[i].Irp->MdlAddress) {
                    MmUnlockPages(context.stripes[i].Irp->MdlAddress);
                    IoFreeMdl(context.stripes[i].Irp->MdlAddress);
                }
                IoFreeIrp(context.stripes[i].Irp);
                context.stripes[i].Irp = NULL;
            }
        }

        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            if (c->devices[i]->devobj && context.stripes[i].rewrite) {
                Status = write_data_phys(c->devices[i]->devobj, c->devices[i]->fileobj, c->chunk_item->stripe[i].offset + context.stripes[i].offset,
                                         context.stripes[i].buf, (uint32_t)(read_stripes * c->chunk_item->stripe_len));

                if (!NT_SUCCESS(Status)) {
                    ERR("write_data_phys returned %08lx\n", Status);
                    log_device_error(Vcb, c->devices[i], BTRFS_DEV_STAT_WRITE_ERRS);
                    goto end3;
                }
            }
        }

        if (!NT_SUCCESS(Status))
            break;
    } while (stripe <= stripe_end);

end3:
    chunk_unlock_range(Vcb, c, run_start, run_end - run_start);

end2:
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        ExFreePool(context.stripes[i].buf);
        ExFreePool(context.stripes[i].errorarr);
    }
    ExFreePool(context.stripes);

end:
    ExFreePool(treearr);
    ExFreePool(allocarr);
    ExFreePool(context.parity_scratch);

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)
        ExFreePool(context.parity_scratch2);

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DATA) {
        ExFreePool(csumarr);
        ExFreePool(context.csum);
    }

    return Status;
}
