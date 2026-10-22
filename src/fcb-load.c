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

/* Load an inode's file control block and its on-disk metadata. */
#include <sys/stat.h>
#include "btrfs_drv.h"
#include "crc32c.h"

NTSTATUS open_fcb(_Requires_lock_held_(_Curr_->tree_lock) _Requires_exclusive_lock_held_(_Curr_->fcb_lock) device_extension* Vcb,
                  root* subvol, uint64_t inode, uint8_t type, PANSI_STRING utf8, bool always_add_hl, fcb* parent, fcb** pfcb, POOL_TYPE pooltype, PIRP Irp) {
    struct btrfs_key searchkey;
    traverse_ptr tp, next_tp;
    NTSTATUS Status;
    fcb *fcb, *deleted_fcb = NULL;
    bool atts_set = false, sd_set = false, no_data;
    LIST_ENTRY* lastle = NULL;
    struct btrfs_file_extent_item* ed = NULL;
    uint64_t fcbs_version = 0;
    uint32_t hash;

    hash = calc_crc32c(0xffffffff, (uint8_t*)&inode, sizeof(uint64_t));

    acquire_fcb_lock_shared(Vcb);

    if (subvol->fcbs_ptrs[hash >> 24]) {
        LIST_ENTRY* le = subvol->fcbs_ptrs[hash >> 24];

        while (le != &subvol->fcbs) {
            fcb = CONTAINING_RECORD(le, struct _fcb, list_entry);

            if (fcb->inode == inode) {
                if (!fcb->ads) {
                    if (fcb->deleted)
                        deleted_fcb = fcb;
                    else {
#ifdef DEBUG_FCB_REFCOUNTS
                        LONG rc = InterlockedIncrement(&fcb->refcount);

                        WARN("fcb %p: refcount now %i (subvol %I64x, inode %I64x)\n", fcb, rc, fcb->subvol->id, fcb->inode);
#else
                        InterlockedIncrement(&fcb->refcount);
#endif

                        *pfcb = fcb;
                        release_fcb_lock(Vcb);
                        return STATUS_SUCCESS;
                    }
                }
            } else if (fcb->hash > hash) {
                if (deleted_fcb) {
                    InterlockedIncrement(&deleted_fcb->refcount);
                    *pfcb = deleted_fcb;
                    release_fcb_lock(Vcb);
                    return STATUS_SUCCESS;
                }

                lastle = le->Blink;
                fcbs_version = subvol->fcbs_version;

                break;
            }

            le = le->Flink;
        }
    }

    release_fcb_lock(Vcb);

    if (deleted_fcb) {
        InterlockedIncrement(&deleted_fcb->refcount);
        *pfcb = deleted_fcb;
        return STATUS_SUCCESS;
    }

    fcb = create_fcb(Vcb, pooltype);
    if (!fcb) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fcb->Vcb = Vcb;

    fcb->subvol = subvol;
    fcb->inode = inode;
    fcb->hash = hash;
    fcb->type = type;

    searchkey.objectid = inode;
    searchkey.type = BTRFS_INODE_ITEM_KEY;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(Vcb, subvol, &tp, &searchkey, false, Irp);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08lx\n", Status);
        reap_fcb(fcb);
        return Status;
    }

    if (tp.item->key.objectid != searchkey.objectid || tp.item->key.type != searchkey.type) {
        WARN("couldn't find INODE_ITEM for inode %I64x in subvol %I64x\n", inode, subvol->id);
        reap_fcb(fcb);
        return STATUS_INVALID_PARAMETER;
    }

    if (tp.item->size > 0)
        RtlCopyMemory(&fcb->inode_item, tp.item->data, min(sizeof(struct btrfs_inode_item), tp.item->size));

    if (fcb->type == 0) { // guess the type from the inode mode, if the caller doesn't know already
        if ((fcb->inode_item.mode & __S_IFDIR) == __S_IFDIR)
            fcb->type = BTRFS_FT_DIR;
        else if ((fcb->inode_item.mode & __S_IFCHR) == __S_IFCHR)
            fcb->type = BTRFS_FT_CHRDEV;
        else if ((fcb->inode_item.mode & __S_IFBLK) == __S_IFBLK)
            fcb->type = BTRFS_FT_BLKDEV;
        else if ((fcb->inode_item.mode & __S_IFIFO) == __S_IFIFO)
            fcb->type = BTRFS_FT_FIFO;
        else if ((fcb->inode_item.mode & __S_IFLNK) == __S_IFLNK)
            fcb->type = BTRFS_FT_SYMLINK;
        else if ((fcb->inode_item.mode & __S_IFSOCK) == __S_IFSOCK)
            fcb->type = BTRFS_FT_SOCK;
        else
            fcb->type = BTRFS_FT_REG_FILE;
    }

    no_data = fcb->inode_item.size == 0 || (fcb->type != BTRFS_FT_REG_FILE && fcb->type != BTRFS_FT_SYMLINK);

    while (find_next_item(Vcb, &tp, &next_tp, false, Irp)) {
        tp = next_tp;

        if (tp.item->key.objectid > inode)
            break;

        if ((no_data && tp.item->key.type > BTRFS_XATTR_ITEM_KEY) || tp.item->key.type > BTRFS_EXTENT_DATA_KEY)
            break;

        if ((always_add_hl || fcb->inode_item.nlink > 1) && tp.item->key.type == BTRFS_INODE_REF_KEY) {
            ULONG len;
            struct btrfs_inode_ref* ir;

            len = tp.item->size;
            ir = (struct btrfs_inode_ref*)tp.item->data;

            while (len >= sizeof(struct btrfs_inode_ref)) {
                hardlink* hl;
                ULONG stringlen;

                hl = ExAllocatePoolWithTag(pooltype, sizeof(hardlink), ALLOC_TAG);
                if (!hl) {
                    ERR("out of memory\n");
                    reap_fcb(fcb);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                hl->parent = tp.item->key.offset;
                hl->index = ir->index;

                hl->utf8.Length = hl->utf8.MaximumLength = ir->name_len;

                if (hl->utf8.Length > 0) {
                    hl->utf8.Buffer = ExAllocatePoolWithTag(pooltype, hl->utf8.MaximumLength, ALLOC_TAG);
                    RtlCopyMemory(hl->utf8.Buffer, &ir[1], ir->name_len);
                }

                Status = utf8_to_utf16(NULL, 0, &stringlen, (char*)&ir[1], ir->name_len);
                if (!NT_SUCCESS(Status)) {
                    ERR("utf8_to_utf16 1 returned %08lx\n", Status);
                    ExFreePool(hl);
                    reap_fcb(fcb);
                    return Status;
                }

                hl->name.Length = hl->name.MaximumLength = (uint16_t)stringlen;

                if (stringlen == 0)
                    hl->name.Buffer = NULL;
                else {
                    hl->name.Buffer = ExAllocatePoolWithTag(pooltype, hl->name.MaximumLength, ALLOC_TAG);

                    if (!hl->name.Buffer) {
                        ERR("out of memory\n");
                        ExFreePool(hl);
                        reap_fcb(fcb);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    Status = utf8_to_utf16(hl->name.Buffer, stringlen, &stringlen, (char*)&ir[1], ir->name_len);
                    if (!NT_SUCCESS(Status)) {
                        ERR("utf8_to_utf16 2 returned %08lx\n", Status);
                        ExFreePool(hl->name.Buffer);
                        ExFreePool(hl);
                        reap_fcb(fcb);
                        return Status;
                    }
                }

                InsertTailList(&fcb->hardlinks, &hl->list_entry);

                len -= sizeof(struct btrfs_inode_ref) + ir->name_len;
                ir = (struct btrfs_inode_ref*)((uint8_t*)&ir[1] + ir->name_len);
            }
        } else if ((always_add_hl || fcb->inode_item.nlink > 1) && tp.item->key.type == BTRFS_INODE_EXTREF_KEY) {
            ULONG len;
            struct btrfs_inode_extref* ier;

            len = tp.item->size;
            ier = (struct btrfs_inode_extref*)tp.item->data;

            while (len >= offsetof(struct btrfs_inode_extref, name)) {
                hardlink* hl;
                ULONG stringlen;

                hl = ExAllocatePoolWithTag(pooltype, sizeof(hardlink), ALLOC_TAG);
                if (!hl) {
                    ERR("out of memory\n");
                    reap_fcb(fcb);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                hl->parent = ier->parent_objectid;
                hl->index = ier->index;

                hl->utf8.Length = hl->utf8.MaximumLength = ier->name_len;

                if (hl->utf8.Length > 0) {
                    hl->utf8.Buffer = ExAllocatePoolWithTag(pooltype, hl->utf8.MaximumLength, ALLOC_TAG);
                    RtlCopyMemory(hl->utf8.Buffer, ier->name, ier->name_len);
                }

                Status = utf8_to_utf16(NULL, 0, &stringlen, (char*)ier->name, ier->name_len);
                if (!NT_SUCCESS(Status)) {
                    ERR("utf8_to_utf16 1 returned %08lx\n", Status);
                    ExFreePool(hl);
                    reap_fcb(fcb);
                    return Status;
                }

                hl->name.Length = hl->name.MaximumLength = (uint16_t)stringlen;

                if (stringlen == 0)
                    hl->name.Buffer = NULL;
                else {
                    hl->name.Buffer = ExAllocatePoolWithTag(pooltype, hl->name.MaximumLength, ALLOC_TAG);

                    if (!hl->name.Buffer) {
                        ERR("out of memory\n");
                        ExFreePool(hl);
                        reap_fcb(fcb);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    Status = utf8_to_utf16(hl->name.Buffer, stringlen, &stringlen, (char*)ier->name, ier->name_len);
                    if (!NT_SUCCESS(Status)) {
                        ERR("utf8_to_utf16 2 returned %08lx\n", Status);
                        ExFreePool(hl->name.Buffer);
                        ExFreePool(hl);
                        reap_fcb(fcb);
                        return Status;
                    }
                }

                InsertTailList(&fcb->hardlinks, &hl->list_entry);

                len -= offsetof(struct btrfs_inode_extref, name) + ier->name_len;
                ier = (struct btrfs_inode_extref*)&ier->name[ier->name_len];
            }
        } else if (tp.item->key.type == BTRFS_XATTR_ITEM_KEY) {
            ULONG len;
            struct btrfs_dir_item* di;

            static const char xapref[] = "user.";

            if (tp.item->size < sizeof(struct btrfs_dir_item)) {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset, tp.item->size, sizeof(struct btrfs_dir_item));
                continue;
            }

            len = tp.item->size;
            di = (struct btrfs_dir_item*)tp.item->data;

            do {
                if (len < sizeof(struct btrfs_dir_item) + di->data_len + di->name_len)
                    break;

                if (tp.item->key.offset == EA_REPARSE_HASH && di->name_len == sizeof(EA_REPARSE) - 1 && RtlCompareMemory(EA_REPARSE, &di[1], di->name_len) == di->name_len) {
                    if (di->data_len > 0) {
                        fcb->reparse_xattr.Buffer = ExAllocatePoolWithTag(PagedPool, di->data_len, ALLOC_TAG);
                        if (!fcb->reparse_xattr.Buffer) {
                            ERR("out of memory\n");
                            reap_fcb(fcb);
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }

                        RtlCopyMemory(fcb->reparse_xattr.Buffer, (uint8_t*)&di[1] + di->name_len, di->data_len);
                    } else
                        fcb->reparse_xattr.Buffer = NULL;

                    fcb->reparse_xattr.Length = fcb->reparse_xattr.MaximumLength = di->data_len;
                } else if (tp.item->key.offset == EA_EA_HASH && di->name_len == sizeof(EA_EA) - 1 && RtlCompareMemory(EA_EA, &di[1], di->name_len) == di->name_len) {
                    if (di->data_len > 0) {
                        ULONG offset;

                        Status = IoCheckEaBufferValidity((FILE_FULL_EA_INFORMATION*)((uint8_t*)&di[1] + di->name_len), di->data_len, &offset);

                        if (!NT_SUCCESS(Status))
                            WARN("IoCheckEaBufferValidity returned %08lx (error at offset %lu)\n", Status, offset);
                        else {
                            FILE_FULL_EA_INFORMATION* eainfo;

                            fcb->ea_xattr.Buffer = ExAllocatePoolWithTag(PagedPool, di->data_len, ALLOC_TAG);
                            if (!fcb->ea_xattr.Buffer) {
                                ERR("out of memory\n");
                                reap_fcb(fcb);
                                return STATUS_INSUFFICIENT_RESOURCES;
                            }

                            RtlCopyMemory(fcb->ea_xattr.Buffer, (uint8_t*)&di[1] + di->name_len, di->data_len);

                            fcb->ea_xattr.Length = fcb->ea_xattr.MaximumLength = di->data_len;

                            fcb->ealen = 4;

                            // calculate ealen
                            eainfo = (FILE_FULL_EA_INFORMATION*)((uint8_t*)&di[1] + di->name_len);
                            do {
                                fcb->ealen += 5 + eainfo->EaNameLength + eainfo->EaValueLength;

                                if (eainfo->NextEntryOffset == 0)
                                    break;

                                eainfo = (FILE_FULL_EA_INFORMATION*)(((uint8_t*)eainfo) + eainfo->NextEntryOffset);
                            } while (true);
                        }
                    }
                } else if (tp.item->key.offset == EA_DOSATTRIB_HASH && di->name_len == sizeof(EA_DOSATTRIB) - 1 && RtlCompareMemory(EA_DOSATTRIB, &di[1], di->name_len) == di->name_len) {
                    if (di->data_len > 0) {
                        if (get_file_attributes_from_xattr((char*)&di[1] + di->name_len, di->data_len, &fcb->atts)) {
                            atts_set = true;

                            if (fcb->type == BTRFS_FT_DIR)
                                fcb->atts |= FILE_ATTRIBUTE_DIRECTORY;
                            else if (fcb->type == BTRFS_FT_SYMLINK)
                                fcb->atts |= FILE_ATTRIBUTE_REPARSE_POINT;

                            if (fcb->type != BTRFS_FT_DIR)
                                fcb->atts &= ~FILE_ATTRIBUTE_DIRECTORY;

                            if (inode == BTRFS_FIRST_FREE_OBJECTID) {
                                if (subvol->root_item.flags & BTRFS_ROOT_SUBVOL_RDONLY)
                                    fcb->atts |= FILE_ATTRIBUTE_READONLY;
                                else
                                    fcb->atts &= ~FILE_ATTRIBUTE_READONLY;
                            }
                        }
                    }
                } else if (tp.item->key.offset == EA_NTACL_HASH && di->name_len == sizeof(EA_NTACL) - 1 && RtlCompareMemory(EA_NTACL, &di[1], di->name_len) == di->name_len) {
                    if (di->data_len > 0) {
                        fcb->sd = ExAllocatePoolWithTag(PagedPool, di->data_len, ALLOC_TAG);
                        if (!fcb->sd) {
                            ERR("out of memory\n");
                            reap_fcb(fcb);
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }

                        RtlCopyMemory(fcb->sd, (uint8_t*)&di[1] + di->name_len, di->data_len);

                        // We have to test against our copy rather than the source, as RtlValidRelativeSecurityDescriptor
                        // will fail if the ACLs aren't 32-bit aligned.
                        if (!RtlValidRelativeSecurityDescriptor(fcb->sd, di->data_len, 0))
                            ExFreePool(fcb->sd);
                        else
                            sd_set = true;
                    }
                } else if (tp.item->key.offset == EA_PROP_COMPRESSION_HASH && di->name_len == sizeof(EA_PROP_COMPRESSION) - 1 && RtlCompareMemory(EA_PROP_COMPRESSION, &di[1], di->name_len) == di->name_len) {
                    if (di->data_len > 0) {
                        static const char lzo[] = "lzo";
                        static const char zlib[] = "zlib";
                        static const char zstd[] = "zstd";

                        if (di->data_len == sizeof(lzo) - 1 && RtlCompareMemory((uint8_t*)&di[1] + di->name_len, lzo, di->data_len) == di->data_len)
                            fcb->prop_compression = PropCompression_LZO;
                        else if (di->data_len == sizeof(zlib) - 1 && RtlCompareMemory((uint8_t*)&di[1] + di->name_len, zlib, di->data_len) == di->data_len)
                            fcb->prop_compression = PropCompression_Zlib;
                        else if (di->data_len == sizeof(zstd) - 1 && RtlCompareMemory((uint8_t*)&di[1] + di->name_len, zstd, di->data_len) == di->data_len)
                            fcb->prop_compression = PropCompression_ZSTD;
                        else
                            fcb->prop_compression = PropCompression_None;
                    }
                } else if (tp.item->key.offset == EA_CASE_SENSITIVE_HASH && di->name_len == sizeof(EA_CASE_SENSITIVE) - 1 && RtlCompareMemory(EA_CASE_SENSITIVE, &di[1], di->name_len) == di->name_len) {
                    if (di->data_len > 0) {
                        fcb->case_sensitive = di->data_len == 1 && *((char*)&di[1] + di->name_len) == '1';
                        fcb->case_sensitive_set = true;
                    }
                } else if (di->name_len > sizeof(xapref) - 1 && RtlCompareMemory(xapref, &di[1], sizeof(xapref) - 1) == sizeof(xapref) - 1) {
                    dir_child* dc;
                    ULONG utf16len;

                    Status = utf8_to_utf16(NULL, 0, &utf16len, (char*)&di[1] + sizeof(xapref) - 1, di->name_len + 1 - sizeof(xapref));
                    if (!NT_SUCCESS(Status)) {
                        ERR("utf8_to_utf16 1 returned %08lx\n", Status);
                        reap_fcb(fcb);
                        return Status;
                    }

                    dc = ExAllocatePoolWithTag(PagedPool, sizeof(dir_child), ALLOC_TAG);
                    if (!dc) {
                        ERR("out of memory\n");
                        reap_fcb(fcb);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    RtlZeroMemory(dc, sizeof(dir_child));

                    dc->utf8.MaximumLength = dc->utf8.Length = di->name_len + 1 - sizeof(xapref);
                    dc->utf8.Buffer = ExAllocatePoolWithTag(PagedPool, dc->utf8.MaximumLength, ALLOC_TAG);
                    if (!dc->utf8.Buffer) {
                        ERR("out of memory\n");
                        ExFreePool(dc);
                        reap_fcb(fcb);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    RtlCopyMemory(dc->utf8.Buffer, (char*)&di[1] + sizeof(xapref) - 1, dc->utf8.Length);

                    dc->name.MaximumLength = dc->name.Length = (uint16_t)utf16len;
                    dc->name.Buffer = ExAllocatePoolWithTag(PagedPool, dc->name.MaximumLength, ALLOC_TAG);
                    if (!dc->name.Buffer) {
                        ERR("out of memory\n");
                        ExFreePool(dc->utf8.Buffer);
                        ExFreePool(dc);
                        reap_fcb(fcb);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    Status = utf8_to_utf16(dc->name.Buffer, utf16len, &utf16len, dc->utf8.Buffer, dc->utf8.Length);
                    if (!NT_SUCCESS(Status)) {
                        ERR("utf8_to_utf16 2 returned %08lx\n", Status);
                        ExFreePool(dc->utf8.Buffer);
                        ExFreePool(dc->name.Buffer);
                        ExFreePool(dc);
                        reap_fcb(fcb);
                        return Status;
                    }

                    Status = RtlUpcaseUnicodeString(&dc->name_uc, &dc->name, true);
                    if (!NT_SUCCESS(Status)) {
                        ERR("RtlUpcaseUnicodeString returned %08lx\n", Status);
                        ExFreePool(dc->utf8.Buffer);
                        ExFreePool(dc->name.Buffer);
                        ExFreePool(dc);
                        reap_fcb(fcb);
                        return Status;
                    }

                    dc->size = di->data_len;

                    InsertTailList(&fcb->dir_children_index, &dc->list_entry_index);
                } else {
                    xattr* xa;

                    xa = ExAllocatePoolWithTag(PagedPool, offsetof(xattr, data[0]) + di->data_len + di->name_len, ALLOC_TAG);
                    if (!xa) {
                        ERR("out of memory\n");
                        reap_fcb(fcb);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    xa->namelen = di->name_len;
                    xa->valuelen = di->data_len;
                    xa->dirty = false;
                    RtlCopyMemory(xa->data, &di[1], di->data_len + di->name_len);

                    InsertTailList(&fcb->xattrs, &xa->list_entry);
                }

                len -= (ULONG)sizeof(struct btrfs_dir_item) + di->data_len + di->name_len;

                if (len < sizeof(struct btrfs_dir_item))
                    break;

                di = (struct btrfs_dir_item*)((uint8_t*)&di[1] + di->data_len + di->name_len);
            } while (true);
        } else if (tp.item->key.type == BTRFS_EXTENT_DATA_KEY) {
            extent* ext;
            bool unique = false;

            ed = (struct btrfs_file_extent_item*)tp.item->data;

            if (tp.item->size < offsetof(struct btrfs_file_extent_item, disk_bytenr)) {
                ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset,
                    tp.item->size, offsetof(struct btrfs_file_extent_item, disk_bytenr));

                reap_fcb(fcb);
                return STATUS_INTERNAL_ERROR;
            }

            if (ed->type == BTRFS_FILE_EXTENT_REG || ed->type == BTRFS_FILE_EXTENT_PREALLOC) {
                if (tp.item->size < sizeof(struct btrfs_file_extent_item)) {
                    ERR("(%I64x,%x,%I64x) was %u bytes, expected at least %Iu\n", tp.item->key.objectid, tp.item->key.type, tp.item->key.offset,
                        tp.item->size, sizeof(struct btrfs_file_extent_item));

                    reap_fcb(fcb);
                    return STATUS_INTERNAL_ERROR;
                }

                if (ed->disk_bytenr == 0 || ed->disk_num_bytes == 0) // sparse
                    continue;

                if (ed->disk_num_bytes != 0 && is_tree_unique(Vcb, tp.tree, Irp))
                    unique = is_extent_unique(Vcb, ed->disk_bytenr, ed->disk_num_bytes, Irp);
            }

            ext = ExAllocatePoolWithTag(pooltype, offsetof(extent, extent_data) + tp.item->size, ALLOC_TAG);
            if (!ext) {
                ERR("out of memory\n");
                reap_fcb(fcb);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            ext->offset = tp.item->key.offset;
            RtlCopyMemory(&ext->extent_data, tp.item->data, tp.item->size);
            ext->datalen = tp.item->size;
            ext->unique = unique;
            ext->ignore = false;
            ext->inserted = false;
            ext->csum = NULL;

            InsertTailList(&fcb->extents, &ext->list_entry);
        }
    }

    if (fcb->type == BTRFS_FT_DIR) {
        Status = load_dir_children(Vcb, fcb, false, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("load_dir_children returned %08lx\n", Status);
            reap_fcb(fcb);
            return Status;
        }
    }

    if (no_data) {
        fcb->Header.AllocationSize.QuadPart = 0;
        fcb->Header.FileSize.QuadPart = 0;
        fcb->Header.ValidDataLength.QuadPart = 0;
    } else {
        if (ed && ed->type == BTRFS_FILE_EXTENT_INLINE)
            fcb->Header.AllocationSize.QuadPart = fcb->inode_item.size;
        else
            fcb->Header.AllocationSize.QuadPart = sector_align(fcb->inode_item.size, fcb->Vcb->superblock.sectorsize);

        fcb->Header.FileSize.QuadPart = fcb->inode_item.size;
        fcb->Header.ValidDataLength.QuadPart = fcb->inode_item.size;
    }

    if (!atts_set)
        fcb->atts = get_file_attributes(Vcb, fcb->subvol, fcb->inode, fcb->type, utf8 && utf8->Buffer[0] == '.', true, Irp);

    if (!sd_set) {
        if (parent)
            ExAcquireResourceSharedLite(parent->Header.Resource, true);

        Status = fcb_get_sd(fcb, parent, false, Irp);

        if (parent)
            ExReleaseResourceLite(parent->Header.Resource);

        if (!NT_SUCCESS(Status)) {
            ERR("fcb_get_sd returned %08lx\n", Status);
            reap_fcb(fcb);
            return Status;
        }
    }

    acquire_fcb_lock_exclusive(Vcb);

    if (lastle && subvol->fcbs_version == fcbs_version) {
        InsertHeadList(lastle, &fcb->list_entry);

        if (!subvol->fcbs_ptrs[hash >> 24] || CONTAINING_RECORD(subvol->fcbs_ptrs[hash >> 24], struct _fcb, list_entry)->hash > hash)
            subvol->fcbs_ptrs[hash >> 24] = &fcb->list_entry;
    } else {
        lastle = NULL;

        if (subvol->fcbs_ptrs[hash >> 24]) {
            LIST_ENTRY* le = subvol->fcbs_ptrs[hash >> 24];

            while (le != &subvol->fcbs) {
                struct _fcb* fcb2 = CONTAINING_RECORD(le, struct _fcb, list_entry);

                if (fcb2->inode == inode) {
                    if (!fcb2->ads) {
                        if (fcb2->deleted)
                            deleted_fcb = fcb2;
                        else {
#ifdef DEBUG_FCB_REFCOUNTS
                            LONG rc = InterlockedIncrement(&fcb2->refcount);

                            WARN("fcb %p: refcount now %i (subvol %I64x, inode %I64x)\n", fcb2, rc, fcb2->subvol->id, fcb2->inode);
#else
                            InterlockedIncrement(&fcb2->refcount);
#endif

                            *pfcb = fcb2;
                            reap_fcb(fcb);
                            release_fcb_lock(Vcb);
                            return STATUS_SUCCESS;
                        }
                    }
                } else if (fcb2->hash > hash) {
                    if (deleted_fcb) {
                        InterlockedIncrement(&deleted_fcb->refcount);
                        *pfcb = deleted_fcb;
                        reap_fcb(fcb);
                        release_fcb_lock(Vcb);
                        return STATUS_SUCCESS;
                    }

                    lastle = le->Blink;
                    break;
                }

                le = le->Flink;
            }
        }

        if (fcb->type == BTRFS_FT_DIR && fcb->atts & FILE_ATTRIBUTE_REPARSE_POINT && fcb->reparse_xattr.Length == 0) {
            fcb->atts &= ~FILE_ATTRIBUTE_REPARSE_POINT;

            if (!Vcb->readonly && !is_subvol_readonly(subvol, Irp)) {
                fcb->atts_changed = true;
                mark_fcb_dirty(fcb);
            }
        }

        if (!lastle) {
            uint8_t c = hash >> 24;

            if (c != 0xff) {
                uint8_t d = c + 1;

                do {
                    if (subvol->fcbs_ptrs[d]) {
                        lastle = subvol->fcbs_ptrs[d]->Blink;
                        break;
                    }

                    d++;
                } while (d != 0);
            }
        }

        if (lastle) {
            InsertHeadList(lastle, &fcb->list_entry);

            if (lastle == &subvol->fcbs || (CONTAINING_RECORD(lastle, struct _fcb, list_entry)->hash >> 24) != (hash >> 24))
                subvol->fcbs_ptrs[hash >> 24] = &fcb->list_entry;
        } else {
            InsertTailList(&subvol->fcbs, &fcb->list_entry);

            if (fcb->list_entry.Blink == &subvol->fcbs || (CONTAINING_RECORD(fcb->list_entry.Blink, struct _fcb, list_entry)->hash >> 24) != (hash >> 24))
                subvol->fcbs_ptrs[hash >> 24] = &fcb->list_entry;
        }
    }

    if (fcb->inode == BTRFS_FIRST_FREE_OBJECTID && fcb->subvol->id == BTRFS_FS_TREE_OBJECTID && fcb->subvol != Vcb->root_fileref->fcb->subvol)
        fcb->atts |= FILE_ATTRIBUTE_HIDDEN;

    subvol->fcbs_version++;

    InsertTailList(&Vcb->all_fcbs, &fcb->list_entry_all);

    release_fcb_lock(Vcb);

    fcb->Header.IsFastIoPossible = fast_io_possible(fcb);

    *pfcb = fcb;

    return STATUS_SUCCESS;
}
