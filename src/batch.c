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

/* Apply and release batched tree updates. */
#include "batch.h"

__attribute__((nonnull(1,2,3)))
static NTSTATUS find_tree_end(tree* t, struct btrfs_key* tree_end, bool* no_end) {
    tree* p;

    p = t;
    do {
        tree_data* pi;

        if (!p->parent) {
            tree_end->objectid = 0xffffffffffffffff;
            tree_end->type = 0xff;
            tree_end->offset = 0xffffffffffffffff;
            *no_end = true;
            return STATUS_SUCCESS;
        }

        pi = p->paritem;

        if (pi->list_entry.Flink != &p->parent->itemlist) {
            tree_data* td = CONTAINING_RECORD(pi->list_entry.Flink, tree_data, list_entry);

            *tree_end = td->key;
            *no_end = false;
            return STATUS_SUCCESS;
        }

        p = p->parent;
    } while (p);

    return STATUS_INTERNAL_ERROR;
}

__attribute__((nonnull(1,2)))
void clear_batch_list(device_extension* Vcb, LIST_ENTRY* batchlist) {
    while (!IsListEmpty(batchlist)) {
        LIST_ENTRY* le = RemoveHeadList(batchlist);
        batch_root* br = CONTAINING_RECORD(le, batch_root, list_entry);

        while (!IsListEmpty(&br->items_ind)) {
            batch_item_ind* bii = CONTAINING_RECORD(RemoveHeadList(&br->items_ind), batch_item_ind, list_entry);

            while (!IsListEmpty(&bii->items)) {
                batch_item* bi = CONTAINING_RECORD(RemoveHeadList(&bii->items), batch_item, list_entry);

                ExFreeToPagedLookasideList(&Vcb->batch_item_lookaside, bi);
            }

            ExFreePool(bii);
        }

        ExFreePool(br);
    }
}

