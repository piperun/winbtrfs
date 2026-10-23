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
#include "tree-search.h"
#include "crc32c.h"
#include "tree-load.h"

// Construction failures leave the raw node buffer owned by the caller.
static void free_unpublished_tree(tree* t) {
    invalidate_tree_search(t);

    while (!IsListEmpty(&t->itemlist)) {
        tree_data* td = CONTAINING_RECORD(RemoveHeadList(&t->itemlist), tree_data, list_entry);
        ExFreeToPagedLookasideList(&t->Vcb->tree_data_lookaside, td);
    }

    if (t->nonpaged)
        ExFreePool(t->nonpaged);

    ExFreePool(t);
}

__attribute__((nonnull(1,3,4,5)))
NTSTATUS load_tree(device_extension* Vcb, uint64_t addr, uint8_t* buf, root* r, tree** pt) {
    struct btrfs_header* th;
    tree* t;
    tree_data* td;
    uint8_t h;
    bool inserted;
    LIST_ENTRY* le;

    th = (struct btrfs_header*)buf;

    t = ExAllocatePoolWithTag(PagedPool, sizeof(tree), ALLOC_TAG);
    if (!t) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    initialize_tree_search(t);
    t->Vcb = Vcb;
    t->nonpaged = NULL;
    InitializeListHead(&t->itemlist);

    if (th->level > 0) {
        t->nonpaged = ExAllocatePoolWithTag(NonPagedPool, sizeof(tree_nonpaged), ALLOC_TAG);
        if (!t->nonpaged) {
            ERR("out of memory\n");
            free_unpublished_tree(t);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ExInitializeFastMutex(&t->nonpaged->mutex);
    } else
        t->nonpaged = NULL;

    RtlCopyMemory(&t->header, th, sizeof(struct btrfs_header));
    t->hash = calc_crc32c(0xffffffff, (uint8_t*)&addr, sizeof(uint64_t));
    t->has_address = true;
    t->Vcb = Vcb;
    t->parent = NULL;
    t->root = r;
    t->paritem = NULL;
    t->size = 0;
    t->new_address = 0;
    t->has_new_address = false;
    t->updated_extents = false;
    t->write = false;
    t->uniqueness_determined = false;

    InitializeListHead(&t->itemlist);

    if (t->header.level == 0) { // leaf node
        struct btrfs_item* ln = (struct btrfs_item*)(buf + sizeof(struct btrfs_header));
        unsigned int i;

        if ((t->header.nritems * sizeof(struct btrfs_item)) + sizeof(struct btrfs_header) > Vcb->superblock.nodesize) {
            ERR("tree at %I64x has more items than expected (%x)\n", addr, t->header.nritems);
            free_unpublished_tree(t);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        for (i = 0; i < t->header.nritems; i++) {
            td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
            if (!td) {
                ERR("out of memory\n");
                free_unpublished_tree(t);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            td->key = ln[i].key;

            if (ln[i].size > 0)
                td->data = buf + sizeof(struct btrfs_header) + ln[i].offset;
            else
                td->data = NULL;

            if (ln[i].size + sizeof(struct btrfs_header) + sizeof(struct btrfs_item) > Vcb->superblock.nodesize) {
                ERR("overlarge item in tree %I64x: %u > %Iu\n", addr, ln[i].size, Vcb->superblock.nodesize - sizeof(struct btrfs_header) - sizeof(struct btrfs_item));
                ExFreeToPagedLookasideList(&t->Vcb->tree_data_lookaside, td);
                free_unpublished_tree(t);
                return STATUS_INTERNAL_ERROR;
            }

            td->size = (uint16_t)ln[i].size;
            td->ignore = false;
            td->inserted = false;

            InsertTailList(&t->itemlist, &td->list_entry);

            t->size += ln[i].size;
        }

        t->size += t->header.nritems * sizeof(struct btrfs_item);
        t->buf = buf;
    } else {
        struct btrfs_key_ptr* in = (struct btrfs_key_ptr*)(buf + sizeof(struct btrfs_header));
        unsigned int i;

        if ((t->header.nritems * sizeof(struct btrfs_key_ptr)) + sizeof(struct btrfs_header) > Vcb->superblock.nodesize) {
            ERR("tree at %I64x has more items than expected (%x)\n", addr, t->header.nritems);
            free_unpublished_tree(t);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        for (i = 0; i < t->header.nritems; i++) {
            td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
            if (!td) {
                ERR("out of memory\n");
                free_unpublished_tree(t);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            td->key = in[i].key;

            td->treeholder.address = in[i].blockptr;
            td->treeholder.generation = in[i].generation;
            td->treeholder.tree = NULL;
            td->ignore = false;
            td->inserted = false;

            InsertTailList(&t->itemlist, &td->list_entry);
        }

        t->size = t->header.nritems * sizeof(struct btrfs_key_ptr);
        t->buf = NULL;
    }

    cache_tree_search(t);

    ExAcquireFastMutex(&Vcb->trees_list_mutex);

    InsertTailList(&Vcb->trees, &t->list_entry);

    h = t->hash >> 24;

    if (!Vcb->trees_ptrs[h]) {
        uint8_t h2 = h;

        le = Vcb->trees_hash.Flink;

        if (h2 > 0) {
            h2--;
            do {
                if (Vcb->trees_ptrs[h2]) {
                    le = Vcb->trees_ptrs[h2];
                    break;
                }

                h2--;
            } while (h2 > 0);
        }
    } else
        le = Vcb->trees_ptrs[h];

    inserted = false;
    while (le != &Vcb->trees_hash) {
        tree* t2 = CONTAINING_RECORD(le, tree, list_entry_hash);

        if (t2->hash >= t->hash) {
            InsertHeadList(le->Blink, &t->list_entry_hash);
            inserted = true;
            break;
        }

        le = le->Flink;
    }

    if (!inserted)
        InsertTailList(&Vcb->trees_hash, &t->list_entry_hash);

    if (!Vcb->trees_ptrs[h] || t->list_entry_hash.Flink == Vcb->trees_ptrs[h])
        Vcb->trees_ptrs[h] = &t->list_entry_hash;

    ExReleaseFastMutex(&Vcb->trees_list_mutex);

    TRACE("returning %p\n", t);

    *pt = t;

    return STATUS_SUCCESS;
}

__attribute__((nonnull(1,2,3,4)))
static NTSTATUS do_load_tree2(device_extension* Vcb, tree_holder* th, uint8_t* buf, root* r, tree* t, tree_data* td) {
    if (!th->tree) {
        NTSTATUS Status;
        tree* nt;

        Status = load_tree(Vcb, th->address, buf, r, &nt);
        if (!NT_SUCCESS(Status)) {
            ERR("load_tree returned %08lx\n", Status);
            return Status;
        }

        nt->parent = t;

#ifdef DEBUG_PARANOID
        if (t && t->header.level <= nt->header.level) int3;
#endif

        nt->paritem = td;

        th->tree = nt;
    }

    return STATUS_SUCCESS;
}

__attribute__((nonnull(1,2,3)))
NTSTATUS do_load_tree(device_extension* Vcb, tree_holder* th, root* r, tree* t, tree_data* td, PIRP Irp) {
    NTSTATUS Status;
    uint8_t* buf;
    chunk* c;

    buf = ExAllocatePoolWithTag(PagedPool, Vcb->superblock.nodesize, ALLOC_TAG);
    if (!buf) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = read_data(Vcb, th->address, Vcb->superblock.nodesize, NULL, true, buf, NULL,
                       &c, Irp, th->generation, false, NormalPagePriority);
    if (!NT_SUCCESS(Status)) {
        ERR("read_data returned 0x%08lx\n", Status);
        ExFreePool(buf);
        return Status;
    }

    if (t)
        ExAcquireFastMutex(&t->nonpaged->mutex);
    else
        ExAcquireResourceExclusiveLite(&r->nonpaged->load_tree_lock, true);

    Status = do_load_tree2(Vcb, th, buf, r, t, td);

    if (t)
        ExReleaseFastMutex(&t->nonpaged->mutex);
    else
        ExReleaseResourceLite(&r->nonpaged->load_tree_lock);

    if (!th->tree || th->tree->buf != buf)
        ExFreePool(buf);

    if (!NT_SUCCESS(Status)) {
        ERR("do_load_tree2 returned %08lx\n", Status);
        return Status;
    }

    return Status;
}

bool publish_prefetched_tree(device_extension* Vcb, tree_holder* holder, root* r,
                             tree* parent, tree_data* item, uint64_t address,
                             uint64_t generation, uint8_t* buf) {
    struct btrfs_header* header = (struct btrfs_header*)buf;
    struct btrfs_item* entries = (struct btrfs_item*)(buf + sizeof(*header));
    uint32_t i, payload;
    bool adopted = false;

    if (!parent || !parent->nonpaged || header->level || !header->nritems || header->bytenr != address ||
        (generation && header->generation != generation) ||
        header->nritems > (Vcb->superblock.nodesize - sizeof(*header)) / sizeof(*entries))
        return false;
    payload = Vcb->superblock.nodesize - sizeof(*header);
    for (i = 0; i < header->nritems; i++) {
        if (entries[i].offset > payload || entries[i].size > payload - entries[i].offset ||
            entries[i].size > payload - sizeof(*entries))
            return false;
    }

    ExAcquireFastMutex(&parent->nonpaged->mutex);
    if (!holder->tree && holder->address == address && holder->generation == generation &&
        NT_SUCCESS(do_load_tree2(Vcb, holder, buf, r, parent, item)))
        adopted = true;
    ExReleaseFastMutex(&parent->nonpaged->mutex);
    return adopted;
}

