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

// Ordered free-space list insertion, subtraction and range merging.
#include "btrfs_drv.h"
#include "space-list.h"

static NTSTATUS add_rollback_space(LIST_ENTRY* rollback, bool add, LIST_ENTRY* list,
                                   LIST_ENTRY* list_size, uint64_t address,
                                   uint64_t length, chunk* c) {
    rollback_item* ri;

    ri = ExAllocatePoolWithTag(PagedPool, sizeof(rollback_item), ALLOC_TAG);
    if (!ri) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ri->type = add ? ROLLBACK_ADD_SPACE : ROLLBACK_SUBTRACT_SPACE;
    ri->space.list = list;
    ri->space.list_size = list_size;
    ri->space.address = address;
    ri->space.length = length;
    ri->space.chunk = c;

    InsertTailList(rollback, &ri->list_entry);

    return STATUS_SUCCESS;
}

void order_space_entry(space* s, LIST_ENTRY* list_size) {
    LIST_ENTRY* le;

    if (IsListEmpty(list_size)) {
        InsertHeadList(list_size, &s->list_entry_size);
        return;
    }

    le = list_size->Flink;

    while (le != list_size) {
        space* s2 = CONTAINING_RECORD(le, space, list_entry_size);

        if (s2->size <= s->size) {
            InsertHeadList(le->Blink, &s->list_entry_size);
            return;
        }

        le = le->Flink;
    }

    InsertTailList(list_size, &s->list_entry_size);
}

// Puts s into the size list in order or, if touched is not NULL, onto touched
// for order_space_entries to place later.
static void place_space_entry(space* s, LIST_ENTRY* list_size, LIST_ENTRY* touched) {
    if (touched)
        InsertTailList(touched, &s->list_entry_size);
    else
        order_space_entry(s, list_size);
}

