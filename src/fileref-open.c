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

NTSTATUS find_file_in_dir(PUNICODE_STRING filename, fcb* fcb, root** subvol, uint64_t* inode, dir_child** pdc, bool case_sensitive) {
    NTSTATUS Status;
    UNICODE_STRING fnus;
    uint32_t hash;
    LIST_ENTRY* le;
    uint8_t c;
    bool locked = false;

    if (!case_sensitive) {
        Status = RtlUpcaseUnicodeString(&fnus, filename, true);

        if (!NT_SUCCESS(Status)) {
            ERR("RtlUpcaseUnicodeString returned %08lx\n", Status);
            return Status;
        }
    } else
        fnus = *filename;

    Status = check_file_name_valid(filename, false, false);
    if (!NT_SUCCESS(Status))
        goto end;

    hash = calc_crc32c(0xffffffff, (uint8_t*)fnus.Buffer, fnus.Length);

    c = hash >> 24;

    if (!ExIsResourceAcquiredSharedLite(&fcb->nonpaged->dir_children_lock)) {
        ExAcquireResourceSharedLite(&fcb->nonpaged->dir_children_lock, true);
        locked = true;
    }

    if (case_sensitive) {
        if (!fcb->hash_ptrs[c]) {
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
            goto end;
        }

        le = fcb->hash_ptrs[c];
        while (le != &fcb->dir_children_hash) {
            dir_child* dc = CONTAINING_RECORD(le, dir_child, list_entry_hash);

            if (dc->hash == hash) {
                if (dc->name.Length == fnus.Length && RtlCompareMemory(dc->name.Buffer, fnus.Buffer, fnus.Length) == fnus.Length) {
                    if (dc->key.type == BTRFS_ROOT_ITEM_KEY) {
                        LIST_ENTRY* le2;

                        *subvol = NULL;

                        le2 = fcb->Vcb->roots.Flink;
                        while (le2 != &fcb->Vcb->roots) {
                            root* r2 = CONTAINING_RECORD(le2, root, list_entry);

                            if (r2->id == dc->key.objectid) {
                                *subvol = r2;
                                break;
                            }

                            le2 = le2->Flink;
                        }

                        *inode = BTRFS_FIRST_FREE_OBJECTID;
                    } else {
                        *subvol = fcb->subvol;
                        *inode = dc->key.objectid;
                    }

                    *pdc = dc;

                    Status = STATUS_SUCCESS;
                    goto end;
                }
            } else if (dc->hash > hash) {
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                goto end;
            }

            le = le->Flink;
        }
    } else {
        if (!fcb->hash_ptrs_uc[c]) {
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
            goto end;
        }

        le = fcb->hash_ptrs_uc[c];
        while (le != &fcb->dir_children_hash_uc) {
            dir_child* dc = CONTAINING_RECORD(le, dir_child, list_entry_hash_uc);

            if (dc->hash_uc == hash) {
                if (dc->name_uc.Length == fnus.Length && RtlCompareMemory(dc->name_uc.Buffer, fnus.Buffer, fnus.Length) == fnus.Length) {
                    if (dc->key.type == BTRFS_ROOT_ITEM_KEY) {
                        LIST_ENTRY* le2;

                        *subvol = NULL;

                        le2 = fcb->Vcb->roots.Flink;
                        while (le2 != &fcb->Vcb->roots) {
                            root* r2 = CONTAINING_RECORD(le2, root, list_entry);

                            if (r2->id == dc->key.objectid) {
                                *subvol = r2;
                                break;
                            }

                            le2 = le2->Flink;
                        }

                        *inode = BTRFS_FIRST_FREE_OBJECTID;
                    } else {
                        *subvol = fcb->subvol;
                        *inode = dc->key.objectid;
                    }

                    *pdc = dc;

                    Status = STATUS_SUCCESS;
                    goto end;
                }
            } else if (dc->hash_uc > hash) {
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                goto end;
            }

            le = le->Flink;
        }
    }

    Status = STATUS_OBJECT_NAME_NOT_FOUND;

end:
    if (locked)
        ExReleaseResourceLite(&fcb->nonpaged->dir_children_lock);

    if (!case_sensitive)
        ExFreePool(fnus.Buffer);

    return Status;
}

