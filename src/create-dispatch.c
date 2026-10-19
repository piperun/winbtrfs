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

#include "create-dispatch.h"
#include "file-create.h"
#include "stream-create.h"

extern tFsRtlGetEcpListFromIrp fFsRtlGetEcpListFromIrp;
extern tFsRtlGetNextExtraCreateParameter fFsRtlGetNextExtraCreateParameter;

// Windows 10
#define ATOMIC_CREATE_ECP_IN_FLAG_REPARSE_POINT_SPECIFIED   0x0002
#define ATOMIC_CREATE_ECP_IN_FLAG_OP_FLAGS_SPECIFIED        0x0080
#define ATOMIC_CREATE_ECP_IN_FLAG_BEST_EFFORT               0x0100

#define ATOMIC_CREATE_ECP_OUT_FLAG_REPARSE_POINT_SET        0x0002
#define ATOMIC_CREATE_ECP_OUT_FLAG_OP_FLAGS_HONORED         0x0080

#define ATOMIC_CREATE_ECP_IN_OP_FLAG_CASE_SENSITIVE_FLAGS_SPECIFIED       1
#define ATOMIC_CREATE_ECP_OUT_OP_FLAG_CASE_SENSITIVE_FLAGS_SET            1

typedef struct _FILE_TIMESTAMPS {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
} FILE_TIMESTAMPS, *PFILE_TIMESTAMPS;

typedef struct _ATOMIC_CREATE_ECP_CONTEXT {
    USHORT Size;
    USHORT InFlags;
    USHORT OutFlags;
    USHORT ReparseBufferLength;
    PREPARSE_DATA_BUFFER ReparseBuffer;
    LONGLONG FileSize;
    LONGLONG ValidDataLength;
    PFILE_TIMESTAMPS FileTimestamps;
    ULONG FileAttributes;
    ULONG UsnSourceInfo;
    USN Usn;
    ULONG SuppressFileAttributeInheritanceMask;
    ULONG InOpFlags;
    ULONG OutOpFlags;
    ULONG InGenFlags;
    ULONG OutGenFlags;
    ULONG CaseSensitiveFlagsMask;
    ULONG InCaseSensitiveFlags;
    ULONG OutCaseSensitiveFlags;
} ATOMIC_CREATE_ECP_CONTEXT, *PATOMIC_CREATE_ECP_CONTEXT;

static const GUID GUID_ECP_ATOMIC_CREATE = { 0x4720bd83, 0x52ac, 0x4104, { 0xa1, 0x30, 0xd1, 0xec, 0x6a, 0x8c, 0xc8, 0xe5 } };
static const GUID GUID_ECP_QUERY_ON_CREATE = { 0x1aca62e9, 0xabb4, 0x4ff2, { 0xbb, 0x5c, 0x1c, 0x79, 0x02, 0x5e, 0x41, 0x7f } };
static const GUID GUID_ECP_CREATE_REDIRECTION = { 0x188d6bd6, 0xa126, 0x4fa8, { 0xbd, 0xf2, 0x1c, 0xcd, 0xf8, 0x96, 0xf3, 0xe0 } };

