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
#include "crc32c.h"

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

    if (th->level > 0) {
        t->nonpaged = ExAllocatePoolWithTag(NonPagedPool, sizeof(tree_nonpaged), ALLOC_TAG);
        if (!t->nonpaged) {
            ERR("out of memory\n");
            ExFreePool(t);
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
            ExFreePool(t);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        for (i = 0; i < t->header.nritems; i++) {
            td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
            if (!td) {
                ERR("out of memory\n");
                ExFreePool(t);
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
                ExFreePool(t);
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
            ExFreePool(t);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        for (i = 0; i < t->header.nritems; i++) {
            td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
            if (!td) {
                ERR("out of memory\n");
                ExFreePool(t);
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

__attribute__((nonnull(1)))
void free_tree(tree* t) {
    tree* par;
    root* r = t->root;

    // No need to acquire lock, as this is only ever called while Vcb->tree_lock held exclusively

    par = t->parent;

    if (r && r->treeholder.tree != t)
        r = NULL;

    if (par) {
        if (t->paritem)
            t->paritem->treeholder.tree = NULL;
    }

    while (!IsListEmpty(&t->itemlist)) {
        tree_data* td = CONTAINING_RECORD(RemoveHeadList(&t->itemlist), tree_data, list_entry);

        if (t->header.level == 0 && td->data && td->inserted)
            ExFreePool(td->data);

        ExFreeToPagedLookasideList(&t->Vcb->tree_data_lookaside, td);
    }

    RemoveEntryList(&t->list_entry);

    if (r)
        r->treeholder.tree = NULL;

    if (t->list_entry_hash.Flink) {
        uint8_t h = t->hash >> 24;
        if (t->Vcb->trees_ptrs[h] == &t->list_entry_hash) {
            if (t->list_entry_hash.Flink != &t->Vcb->trees_hash) {
                tree* t2 = CONTAINING_RECORD(t->list_entry_hash.Flink, tree, list_entry_hash);

                if ((t2->hash >> 24) == h)
                    t->Vcb->trees_ptrs[h] = &t2->list_entry_hash;
                else
                    t->Vcb->trees_ptrs[h] = NULL;
            } else
                t->Vcb->trees_ptrs[h] = NULL;
        }

        RemoveEntryList(&t->list_entry_hash);
    }

    if (t->buf)
        ExFreePool(t->buf);

    if (t->nonpaged)
        ExFreePool(t->nonpaged);

    ExFreePool(t);
}

__attribute__((nonnull(1,2)))
void free_trees_root(device_extension* Vcb, root* r) {
    LIST_ENTRY* le;
    ULONG level;

    for (level = 0; level <= 255; level++) {
        bool empty = true;

        le = Vcb->trees.Flink;

        while (le != &Vcb->trees) {
            LIST_ENTRY* nextle = le->Flink;
            tree* t = CONTAINING_RECORD(le, tree, list_entry);

            if (t->root == r) {
                if (t->header.level == level) {
                    bool top = !t->paritem;

                    empty = false;

                    free_tree(t);
                    if (top && r->treeholder.tree == t)
                        r->treeholder.tree = NULL;

                    if (IsListEmpty(&Vcb->trees))
                        return;
                } else if (t->header.level > level)
                    empty = false;
            }

            le = nextle;
        }

        if (empty)
            break;
    }
}

__attribute__((nonnull(1)))
void free_trees(device_extension* Vcb) {
    LIST_ENTRY* le;
    ULONG level;

    for (level = 0; level <= 255; level++) {
        bool empty = true;

        le = Vcb->trees.Flink;

        while (le != &Vcb->trees) {
            LIST_ENTRY* nextle = le->Flink;
            tree* t = CONTAINING_RECORD(le, tree, list_entry);
            root* r = t->root;

            if (t->header.level == level) {
                bool top = !t->paritem;

                empty = false;

                free_tree(t);
                if (top && r->treeholder.tree == t)
                    r->treeholder.tree = NULL;

                if (IsListEmpty(&Vcb->trees))
                    break;
            } else if (t->header.level > level)
                empty = false;

            le = nextle;
        }

        if (empty)
            break;
    }

    reap_filerefs(Vcb);
    reap_fcbs(Vcb);
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(suppress: 28194)
#endif
__attribute__((nonnull(1,2)))
NTSTATUS insert_tree_item(_In_ _Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_ root* r, _In_ uint64_t objectid,
                          _In_ uint8_t type, _In_ uint64_t offset, _In_reads_bytes_opt_(size) _When_(return >= 0, __drv_aliasesMem) void* data,
                          _In_ uint16_t size, _Out_opt_ traverse_ptr* ptp, _In_opt_ PIRP Irp) {
    traverse_ptr tp;
    struct btrfs_key searchkey;
    int cmp;
    tree_data *td, *paritem;
    tree* t;
#ifdef _DEBUG
    LIST_ENTRY* le;
    struct btrfs_key firstitem = {0xcccccccccccccccc,0xcc,0xcccccccccccccccc};
#endif
    NTSTATUS Status;

    TRACE("(%p, %p, %I64x, %x, %I64x, %p, %x, %p)\n", Vcb, r, objectid, type, offset, data, size, ptp);

    searchkey.objectid = objectid;
    searchkey.type = type;
    searchkey.offset = offset;

    Status = find_item(Vcb, r, &tp, &searchkey, true, Irp);
    if (Status == STATUS_NOT_FOUND) {
        if (!r->treeholder.tree) {
            Status = do_load_tree(Vcb, &r->treeholder, r, NULL, NULL, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("do_load_tree returned %08lx\n", Status);
                return Status;
            }
        }

        if (r->treeholder.tree && r->treeholder.tree->header.nritems == 0) {
            tp.tree = r->treeholder.tree;
            tp.item = NULL;
        } else {
            ERR("error: unable to load tree for root %I64x\n", r->id);
            return STATUS_INTERNAL_ERROR;
        }
    } else if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        return Status;
    }

    TRACE("tp.item = %p\n", tp.item);

    if (tp.item) {
        TRACE("tp.item->key = %p\n", &tp.item->key);
        cmp = keycmp(searchkey, tp.item->key);

        if (cmp == 0 && !tp.item->ignore) {
            ERR("error: key (%I64x,%x,%I64x) already present\n", objectid, type, offset);
#ifdef DEBUG_PARANOID
            int3;
#endif
            return STATUS_INTERNAL_ERROR;
        }
    } else
        cmp = -1;

    td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
    if (!td) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    td->key = searchkey;
    td->size = size;
    td->data = data;
    td->ignore = false;
    td->inserted = true;

#ifdef _DEBUG
    le = tp.tree->itemlist.Flink;
    while (le != &tp.tree->itemlist) {
        tree_data* td2 = CONTAINING_RECORD(le, tree_data, list_entry);
        firstitem = td2->key;
        break;
    }

    TRACE("inserting %I64x,%x,%I64x into tree beginning %I64x,%x,%I64x (num_items %x)\n", objectid, type, offset, firstitem.objectid, firstitem.type, firstitem.offset, tp.tree->header.nritems);
#endif

    if (cmp == -1) { // very first key in root
        InsertHeadList(&tp.tree->itemlist, &td->list_entry);

        paritem = tp.tree->paritem;
        while (paritem) {
            if (!keycmp(paritem->key, tp.item->key)) {
                paritem->key = searchkey;
            } else
                break;

            paritem = paritem->treeholder.tree->paritem;
        }
    } else if (cmp == 0)
        InsertHeadList(tp.item->list_entry.Blink, &td->list_entry); // make sure non-deleted item is before deleted ones
    else
        InsertHeadList(&tp.item->list_entry, &td->list_entry);

    tp.tree->header.nritems++;
    tp.tree->size += size + sizeof(struct btrfs_item);

    if (!tp.tree->write) {
        tp.tree->write = true;
        Vcb->need_write = true;
    }

    if (ptp)
        *ptp = tp;

    t = tp.tree;
    while (t) {
        if (t->paritem && t->paritem->ignore) {
            t->paritem->ignore = false;
            t->parent->header.nritems++;
            t->parent->size += sizeof(struct btrfs_key_ptr);
        }

        t->header.generation = Vcb->superblock.generation;
        t = t->parent;
    }

    return STATUS_SUCCESS;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

__attribute__((nonnull(1,2)))
NTSTATUS delete_tree_item(_In_ _Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _Inout_ traverse_ptr* tp) {
    tree* t;
    uint64_t gen;

    TRACE("deleting item %I64x,%x,%I64x (ignore = %s)\n", tp->item->key.objectid, tp->item->key.type, tp->item->key.offset, tp->item->ignore ? "true" : "false");

#ifdef DEBUG_PARANOID
    if (tp->item->ignore) {
        ERR("trying to delete already-deleted item %I64x,%x,%I64x\n", tp->item->key.objectid, tp->item->key.type, tp->item->key.offset);
        int3;
        return STATUS_INTERNAL_ERROR;
    }
#endif

    tp->item->ignore = true;

    if (!tp->tree->write) {
        tp->tree->write = true;
        Vcb->need_write = true;
    }

    tp->tree->header.nritems--;

    if (tp->tree->header.level == 0)
        tp->tree->size -= sizeof(struct btrfs_item) + tp->item->size;
    else
        tp->tree->size -= sizeof(struct btrfs_key_ptr);

    gen = tp->tree->Vcb->superblock.generation;

    t = tp->tree;
    while (t) {
        t->header.generation = gen;
        t = t->parent;
    }

    return STATUS_SUCCESS;
}