__attribute__((nonnull(1,2)))
static NTSTATUS commit_batch_list_root(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, batch_root* br, PIRP Irp) {
    LIST_ENTRY items;
    LIST_ENTRY* le;
    NTSTATUS Status;

    TRACE("root: %I64x\n", br->r->id);

    InitializeListHead(&items);

    // move sub-lists into one big list

    while (!IsListEmpty(&br->items_ind)) {
        batch_item_ind* bii = CONTAINING_RECORD(RemoveHeadList(&br->items_ind), batch_item_ind, list_entry);

        items.Blink->Flink = bii->items.Flink;
        bii->items.Flink->Blink = items.Blink;
        items.Blink = bii->items.Blink;
        bii->items.Blink->Flink = &items;

        ExFreePool(bii);
    }

    le = items.Flink;
    while (le != &items) {
        batch_item* bi = CONTAINING_RECORD(le, batch_item, list_entry);
        LIST_ENTRY* le2;
        traverse_ptr tp;
        struct btrfs_key tree_end;
        bool no_end;
        tree_data *td, *listhead;
        int cmp;
        tree* t;
        bool ignore = false;

        TRACE("(%I64x,%x,%I64x)\n", bi->key.objectid, bi->key.type, bi->key.offset);

        Status = find_item(Vcb, br->r, &tp, &bi->key, true, Irp);
        if (!NT_SUCCESS(Status)) { // FIXME - handle STATUS_NOT_FOUND
            ERR("find_item returned %08lx\n", Status);
            return Status;
        }

        Status = find_tree_end(tp.tree, &tree_end, &no_end);
        if (!NT_SUCCESS(Status)) {
            ERR("find_tree_end returned %08lx\n", Status);
            return Status;
        }

        if (bi->operation == Batch_DeleteInode) {
            if (tp.item->key.objectid == bi->key.objectid) {
                bool ended = false;

                td = tp.item;

                if (!tp.item->ignore) {
                    tp.item->ignore = true;
                    tp.tree->header.nritems--;
                    tp.tree->size -= tp.item->size + sizeof(struct btrfs_item);
                    tp.tree->write = true;
                }

                le2 = tp.item->list_entry.Flink;
                while (le2 != &tp.tree->itemlist) {
                    td = CONTAINING_RECORD(le2, tree_data, list_entry);

                    if (td->key.objectid == bi->key.objectid) {
                        if (!td->ignore) {
                            td->ignore = true;
                            tp.tree->header.nritems--;
                            tp.tree->size -= td->size + sizeof(struct btrfs_item);
                            tp.tree->write = true;
                        }
                    } else {
                        ended = true;
                        break;
                    }

                    le2 = le2->Flink;
                }

                while (!ended) {
                    traverse_ptr next_tp;

                    tp.item = td;

                    if (!find_next_item(Vcb, &tp, &next_tp, false, Irp))
                        break;

                    tp = next_tp;

                    le2 = &tp.item->list_entry;
                    while (le2 != &tp.tree->itemlist) {
                        td = CONTAINING_RECORD(le2, tree_data, list_entry);

                        if (td->key.objectid == bi->key.objectid) {
                            if (!td->ignore) {
                                td->ignore = true;
                                tp.tree->header.nritems--;
                                tp.tree->size -= td->size + sizeof(struct btrfs_item);
                                tp.tree->write = true;
                            }
                        } else {
                            ended = true;
                            break;
                        }

                        le2 = le2->Flink;
                    }
                }
            }
        } else if (bi->operation == Batch_DeleteExtentData) {
            if (tp.item->key.objectid < bi->key.objectid || (tp.item->key.objectid == bi->key.objectid && tp.item->key.type < bi->key.type)) {
                traverse_ptr tp2;

                if (find_next_item(Vcb, &tp, &tp2, false, Irp)) {
                    if (tp2.item->key.objectid == bi->key.objectid && tp2.item->key.type == bi->key.type) {
                        tp = tp2;
                        Status = find_tree_end(tp.tree, &tree_end, &no_end);
                        if (!NT_SUCCESS(Status)) {
                            ERR("find_tree_end returned %08lx\n", Status);
                            return Status;
                        }
                    }
                }
            }

            if (tp.item->key.objectid == bi->key.objectid && tp.item->key.type == bi->key.type) {
                bool ended = false;

                td = tp.item;

                if (!tp.item->ignore) {
                    tp.item->ignore = true;
                    tp.tree->header.nritems--;
                    tp.tree->size -= tp.item->size + sizeof(struct btrfs_item);
                    tp.tree->write = true;
                }

                le2 = tp.item->list_entry.Flink;
                while (le2 != &tp.tree->itemlist) {
                    td = CONTAINING_RECORD(le2, tree_data, list_entry);

                    if (td->key.objectid == bi->key.objectid && td->key.type == bi->key.type) {
                        if (!td->ignore) {
                            td->ignore = true;
                            tp.tree->header.nritems--;
                            tp.tree->size -= td->size + sizeof(struct btrfs_item);
                            tp.tree->write = true;
                        }
                    } else {
                        ended = true;
                        break;
                    }

                    le2 = le2->Flink;
                }

                while (!ended) {
                    traverse_ptr next_tp;

                    tp.item = td;

                    if (!find_next_item(Vcb, &tp, &next_tp, false, Irp))
                        break;

                    tp = next_tp;

                    le2 = &tp.item->list_entry;
                    while (le2 != &tp.tree->itemlist) {
                        td = CONTAINING_RECORD(le2, tree_data, list_entry);

                        if (td->key.objectid == bi->key.objectid && td->key.type == bi->key.type) {
                            if (!td->ignore) {
                                td->ignore = true;
                                tp.tree->header.nritems--;
                                tp.tree->size -= td->size + sizeof(struct btrfs_item);
                                tp.tree->write = true;
                            }
                        } else {
                            ended = true;
                            break;
                        }

                        le2 = le2->Flink;
                    }
                }
            }
        } else if (bi->operation == Batch_DeleteFreeSpace) {
            if (tp.item->key.objectid >= bi->key.objectid && tp.item->key.objectid < bi->key.objectid + bi->key.offset) {
                bool ended = false;

                td = tp.item;

                if (!tp.item->ignore) {
                    tp.item->ignore = true;
                    tp.tree->header.nritems--;
                    tp.tree->size -= tp.item->size + sizeof(struct btrfs_item);
                    tp.tree->write = true;
                }

                le2 = tp.item->list_entry.Flink;
                while (le2 != &tp.tree->itemlist) {
                    td = CONTAINING_RECORD(le2, tree_data, list_entry);

                    if (td->key.objectid >= bi->key.objectid && td->key.objectid < bi->key.objectid + bi->key.offset) {
                        if (!td->ignore) {
                            td->ignore = true;
                            tp.tree->header.nritems--;
                            tp.tree->size -= td->size + sizeof(struct btrfs_item);
                            tp.tree->write = true;
                        }
                    } else {
                        ended = true;
                        break;
                    }

                    le2 = le2->Flink;
                }

                while (!ended) {
                    traverse_ptr next_tp;

                    tp.item = td;

                    if (!find_next_item(Vcb, &tp, &next_tp, false, Irp))
                        break;

                    tp = next_tp;

                    le2 = &tp.item->list_entry;
                    while (le2 != &tp.tree->itemlist) {
                        td = CONTAINING_RECORD(le2, tree_data, list_entry);

                        if (td->key.objectid >= bi->key.objectid && td->key.objectid < bi->key.objectid + bi->key.offset) {
                            if (!td->ignore) {
                                td->ignore = true;
                                tp.tree->header.nritems--;
                                tp.tree->size -= td->size + sizeof(struct btrfs_item);
                                tp.tree->write = true;
                            }
                        } else {
                            ended = true;
                            break;
                        }

                        le2 = le2->Flink;
                    }
                }
            }
        } else {
            if (bi->operation == Batch_Delete || bi->operation == Batch_DeleteDirItem || bi->operation == Batch_DeleteInodeRef ||
                bi->operation == Batch_DeleteInodeExtRef || bi->operation == Batch_DeleteXattr)
                td = NULL;
            else {
                td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
                if (!td) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                td->key = bi->key;
                td->size = bi->datalen;
                td->data = bi->data;
                td->ignore = false;
                td->inserted = true;
            }

            cmp = keycmp(bi->key, tp.item->key);

            if (cmp == -1) { // very first key in root
                if (td) {
                    tree_data* paritem;

                    InsertHeadList(&tp.tree->itemlist, &td->list_entry);

                    paritem = tp.tree->paritem;
                    while (paritem) {
                        if (!keycmp(paritem->key, tp.item->key)) {
                            paritem->key = bi->key;
                        } else
                            break;

                        paritem = paritem->treeholder.tree->paritem;
                    }
                }
            } else if (cmp == 0) { // item already exists
                if (tp.item->ignore) {
                    if (td)
                        InsertHeadList(tp.item->list_entry.Blink, &td->list_entry);
                } else {
                    Status = handle_batch_collision(Vcb, bi, tp.tree, tp.item, td, &items, &ignore);
                    if (!NT_SUCCESS(Status)) {
                        ERR("handle_batch_collision returned %08lx\n", Status);
#ifdef _DEBUG
                        int3;
#endif

                        if (td)
                            ExFreeToPagedLookasideList(&Vcb->tree_data_lookaside, td);

                        return Status;
                    }
                }
            } else if (td) {
                InsertHeadList(&tp.item->list_entry, &td->list_entry);
            }

            if (bi->operation == Batch_DeleteInodeRef && cmp != 0 && Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF) {
                Status = add_delete_inode_extref(Vcb, bi, &items);
                if (!NT_SUCCESS(Status)) {
                    ERR("add_delete_inode_extref returned %08lx\n", Status);
                    return Status;
                }
            }

            if (!ignore && td) {
                tp.tree->header.nritems++;
                tp.tree->size += bi->datalen + sizeof(struct btrfs_item);
                tp.tree->write = true;

                listhead = td;
            } else
                listhead = tp.item;

            while (listhead->list_entry.Blink != &tp.tree->itemlist) {
                tree_data* prevtd = CONTAINING_RECORD(listhead->list_entry.Blink, tree_data, list_entry);

                if (!keycmp(prevtd->key, listhead->key))
                    listhead = prevtd;
                else
                    break;
            }

            le2 = le->Flink;
            while (le2 != &items) {
                batch_item* bi2 = CONTAINING_RECORD(le2, batch_item, list_entry);

                if (bi2->operation == Batch_DeleteInode || bi2->operation == Batch_DeleteExtentData || bi2->operation == Batch_DeleteFreeSpace)
                    break;

                if (no_end || keycmp(bi2->key, tree_end) == -1) {
                    LIST_ENTRY* le3;
                    bool inserted = false;

                    ignore = false;

                    if (bi2->operation == Batch_Delete || bi2->operation == Batch_DeleteDirItem || bi2->operation == Batch_DeleteInodeRef ||
                        bi2->operation == Batch_DeleteInodeExtRef || bi2->operation == Batch_DeleteXattr)
                        td = NULL;
                    else {
                        td = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
                        if (!td) {
                            ERR("out of memory\n");
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }

                        td->key = bi2->key;
                        td->size = bi2->datalen;
                        td->data = bi2->data;
                        td->ignore = false;
                        td->inserted = true;
                    }

                    le3 = &listhead->list_entry;
                    while (le3 != &tp.tree->itemlist) {
                        tree_data* td2 = CONTAINING_RECORD(le3, tree_data, list_entry);

                        cmp = keycmp(bi2->key, td2->key);

                        if (cmp == 0) {
                            if (td2->ignore) {
                                if (td) {
                                    InsertHeadList(le3->Blink, &td->list_entry);
                                    inserted = true;
                                } else if (bi2->operation == Batch_DeleteInodeRef && Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF) {
                                    Status = add_delete_inode_extref(Vcb, bi2, &items);
                                    if (!NT_SUCCESS(Status)) {
                                        ERR("add_delete_inode_extref returned %08lx\n", Status);
                                        return Status;
                                    }
                                }
                            } else {
                                Status = handle_batch_collision(Vcb, bi2, tp.tree, td2, td, &items, &ignore);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("handle_batch_collision returned %08lx\n", Status);
#ifdef _DEBUG
                                    int3;
#endif
                                    return Status;
                                }
                            }

                            inserted = true;
                            break;
                        } else if (cmp == -1) {
                            if (td) {
                                InsertHeadList(le3->Blink, &td->list_entry);
                                inserted = true;
                            } else if (bi2->operation == Batch_DeleteInodeRef && Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF) {
                                Status = add_delete_inode_extref(Vcb, bi2, &items);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("add_delete_inode_extref returned %08lx\n", Status);
                                    return Status;
                                }
                            }
                            break;
                        }

                        le3 = le3->Flink;
                    }

                    if (td) {
                        if (!inserted)
                            InsertTailList(&tp.tree->itemlist, &td->list_entry);

                        if (!ignore) {
                            tp.tree->header.nritems++;
                            tp.tree->size += bi2->datalen + sizeof(struct btrfs_item);

                            listhead = td;
                        }
                    } else if (!inserted && bi2->operation == Batch_DeleteInodeRef && Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF) {
                        Status = add_delete_inode_extref(Vcb, bi2, &items);
                        if (!NT_SUCCESS(Status)) {
                            ERR("add_delete_inode_extref returned %08lx\n", Status);
                            return Status;
                        }
                    }

                    while (listhead->list_entry.Blink != &tp.tree->itemlist) {
                        tree_data* prevtd = CONTAINING_RECORD(listhead->list_entry.Blink, tree_data, list_entry);

                        if (!keycmp(prevtd->key, listhead->key))
                            listhead = prevtd;
                        else
                            break;
                    }

                    le = le2;
                } else
                    break;

                le2 = le2->Flink;
            }

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
        }

        le = le->Flink;
    }

    // FIXME - remove as we are going along
    while (!IsListEmpty(&items)) {
        batch_item* bi = CONTAINING_RECORD(RemoveHeadList(&items), batch_item, list_entry);

        if ((bi->operation == Batch_DeleteDirItem || bi->operation == Batch_DeleteInodeRef ||
            bi->operation == Batch_DeleteInodeExtRef || bi->operation == Batch_DeleteXattr) && bi->data)
            ExFreePool(bi->data);

        ExFreeToPagedLookasideList(&Vcb->batch_item_lookaside, bi);
    }

    return STATUS_SUCCESS;
}

__attribute__((nonnull(1,2)))
NTSTATUS commit_batch_list(_Requires_exclusive_lock_held_(_Curr_->tree_lock) device_extension* Vcb, LIST_ENTRY* batchlist, PIRP Irp) {
    NTSTATUS Status;

    while (!IsListEmpty(batchlist)) {
        LIST_ENTRY* le = RemoveHeadList(batchlist);
        batch_root* br2 = CONTAINING_RECORD(le, batch_root, list_entry);

        Status = commit_batch_list_root(Vcb, br2, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("commit_batch_list_root returned %08lx\n", Status);
            return Status;
        }

        ExFreePool(br2);
    }

    return STATUS_SUCCESS;
}