NTSTATUS file_create(PIRP Irp, _Requires_lock_held_(_Curr_->tree_lock) _Requires_exclusive_lock_held_(_Curr_->fcb_lock) device_extension* Vcb,
                            PFILE_OBJECT FileObject, file_ref* related, bool loaded_related, PUNICODE_STRING fnus, ULONG disposition, ULONG options,
                            file_ref** existing_fileref, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    file_ref *fileref, *parfileref = NULL;
    ULONG i, j;
    ccb* ccb;
    static const WCHAR datasuf[] = {':','$','D','A','T','A',0};
    UNICODE_STRING dsus, fpus, stream;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    POOL_TYPE pool_type = IrpSp->Flags & SL_OPEN_PAGING_FILE ? NonPagedPool : PagedPool;
    ECP_LIST* ecp_list;
    ATOMIC_CREATE_ECP_CONTEXT* acec = NULL;
#ifdef DEBUG_FCB_REFCOUNTS
    LONG oc;
#endif

    TRACE("(%p, %p, %p, %.*S, %lx, %lx)\n", Irp, Vcb, FileObject, (int)(fnus->Length / sizeof(WCHAR)), fnus->Buffer, disposition, options);

    if (Vcb->readonly)
        return STATUS_MEDIA_WRITE_PROTECTED;

    if (options & FILE_DELETE_ON_CLOSE && IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_READONLY &&
        !(IrpSp->Flags & SL_IGNORE_READONLY_ATTRIBUTE)) {
        return STATUS_CANNOT_DELETE;
    }

    if (fFsRtlGetEcpListFromIrp && fFsRtlGetNextExtraCreateParameter) {
        if (NT_SUCCESS(fFsRtlGetEcpListFromIrp(Irp, &ecp_list)) && ecp_list) {
            void* ctx = NULL;
            GUID type;
            ULONG ctxsize;

            do {
                Status = fFsRtlGetNextExtraCreateParameter(ecp_list, ctx, &type, &ctx, &ctxsize);

                if (NT_SUCCESS(Status)) {
                    if (RtlCompareMemory(&type, &GUID_ECP_ATOMIC_CREATE, sizeof(GUID)) == sizeof(GUID)) {
                        if (ctxsize >= sizeof(ATOMIC_CREATE_ECP_CONTEXT))
                            acec = ctx;
                        else {
                            ERR("GUID_ECP_ATOMIC_CREATE context was too short: %lu bytes, expected %Iu\n", ctxsize,
                                sizeof(ATOMIC_CREATE_ECP_CONTEXT));
                        }
                    } else if (RtlCompareMemory(&type, &GUID_ECP_QUERY_ON_CREATE, sizeof(GUID)) == sizeof(GUID))
                        WARN("unhandled ECP GUID_ECP_QUERY_ON_CREATE\n");
                    else if (RtlCompareMemory(&type, &GUID_ECP_CREATE_REDIRECTION, sizeof(GUID)) == sizeof(GUID))
                        WARN("unhandled ECP GUID_ECP_CREATE_REDIRECTION\n");
                    else {
                        WARN("unhandled ECP {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n", type.Data1, type.Data2,
                             type.Data3, type.Data4[0], type.Data4[1], type.Data4[2], type.Data4[3], type.Data4[4], type.Data4[5],
                             type.Data4[6], type.Data4[7]);
                    }
                }
            } while (NT_SUCCESS(Status));
        }
    }

    dsus.Buffer = (WCHAR*)datasuf;
    dsus.Length = dsus.MaximumLength = sizeof(datasuf) - sizeof(WCHAR);
    fpus.Buffer = NULL;

    if (!loaded_related) {
        Status = open_fileref(Vcb, &parfileref, fnus, related, true, NULL, NULL, pool_type, IrpSp->Flags & SL_CASE_SENSITIVE, Irp);

        if (!NT_SUCCESS(Status))
            goto end;
    } else
        parfileref = related;

    if (parfileref->fcb->type != BTRFS_FT_DIR && (fnus->Length < sizeof(WCHAR) || fnus->Buffer[0] != ':')) {
        Status = STATUS_OBJECT_PATH_NOT_FOUND;
        goto end;
    }

    if (is_subvol_readonly(parfileref->fcb->subvol, Irp)) {
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }

    i = (fnus->Length / sizeof(WCHAR))-1;
    while ((fnus->Buffer[i] == '\\' || fnus->Buffer[i] == '/') && i > 0) { i--; }

    j = i;

    while (i > 0 && fnus->Buffer[i-1] != '\\' && fnus->Buffer[i-1] != '/') { i--; }

    fpus.MaximumLength = (USHORT)((j - i + 2) * sizeof(WCHAR));
    fpus.Buffer = ExAllocatePoolWithTag(pool_type, fpus.MaximumLength, ALLOC_TAG);
    if (!fpus.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    fpus.Length = (USHORT)((j - i + 1) * sizeof(WCHAR));

    RtlCopyMemory(fpus.Buffer, &fnus->Buffer[i], (j - i + 1) * sizeof(WCHAR));
    fpus.Buffer[j - i + 1] = 0;

    if (fpus.Length > dsus.Length) { // check for :$DATA suffix
        UNICODE_STRING lb;

        lb.Buffer = &fpus.Buffer[(fpus.Length - dsus.Length)/sizeof(WCHAR)];
        lb.Length = lb.MaximumLength = dsus.Length;

        TRACE("lb = %.*S\n", (int)(lb.Length/sizeof(WCHAR)), lb.Buffer);

        if (FsRtlAreNamesEqual(&dsus, &lb, true, NULL)) {
            TRACE("ignoring :$DATA suffix\n");

            fpus.Length -= lb.Length;

            if (fpus.Length > sizeof(WCHAR) && fpus.Buffer[(fpus.Length-1)/sizeof(WCHAR)] == ':')
                fpus.Length -= sizeof(WCHAR);

            TRACE("fpus = %.*S\n", (int)(fpus.Length / sizeof(WCHAR)), fpus.Buffer);
        }
    }

    stream.Length = 0;

    for (i = 0; i < fpus.Length / sizeof(WCHAR); i++) {
        if (fpus.Buffer[i] == ':') {
            stream.Length = (USHORT)(fpus.Length - (i * sizeof(WCHAR)) - sizeof(WCHAR));
            stream.Buffer = &fpus.Buffer[i+1];
            fpus.Buffer[i] = 0;
            fpus.Length = (USHORT)(i * sizeof(WCHAR));
            break;
        }
    }

    if (stream.Length > 0) {
        file_ref* stream_parent = parfileref;

        Status = create_stream(Vcb, &fileref, &stream_parent, &fpus, &stream, Irp, options, pool_type, IrpSp->Flags & SL_CASE_SENSITIVE, rollback);
        if (Status == STATUS_OBJECT_NAME_COLLISION) {
            *existing_fileref = fileref;
            goto end;
        } else if (!NT_SUCCESS(Status)) {
            ERR("create_stream returned %08lx\n", Status);
            goto end;
        }

        // The new stream owns its parent hold; release the extra returned hold.
        free_fileref(stream_parent);

        IoSetShareAccess(IrpSp->Parameters.Create.SecurityContext->DesiredAccess, IrpSp->Parameters.Create.ShareAccess,
                         FileObject, &fileref->fcb->share_access);
    } else {
        ACCESS_MASK granted_access;

        Status = check_file_name_valid(&fpus, false, false);
        if (!NT_SUCCESS(Status))
            goto end;

        SeLockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);

        ExAcquireResourceSharedLite(parfileref->fcb->Header.Resource, true);

        if (!SeAccessCheck(parfileref->fcb->sd, &IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext,
                           true, options & FILE_DIRECTORY_FILE ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE, 0, NULL,
                           IoGetFileObjectGenericMapping(), IrpSp->Flags & SL_FORCE_ACCESS_CHECK ? UserMode : Irp->RequestorMode,
                           &granted_access, &Status)) {
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);
            SeUnlockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);
            goto end;
        }

        ExReleaseResourceLite(parfileref->fcb->Header.Resource);

        SeUnlockSubjectContext(&IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext);

        if (Irp->AssociatedIrp.SystemBuffer && IrpSp->Parameters.Create.EaLength > 0) {
            ULONG offset;

            Status = IoCheckEaBufferValidity(Irp->AssociatedIrp.SystemBuffer, IrpSp->Parameters.Create.EaLength, &offset);
            if (!NT_SUCCESS(Status)) {
                ERR("IoCheckEaBufferValidity returned %08lx (error at offset %lu)\n", Status, offset);
                goto end;
            }
        }

        Status = file_create2(Irp, Vcb, &fpus, parfileref, options, Irp->AssociatedIrp.SystemBuffer, IrpSp->Parameters.Create.EaLength,
                              &fileref, IrpSp->Flags & SL_CASE_SENSITIVE, rollback);

        if (Status == STATUS_OBJECT_NAME_COLLISION) {
            *existing_fileref = fileref;
            goto end;
        } else if (!NT_SUCCESS(Status)) {
            ERR("file_create2 returned %08lx\n", Status);
            goto end;
        }

        IoSetShareAccess(IrpSp->Parameters.Create.SecurityContext->DesiredAccess, IrpSp->Parameters.Create.ShareAccess, FileObject, &fileref->fcb->share_access);

        send_notification_fileref(fileref, options & FILE_DIRECTORY_FILE ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED, NULL);
        queue_notification_fcb(fileref->parent, FILE_NOTIFY_CHANGE_LAST_WRITE, FILE_ACTION_MODIFIED, NULL);
    }

    FileObject->FsContext = fileref->fcb;

    ccb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*ccb), ALLOC_TAG);
    if (!ccb) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        fileref->deleted = true;
        fileref->fcb->deleted = true;

        if (stream.Length == 0) {
            ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
            parfileref->fcb->inode_item.size -= fileref->dc->utf8.Length * 2;
            ExReleaseResourceLite(parfileref->fcb->Header.Resource);
        }

        free_fileref(fileref);
        goto end;
    }

    RtlZeroMemory(ccb, sizeof(*ccb));

    ccb->fileref = fileref;

    ccb->NodeType = BTRFS_NODE_TYPE_CCB;
    ccb->NodeSize = sizeof(*ccb);
    ccb->disposition = disposition;
    ccb->options = options;
    ccb->query_dir_offset = 0;
    RtlInitUnicodeString(&ccb->query_string, NULL);
    ccb->has_wildcard = false;
    ccb->specific_file = false;
    ccb->access = IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
    ccb->case_sensitive = IrpSp->Flags & SL_CASE_SENSITIVE;
    ccb->reserving = false;
    ccb->lxss = called_from_lxss();