// Adds the range to the address-sorted list, comparing it with the entries
// from start onwards. Every entry before start must end strictly below
// address: an entry ending at address is adjacent and has to be merged, so it
// must not lie before start. If touched is not NULL, entries that would be
// ordered into the size list go onto it instead, for order_space_entries to
// place. On success, *ps is the entry that now holds the range.
static NTSTATUS space_list_add_from(LIST_ENTRY* list, LIST_ENTRY* list_size, LIST_ENTRY* touched,
                                    LIST_ENTRY* start, uint64_t address, uint64_t length,
                                    chunk* c, LIST_ENTRY* rollback, space** ps) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    space *s, *s2;

    if (IsListEmpty(list)) {
        s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);

        if (!s) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        s->address = address;
        s->size = length;
        InsertTailList(list, &s->list_entry);

        if (list_size)
            InsertTailList(list_size, &s->list_entry_size);

        if (rollback) {
            Status = add_rollback_space(rollback, true, list, list_size,
                                        address, length, c);
            if (!NT_SUCCESS(Status))
                return Status;
        }

        *ps = s;
        return STATUS_SUCCESS;
    }

    le = start;
    do {
        s2 = CONTAINING_RECORD(le, space, list_entry);

        // old entry envelops new one completely
        if (s2->address <= address && s2->address + s2->size >= address + length) {
            *ps = s2;
            return STATUS_SUCCESS;
        }

        // new entry envelops old one completely
        if (address <= s2->address && address + length >= s2->address + s2->size) {
            if (address < s2->address) {
                if (rollback) {
                    Status = add_rollback_space(rollback, true, list, list_size,
                                                address, s2->address - address,
                                                c);
                    if (!NT_SUCCESS(Status))
                        return Status;
                }

                s2->size += s2->address - address;
                s2->address = address;

                while (s2->list_entry.Blink != list) {
                    space* s3 = CONTAINING_RECORD(s2->list_entry.Blink, space, list_entry);

                    if (s3->address + s3->size == s2->address) {
                        s2->address = s3->address;
                        s2->size += s3->size;

                        RemoveEntryList(&s3->list_entry);

                        if (list_size)
                            RemoveEntryList(&s3->list_entry_size);

                        ExFreePool(s3);
                    } else
                        break;
                }
            }

            if (length > s2->size) {
                if (rollback) {
                    Status = add_rollback_space(rollback, true, list, list_size,
                                                s2->address + s2->size, address + length - s2->address - s2->size,
                                                c);
                    if (!NT_SUCCESS(Status))
                        return Status;
                }

                s2->size = length;

                while (s2->list_entry.Flink != list) {
                    space* s3 = CONTAINING_RECORD(s2->list_entry.Flink, space, list_entry);

                    if (s3->address <= s2->address + s2->size) {
                        s2->size = max(s2->size, s3->address + s3->size - s2->address);

                        RemoveEntryList(&s3->list_entry);

                        if (list_size)
                            RemoveEntryList(&s3->list_entry_size);

                        ExFreePool(s3);
                    } else
                        break;
                }
            }

            if (list_size) {
                RemoveEntryList(&s2->list_entry_size);
                place_space_entry(s2, list_size, touched);
            }

            *ps = s2;
            return STATUS_SUCCESS;
        }

        // new entry overlaps start of old one
        if (address < s2->address && address + length >= s2->address) {
            if (rollback) {
                Status = add_rollback_space(rollback, true, list, list_size,
                                            address, s2->address - address, c);
                if (!NT_SUCCESS(Status))
                    return Status;
            }

            s2->size += s2->address - address;
            s2->address = address;

            while (s2->list_entry.Blink != list) {
                space* s3 = CONTAINING_RECORD(s2->list_entry.Blink, space, list_entry);

                if (s3->address + s3->size == s2->address) {
                    s2->address = s3->address;
                    s2->size += s3->size;

                    RemoveEntryList(&s3->list_entry);

                    if (list_size)
                        RemoveEntryList(&s3->list_entry_size);

                    ExFreePool(s3);
                } else
                    break;
            }

            if (list_size) {
                RemoveEntryList(&s2->list_entry_size);
                place_space_entry(s2, list_size, touched);
            }

            *ps = s2;
            return STATUS_SUCCESS;
        }

        // new entry overlaps end of old one
        if (address <= s2->address + s2->size && address + length > s2->address + s2->size) {
            if (rollback) {
                Status = add_rollback_space(rollback, true, list, list_size, address,
                                            s2->address + s2->size - address, c);
                if (!NT_SUCCESS(Status))
                    return Status;
            }

            s2->size = address + length - s2->address;

            while (s2->list_entry.Flink != list) {
                space* s3 = CONTAINING_RECORD(s2->list_entry.Flink, space, list_entry);

                if (s3->address <= s2->address + s2->size) {
                    s2->size = max(s2->size, s3->address + s3->size - s2->address);

                    RemoveEntryList(&s3->list_entry);

                    if (list_size)
                        RemoveEntryList(&s3->list_entry_size);

                    ExFreePool(s3);
                } else
                    break;
            }

            if (list_size) {
                RemoveEntryList(&s2->list_entry_size);
                place_space_entry(s2, list_size, touched);
            }

            *ps = s2;
            return STATUS_SUCCESS;
        }

        // add completely separate entry
        if (s2->address > address + length) {
            s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);

            if (!s) {
                ERR("out of memory\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            if (rollback) {
                Status = add_rollback_space(rollback, true, list, list_size,
                                            address, length, c);
                if (!NT_SUCCESS(Status)) {
                    ExFreePool(s);
                    return Status;
                }
            }

            s->address = address;
            s->size = length;
            InsertHeadList(s2->list_entry.Blink, &s->list_entry);

            if (list_size)
                place_space_entry(s, list_size, touched);

            *ps = s;
            return STATUS_SUCCESS;
        }

        le = le->Flink;
    } while (le != list);

    // check if contiguous with last entry
    if (s2->address + s2->size == address) {
        s2->size += length;

        if (list_size) {
            RemoveEntryList(&s2->list_entry_size);
            place_space_entry(s2, list_size, touched);
        }

        *ps = s2;
        return STATUS_SUCCESS;
    }

    // otherwise, insert at end
    s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);

    if (!s) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (rollback) {
        Status = add_rollback_space(rollback, true, list, list_size, address,
                                    length, c);
        if (!NT_SUCCESS(Status)) {
            ExFreePool(s);
            return Status;
        }
    }

    s->address = address;
    s->size = length;
    InsertTailList(list, &s->list_entry);

    if (list_size)
        place_space_entry(s, list_size, touched);

    *ps = s;
    return STATUS_SUCCESS;
}

