/* Bounded dirty-range snapshot and one canonical checksum-tree walk. */
#include "btrfs_drv.h"
#include "metadata-prefetch-private.h"

static void sift_ranges(prefetch_range* ranges, uint32_t count, uint32_t pos) {
    while (pos < count / 2) {
        uint32_t child = 2 * pos + 1;
        prefetch_range swap;

        if (child + 1 < count && ranges[child].start < ranges[child + 1].start)
            child++;
        if (ranges[pos].start >= ranges[child].start)
            return;
        swap = ranges[pos]; ranges[pos] = ranges[child]; ranges[child] = swap;
        pos = child;
    }
}

static void merge_ranges(prefetch_context* ctx) {
    uint32_t i, merged = 0, count = ctx->range_count;

    for (i = count / 2; i > 0; i--)
        sift_ranges(ctx->ranges, count, i - 1);
    for (i = count; i > 1; i--) {
        prefetch_range swap = ctx->ranges[0];
        ctx->ranges[0] = ctx->ranges[i - 1]; ctx->ranges[i - 1] = swap;
        sift_ranges(ctx->ranges, i - 1, 0);
    }
    for (i = 0; i < count; i++) {
        prefetch_range range = ctx->ranges[i];
        if (merged && range.start <= ctx->ranges[merged - 1].end) {
            if (range.end > ctx->ranges[merged - 1].end)
                ctx->ranges[merged - 1].end = range.end;
        } else
            ctx->ranges[merged++] = range;
    }
    ctx->range_count = merged;
}

void prefetch_collect_ranges(prefetch_context* ctx) {
    device_extension* Vcb = ctx->Vcb;
    LIST_ENTRY* le;

    ExAcquireResourceExclusiveLite(&Vcb->dirty_fcbs_lock, true);
    for (le = Vcb->dirty_fcbs.Flink; le != &Vcb->dirty_fcbs && ctx->range_count < PREFETCH_MAX_RANGES; le = le->Flink) {
        fcb* file = CONTAINING_RECORD(le, fcb, list_entry_dirty);
        LIST_ENTRY* item;

        ExAcquireResourceExclusiveLite(file->Header.Resource, true);
        if (!file->deleted && !file->ads && file->extents_changed) {
            for (item = file->extents.Flink; item != &file->extents && ctx->range_count < PREFETCH_MAX_RANGES; item = item->Flink) {
                extent* ext = CONTAINING_RECORD(item, extent, list_entry);
                uint64_t start, length;

                if (ext->ignore || !ext->inserted || !ext->csum ||
                    ext->extent_data.type != BTRFS_FILE_EXTENT_REG || !ext->extent_data.disk_num_bytes)
                    continue;
                start = ext->extent_data.disk_bytenr;
                if (ext->extent_data.compression == BTRFS_COMPRESS_NONE) {
                    if (ext->extent_data.offset > (uint64_t)-1 - start)
                        continue;
                    start += ext->extent_data.offset;
                    length = ext->extent_data.num_bytes;
                } else
                    length = ext->extent_data.disk_num_bytes;
                // flush_fcb passes a 32-bit sector count to add_checksum_entry.
                length >>= Vcb->sector_shift;
                if (!length || length > (uint32_t)-1)
                    continue;
                length <<= Vcb->sector_shift;
                if (length > (uint64_t)-1 - start)
                    continue;
                ctx->ranges[ctx->range_count].start = start;
                ctx->ranges[ctx->range_count++].end = start + length;
            }
        }
        ExReleaseResourceLite(file->Header.Resource);
    }
    ExReleaseResourceLite(&Vcb->dirty_fcbs_lock);
    merge_ranges(ctx);
}

size_t prefetch_tree_bytes(uint32_t nodesize, uint32_t count, uint8_t level) {
    // Reserve the raw input even for internal nodes whose loader frees it.
    return nodesize + sizeof(tree) + (size_t)count * sizeof(tree_data) +
           (level ? sizeof(tree_nonpaged) + (size_t)count * sizeof(tree_data*) : 0);
}

