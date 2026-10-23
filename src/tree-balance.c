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

// Tree splitting, merging and height changes inside the existing transaction.
#include "btrfs_drv.h"
#include "tree-transaction.h"

static void get_first_item(tree* t, struct btrfs_key* key) {
    LIST_ENTRY* le;

    le = t->itemlist.Flink;
    while (le != &t->itemlist) {
        tree_data* td = CONTAINING_RECORD(le, tree_data, list_entry);

        *key = td->key;
        return;
    }
}

static NTSTATUS split_tree_at(device_extension* Vcb, tree* t, tree_data* newfirstitem, uint32_t numitems, uint32_t size) {
    tree *nt, *pt;
    tree_data* td;
    tree_data* oldlastitem;

    TRACE("splitting tree in %I64x at (%I64x,%x,%I64x)\n", t->root->id, newfirstitem->key.objectid, newfirstitem->key.type, newfirstitem->key.offset);

    nt = ExAllocatePoolWithTag(PagedPool, sizeof(tree), ALLOC_TAG);
    if (!nt) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (t->header.level > 0) {
        nt->nonpaged = ExAllocatePoolWithTag(NonPagedPool, sizeof(tree_nonpaged), ALLOC_TAG);
        if (!nt->nonpaged) {
            ERR("out of memory\n");
            ExFreePool(nt);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ExInitializeFastMutex(&nt->nonpaged->mutex);
    } else
        nt->nonpaged = NULL;

    RtlCopyMemory(&nt->header, &t->header, sizeof(struct btrfs_header));
    nt->header.bytenr = 0;
    nt->header.generation = Vcb->superblock.generation;
    nt->header.nritems = t->header.nritems - numitems;
    nt->header.flags = (BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT) | BTRFS_HEADER_FLAG_WRITTEN;

    nt->has_address = false;
    nt->Vcb = Vcb;
    nt->parent = t->parent;

#ifdef DEBUG_PARANOID
    if (nt->parent && nt->parent->header.level <= nt->header.level) int3;
#endif

    nt->root = t->root;
    nt->new_address = 0;
    nt->has_new_address = false;
    nt->updated_extents = false;
    nt->uniqueness_determined = true;
    nt->is_unique = true;
    nt->list_entry_hash.Flink = NULL;
    nt->buf = NULL;
    InitializeListHead(&nt->itemlist);

    oldlastitem = CONTAINING_RECORD(newfirstitem->list_entry.Blink, tree_data, list_entry);

    nt->itemlist.Flink = &newfirstitem->list_entry;
    nt->itemlist.Blink = t->itemlist.Blink;
    nt->itemlist.Flink->Blink = &nt->itemlist;
    nt->itemlist.Blink->Flink = &nt->itemlist;

    t->itemlist.Blink = &oldlastitem->list_entry;
    t->itemlist.Blink->Flink = &t->itemlist;

    nt->size = t->size - size;
    t->size = size;
    t->header.nritems = numitems;
    nt->write = true;

    InsertTailList(&Vcb->trees, &nt->list_entry);

    if (nt->header.level > 0) {
        LIST_ENTRY* le = nt->itemlist.Flink;

        while (le != &nt->itemlist) {
            tree_data* td2 = CONTAINING_RECORD(le, tree_data, list_entry);

            if (td2->treeholder.tree) {
                td2->treeholder.tree->parent = nt;
#ifdef DEBUG_PARANOID
                if (td2->treeholder.tree->parent && td2->treeholder.tree->parent->header.level <= td2->treeholder.tree->header.level) int3;
#endif
            }

            le = le->Flink;
        }
    } else {
        LIST_ENTRY* le = nt->itemlist.Flink;

        while (le != &nt->itemlist) {
            tree_data* td2 = CONTAINING_RECORD(le, tree_data, list_entry);

            if (!td2->inserted && td2->data) {
                uint8_t* data = ExAllocatePoolWithTag(PagedPool, td2->size, ALLOC_TAG);

                if (!data) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(data, td2->data, td2->size);
                td2->data = data;
                td2->inserted = true;
            }

            le = le->Flink;
        }
    }

    if (nt->parent) {
        td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
        if (!td) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        td->key = newfirstitem->key;

        InsertHeadList(&t->paritem->list_entry, &td->list_entry);

        td->ignore = false;
        td->inserted = true;
        td->treeholder.tree = nt;
        nt->paritem = td;

        nt->parent->header.nritems++;
        nt->parent->size += sizeof(struct btrfs_key_ptr);

        goto end;
    }

    TRACE("adding new tree parent\n");

    if (nt->header.level == 255) {
        ERR("cannot add parent to tree at level 255\n");
        return STATUS_INTERNAL_ERROR;
    }

    pt = ExAllocatePoolWithTag(PagedPool, sizeof(tree), ALLOC_TAG);
    if (!pt) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pt->nonpaged = ExAllocatePoolWithTag(NonPagedPool, sizeof(tree_nonpaged), ALLOC_TAG);
    if (!pt->nonpaged) {
        ERR("out of memory\n");
        ExFreePool(pt);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExInitializeFastMutex(&pt->nonpaged->mutex);

    RtlCopyMemory(&pt->header, &nt->header, sizeof(struct btrfs_header));
    pt->header.bytenr = 0;
    pt->header.nritems = 2;
    pt->header.level = nt->header.level + 1;
    pt->header.flags = (BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT) | BTRFS_HEADER_FLAG_WRITTEN;

    pt->has_address = false;
    pt->Vcb = Vcb;
    pt->parent = NULL;
    pt->paritem = NULL;
    pt->root = t->root;
    pt->new_address = 0;
    pt->has_new_address = false;
    pt->updated_extents = false;
    pt->size = pt->header.nritems * sizeof(struct btrfs_key_ptr);
    pt->uniqueness_determined = true;
    pt->is_unique = true;
    pt->list_entry_hash.Flink = NULL;
    pt->buf = NULL;
    InitializeListHead(&pt->itemlist);

    InsertTailList(&Vcb->trees, &pt->list_entry);

    td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
    if (!td) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    get_first_item(t, &td->key);
    td->ignore = false;
    td->inserted = false;
    td->treeholder.address = 0;
    td->treeholder.generation = Vcb->superblock.generation;
    td->treeholder.tree = t;
    InsertTailList(&pt->itemlist, &td->list_entry);
    t->paritem = td;

    td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
    if (!td) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    td->key = newfirstitem->key;
    td->ignore = false;
    td->inserted = false;
    td->treeholder.address = 0;
    td->treeholder.generation = Vcb->superblock.generation;
    td->treeholder.tree = nt;
    InsertTailList(&pt->itemlist, &td->list_entry);
    nt->paritem = td;

    pt->write = true;

    t->root->treeholder.tree = pt;

    t->parent = pt;
    nt->parent = pt;

#ifdef DEBUG_PARANOID
    if (t->parent && t->parent->header.level <= t->header.level) int3;
    if (nt->parent && nt->parent->header.level <= nt->header.level) int3;
#endif

end:
    t->root->root_item.bytes_used += Vcb->superblock.nodesize;

    return STATUS_SUCCESS;
}

static NTSTATUS split_tree(device_extension* Vcb, tree* t) {
    LIST_ENTRY* le;
    uint32_t size, ds, numitems;

    size = 0;
    numitems = 0;

    // FIXME - naïve implementation: maximizes number of filled trees

    le = t->itemlist.Flink;
    while (le != &t->itemlist) {
        tree_data* td = CONTAINING_RECORD(le, tree_data, list_entry);

        if (!td->ignore) {
            if (t->header.level == 0)
                ds = sizeof(struct btrfs_item) + td->size;
            else
                ds = sizeof(struct btrfs_key_ptr);

            if (numitems == 0 && ds > Vcb->superblock.nodesize - sizeof(struct btrfs_header)) {
                ERR("(%I64x,%x,%I64x) in tree %I64x is too large (%x > %Ix)\n",
                    td->key.objectid, td->key.type, td->key.offset, t->root->id,
                    ds, Vcb->superblock.nodesize - sizeof(struct btrfs_header));
                return STATUS_INTERNAL_ERROR;
            }

            // FIXME - move back if previous item was deleted item with same key
            if (size + ds > Vcb->superblock.nodesize - sizeof(struct btrfs_header))
                return split_tree_at(Vcb, t, td, numitems, size);

            size += ds;
            numitems++;
        }

        le = le->Flink;
    }

    return STATUS_SUCCESS;
}

bool is_tree_unique(device_extension* Vcb, tree* t, PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    bool ret = false;
    struct btrfs_extent_item* ei;
    uint8_t* type;

    if (t->uniqueness_determined)
        return t->is_unique;

    if (t->parent && !is_tree_unique(Vcb, t->parent, Irp))
        goto end;

    if (t->has_address) {
        searchkey.objectid = t->header.bytenr;
        searchkey.type = Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA ? BTRFS_METADATA_ITEM_KEY : BTRFS_EXTENT_ITEM_KEY;
        searchkey.offset = 0xffffffffffffffff;

        Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            goto end;
        }

        if (tp.item->key.objectid != t->header.bytenr || (tp.item->key.type != BTRFS_METADATA_ITEM_KEY && tp.item->key.type != BTRFS_EXTENT_ITEM_KEY))
            goto end;

        if (tp.item->key.type == BTRFS_EXTENT_ITEM_KEY && tp.item->size == sizeof(struct btrfs_extent_item_v0))
            goto end;

        if (tp.item->size < sizeof(struct btrfs_extent_item))
            goto end;

        ei = (struct btrfs_extent_item*)tp.item->data;

        if (ei->refs > 1)
            goto end;

        if (tp.item->key.type == BTRFS_EXTENT_ITEM_KEY && ei->flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
            struct btrfs_tree_block_info* ei2;

            if (tp.item->size < sizeof(struct btrfs_extent_item) + sizeof(struct btrfs_tree_block_info))
                goto end;

            ei2 = (struct btrfs_tree_block_info*)&ei[1];
            type = (uint8_t*)&ei2[1];
        } else
            type = (uint8_t*)&ei[1];

        if (type >= tp.item->data + tp.item->size || *type != BTRFS_TREE_BLOCK_REF_KEY)
            goto end;
    }

    ret = true;

end:
    t->is_unique = ret;
    t->uniqueness_determined = true;

    return ret;
}

static NTSTATUS try_tree_amalgamate(device_extension* Vcb, tree* t, bool* done,
                                    bool* done_deletions, PIRP Irp) {
    LIST_ENTRY* le;
    tree_data* nextparitem = NULL;
    NTSTATUS Status;
    tree *next_tree, *par;

    *done = false;

    TRACE("trying to amalgamate tree in root %I64x, level %x (size %u)\n", t->root->id, t->header.level, t->size);

    // FIXME - doesn't capture everything, as it doesn't ascend
    le = t->paritem->list_entry.Flink;
    while (le != &t->parent->itemlist) {
        tree_data* td = CONTAINING_RECORD(le, tree_data, list_entry);

        if (!td->ignore) {
            nextparitem = td;
            break;
        }

        le = le->Flink;
    }

    if (!nextparitem)
        return STATUS_SUCCESS;

    TRACE("nextparitem: key = %I64x,%x,%I64x\n", nextparitem->key.objectid, nextparitem->key.type, nextparitem->key.offset);

    if (!nextparitem->treeholder.tree) {
        Status = do_load_tree(Vcb, &nextparitem->treeholder, t->root, t->parent, nextparitem, NULL);
        if (!NT_SUCCESS(Status)) {
            ERR("do_load_tree returned %08lx\n", Status);
            return Status;
        }
    }

    if (!is_tree_unique(Vcb, nextparitem->treeholder.tree, Irp))
        return STATUS_SUCCESS;

    next_tree = nextparitem->treeholder.tree;

    if (!next_tree->updated_extents && next_tree->has_address) {
        Status = update_tree_extents(Vcb, next_tree, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("update_tree_extents returned %08lx\n", Status);
            return Status;
        }
    }

    if (t->size + next_tree->size <= Vcb->superblock.nodesize - sizeof(struct btrfs_header)) {
        // merge two trees into one

        t->header.nritems += next_tree->header.nritems;
        t->size += next_tree->size;

        if (next_tree->header.level > 0) {
            le = next_tree->itemlist.Flink;

            while (le != &next_tree->itemlist) {
                tree_data* td2 = CONTAINING_RECORD(le, tree_data, list_entry);

                if (td2->treeholder.tree) {
                    td2->treeholder.tree->parent = t;
#ifdef DEBUG_PARANOID
                    if (td2->treeholder.tree->parent && td2->treeholder.tree->parent->header.level <= td2->treeholder.tree->header.level) int3;
#endif
                }

                td2->inserted = true;
                le = le->Flink;
            }
        } else {
            le = next_tree->itemlist.Flink;

            while (le != &next_tree->itemlist) {
                tree_data* td2 = CONTAINING_RECORD(le, tree_data, list_entry);

                if (!td2->inserted && td2->data) {
                    uint8_t* data = ExAllocatePoolWithTag(PagedPool, td2->size, ALLOC_TAG);

                    if (!data) {
                        ERR("out of memory\n");
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    RtlCopyMemory(data, td2->data, td2->size);
                    td2->data = data;
                    td2->inserted = true;
                }

                le = le->Flink;
            }
        }

        t->itemlist.Blink->Flink = next_tree->itemlist.Flink;
        t->itemlist.Blink->Flink->Blink = t->itemlist.Blink;
        t->itemlist.Blink = next_tree->itemlist.Blink;
        t->itemlist.Blink->Flink = &t->itemlist;

        next_tree->itemlist.Flink = next_tree->itemlist.Blink = &next_tree->itemlist;

        next_tree->header.nritems = 0;
        next_tree->size = 0;

        if (next_tree->has_new_address) { // delete associated EXTENT_ITEM
            Status = reduce_tree_extent(Vcb, next_tree->new_address, next_tree,
                                        next_tree->parent->header.owner, Irp);

            if (!NT_SUCCESS(Status)) {
                ERR("reduce_tree_extent returned %08lx\n", Status);
                return Status;
            }
        } else if (next_tree->has_address) {
            Status = reduce_tree_extent(Vcb, next_tree->header.bytenr, next_tree,
                                        next_tree->parent->header.owner, Irp);

            if (!NT_SUCCESS(Status)) {
                ERR("reduce_tree_extent returned %08lx\n", Status);
                return Status;
            }
        }

        if (!nextparitem->ignore) {
            nextparitem->ignore = true;
            next_tree->parent->header.nritems--;
            next_tree->parent->size -= sizeof(struct btrfs_key_ptr);

            *done_deletions = true;
        }

        par = next_tree->parent;
        while (par) {
            par->write = true;
            par = par->parent;
        }

        RemoveEntryList(&nextparitem->list_entry);
        ExFreePool(next_tree->paritem);
        next_tree->paritem = NULL;

        next_tree->root->root_item.bytes_used -= Vcb->superblock.nodesize;

        free_tree(next_tree);

        *done = true;
    } else {
        // rebalance by moving items from second tree into first
        ULONG avg_size = (t->size + next_tree->size) / 2;
        struct btrfs_key firstitem = {0, 0, 0};
        bool changed = false;

        TRACE("attempting rebalance\n");

        le = next_tree->itemlist.Flink;
        while (le != &next_tree->itemlist && t->size < avg_size && next_tree->header.nritems > 1) {
            tree_data* td = CONTAINING_RECORD(le, tree_data, list_entry);
            ULONG size;

            if (!td->ignore) {
                if (next_tree->header.level == 0)
                    size = sizeof(struct btrfs_item) + td->size;
                else
                    size = sizeof(struct btrfs_key_ptr);
            } else
                size = 0;

            if (t->size + size < Vcb->superblock.nodesize - sizeof(struct btrfs_header)) {
                RemoveEntryList(&td->list_entry);
                InsertTailList(&t->itemlist, &td->list_entry);

                if (next_tree->header.level > 0 && td->treeholder.tree) {
                    td->treeholder.tree->parent = t;
#ifdef DEBUG_PARANOID
                    if (td->treeholder.tree->parent && td->treeholder.tree->parent->header.level <= td->treeholder.tree->header.level) int3;
#endif
                } else if (next_tree->header.level == 0 && !td->inserted && td->size > 0) {
                    uint8_t* data = ExAllocatePoolWithTag(PagedPool, td->size, ALLOC_TAG);

                    if (!data) {
                        ERR("out of memory\n");
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    RtlCopyMemory(data, td->data, td->size);
                    td->data = data;
                }

                td->inserted = true;

                if (!td->ignore) {
                    next_tree->size -= size;
                    t->size += size;
                    next_tree->header.nritems--;
                    t->header.nritems++;
                }

                changed = true;
            } else
                break;

            le = next_tree->itemlist.Flink;
        }

        le = next_tree->itemlist.Flink;
        while (le != &next_tree->itemlist) {
            tree_data* td = CONTAINING_RECORD(le, tree_data, list_entry);

            if (!td->ignore) {
                firstitem = td->key;
                break;
            }

            le = le->Flink;
        }

        // FIXME - once ascension is working, make this work with parent's parent, etc.
        if (next_tree->paritem)
            next_tree->paritem->key = firstitem;

        par = next_tree;
        while (par) {
            par->write = true;
            par = par->parent;
        }

        if (changed)
            *done = true;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS update_extent_level(device_extension* Vcb, uint64_t address, tree* t, uint8_t level, PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp;
    NTSTATUS Status;

    if (Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA) {
        searchkey.objectid = address;
        searchkey.type = BTRFS_METADATA_ITEM_KEY;
        searchkey.offset = t->header.level;

        Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08lx\n", Status);
            return Status;
        }

        if (!keycmp(tp.item->key, searchkey)) {
            EXTENT_ITEM_SKINNY_METADATA* eism;

            if (tp.item->size > 0) {
                eism = ExAllocatePoolWithTag(PagedPool, tp.item->size, ALLOC_TAG);

                if (!eism) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(eism, tp.item->data, tp.item->size);
            } else
                eism = NULL;

            Status = delete_tree_item(Vcb, &tp);
            if (!NT_SUCCESS(Status)) {
                ERR("delete_tree_item returned %08lx\n", Status);
                if (eism) ExFreePool(eism);
                return Status;
            }

            Status = insert_tree_item(Vcb, Vcb->extent_root, address, BTRFS_METADATA_ITEM_KEY, level, eism, tp.item->size, NULL, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("insert_tree_item returned %08lx\n", Status);
                if (eism) ExFreePool(eism);
                return Status;
            }

            return STATUS_SUCCESS;
        }
    }

    searchkey.objectid = address;
    searchkey.type = BTRFS_EXTENT_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        return Status;
    }

    if (tp.item->key.objectid == searchkey.objectid && tp.item->key.type == searchkey.type) {
        struct btrfs_extent_item* ei;
        struct btrfs_tree_block_info* tbi;

        if (tp.item->size < sizeof(struct btrfs_extent_item) + sizeof(struct btrfs_tree_block_info)) {
            ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_extent_item) + sizeof(struct btrfs_tree_block_info));
            return STATUS_INTERNAL_ERROR;
        }

        ei = ExAllocatePoolWithTag(PagedPool, tp.item->size, ALLOC_TAG);

        if (!ei) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(ei, tp.item->data, tp.item->size);

        Status = delete_tree_item(Vcb, &tp);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_tree_item returned %08lx\n", Status);
            ExFreePool(ei);
            return Status;
        }

        tbi = (struct btrfs_tree_block_info*)&ei[1];

        tbi->level = level;

        Status = insert_tree_item(Vcb, Vcb->extent_root, tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, ei, tp.item->size, NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_tree_item returned %08lx\n", Status);
            ExFreePool(ei);
            return Status;
        }

        return STATUS_SUCCESS;
    }

    ERR("could not find EXTENT_ITEM for address %I64x\n", address);

    return STATUS_INTERNAL_ERROR;
}

