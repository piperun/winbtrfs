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

#pragma once

#include "btrfs_drv.h"

#if !defined(_MSC_VER) || NTDDI_VERSION < NTDDI_WIN10_RS1
typedef struct _FILE_RENAME_INFORMATION_EX {
    union {
        BOOLEAN ReplaceIfExists;
        ULONG Flags;
    };
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_RENAME_INFORMATION_EX, *PFILE_RENAME_INFORMATION_EX;

C_ASSERT(FIELD_OFFSET(FILE_RENAME_INFORMATION_EX, RootDirectory) == FIELD_OFFSET(FILE_RENAME_INFORMATION, RootDirectory));
C_ASSERT(FIELD_OFFSET(FILE_RENAME_INFORMATION_EX, FileName) == FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName));

#else
#define FILE_RENAME_INFORMATION_EX FILE_RENAME_INFORMATION
#endif

#ifndef FILE_RENAME_REPLACE_IF_EXISTS
#define FILE_RENAME_REPLACE_IF_EXISTS                       0x001
#endif
#ifndef FILE_RENAME_POSIX_SEMANTICS
#define FILE_RENAME_POSIX_SEMANTICS                         0x002
#endif
#ifndef FILE_RENAME_SUPPRESS_PIN_STATE_INHERITANCE
#define FILE_RENAME_SUPPRESS_PIN_STATE_INHERITANCE          0x004
#endif
#ifndef FILE_RENAME_SUPPRESS_STORAGE_RESERVE_INHERITANCE
#define FILE_RENAME_SUPPRESS_STORAGE_RESERVE_INHERITANCE    0x008
#endif
#ifndef FILE_RENAME_NO_INCREASE_AVAILABLE_SPACE
#define FILE_RENAME_NO_INCREASE_AVAILABLE_SPACE             0x010
#endif
#ifndef FILE_RENAME_NO_DECREASE_AVAILABLE_SPACE
#define FILE_RENAME_NO_DECREASE_AVAILABLE_SPACE             0x020
#endif
#ifndef FILE_RENAME_IGNORE_READONLY_ATTRIBUTE
#define FILE_RENAME_IGNORE_READONLY_ATTRIBUTE               0x040
#endif
#ifndef FILE_RENAME_FORCE_RESIZE_TARGET_SR
#define FILE_RENAME_FORCE_RESIZE_TARGET_SR                  0x080
#endif
#ifndef FILE_RENAME_FORCE_RESIZE_SOURCE_SR
#define FILE_RENAME_FORCE_RESIZE_SOURCE_SR                  0x100
#endif

NTSTATUS move_across_subvols(file_ref* fileref, ccb* ccb, file_ref* destdir,
                                    PANSI_STRING utf8, PUNICODE_STRING fnus,
                                    PIRP Irp);
NTSTATUS move_across_directories(file_ref* fileref, ccb* ccb,
                                        file_ref* related, ANSI_STRING* utf8,
                                        UNICODE_STRING* fnus, PIRP Irp);
NTSTATUS rename_stream(device_extension* Vcb, file_ref* fileref, ccb* ccb, FILE_RENAME_INFORMATION_EX* fri,
                              ULONG flags, PIRP Irp, LIST_ENTRY* rollback);
NTSTATUS rename_file_to_stream(device_extension* Vcb, file_ref* fileref, ccb* ccb, FILE_RENAME_INFORMATION_EX* fri,
                                      ULONG flags, PIRP Irp, LIST_ENTRY* rollback);

NTSTATUS set_rename_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject, PFILE_OBJECT tfo, bool ex);