static bool intersects_ranges(prefetch_context* ctx, const struct btrfs_key* lower, const struct btrfs_key* upper) {
    uint32_t low = 0, high = ctx->range_count;
    struct btrfs_key key = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, 0 };

    // Find the first inclusive end not below this child interval.
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        key.offset = ctx->ranges[mid].end;
        if (lower && keycmp(key, (*lower)) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    if (low == ctx->range_count)
        return false;
    key.offset = ctx->ranges[low].start;
    return !upper || keycmp(key, (*upper)) < 0;
}

static tree* discover_internal(prefetch_context* ctx, tree_holder* holder, tree* parent, tree_data* item, uint8_t level) {
    device_extension* Vcb = ctx->Vcb;
    size_t item_size = sizeof(struct btrfs_key_ptr) < sizeof(struct btrfs_item) ? sizeof(struct btrfs_key_ptr) : sizeof(struct btrfs_item);
    uint32_t count = (uint32_t)((Vcb->superblock.nodesize - sizeof(struct btrfs_header)) / item_size);
    size_t reserve = prefetch_tree_bytes(Vcb->superblock.nodesize, count, level);
    NTSTATUS status;

    if (holder->tree)
        return holder->tree;
    if (!prefetch_io_bytes(Vcb, holder->address) || reserve > PREFETCH_TREE_BUDGET - ctx->tree_bytes) {
        ctx->full = true;
        return NULL;
    }
    status = do_load_tree(Vcb, holder, Vcb->checksum_root, parent, item, NULL);
    if (!NT_SUCCESS(status))
        return NULL;
    // Lookup skips an already loaded empty child backward, whereas an unopened
    // empty child returns NOT_FOUND after loading. Preserve the demand state.
    if (IsListEmpty(&holder->tree->itemlist)) {
        free_tree(holder->tree);
        return NULL;
    }
    ctx->tree_bytes += prefetch_tree_bytes(Vcb->superblock.nodesize, holder->tree->header.nritems, holder->tree->header.level);
    return holder->tree;
}

static void walk_checksum_children(prefetch_context* ctx, tree* node,
                                    const struct btrfs_key* lower, const struct btrfs_key* upper, uint8_t level) {
    LIST_ENTRY* le;
    tree_data* previous = NULL;
    uint32_t count = 0;

    if (!node || node->header.level != level || !level || level > 8)
        return;
    // Changed, ignored or duplicate separators have no optional interpretation.
    for (le = node->itemlist.Flink; le != &node->itemlist; le = le->Flink) {
        tree_data* item = CONTAINING_RECORD(le, tree_data, list_entry);
        if (item->ignore || (previous && keycmp(previous->key, item->key) >= 0))
            return;
        previous = item;
        count++;
    }
    if (!count || count != node->header.nritems)
        return;

    for (le = node->itemlist.Flink; le != &node->itemlist && !ctx->full; le = le->Flink) {
        tree_data* item = CONTAINING_RECORD(le, tree_data, list_entry);
        const struct btrfs_key* lo = le == node->itemlist.Flink ? lower : &item->key;
        const struct btrfs_key* hi = le->Flink == &node->itemlist ? upper :
            &CONTAINING_RECORD(le->Flink, tree_data, list_entry)->key;
        bool direct = intersects_ranges(ctx, lo, hi);

        if (!direct && !ctx->successor)
            continue;
        if (level == 1) {
            ctx->successor = direct;
            if (!item->treeholder.tree && prefetch_io_bytes(ctx->Vcb, item->treeholder.address)) {
                prefetch_job* job = &ctx->jobs[ctx->job_count++];
                job->address = item->treeholder.address;
                job->generation = item->treeholder.generation;
                job->parent = node;
                job->item = item;
                if (ctx->job_count == PREFETCH_MAX_JOBS)
                    ctx->full = true;
            }
        } else {
            tree* child = discover_internal(ctx, &item->treeholder, node, item, level - 1);
            walk_checksum_children(ctx, child, lo, hi, level - 1);
        }
    }
}

void prefetch_plan_trees(prefetch_context* ctx) {
    root* r = ctx->Vcb->checksum_root;
    tree* node = r->treeholder.tree;
    uint8_t level = node ? node->header.level : r->root_item.level;

    if (!level || level > 8)
        return;
    node = discover_internal(ctx, &r->treeholder, NULL, NULL, level);
    walk_checksum_children(ctx, node, NULL, NULL, level);
}
