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

// Ordered tree lookup and traversal with the caller's existing tree lock.
#include "btrfs_drv.h"

__attribute__((nonnull(1)))
static __inline tree_data* first_item(tree* t) {
    LIST_ENTRY* le = t->itemlist.Flink;

    if (le == &t->itemlist)
        return NULL;

    return CONTAINING_RECORD(le, tree_data, list_entry);
}

__attribute__((nonnull(1,2)))
static __inline tree_data* prev_item(tree* t, tree_data* td) {
    LIST_ENTRY* le = td->list_entry.Blink;

    if (le == &t->itemlist)
        return NULL;

    return CONTAINING_RECORD(le, tree_data, list_entry);
}

__attribute__((nonnull(1,2)))
static __inline tree_data* next_item(tree* t, tree_data* td) {
    LIST_ENTRY* le = td->list_entry.Flink;

    if (le == &t->itemlist)
        return NULL;

    return CONTAINING_RECORD(le, tree_data, list_entry);
}

__attribute__((nonnull(1,2,3,4)))
static NTSTATUS next_item2(device_extension* Vcb, tree* t, tree_data* td, traverse_ptr* tp) {
    tree_data* td2 = next_item(t, td);
    tree* t2;

    if (td2) {
        tp->tree = t;
        tp->item = td2;
        return STATUS_SUCCESS;
    }

    t2 = t;

    do {
        td2 = t2->paritem;
        t2 = t2->parent;
    } while (td2 && !next_item(t2, td2));

    if (!td2)
        return STATUS_NOT_FOUND;

    td2 = next_item(t2, td2);

    return find_item_to_level(Vcb, t2->root, tp, &td2->key, false, t->header.level, NULL);
}

__attribute__((nonnull(1,2,3,4,5)))
NTSTATUS skip_to_difference(device_extension* Vcb, traverse_ptr* tp, traverse_ptr* tp2, bool* ended1, bool* ended2) {
    NTSTATUS Status;
    tree *t1, *t2;
    tree_data *td1, *td2;

    t1 = tp->tree;
    t2 = tp2->tree;

    do {
        td1 = t1->paritem;
        td2 = t2->paritem;
        t1 = t1->parent;
        t2 = t2->parent;
    } while (t1 && t2 && t1->header.bytenr == t2->header.bytenr);

    while (true) {
        traverse_ptr tp3, tp4;

        Status = next_item2(Vcb, t1, td1, &tp3);
        if (Status == STATUS_NOT_FOUND)
            *ended1 = true;
        else if (!NT_SUCCESS(Status)) {
            ERR("next_item2 returned %08lx\n", Status);
            return Status;
        }

        Status = next_item2(Vcb, t2, td2, &tp4);
        if (Status == STATUS_NOT_FOUND)
            *ended2 = true;
        else if (!NT_SUCCESS(Status)) {
            ERR("next_item2 returned %08lx\n", Status);
            return Status;
        }

        if (*ended1 || *ended2) {
            if (!*ended1) {
                Status = find_item(Vcb, t1->root, tp, &tp3.item->key, false, NULL);
                if (!NT_SUCCESS(Status)) {
                    ERR("find_item returned %08lx\n", Status);
                    return Status;
                }
            } else if (!*ended2) {
                Status = find_item(Vcb, t2->root, tp2, &tp4.item->key, false, NULL);
                if (!NT_SUCCESS(Status)) {
                    ERR("find_item returned %08lx\n", Status);
                    return Status;
                }
            }

            return STATUS_SUCCESS;
        }

        if (tp3.tree->header.bytenr != tp4.tree->header.bytenr) {
            Status = find_item(Vcb, t1->root, tp, &tp3.item->key, false, NULL);
            if (!NT_SUCCESS(Status)) {
                ERR("find_item returned %08lx\n", Status);
                return Status;
            }

            Status = find_item(Vcb, t2->root, tp2, &tp4.item->key, false, NULL);
            if (!NT_SUCCESS(Status)) {
                ERR("find_item returned %08lx\n", Status);
                return Status;
            }

            return STATUS_SUCCESS;
        }

        t1 = tp3.tree;
        td1 = tp3.item;
        t2 = tp4.tree;
        td2 = tp4.item;
    }
}