static NTSTATUS open_fcb_stream(_Requires_lock_held_(_Curr_->tree_lock) _Requires_exclusive_lock_held_(_Curr_->fcb_lock) device_extension* Vcb,
                                dir_child* dc, fcb* parent, fcb** pfcb, PIRP Irp) {
    fcb* fcb;
    uint8_t* xattrdata;
    uint16_t xattrlen, overhead;
    NTSTATUS Status;
    struct btrfs_key searchkey;
    traverse_ptr tp;
    static const char xapref[] = "user.";
    ANSI_STRING xattr;
    uint32_t crc32;

    xattr.Length = sizeof(xapref) - 1 + dc->utf8.Length;
    xattr.MaximumLength = xattr.Length + 1;
    xattr.Buffer = ExAllocatePoolWithTag(PagedPool, xattr.MaximumLength, ALLOC_TAG);
    if (!xattr.Buffer) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(xattr.Buffer, xapref, sizeof(xapref) - 1);
    RtlCopyMemory(&xattr.Buffer[sizeof(xapref) - 1], dc->utf8.Buffer, dc->utf8.Length);
    xattr.Buffer[xattr.Length] = 0;

    fcb = create_fcb(Vcb, PagedPool);
    if (!fcb) {
        ERR("out of memory\n");
        ExFreePool(xattr.Buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fcb->Vcb = Vcb;

    crc32 = calc_crc32c(0xfffffffe, (uint8_t*)xattr.Buffer, xattr.Length);

    if (!get_xattr(Vcb, parent->subvol, parent->inode, xattr.Buffer, crc32, &xattrdata, &xattrlen, Irp)) {
        ERR("get_xattr failed\n");
        reap_fcb(fcb);
        ExFreePool(xattr.Buffer);
        return STATUS_INTERNAL_ERROR;
    }

    fcb->subvol = parent->subvol;
    fcb->inode = parent->inode;
    fcb->type = parent->type;
    fcb->ads = true;
    fcb->adshash = crc32;
    fcb->adsxattr = xattr;

    fcb->adsdata.Buffer = (char*)xattrdata;
    fcb->adsdata.Length = fcb->adsdata.MaximumLength = xattrlen;

    // find XATTR_ITEM overhead and hence calculate maximum length

    searchkey.objectid = parent->inode;
    searchkey.type = BTRFS_XATTR_ITEM_KEY;
    searchkey.offset = crc32;

    Status = find_item(Vcb, parent->subvol, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("find_item returned %08lx\n", Status);
        reap_fcb(fcb);
        return Status;
    }

    if (keycmp(tp.item->key, searchkey)) {
        ERR("error - could not find key for xattr\n");
        reap_fcb(fcb);
        return STATUS_INTERNAL_ERROR;
    }

    if (tp.item->size < xattrlen) {
        ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %u\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, xattrlen);
        reap_fcb(fcb);
        return STATUS_INTERNAL_ERROR;
    }

    overhead = tp.item->size - xattrlen;

    fcb->adsmaxlen = Vcb->superblock.nodesize - sizeof(struct btrfs_header) - sizeof(struct btrfs_item) - overhead;

    fcb->Header.IsFastIoPossible = fast_io_possible(fcb);
    fcb->Header.AllocationSize.QuadPart = xattrlen;
    fcb->Header.FileSize.QuadPart = xattrlen;
    fcb->Header.ValidDataLength.QuadPart = xattrlen;

    TRACE("stream found: size = %x, hash = %08x\n", xattrlen, fcb->adshash);

    *pfcb = fcb;

    return STATUS_SUCCESS;
}

NTSTATUS open_fileref_child(_Requires_lock_held_(_Curr_->tree_lock) _Requires_exclusive_lock_held_(_Curr_->fcb_lock) _In_ device_extension* Vcb,
                            _In_ file_ref* sf, _In_ PUNICODE_STRING name, _In_ bool case_sensitive, _In_ bool lastpart, _In_ bool streampart,
                            _In_ POOL_TYPE pooltype, _Out_ file_ref** psf2, _In_opt_ PIRP Irp) {
    NTSTATUS Status;
    file_ref* sf2;

    if (sf->fcb == Vcb->dummy_fcb)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (streampart) {
        bool locked = false;
        LIST_ENTRY* le;
        UNICODE_STRING name_uc;
        dir_child* dc = NULL;
        fcb* fcb;
        struct _fcb* duff_fcb = NULL;
        file_ref* duff_fr = NULL;

        if (!case_sensitive) {
            Status = RtlUpcaseUnicodeString(&name_uc, name, true);
            if (!NT_SUCCESS(Status)) {
                ERR("RtlUpcaseUnicodeString returned %08lx\n", Status);
                return Status;
            }
        }

        if (!ExIsResourceAcquiredSharedLite(&sf->fcb->nonpaged->dir_children_lock)) {
            ExAcquireResourceSharedLite(&sf->fcb->nonpaged->dir_children_lock, true);
            locked = true;
        }

        le = sf->fcb->dir_children_index.Flink;
        while (le != &sf->fcb->dir_children_index) {
            dir_child* dc2 = CONTAINING_RECORD(le, dir_child, list_entry_index);

            if (dc2->index == 0) {
                if ((case_sensitive && dc2->name.Length == name->Length && RtlCompareMemory(dc2->name.Buffer, name->Buffer, dc2->name.Length) == dc2->name.Length) ||
                    (!case_sensitive && dc2->name_uc.Length == name_uc.Length && RtlCompareMemory(dc2->name_uc.Buffer, name_uc.Buffer, dc2->name_uc.Length) == dc2->name_uc.Length)
                ) {
                    dc = dc2;
                    break;
                }
            } else
                break;

            le = le->Flink;
        }

        if (!dc) {
            if (locked)
                ExReleaseResourceLite(&sf->fcb->nonpaged->dir_children_lock);

            if (!case_sensitive)
                ExFreePool(name_uc.Buffer);

            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        if (dc->fileref) {
            if (locked)
                ExReleaseResourceLite(&sf->fcb->nonpaged->dir_children_lock);

            if (!case_sensitive)
                ExFreePool(name_uc.Buffer);

            increase_fileref_refcount(dc->fileref);
            *psf2 = dc->fileref;
            return STATUS_SUCCESS;
        }

        if (locked)
            ExReleaseResourceLite(&sf->fcb->nonpaged->dir_children_lock);

        if (!case_sensitive)
            ExFreePool(name_uc.Buffer);

        Status = open_fcb_stream(Vcb, dc, sf->fcb, &fcb, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("open_fcb_stream returned %08lx\n", Status);
            return Status;
        }

        fcb->hash = sf->fcb->hash;

        acquire_fcb_lock_exclusive(Vcb);

        if (sf->fcb->subvol->fcbs_ptrs[fcb->hash >> 24]) {
            le = sf->fcb->subvol->fcbs_ptrs[fcb->hash >> 24];

            while (le != &sf->fcb->subvol->fcbs) {
                struct _fcb* fcb2 = CONTAINING_RECORD(le, struct _fcb, list_entry);

                if (fcb2->inode == fcb->inode) {
                    if (fcb2->ads && fcb2->adshash == fcb->adshash &&
                        fcb2->adsxattr.Length == fcb->adsxattr.Length &&
                        RtlCompareMemory(fcb2->adsxattr.Buffer, fcb->adsxattr.Buffer, fcb->adsxattr.Length) == fcb->adsxattr.Length) {
                        duff_fcb = fcb;
                        fcb = fcb2;
                        break;
                    }
                } else if (fcb2->hash > fcb->hash)
                    break;

                le = le->Flink;
            }
        }

        if (!duff_fcb) {
            InsertHeadList(&sf->fcb->list_entry, &fcb->list_entry);
            InsertTailList(&Vcb->all_fcbs, &fcb->list_entry_all);
            fcb->subvol->fcbs_version++;
        }

        release_fcb_lock(Vcb);

        if (duff_fcb) {
            reap_fcb(duff_fcb);
            InterlockedIncrement(&fcb->refcount);
        }

        sf2 = create_fileref(Vcb);
        if (!sf2) {
            ERR("out of memory\n");
            free_fcb(fcb);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        sf2->fcb = fcb;

        ExAcquireResourceExclusiveLite(&sf->fcb->nonpaged->dir_children_lock, true);

        if (dc->fileref) {
            duff_fr = sf2;
            sf2 = dc->fileref;
            increase_fileref_refcount(sf2);
        } else {
            sf2->parent = (struct _file_ref*)sf;
            sf2->dc = dc;
            dc->fileref = sf2;
            increase_fileref_refcount(sf);
            InsertTailList(&sf->children, &sf2->list_entry);
        }

        ExReleaseResourceLite(&sf->fcb->nonpaged->dir_children_lock);

        if (duff_fr)
            reap_fileref(Vcb, duff_fr);
    } else {
        root* subvol;
        uint64_t inode;
        dir_child* dc;

        Status = find_file_in_dir(name, sf->fcb, &subvol, &inode, &dc, case_sensitive);
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
            TRACE("could not find %.*S\n", (int)(name->Length / sizeof(WCHAR)), name->Buffer);

            return lastpart ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
        } else if (Status == STATUS_OBJECT_NAME_INVALID) {
            TRACE("invalid filename: %.*S\n", (int)(name->Length / sizeof(WCHAR)), name->Buffer);
            return Status;
        } else if (!NT_SUCCESS(Status)) {
            ERR("find_file_in_dir returned %08lx\n", Status);
            return Status;
        } else {
            fcb* fcb;
            file_ref* duff_fr = NULL;

            if (dc->fileref) {
                if (!lastpart && dc->type != BTRFS_FT_DIR) {
                    TRACE("passed path including file as subdirectory\n");
                    return STATUS_OBJECT_PATH_NOT_FOUND;
                }

                InterlockedIncrement(&dc->fileref->refcount);
                *psf2 = dc->fileref;
                return STATUS_SUCCESS;
            }

            if (!subvol || (subvol != Vcb->root_fileref->fcb->subvol && inode == BTRFS_FIRST_FREE_OBJECTID && subvol->parent != sf->fcb->subvol->id && !dc->root_dir)) {
                fcb = Vcb->dummy_fcb;
                InterlockedIncrement(&fcb->refcount);
            } else {
                Status = open_fcb(Vcb, subvol, inode, dc->type, &dc->utf8, false, sf->fcb, &fcb, pooltype, Irp);

                if (!NT_SUCCESS(Status)) {
                    ERR("open_fcb returned %08lx\n", Status);
                    return Status;
                }
            }

            if (dc->type != BTRFS_FT_DIR && !lastpart && !(fcb->atts & FILE_ATTRIBUTE_REPARSE_POINT)) {
                TRACE("passed path including file as subdirectory\n");
                free_fcb(fcb);
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }

            sf2 = create_fileref(Vcb);
            if (!sf2) {
                ERR("out of memory\n");
                free_fcb(fcb);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            sf2->fcb = fcb;

            ExAcquireResourceExclusiveLite(&sf->fcb->nonpaged->dir_children_lock, true);

            if (!dc->fileref) {
                sf2->parent = (struct _file_ref*)sf;
                sf2->dc = dc;
                dc->fileref = sf2;
                InsertTailList(&sf->children, &sf2->list_entry);
                increase_fileref_refcount(sf);

                if (dc->type == BTRFS_FT_DIR)
                    fcb->fileref = sf2;
            } else {
                duff_fr = sf2;
                sf2 = dc->fileref;
                increase_fileref_refcount(sf2);
            }

            ExReleaseResourceLite(&sf->fcb->nonpaged->dir_children_lock);

            if (duff_fr)
                reap_fileref(Vcb, duff_fr);
        }
    }

    *psf2 = sf2;

    return STATUS_SUCCESS;
}