#ifdef DEBUG_FCB_REFCOUNTS
    oc = InterlockedIncrement(&fileref->open_count);
    ERR("fileref %p: open_count now %i\n", fileref, oc);
#else
    InterlockedIncrement(&fileref->open_count);
#endif
    InterlockedIncrement(&Vcb->open_files);

    FileObject->FsContext2 = ccb;

    FileObject->SectionObjectPointer = &fileref->fcb->nonpaged->segment_object;

    // FIXME - ATOMIC_CREATE_ECP_IN_FLAG_BEST_EFFORT
    if (acec && acec->InFlags & ATOMIC_CREATE_ECP_IN_FLAG_REPARSE_POINT_SPECIFIED) {
        if (acec->ReparseBufferLength > sizeof(uint32_t) && *(uint32_t*)acec->ReparseBuffer == IO_REPARSE_TAG_SYMLINK) {
            fileref->fcb->inode_item.mode &= ~(__S_IFIFO | __S_IFCHR | __S_IFBLK | __S_IFSOCK);
            fileref->fcb->type = BTRFS_FT_REG_FILE;
            fileref->fcb->atts &= ~FILE_ATTRIBUTE_DIRECTORY;
        }

        if (fileref->fcb->type == BTRFS_FT_SOCK || fileref->fcb->type == BTRFS_FT_FIFO ||
            fileref->fcb->type == BTRFS_FT_CHRDEV || fileref->fcb->type == BTRFS_FT_BLKDEV) {
            // NOP. If called from LXSS, humour it - we hardcode the values elsewhere.
        } else {
            Status = set_reparse_point2(fileref->fcb, acec->ReparseBuffer, acec->ReparseBufferLength, NULL, NULL, Irp, rollback);
            if (!NT_SUCCESS(Status)) {
                ERR("set_reparse_point2 returned %08lx\n", Status);
                fileref->deleted = true;
                fileref->fcb->deleted = true;

                if (stream.Length == 0) {
                    ExAcquireResourceExclusiveLite(parfileref->fcb->Header.Resource, true);
                    parfileref->fcb->inode_item.size -= fileref->dc->utf8.Length * 2;
                    ExReleaseResourceLite(parfileref->fcb->Header.Resource);
                }

                free_fileref(fileref);
                return Status;
            }
        }

        acec->OutFlags |= ATOMIC_CREATE_ECP_OUT_FLAG_REPARSE_POINT_SET;
    }

    if (acec && acec->InFlags & ATOMIC_CREATE_ECP_IN_FLAG_OP_FLAGS_SPECIFIED) {
        if (acec->InOpFlags & ATOMIC_CREATE_ECP_IN_OP_FLAG_CASE_SENSITIVE_FLAGS_SPECIFIED && fileref->fcb->atts & FILE_ATTRIBUTE_DIRECTORY) {
            if ((acec->InCaseSensitiveFlags & acec->CaseSensitiveFlagsMask) & FILE_CS_FLAG_CASE_SENSITIVE_DIR) {
                acec->OutCaseSensitiveFlags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
                fileref->fcb->case_sensitive = true;
                ccb->case_sensitive = true;
            }

            acec->OutOpFlags |= ATOMIC_CREATE_ECP_OUT_OP_FLAG_CASE_SENSITIVE_FLAGS_SET;
        }

        acec->OutFlags |= ATOMIC_CREATE_ECP_OUT_FLAG_OP_FLAGS_HONORED;
    }

    fileref->dc->type = fileref->fcb->type;

end:
    if (fpus.Buffer)
        ExFreePool(fpus.Buffer);

    if (parfileref && !loaded_related)
        free_fileref(parfileref);

    return Status;
}