NTSTATUS space_list_add2(LIST_ENTRY* list, LIST_ENTRY* list_size, uint64_t address,
                         uint64_t length, chunk* c, LIST_ENTRY* rollback) {
    space* s;

    return space_list_add_from(list, list_size, NULL, list->Flink, address, length, c, rollback, &s);
}

// Moves the entries on touched into the size list, which stays ordered by size,
// largest first. touched is sorted with a bottom-up merge sort, then spliced in
// during one pass along the size list, so for m entries on touched and n in the
// size list this takes O(n + m log m) time, without allocating or recursing.
static void order_space_entries(LIST_ENTRY* list_size, LIST_ENTRY* touched) {
    LIST_ENTRY *le, *head, **tail;
    size_t run = 1, merges;

    if (IsListEmpty(touched))
        return;

    // treat touched as a chain through Flink that ends in NULL
    head = touched->Flink;
    touched->Blink->Flink = NULL;
    InitializeListHead(touched);

    // merge each pair of neighbouring sorted runs, doubling the run length on each pass
    do {
        LIST_ENTRY* p = head;

        head = NULL;
        tail = &head;
        merges = 0;

        while (p) {
            LIST_ENTRY* q = p;
            size_t psize = 0, qsize = run;

            merges++;

            while (q && psize < run) {
                psize++;
                q = q->Flink;
            }

            while (psize > 0 || (qsize > 0 && q)) {
                LIST_ENTRY* e;

                if (psize > 0 && (qsize == 0 || !q ||
                    CONTAINING_RECORD(p, space, list_entry_size)->size >= CONTAINING_RECORD(q, space, list_entry_size)->size)) {
                    e = p;
                    p = p->Flink;
                    psize--;
                } else {
                    e = q;
                    q = q->Flink;
                    qsize--;
                }

                *tail = e;
                tail = &e->Flink;
            }

            p = q;
        }

        *tail = NULL;
        run *= 2;
    } while (merges > 1);

    // Splice the chain into the size list in one forward pass: each entry goes
    // before the first entry ahead of the cursor that is no bigger. The order
    // of entries of equal size is not significant.
    le = list_size->Flink;

    while (head) {
        space* s = CONTAINING_RECORD(head, space, list_entry_size);

        head = head->Flink;

        while (le != list_size && CONTAINING_RECORD(le, space, list_entry_size)->size > s->size) {
            le = le->Flink;
        }

        InsertHeadList(le->Blink, &s->list_entry_size);
    }
}

NTSTATUS space_list_merge(LIST_ENTRY* spacelist, LIST_ENTRY* spacelist_size,
                          LIST_ENTRY* deleting) {
    NTSTATUS Status = STATUS_SUCCESS;
    LIST_ENTRY* le = deleting->Flink;
    LIST_ENTRY* start = spacelist->Flink;
    LIST_ENTRY touched;

    if (IsListEmpty(deleting))
        return STATUS_SUCCESS;

    // Ordering each added or resized entry into the size list straight away
    // would mean a walk along the size list for every range. Collect them on
    // touched instead and order them all in at the end, which also happens
    // after a failed allocation.
    InitializeListHead(&touched);

    while (le != deleting) {
        space* s = CONTAINING_RECORD(le, space, list_entry);
        space* s2;

        // Both lists are sorted by address, so each search can carry on from
        // the entry that took the previous range: the entries before it end
        // strictly below where it starts. If the range starts before that
        // entry, deleting is out of order: start again from the head.
        if (start != spacelist && s->address < CONTAINING_RECORD(start, space, list_entry)->address)
            start = spacelist->Flink;

        Status = space_list_add_from(spacelist, spacelist_size, &touched, start,
                                     s->address, s->size, NULL, NULL, &s2);
        if (!NT_SUCCESS(Status))
            break;

        // Adding a range can free the entries next to the one that took it,
        // so carry on from that entry rather than an older one.
        start = &s2->list_entry;

        le = le->Flink;
    }

    if (spacelist_size)
        order_space_entries(spacelist_size, &touched);

    return Status;
}