__attribute__((nonnull(1,2,3,4)))
static NTSTATUS find_item_in_tree(device_extension* Vcb, tree* t, traverse_ptr* tp, const struct btrfs_key* searchkey, bool ignore, uint8_t level, PIRP Irp) {
    int cmp;
    tree_data *td, *lasttd;
    struct btrfs_key key2;

    cmp = 1;
    td = first_item(t);
    lasttd = NULL;

    if (!td) return STATUS_NOT_FOUND;

    key2 = *searchkey;

    do {
        cmp = keycmp(key2, td->key);

        if (cmp == 1) {
            lasttd = td;
            td = next_item(t, td);
        }

        if (t->header.level == 0 && cmp == 0 && !ignore && td && td->ignore) {
            tree_data* origtd = td;

            while (td && td->ignore)
                td = next_item(t, td);

            if (td) {
                cmp = keycmp(key2, td->key);

                if (cmp != 0) {
                    td = origtd;
                    cmp = 0;
                }
            } else
                td = origtd;
        }
    } while (td && cmp == 1);

    if ((cmp == -1 || !td) && lasttd)
        td = lasttd;

    if (t->header.level == 0) {
        if (td->ignore && !ignore) {
            traverse_ptr oldtp;

            oldtp.tree = t;
            oldtp.item = td;

            while (find_prev_item(Vcb, &oldtp, tp, Irp)) {
                if (!tp->item->ignore)
                    return STATUS_SUCCESS;

                oldtp = *tp;
            }

            // if no valid entries before where item should be, look afterwards instead

            oldtp.tree = t;
            oldtp.item = td;

            while (find_next_item(Vcb, &oldtp, tp, true, Irp)) {
                if (!tp->item->ignore)
                    return STATUS_SUCCESS;

                oldtp = *tp;
            }

            return STATUS_NOT_FOUND;
        } else {
            tp->tree = t;
            tp->item = td;
        }

        return STATUS_SUCCESS;
    } else {
        NTSTATUS Status;

        while (td && td->treeholder.tree && IsListEmpty(&td->treeholder.tree->itemlist)) {
            td = prev_item(t, td);
        }

        if (!td)
            return STATUS_NOT_FOUND;

        if (t->header.level <= level) {
            tp->tree = t;
            tp->item = td;
            return STATUS_SUCCESS;
        }

        if (!td->treeholder.tree) {
            Status = do_load_tree(Vcb, &td->treeholder, t->root, t, td, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("do_load_tree returned %08lx\n", Status);
                return Status;
            }
        }

        Status = find_item_in_tree(Vcb, td->treeholder.tree, tp, searchkey, ignore, level, Irp);

        return Status;
    }
}

__attribute__((nonnull(1,2,3,4)))
NTSTATUS find_item(_In_ _Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, _In_ root* r, _Out_ traverse_ptr* tp,
                   _In_ const struct btrfs_key* searchkey, _In_ bool ignore, _In_opt_ PIRP Irp) {
    NTSTATUS Status;

    if (!r->treeholder.tree) {
        Status = do_load_tree(Vcb, &r->treeholder, r, NULL, NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_load_tree returned %08lx\n", Status);
            return Status;
        }
    }

    Status = find_item_in_tree(Vcb, r->treeholder.tree, tp, searchkey, ignore, 0, Irp);
    if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
        ERR("find_item_in_tree returned %08lx\n", Status);
    }

    return Status;
}

__attribute__((nonnull(1,2,3,4)))
NTSTATUS find_item_to_level(device_extension* Vcb, root* r, traverse_ptr* tp, const struct btrfs_key* searchkey, bool ignore, uint8_t level, PIRP Irp) {
    NTSTATUS Status;

    if (!r->treeholder.tree) {
        Status = do_load_tree(Vcb, &r->treeholder, r, NULL, NULL, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_load_tree returned %08lx\n", Status);
            return Status;
        }
    }

    Status = find_item_in_tree(Vcb, r->treeholder.tree, tp, searchkey, ignore, level, Irp);
    if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
        ERR("find_item_in_tree returned %08lx\n", Status);
    }

    if (Status == STATUS_NOT_FOUND) {
        tp->tree = r->treeholder.tree;
        tp->item = NULL;
    }

    return Status;
}