static NTSTATUS update_tree_extents_recursive(device_extension* Vcb, tree* t,
                                              PIRP Irp) {
    NTSTATUS Status;

    if (t->parent && !t->parent->updated_extents && t->parent->has_address) {
        Status = update_tree_extents_recursive(Vcb, t->parent, Irp);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = update_tree_extents(Vcb, t, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("update_tree_extents returned %08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS do_splits(device_extension* Vcb, PIRP Irp) {
    ULONG level, max_level;
    uint32_t min_size, min_size_fst;
    bool empty, done_deletions = false;
    NTSTATUS Status;
    tree* t;

    TRACE("(%p)\n", Vcb);

    max_level = 0;

    for (level = 0; level <= 255; level++) {
        LIST_ENTRY *le, *nextle;

        empty = true;

        TRACE("doing level %lu\n", level);

        le = Vcb->trees.Flink;

        while (le != &Vcb->trees) {
            t = CONTAINING_RECORD(le, tree, list_entry);

            nextle = le->Flink;

            if (t->write && t->header.level == level) {
                empty = false;

                if (t->header.nritems == 0) {
                    if (t->parent) {
                        done_deletions = true;

                        TRACE("deleting tree in root %I64x\n", t->root->id);

                        t->root->root_item.bytes_used -= Vcb->superblock.nodesize;

                        if (t->has_new_address) { // delete associated EXTENT_ITEM
                            Status = reduce_tree_extent(Vcb, t->new_address, t,
                                                        t->parent->header.owner,
                                                        Irp);

                            if (!NT_SUCCESS(Status)) {
                                ERR("reduce_tree_extent returned %08lx\n", Status);
                                return Status;
                            }

                            t->has_new_address = false;
                        } else if (t->has_address) {
                            Status = reduce_tree_extent(Vcb,t->header.bytenr, t,
                                                        t->parent->header.owner,
                                                        Irp);

                            if (!NT_SUCCESS(Status)) {
                                ERR("reduce_tree_extent returned %08lx\n", Status);
                                return Status;
                            }

                            t->has_address = false;
                        }

                        if (!t->paritem->ignore) {
                            t->paritem->ignore = true;
                            t->parent->header.nritems--;
                            t->parent->size -= sizeof(struct btrfs_key_ptr);
                        }

                        RemoveEntryList(&t->paritem->list_entry);
                        ExFreePool(t->paritem);
                        t->paritem = NULL;

                        free_tree(t);
                    } else if (t->header.level != 0) {
                        if (t->has_new_address) {
                            Status = update_extent_level(Vcb, t->new_address, t, 0, Irp);

                            if (!NT_SUCCESS(Status)) {
                                ERR("update_extent_level returned %08lx\n", Status);
                                return Status;
                            }
                        }

                        t->header.level = 0;
                    }
                } else if (t->size > Vcb->superblock.nodesize - sizeof(struct btrfs_header)) {
                    TRACE("splitting overlarge tree (%x > %Ix)\n", t->size, Vcb->superblock.nodesize - sizeof(struct btrfs_header));

                    if (!t->updated_extents && t->has_address) {
                        Status = update_tree_extents_recursive(Vcb, t, Irp);
                        if (!NT_SUCCESS(Status)) {
                            ERR("update_tree_extents_recursive returned %08lx\n", Status);
                            return Status;
                        }
                    }

                    Status = split_tree(Vcb, t);

                    if (!NT_SUCCESS(Status)) {
                        ERR("split_tree returned %08lx\n", Status);
                        return Status;
                    }
                }
            }

            le = nextle;
        }

        if (!empty) {
            max_level = level;
        } else {
            TRACE("nothing found for level %lu\n", level);
            break;
        }
    }

    min_size = (Vcb->superblock.nodesize - sizeof(struct btrfs_header)) / 2;
    min_size_fst = (Vcb->superblock.nodesize - sizeof(struct btrfs_header)) / 4;

    for (level = 0; level <= max_level; level++) {
        LIST_ENTRY* le;

        le = Vcb->trees.Flink;

        while (le != &Vcb->trees) {
            t = CONTAINING_RECORD(le, tree, list_entry);

            if (t->write && t->header.level == level && t->header.nritems > 0 && t->parent &&
                ((t->size < min_size && t->root->id != BTRFS_FREE_SPACE_TREE_OBJECTID) || (t->size < min_size_fst && t->root->id == BTRFS_FREE_SPACE_TREE_OBJECTID)) &&
                is_tree_unique(Vcb, t, Irp)) {
                bool done;

                do {
                    Status = try_tree_amalgamate(Vcb, t, &done, &done_deletions,
                                                 Irp);
                    if (!NT_SUCCESS(Status)) {
                        ERR("try_tree_amalgamate returned %08lx\n", Status);
                        return Status;
                    }
                } while (done && t->size < min_size);
            }

            le = le->Flink;
        }
    }

    // simplify trees if top tree only has one entry

    if (done_deletions) {
        for (level = max_level; level > 0; level--) {
            LIST_ENTRY *le, *nextle;

            le = Vcb->trees.Flink;
            while (le != &Vcb->trees) {
                nextle = le->Flink;
                t = CONTAINING_RECORD(le, tree, list_entry);

                if (t->write && t->header.level == level) {
                    if (!t->parent && t->header.nritems == 1) {
                        LIST_ENTRY* le2 = t->itemlist.Flink;
                        tree_data* td = NULL;
                        tree* child_tree = NULL;

                        while (le2 != &t->itemlist) {
                            td = CONTAINING_RECORD(le2, tree_data, list_entry);
                            if (!td->ignore)
                                break;
                            le2 = le2->Flink;
                        }

                        TRACE("deleting top-level tree in root %I64x with one item\n", t->root->id);

                        if (t->has_new_address) { // delete associated EXTENT_ITEM
                            Status = reduce_tree_extent(Vcb, t->new_address, t,
                                                        t->header.owner, Irp);

                            if (!NT_SUCCESS(Status)) {
                                ERR("reduce_tree_extent returned %08lx\n", Status);
                                return Status;
                            }

                            t->has_new_address = false;
                        } else if (t->has_address) {
                            Status = reduce_tree_extent(Vcb,t->header.bytenr, t,
                                                        t->header.owner, Irp);

                            if (!NT_SUCCESS(Status)) {
                                ERR("reduce_tree_extent returned %08lx\n", Status);
                                return Status;
                            }

                            t->has_address = false;
                        }

                        if (!td->treeholder.tree) { // load first item if not already loaded
                            struct btrfs_key searchkey = {0,0,0};
                            traverse_ptr tp;

                            Status = find_item(Vcb, t->root, &tp, &searchkey, false, Irp);
                            if (!NT_SUCCESS(Status)) {
                                ERR("error - find_item returned %08lx\n", Status);
                                return Status;
                            }
                        }

                        child_tree = td->treeholder.tree;

                        if (child_tree) {
                            child_tree->parent = NULL;
                            child_tree->paritem = NULL;
                        }

                        t->root->root_item.bytes_used -= Vcb->superblock.nodesize;

                        free_tree(t);

                        if (child_tree)
                            child_tree->root->treeholder.tree = child_tree;
                    }
                }

                le = nextle;
            }
        }
    }

    return STATUS_SUCCESS;
}