NTSTATUS space_list_reclaim(LIST_ENTRY* list, LIST_ENTRY* list_size,
                            LIST_ENTRY* ranges, uint64_t limit) {
    NTSTATUS Status = STATUS_SUCCESS;
    LIST_ENTRY* start = list->Flink;
    LIST_ENTRY touched;
    InitializeListHead(&touched);

    while (!IsListEmpty(ranges)) {
        space* s = CONTAINING_RECORD(ranges->Flink, space, list_entry);
        uint64_t size = s->address >= limit ? 0 : min(s->size, limit - s->address);
        if (size) {
            space* holder;
            if (start != list && s->address < CONTAINING_RECORD(start, space, list_entry)->address)
                start = list->Flink;
            Status = space_list_add_from(list, list_size, &touched, start,
                                         s->address, size, NULL, NULL, &holder);
            if (!NT_SUCCESS(Status))
                break;
            start = &holder->list_entry;
        }
        // Published ranges must leave the pending cache view before any later
        // allocation failure can return control to the allocator.
        RemoveEntryList(&s->list_entry);
        ExFreePool(s);
    }
    if (list_size)
        order_space_entries(list_size, &touched);
    return Status;
}

NTSTATUS space_list_subtract2(LIST_ENTRY* list, LIST_ENTRY* list_size,
                              uint64_t address, uint64_t length, chunk* c,
                              LIST_ENTRY* rollback) {
    NTSTATUS Status;
    LIST_ENTRY *le, *le2;
    space *s, *s2;

    if (IsListEmpty(list))
        return STATUS_SUCCESS;

    le = list->Flink;
    while (le != list) {
        s2 = CONTAINING_RECORD(le, space, list_entry);
        le2 = le->Flink;

        if (s2->address >= address + length)
            return STATUS_SUCCESS;

        if (s2->address >= address && s2->address + s2->size <= address + length) { // remove entry entirely
            if (rollback) {
                Status = add_rollback_space(rollback, false, list, list_size,
                                            s2->address, s2->size, c);
                if (!NT_SUCCESS(Status))
                    return Status;
            }

            RemoveEntryList(&s2->list_entry);

            if (list_size)
                RemoveEntryList(&s2->list_entry_size);

            ExFreePool(s2);
        } else if (address + length > s2->address && address + length < s2->address + s2->size) {
            if (address > s2->address) { // cut out hole
                s = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);

                if (!s) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                if (rollback) {
                    Status = add_rollback_space(rollback, false, list, list_size,
                                                address, length, c);
                    if (!NT_SUCCESS(Status)) {
                        ExFreePool(s);
                        return Status;
                    }
                }

                s->address = s2->address;
                s->size = address - s2->address;
                InsertHeadList(s2->list_entry.Blink, &s->list_entry);

                s2->size = s2->address + s2->size - address - length;
                s2->address = address + length;

                if (list_size) {
                    RemoveEntryList(&s2->list_entry_size);
                    order_space_entry(s2, list_size);
                    order_space_entry(s, list_size);
                }

                return STATUS_SUCCESS;
            } else { // remove start of entry
                if (rollback) {
                    Status = add_rollback_space(rollback, false, list, list_size,
                                                s2->address, address + length - s2->address,
                                                c);
                    if (!NT_SUCCESS(Status))
                        return Status;
                }

                s2->size -= address + length - s2->address;
                s2->address = address + length;

                if (list_size) {
                    RemoveEntryList(&s2->list_entry_size);
                    order_space_entry(s2, list_size);
                }
            }
        } else if (address > s2->address && address < s2->address + s2->size) { // remove end of entry
            if (rollback) {
                Status = add_rollback_space(rollback, false, list, list_size,
                                            address, s2->address + s2->size - address,
                                            c);
                if (!NT_SUCCESS(Status))
                    return Status;
            }

            s2->size = address - s2->address;

            if (list_size) {
                RemoveEntryList(&s2->list_entry_size);
                order_space_entry(s2, list_size);
            }
        }

        le = le2;
    }

    return STATUS_SUCCESS;
}
