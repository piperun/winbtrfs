/* Transaction-local checksum readahead. The caller keeps tree_lock exclusive. */
#include "btrfs_drv.h"
#include "metadata-prefetch.h"
#include "metadata-prefetch-private.h"

void preload_checksum_trees(device_extension* Vcb) {
    prefetch_context* ctx;
    size_t planning;

    if (!Vcb->checksum_root || !Vcb->log_to_phys_loaded ||
        Vcb->superblock.nodesize < sizeof(struct btrfs_header) ||
        Vcb->superblock.nodesize > 65536 || Vcb->sector_shift >= 32)
        return;

    planning = sizeof(prefetch_context) + sizeof(prefetch_range) * PREFETCH_MAX_RANGES +
               sizeof(prefetch_job) * PREFETCH_MAX_JOBS +
               (size_t)PREFETCH_READERS * Vcb->superblock.nodesize;
    if (planning > PREFETCH_PLAN_BUDGET)
        return;

    ctx = ExAllocatePoolWithTag(NonPagedPool, sizeof(*ctx), ALLOC_TAG);
    if (!ctx)
        return;
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Vcb = Vcb;
    ctx->ranges = ExAllocatePoolWithTag(PagedPool, sizeof(prefetch_range) * PREFETCH_MAX_RANGES, ALLOC_TAG);
    ctx->jobs = ExAllocatePoolWithTag(PagedPool, sizeof(prefetch_job) * PREFETCH_MAX_JOBS, ALLOC_TAG);
    if (ctx->ranges && ctx->jobs) {
        prefetch_collect_ranges(ctx);
        if (ctx->range_count) {
            prefetch_plan_trees(ctx);
            if (ctx->job_count >= PREFETCH_MIN_JOBS)
                prefetch_read_jobs(ctx);
        }
    }

    if (ctx->jobs)
        ExFreePool(ctx->jobs);
    if (ctx->ranges)
        ExFreePool(ctx->ranges);
    ExFreePool(ctx);
}