__attribute__((nonnull(1,2,3)))
bool find_next_item(_Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, const traverse_ptr* tp, traverse_ptr* next_tp, bool ignore, PIRP Irp) {
    tree* t;
    tree_data *td = NULL, *next;
    NTSTATUS Status;

    next = next_item(tp->tree, tp->item);

    if (!ignore) {
        while (next && next->ignore)
            next = next_item(tp->tree, next);
    }

    if (next) {
        next_tp->tree = tp->tree;
        next_tp->item = next;

#ifdef DEBUG_PARANOID
        if (!ignore && next_tp->item->ignore) {
            ERR("error - returning ignored item\n");
            int3;
        }
#endif

        return true;
    }

    if (!tp->tree->parent)
        return false;

    t = tp->tree;
    do {
        if (t->parent) {
            td = next_item(t->parent, t->paritem);

            if (td) break;
        }

        t = t->parent;
    } while (t);

    if (!t)
        return false;

    if (!td->treeholder.tree) {
        Status = do_load_tree(Vcb, &td->treeholder, t->parent->root, t->parent, td, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_load_tree returned %08lx\n", Status);
            return false;
        }
    }

    t = td->treeholder.tree;

    while (t->header.level != 0) {
        tree_data* fi;

        fi = first_item(t);

        if (!fi)
            return false;

        if (!fi->treeholder.tree) {
            Status = do_load_tree(Vcb, &fi->treeholder, t->parent->root, t, fi, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("do_load_tree returned %08lx\n", Status);
                return false;
            }
        }

        t = fi->treeholder.tree;
    }

    next_tp->tree = t;
    next_tp->item = first_item(t);

    if (!next_tp->item)
        return false;

    if (!ignore && next_tp->item->ignore) {
        traverse_ptr ntp2;
        bool b;

        while ((b = find_next_item(Vcb, next_tp, &ntp2, true, Irp))) {
            *next_tp = ntp2;

            if (!next_tp->item->ignore)
                break;
        }

        if (!b)
            return false;
    }

#ifdef DEBUG_PARANOID
    if (!ignore && next_tp->item->ignore) {
        ERR("error - returning ignored item\n");
        int3;
    }
#endif

    return true;
}

__attribute__((nonnull(1)))
static __inline tree_data* last_item(tree* t) {
    LIST_ENTRY* le = t->itemlist.Blink;

    if (le == &t->itemlist)
        return NULL;

    return CONTAINING_RECORD(le, tree_data, list_entry);
}

__attribute__((nonnull(1,2,3)))
bool find_prev_item(_Requires_lock_held_(_Curr_->tree_lock) device_extension* Vcb, const traverse_ptr* tp, traverse_ptr* prev_tp, PIRP Irp) {
    tree* t;
    tree_data* td;
    NTSTATUS Status;

    // FIXME - support ignore flag
    if (prev_item(tp->tree, tp->item)) {
        prev_tp->tree = tp->tree;
        prev_tp->item = prev_item(tp->tree, tp->item);

        return true;
    }

    if (!tp->tree->parent)
        return false;

    t = tp->tree;
    while (t && (!t->parent || !prev_item(t->parent, t->paritem))) {
        t = t->parent;
    }

    if (!t)
        return false;

    td = prev_item(t->parent, t->paritem);

    if (!td->treeholder.tree) {
        Status = do_load_tree(Vcb, &td->treeholder, t->parent->root, t->parent, td, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_load_tree returned %08lx\n", Status);
            return false;
        }
    }

    t = td->treeholder.tree;

    while (t->header.level != 0) {
        tree_data* li;

        li = last_item(t);

        if (!li->treeholder.tree) {
            Status = do_load_tree(Vcb, &li->treeholder, t->parent->root, t, li, Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("do_load_tree returned %08lx\n", Status);
                return false;
            }
        }

        t = li->treeholder.tree;
    }

    prev_tp->tree = t;
    prev_tp->item = last_item(t);

    return true;
}
