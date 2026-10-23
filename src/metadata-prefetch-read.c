/* Temporary readers only fetch bytes; the lock-owning thread publishes them. */
#include "btrfs_drv.h"
#include "tree-load.h"
#include "metadata-prefetch-private.h"

size_t prefetch_io_bytes(device_extension* Vcb, uint64_t address) {
    chunk* c;
    uint64_t profile;
    uint16_t i;
    size_t irp = 0, mdl, stripe;
    uint32_t nodesize = Vcb->superblock.nodesize;

    if (!Vcb->log_to_phys_loaded)
        return 0;
    c = get_chunk_from_address(Vcb, address);
    if (!c || !c->chunk_item || !c->chunk_item->num_stripes || c->chunk_item->num_stripes > 4 ||
        address < c->offset || address - c->offset > c->chunk_item->length ||
        nodesize > c->chunk_item->length - (address - c->offset))
        return 0;
    profile = c->chunk_item->type & ~(uint64_t)(BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_METADATA);
    if (profile != 0 && profile != BTRFS_BLOCK_GROUP_DUP && profile != BTRFS_BLOCK_GROUP_RAID1 &&
        profile != BTRFS_BLOCK_GROUP_RAID1C3 && profile != BTRFS_BLOCK_GROUP_RAID1C4)
        return 0;
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        unsigned int stack = c->devices && c->devices[i] && c->devices[i]->devobj ? c->devices[i]->devobj->StackSize : 1;
        size_t bytes;
        if (stack > 64)
            return 0;
        bytes = IoSizeOfIrp((CCHAR)stack);
        if (bytes > irp)
            irp = bytes;
    }
    // DUP/SINGLE has one selected stripe. Reserve all stripes anyway, plus
    // recovery's temporary node and synchronous read/write IRPs and MDLs.
    // 256 bounds read_data_stripe's private bookkeeping (currently <128).
    mdl = sizeof(MDL) + ((nodesize + PAGE_SIZE - 1) / PAGE_SIZE + 1) * sizeof(PFN_NUMBER);
    stripe = 256 + irp + mdl + nodesize;
    return (size_t)c->chunk_item->num_stripes * stripe + 2 * (irp + mdl + nodesize);
}

_Function_class_(KSTART_ROUTINE)
static void __stdcall prefetch_reader_thread(void* argument) {
    prefetch_reader* reader = argument;

    for (;;) {
        KeWaitForSingleObject(&reader->work, Executive, KernelMode, false, NULL);
        if (reader->stop)
            break;
        reader->status = read_data(reader->Vcb, reader->job.address, reader->Vcb->superblock.nodesize,
                                   NULL, true, reader->buffer, NULL, NULL, NULL,
                                   reader->job.generation, false, NormalPagePriority);
        KeSetEvent(&reader->done, 0, false);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void prefetch_read_jobs(prefetch_context* ctx) {
    OBJECT_ATTRIBUTES oa;
    uint32_t started = 0, next = 0, i;
    uint32_t nodesize = ctx->Vcb->superblock.nodesize;

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    for (i = 0; i < PREFETCH_READERS; i++) {
        prefetch_reader* reader = &ctx->readers[i];
        NTSTATUS status;

        reader->Vcb = ctx->Vcb;
        KeInitializeEvent(&reader->work, SynchronizationEvent, false);
        KeInitializeEvent(&reader->done, NotificationEvent, false);
        status = PsCreateSystemThread(&reader->handle, SYNCHRONIZE, &oa, NULL, NULL, prefetch_reader_thread, reader);
        if (!NT_SUCCESS(status))
            goto stop;
        started++;
    }

    while (next < ctx->job_count) {
        uint32_t group = 0;
        size_t io_bytes = 0;
        bool exhausted = false;

        while (next < ctx->job_count && group < started) {
            prefetch_job* job = &ctx->jobs[next];
            size_t bytes = prefetch_io_bytes(ctx->Vcb, job->address);
            prefetch_reader* reader = &ctx->readers[group];

            if (!bytes || bytes > PREFETCH_IO_BUDGET) {
                next++;
                continue;
            }
            if (bytes > PREFETCH_IO_BUDGET - io_bytes)
                break;
            reader->buffer = ExAllocatePoolWithTag(PagedPool, nodesize, ALLOC_TAG);
            if (!reader->buffer)
                goto stop;
            reader->job = *job;
            io_bytes += bytes;
            group++;
            next++;
        }
        for (i = 0; i < group; i++) {
            KeClearEvent(&ctx->readers[i].done);
            KeSetEvent(&ctx->readers[i].work, 0, false);
        }
        for (i = 0; i < group; i++) {
            prefetch_reader* reader = &ctx->readers[i];
            struct btrfs_header* header;
            size_t bytes;

            KeWaitForSingleObject(&reader->done, Executive, KernelMode, false, NULL);
            header = (struct btrfs_header*)reader->buffer;
            if (NT_SUCCESS(reader->status) && !header->level &&
                header->nritems <= (nodesize - sizeof(*header)) / sizeof(struct btrfs_item)) {
                bytes = prefetch_tree_bytes(nodesize, header->nritems, 0);
                if (bytes > PREFETCH_TREE_BUDGET - ctx->tree_bytes)
                    exhausted = true;
                else if (publish_prefetched_tree(ctx->Vcb, &reader->job.item->treeholder, ctx->Vcb->checksum_root,
                                         reader->job.parent, reader->job.item, reader->job.address,
                                         reader->job.generation, reader->buffer)) {
                    ctx->tree_bytes += bytes;
                    reader->buffer = NULL;
                }
            }
            if (reader->buffer) {
                ExFreePool(reader->buffer);
                reader->buffer = NULL;
            }
        }
        if (exhausted || ctx->tree_bytes >= PREFETCH_TREE_BUDGET)
            break;
    }

stop:
    // No work is in flight at an allocation/creation failure or loop exit.
    for (i = 0; i < started; i++) {
        ctx->readers[i].stop = true;
        KeSetEvent(&ctx->readers[i].work, 0, false);
    }
    for (i = 0; i < started; i++) {
        ZwWaitForSingleObject(ctx->readers[i].handle, false, NULL);
        ZwClose(ctx->readers[i].handle);
        if (ctx->readers[i].buffer)
            ExFreePool(ctx->readers[i].buffer);
    }
}
