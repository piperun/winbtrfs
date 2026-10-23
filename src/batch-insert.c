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

// Bounded transaction item insertion and ordered sublist splitting.
#include "btrfs_drv.h"
#include "transaction-chunks.h"

#define BATCH_ITEM_LIMIT 1000

static NTSTATUS split_batch_item_list(batch_item_ind* bii) {
    LIST_ENTRY* le;
    unsigned int i = 0;
    LIST_ENTRY* midpoint = NULL;
    batch_item_ind* bii2;
    batch_item* midpoint_item;
    LIST_ENTRY* before_midpoint;

    le = bii->items.Flink;
    while (le != &bii->items) {
        if (i >= bii->num_items / 2) {
            midpoint = le;
            break;
        }

        i++;

        le = le->Flink;
    }

    if (!midpoint)
        return STATUS_SUCCESS;

    // make sure items on either side of split don't have same key

    while (midpoint->Blink != &bii->items) {
        batch_item* item = CONTAINING_RECORD(midpoint, batch_item, list_entry);
        batch_item* prev = CONTAINING_RECORD(midpoint->Blink, batch_item, list_entry);

        if (item->key.objectid != prev->key.objectid)
            break;

        if (item->key.type != prev->key.type)
            break;

        if (item->key.offset != prev->key.offset)
            break;

        midpoint = midpoint->Blink;
        i--;
    }

    if (midpoint->Blink == &bii->items)
        return STATUS_SUCCESS;

    bii2 = ExAllocatePoolWithTag(PagedPool, sizeof(batch_item_ind), ALLOC_TAG);
    if (!bii2) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    midpoint_item = CONTAINING_RECORD(midpoint, batch_item, list_entry);

    bii2->key.objectid = midpoint_item->key.objectid;
    bii2->key.type = midpoint_item->key.type;
    bii2->key.offset = midpoint_item->key.offset;

    bii2->num_items = bii->num_items - i;
    bii->num_items = i;

    before_midpoint = midpoint->Blink;

    bii2->items.Flink = midpoint;
    midpoint->Blink = &bii2->items;
    bii2->items.Blink = bii->items.Blink;
    bii->items.Blink->Flink = &bii2->items;

    bii->items.Blink = before_midpoint;
    before_midpoint->Flink = &bii->items;

    InsertHeadList(&bii->list_entry, &bii2->list_entry);

    return STATUS_SUCCESS;
}

NTSTATUS insert_tree_item_batch(LIST_ENTRY* batchlist, device_extension* Vcb, root* r, uint64_t objid,
                                       uint8_t objtype, uint64_t offset, _In_opt_ _When_(return >= 0, __drv_aliasesMem) void* data,
                                       uint16_t datalen, enum batch_operation operation) {
    LIST_ENTRY* le;
    batch_root* br = NULL;
    batch_item* bi;

    le = batchlist->Flink;
    while (le != batchlist) {
        batch_root* br2 = CONTAINING_RECORD(le, batch_root, list_entry);

        if (br2->r == r) {
            br = br2;
            break;
        }

        le = le->Flink;
    }

    if (!br) {
        br = ExAllocatePoolWithTag(PagedPool, sizeof(batch_root), ALLOC_TAG);
        if (!br) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        br->r = r;
        InitializeListHead(&br->items_ind);
        InsertTailList(batchlist, &br->list_entry);
    }

    if (IsListEmpty(&br->items_ind)) {
        batch_item_ind* bii;

        bii = ExAllocatePoolWithTag(PagedPool, sizeof(batch_item_ind), ALLOC_TAG);
        if (!bii) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        bii->key.objectid = 0;
        bii->key.type = 0;
        bii->key.offset = 0;
        InitializeListHead(&bii->items);
        bii->num_items = 0;
        InsertTailList(&br->items_ind, &bii->list_entry);
    }

    bi = ExAllocateFromPagedLookasideList(&Vcb->batch_item_lookaside);
    if (!bi) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    bi->key.objectid = objid;
    bi->key.type = objtype;
    bi->key.offset = offset;
    bi->data = data;
    bi->datalen = datalen;
    bi->operation = operation;

    le = br->items_ind.Blink;
    while (le != &br->items_ind) {
        LIST_ENTRY* le2;
        batch_item_ind* bii = CONTAINING_RECORD(le, batch_item_ind, list_entry);

        if (keycmp(bii->key, bi->key) == 1) {
            le = le->Blink;
            continue;
        }

        le2 = bii->items.Blink;
        while (le2 != &bii->items) {
            batch_item* bi2 = CONTAINING_RECORD(le2, batch_item, list_entry);
            int cmp = keycmp(bi2->key, bi->key);

            if (cmp == -1 || (cmp == 0 && bi->operation >= bi2->operation)) {
                InsertHeadList(&bi2->list_entry, &bi->list_entry);
                bii->num_items++;
                goto end;
            }

            le2 = le2->Blink;
        }

        InsertHeadList(&bii->items, &bi->list_entry);
        bii->num_items++;

end:
        if (bii->num_items > BATCH_ITEM_LIMIT) {
            NTSTATUS Status = split_batch_item_list(bii);

            if (!NT_SUCCESS(Status)) {
                RemoveEntryList(&bi->list_entry);
                bii->num_items--;
                ExFreeToPagedLookasideList(&Vcb->batch_item_lookaside, bi);
            }

            return Status;
        }

        return STATUS_SUCCESS;
    }

    return STATUS_INTERNAL_ERROR;
}
