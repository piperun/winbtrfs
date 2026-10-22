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

/* Resolve collisions and extended inode references in batched updates. */
#include "batch.h"
#include "crc32c.h"

__attribute__((nonnull(1,2,3)))
NTSTATUS add_delete_inode_extref(device_extension* Vcb, batch_item* bi, LIST_ENTRY* listhead) {
    batch_item* bi2;
    LIST_ENTRY* le;
    struct btrfs_inode_ref* delir = (struct btrfs_inode_ref*)bi->data;
    struct btrfs_inode_extref* ier;

    TRACE("entry in INODE_REF not found, adding Batch_DeleteInodeExtRef entry\n");

    bi2 = ExAllocateFromPagedLookasideList(&Vcb->batch_item_lookaside);
    if (!bi2) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ier = ExAllocatePoolWithTag(PagedPool, offsetof(struct btrfs_inode_extref, name) + delir->name_len, ALLOC_TAG);
    if (!ier) {
        ERR("out of memory\n");
        ExFreeToPagedLookasideList(&Vcb->batch_item_lookaside, bi2);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ier->parent_objectid = bi->key.offset;
    ier->index = delir->index;
    ier->name_len = delir->name_len;
    RtlCopyMemory(ier->name, &delir[1], delir->name_len);

    bi2->key.objectid = bi->key.objectid;
    bi2->key.type = BTRFS_INODE_EXTREF_KEY;
    bi2->key.offset = calc_crc32c((uint32_t)bi->key.offset, (uint8_t*)ier->name, ier->name_len);
    bi2->data = ier;
    bi2->datalen = offsetof(struct btrfs_inode_extref, name) + ier->name_len;
    bi2->operation = Batch_DeleteInodeExtRef;

    le = bi->list_entry.Flink;
    while (le != listhead) {
        batch_item* bi3 = CONTAINING_RECORD(le, batch_item, list_entry);

        if (keycmp(bi3->key, bi2->key) != -1) {
            InsertHeadList(le->Blink, &bi2->list_entry);
            return STATUS_SUCCESS;
        }

        le = le->Flink;
    }

    InsertTailList(listhead, &bi2->list_entry);

    return STATUS_SUCCESS;
}

__attribute__((nonnull(1,2,3,4,6,7)))
NTSTATUS handle_batch_collision(device_extension* Vcb, batch_item* bi, tree* t, tree_data* td, tree_data* newtd, LIST_ENTRY* listhead, bool* ignore) {
    NTSTATUS Status;

    if (bi->operation == Batch_Delete || bi->operation == Batch_SetXattr || bi->operation == Batch_DirItem || bi->operation == Batch_InodeRef ||
        bi->operation == Batch_InodeExtRef || bi->operation == Batch_DeleteDirItem || bi->operation == Batch_DeleteInodeRef ||
        bi->operation == Batch_DeleteInodeExtRef || bi->operation == Batch_DeleteXattr) {
        uint16_t maxlen = (uint16_t)(Vcb->superblock.nodesize - sizeof(struct btrfs_header) - sizeof(struct btrfs_item));

        switch (bi->operation) {
            case Batch_SetXattr: {
                if (td->size < sizeof(struct btrfs_dir_item)) {
                    ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", bi->key.objectid, bi->key.type, bi->key.offset, td->size, sizeof(struct btrfs_dir_item));
                } else {
                    uint8_t* newdata;
                    ULONG size = td->size;
                    struct btrfs_dir_item* newxa = (struct btrfs_dir_item*)bi->data;
                    struct btrfs_dir_item* xa = (struct btrfs_dir_item*)td->data;

                    while (true) {
                        ULONG oldxasize;

                        if (size < sizeof(struct btrfs_dir_item) || size < sizeof(struct btrfs_dir_item) + xa->data_len + xa->name_len) {
                            ERR("(%I64x,%x,%I64x) was truncated\n", bi->key.objectid, bi->key.type, bi->key.offset);
                            break;
                        }

                        oldxasize = sizeof(struct btrfs_dir_item) + xa->data_len + xa->name_len;

                        if (xa->name_len == newxa->name_len && RtlCompareMemory(&newxa[1], &xa[1], xa->name_len) == xa->name_len) {
                            uint64_t pos;

                            // replace

                            if (td->size + bi->datalen - oldxasize > maxlen)
                                ERR("DIR_ITEM would be over maximum size, truncating (%u + %u - %lu > %u)\n", td->size, bi->datalen, oldxasize, maxlen);

                            newdata = ExAllocatePoolWithTag(PagedPool, td->size + bi->datalen - oldxasize, ALLOC_TAG);
                            if (!newdata) {
                                ERR("out of memory\n");
                                return STATUS_INSUFFICIENT_RESOURCES;
                            }

                            pos = (uint8_t*)xa - td->data;
                            if (pos + oldxasize < td->size) // copy after changed xattr
                                RtlCopyMemory(newdata + pos + bi->datalen, td->data + pos + oldxasize, (ULONG)(td->size - pos - oldxasize));

                            if (pos > 0) { // copy before changed xattr
                                RtlCopyMemory(newdata, td->data, (ULONG)pos);
                                xa = (struct btrfs_dir_item*)(newdata + pos);
                            } else
                                xa = (struct btrfs_dir_item*)newdata;

                            RtlCopyMemory(xa, bi->data, bi->datalen);

                            bi->datalen = (uint16_t)min(td->size + bi->datalen - oldxasize, maxlen);

                            ExFreePool(bi->data);
                            bi->data = newdata;

                            break;
                        }

                        if ((uint8_t*)xa - (uint8_t*)td->data + oldxasize >= size) {
                            // not found, add to end of data

                            if (td->size + bi->datalen > maxlen)
                                ERR("DIR_ITEM would be over maximum size, truncating (%u + %u > %u)\n", td->size, bi->datalen, maxlen);

                            newdata = ExAllocatePoolWithTag(PagedPool, td->size + bi->datalen, ALLOC_TAG);
                            if (!newdata) {
                                ERR("out of memory\n");
                                return STATUS_INSUFFICIENT_RESOURCES;
                            }

                            RtlCopyMemory(newdata, td->data, td->size);

                            xa = (struct btrfs_dir_item*)((uint8_t*)newdata + td->size);
                            RtlCopyMemory(xa, bi->data, bi->datalen);

                            bi->datalen = min(bi->datalen + td->size, maxlen);

                            ExFreePool(bi->data);
                            bi->data = newdata;

                            break;
                        } else {
                            xa = (struct btrfs_dir_item*)((uint8_t*)&xa[1] + xa->data_len + xa->name_len);
                            size -= oldxasize;
                        }
                    }
                }
                break;
            }

            case Batch_DirItem: {
                uint8_t* newdata;

                if (td->size + bi->datalen > maxlen) {
                    ERR("DIR_ITEM would be over maximum size (%u + %u > %u)\n", td->size, bi->datalen, maxlen);
                    return STATUS_INTERNAL_ERROR;
                }

                newdata = ExAllocatePoolWithTag(PagedPool, td->size + bi->datalen, ALLOC_TAG);
                if (!newdata) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(newdata, td->data, td->size);

                RtlCopyMemory(newdata + td->size, bi->data, bi->datalen);

                bi->datalen += td->size;

                ExFreePool(bi->data);
                bi->data = newdata;

                break;
            }

            case Batch_InodeRef: {
                uint8_t* newdata;

                if (td->size + bi->datalen > maxlen) {
                    if (Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF) {
                        struct btrfs_inode_ref* ir = (struct btrfs_inode_ref*)bi->data;
                        struct btrfs_inode_extref* ier;
                        uint16_t ierlen;
                        batch_item* bi2;
                        LIST_ENTRY* le;
                        bool inserted = false;

                        TRACE("INODE_REF would be too long, adding INODE_EXTREF instead\n");

                        ierlen = (uint16_t)(offsetof(struct btrfs_inode_extref, name) + ir->name_len);

                        ier = ExAllocatePoolWithTag(PagedPool, ierlen, ALLOC_TAG);
                        if (!ier) {
                            ERR("out of memory\n");
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }

                        ier->parent_objectid = bi->key.offset;
                        ier->index = ir->index;
                        ier->name_len = ir->name_len;
                        RtlCopyMemory(ier->name, &ir[1], ier->name_len);

                        bi2 = ExAllocateFromPagedLookasideList(&Vcb->batch_item_lookaside);
                        if (!bi2) {
                            ERR("out of memory\n");
                            ExFreePool(ier);
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }

                        bi2->key.objectid = bi->key.objectid;
                        bi2->key.type = BTRFS_INODE_EXTREF_KEY;
                        bi2->key.offset = calc_crc32c((uint32_t)ier->parent_objectid, (uint8_t*)ier->name, ier->name_len);
                        bi2->data = ier;
                        bi2->datalen = ierlen;
                        bi2->operation = Batch_InodeExtRef;

                        le = bi->list_entry.Flink;
                        while (le != listhead) {
                            batch_item* bi3 = CONTAINING_RECORD(le, batch_item, list_entry);

                            if (keycmp(bi3->key, bi2->key) != -1) {
                                InsertHeadList(le->Blink, &bi2->list_entry);
                                inserted = true;
                            }

                            le = le->Flink;
                        }

                        if (!inserted)
                            InsertTailList(listhead, &bi2->list_entry);

                        *ignore = true;
                        return STATUS_SUCCESS;
                    } else {
                        ERR("INODE_REF would be over maximum size (%u + %u > %u)\n", td->size, bi->datalen, maxlen);
                        return STATUS_INTERNAL_ERROR;
                    }
                }

                newdata = ExAllocatePoolWithTag(PagedPool, td->size + bi->datalen, ALLOC_TAG);
                if (!newdata) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(newdata, td->data, td->size);

                RtlCopyMemory(newdata + td->size, bi->data, bi->datalen);

                bi->datalen += td->size;

                ExFreePool(bi->data);
                bi->data = newdata;

                break;
            }

            case Batch_InodeExtRef: {
                uint8_t* newdata;

                if (td->size + bi->datalen > maxlen) {
                    ERR("INODE_EXTREF would be over maximum size (%u + %u > %u)\n", td->size, bi->datalen, maxlen);
                    return STATUS_INTERNAL_ERROR;
                }

                newdata = ExAllocatePoolWithTag(PagedPool, td->size + bi->datalen, ALLOC_TAG);
                if (!newdata) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                RtlCopyMemory(newdata, td->data, td->size);

                RtlCopyMemory(newdata + td->size, bi->data, bi->datalen);

                bi->datalen += td->size;

                ExFreePool(bi->data);
                bi->data = newdata;

                break;
            }

            case Batch_DeleteDirItem: {
                if (td->size < sizeof(struct btrfs_dir_item)) {
                    ERR("DIR_ITEM was %u bytes, expected at least %Iu\n", td->size, sizeof(struct btrfs_dir_item));
                    return STATUS_INTERNAL_ERROR;
                } else {
                    struct btrfs_dir_item *di, *deldi;
                    LONG len;

                    deldi = (struct btrfs_dir_item*)bi->data;
                    di = (struct btrfs_dir_item*)td->data;
                    len = td->size;

                    do {
                        if (di->data_len == deldi->data_len && di->name_len == deldi->name_len && RtlCompareMemory(&di[1], &deldi[1], di->name_len + di->data_len) == di->name_len + di->data_len) {
                            uint16_t newlen = td->size - (sizeof(struct btrfs_dir_item) + di->name_len + di->data_len);

                            if (newlen == 0) {
                                TRACE("deleting DIR_ITEM\n");
                            } else {
                                uint8_t *newdi = ExAllocatePoolWithTag(PagedPool, newlen, ALLOC_TAG), *dioff;
                                tree_data* td2;

                                if (!newdi) {
                                    ERR("out of memory\n");
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                TRACE("modifying DIR_ITEM\n");

                                if ((uint8_t*)di > td->data) {
                                    RtlCopyMemory(newdi, td->data, (uint8_t*)di - td->data);
                                    dioff = newdi + ((uint8_t*)di - td->data);
                                } else {
                                    dioff = newdi;
                                }

                                if ((uint8_t*)&di[1] + di->name_len + di->data_len < td->data + td->size)
                                    RtlCopyMemory(dioff, (uint8_t*)&di[1] + di->name_len + di->data_len, td->size - (((uint8_t*)&di[1] + di->name_len + di->data_len) - td->data));

                                td2 = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
                                if (!td2) {
                                    ERR("out of memory\n");
                                    ExFreePool(newdi);
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                td2->key = bi->key;
                                td2->size = newlen;
                                td2->data = newdi;
                                td2->ignore = false;
                                td2->inserted = true;

                                InsertHeadList(td->list_entry.Blink, &td2->list_entry);

                                t->header.nritems++;
                                t->size += newlen + sizeof(struct btrfs_item);
                                t->write = true;
                            }

                            break;
                        }

                        len -= sizeof(struct btrfs_dir_item) + di->name_len + di->data_len;
                        di = (struct btrfs_dir_item*)((uint8_t*)&di[1] + di->data_len + di->name_len);

                        if (len == 0) {
                            TRACE("could not find DIR_ITEM to delete\n");
                            *ignore = true;
                            return STATUS_SUCCESS;
                        }
                    } while (len > 0);
                }
                break;
            }

            case Batch_DeleteInodeRef: {
                if (td->size < sizeof(struct btrfs_inode_ref)) {
                    ERR("INODE_REF was %u bytes, expected at least %Iu\n", td->size, sizeof(struct btrfs_inode_ref));
                    return STATUS_INTERNAL_ERROR;
                } else {
                    struct btrfs_inode_ref *ir, *delir;
                    ULONG len;
                    bool changed = false;

                    delir = (struct btrfs_inode_ref*)bi->data;
                    ir = (struct btrfs_inode_ref*)td->data;
                    len = td->size;

                    do {
                        uint16_t itemlen;

                        if (len < sizeof(struct btrfs_inode_ref) || len < sizeof(struct btrfs_inode_ref) + ir->name_len) {
                            ERR("INODE_REF was truncated\n");
                            break;
                        }

                        itemlen = (uint16_t)sizeof(struct btrfs_inode_ref) + ir->name_len;

                        if (ir->name_len == delir->name_len && RtlCompareMemory(&ir[1], &delir[1], ir->name_len) == ir->name_len) {
                            uint16_t newlen = td->size - itemlen;

                            changed = true;

                            if (newlen == 0)
                                TRACE("deleting INODE_REF\n");
                            else {
                                uint8_t *newir = ExAllocatePoolWithTag(PagedPool, newlen, ALLOC_TAG), *iroff;
                                tree_data* td2;

                                if (!newir) {
                                    ERR("out of memory\n");
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                TRACE("modifying INODE_REF\n");

                                if ((uint8_t*)ir > td->data) {
                                    RtlCopyMemory(newir, td->data, (uint8_t*)ir - td->data);
                                    iroff = newir + ((uint8_t*)ir - td->data);
                                } else {
                                    iroff = newir;
                                }

                                if ((uint8_t*)&ir[1] + ir->name_len < td->data + td->size)
                                    RtlCopyMemory(iroff, (uint8_t*)&ir[1] + ir->name_len, td->size - (((uint8_t*)&ir[1] + ir->name_len) - td->data));

                                td2 = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
                                if (!td2) {
                                    ERR("out of memory\n");
                                    ExFreePool(newir);
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                td2->key = bi->key;
                                td2->size = newlen;
                                td2->data = newir;
                                td2->ignore = false;
                                td2->inserted = true;

                                InsertHeadList(td->list_entry.Blink, &td2->list_entry);

                                t->header.nritems++;
                                t->size += newlen + sizeof(struct btrfs_item);
                                t->write = true;
                            }

                            break;
                        }

                        if (len > itemlen) {
                            len -= itemlen;
                            ir = (struct btrfs_inode_ref*)((uint8_t*)&ir[1] + ir->name_len);
                        } else
                            break;
                    } while (len > 0);

                    if (!changed) {
                        if (Vcb->superblock.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF) {
                            TRACE("entry in INODE_REF not found, adding Batch_DeleteInodeExtRef entry\n");

                            Status = add_delete_inode_extref(Vcb, bi, listhead);
                            if (!NT_SUCCESS(Status)) {
                                ERR("add_delete_inode_extref returned %08lx\n", Status);
                                return Status;
                            }

                            *ignore = true;
                            return STATUS_SUCCESS;
                        } else
                            WARN("entry not found in INODE_REF\n");
                    }
                }

                break;
            }

            case Batch_DeleteInodeExtRef: {
                if (td->size < offsetof(struct btrfs_inode_extref, name)) {
                    ERR("INODE_EXTREF was %u bytes, expected at least %Iu\n", td->size, offsetof(struct btrfs_inode_extref, name));
                    return STATUS_INTERNAL_ERROR;
                } else {
                    struct btrfs_inode_extref *ier, *delier;
                    ULONG len;

                    delier = (struct btrfs_inode_extref*)bi->data;
                    ier = (struct btrfs_inode_extref*)td->data;
                    len = td->size;

                    do {
                        uint16_t itemlen;

                        if (len < offsetof(struct btrfs_inode_extref, name) || len < offsetof(struct btrfs_inode_extref, name) + ier->name_len) {
                            ERR("INODE_REF was truncated\n");
                            break;
                        }

                        itemlen = (uint16_t)offsetof(struct btrfs_inode_extref, name) + ier->name_len;

                        if (ier->parent_objectid == delier->parent_objectid && ier->name_len == delier->name_len && RtlCompareMemory(ier->name, delier->name, ier->name_len) == ier->name_len) {
                            uint16_t newlen = td->size - itemlen;

                            if (newlen == 0)
                                TRACE("deleting INODE_EXTREF\n");
                            else {
                                uint8_t *newier = ExAllocatePoolWithTag(PagedPool, newlen, ALLOC_TAG), *ieroff;
                                tree_data* td2;

                                if (!newier) {
                                    ERR("out of memory\n");
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                TRACE("modifying INODE_EXTREF\n");

                                if ((uint8_t*)ier > td->data) {
                                    RtlCopyMemory(newier, td->data, (uint8_t*)ier - td->data);
                                    ieroff = newier + ((uint8_t*)ier - td->data);
                                } else {
                                    ieroff = newier;
                                }

                                if ((uint8_t*)&ier->name[ier->name_len] < td->data + td->size)
                                    RtlCopyMemory(ieroff, &ier->name[ier->name_len], td->size - ((uint8_t*)&ier->name[ier->name_len] - td->data));

                                td2 = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
                                if (!td2) {
                                    ERR("out of memory\n");
                                    ExFreePool(newier);
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                td2->key = bi->key;
                                td2->size = newlen;
                                td2->data = newier;
                                td2->ignore = false;
                                td2->inserted = true;

                                InsertHeadList(td->list_entry.Blink, &td2->list_entry);

                                t->header.nritems++;
                                t->size += newlen + sizeof(struct btrfs_item);
                                t->write = true;
                            }

                            break;
                        }

                        if (len > itemlen) {
                            len -= itemlen;
                            ier = (struct btrfs_inode_extref*)&ier->name[ier->name_len];
                        } else
                            break;
                    } while (len > 0);
                }
                break;
            }

            case Batch_DeleteXattr: {
                if (td->size < sizeof(struct btrfs_dir_item)) {
                    ERR("XATTR_ITEM was %u bytes, expected at least %Iu\n", td->size, sizeof(struct btrfs_dir_item));
                    return STATUS_INTERNAL_ERROR;
                } else {
                    struct btrfs_dir_item *di, *deldi;
                    LONG len;

                    deldi = (struct btrfs_dir_item*)bi->data;
                    di = (struct btrfs_dir_item*)td->data;
                    len = td->size;

                    do {
                        if (di->name_len == deldi->name_len && RtlCompareMemory(&di[1], &deldi[1], di->name_len) == di->name_len) {
                            uint16_t newlen = td->size - ((uint16_t)sizeof(struct btrfs_dir_item) + di->name_len + di->data_len);

                            if (newlen == 0)
                                TRACE("deleting XATTR_ITEM\n");
                            else {
                                uint8_t *newdi = ExAllocatePoolWithTag(PagedPool, newlen, ALLOC_TAG), *dioff;
                                tree_data* td2;

                                if (!newdi) {
                                    ERR("out of memory\n");
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                TRACE("modifying XATTR_ITEM\n");

                                if ((uint8_t*)di > td->data) {
                                    RtlCopyMemory(newdi, td->data, (uint8_t*)di - td->data);
                                    dioff = newdi + ((uint8_t*)di - td->data);
                                } else
                                    dioff = newdi;

                                if ((uint8_t*)&di[1] + di->name_len + di->data_len < td->data + td->size)
                                    RtlCopyMemory(dioff, (uint8_t*)&di[1] + di->name_len + di->data_len, td->size - (((uint8_t*)&di[1] + di->name_len + di->data_len) - td->data));

                                td2 = ExAllocateFromPagedLookasideList(&Vcb->tree_data_lookaside);
                                if (!td2) {
                                    ERR("out of memory\n");
                                    ExFreePool(newdi);
                                    return STATUS_INSUFFICIENT_RESOURCES;
                                }

                                td2->key = bi->key;
                                td2->size = newlen;
                                td2->data = newdi;
                                td2->ignore = false;
                                td2->inserted = true;

                                InsertHeadList(td->list_entry.Blink, &td2->list_entry);

                                t->header.nritems++;
                                t->size += newlen + sizeof(struct btrfs_item);
                                t->write = true;
                            }

                            break;
                        }

                        len -= sizeof(struct btrfs_dir_item) + di->name_len + di->data_len;
                        di = (struct btrfs_dir_item*)((uint8_t*)&di[1] + di->data_len + di->name_len);

                        if (len == 0) {
                            TRACE("could not find DIR_ITEM to delete\n");
                            *ignore = true;
                            return STATUS_SUCCESS;
                        }
                    } while (len > 0);
                }
                break;
            }

            case Batch_Delete:
                break;

            default:
                ERR("unexpected batch operation type\n");
                return STATUS_INTERNAL_ERROR;
        }

        // delete old item
        if (!td->ignore) {
            td->ignore = true;

            t->header.nritems--;
            t->size -= sizeof(struct btrfs_item) + td->size;
            t->write = true;
        }

        if (newtd) {
            newtd->data = bi->data;
            newtd->size = bi->datalen;
            InsertHeadList(td->list_entry.Blink, &newtd->list_entry);
        }
    } else {
        ERR("(%I64x,%x,%I64x) already exists\n", bi->key.objectid, bi->key.type, bi->key.offset);
        return STATUS_INTERNAL_ERROR;
    }

    *ignore = false;
    return STATUS_SUCCESS;
}
