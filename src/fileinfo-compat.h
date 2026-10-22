#pragma once

// not currently in mingw - introduced with Windows 10
#ifndef _MSC_VER
#define FileIdInformation (enum _FILE_INFORMATION_CLASS)59
#define FileHardLinkFullIdInformation (enum _FILE_INFORMATION_CLASS)62
#define FileDispositionInformationEx (enum _FILE_INFORMATION_CLASS)64
#define FileRenameInformationEx (enum _FILE_INFORMATION_CLASS)65
#define FileStatInformation (enum _FILE_INFORMATION_CLASS)68
#define FileStatLxInformation (enum _FILE_INFORMATION_CLASS)70
#define FileCaseSensitiveInformation (enum _FILE_INFORMATION_CLASS)71
#define FileLinkInformationEx (enum _FILE_INFORMATION_CLASS)72
#define FileStorageReserveIdInformation (enum _FILE_INFORMATION_CLASS)74

typedef struct _FILE_ID_INFORMATION {
    ULONGLONG VolumeSerialNumber;
    FILE_ID_128 FileId;
} FILE_ID_INFORMATION, *PFILE_ID_INFORMATION;

typedef struct _FILE_STAT_INFORMATION {
    LARGE_INTEGER FileId;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG FileAttributes;
    ULONG ReparseTag;
    ULONG NumberOfLinks;
    ACCESS_MASK EffectiveAccess;
} FILE_STAT_INFORMATION, *PFILE_STAT_INFORMATION;

typedef struct _FILE_STAT_LX_INFORMATION {
    LARGE_INTEGER FileId;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG         FileAttributes;
    ULONG         ReparseTag;
    ULONG         NumberOfLinks;
    ACCESS_MASK   EffectiveAccess;
    ULONG         LxFlags;
    ULONG         LxUid;
    ULONG         LxGid;
    ULONG         LxMode;
    ULONG         LxDeviceIdMajor;
    ULONG         LxDeviceIdMinor;
} FILE_STAT_LX_INFORMATION, *PFILE_STAT_LX_INFORMATION;

#define LX_FILE_METADATA_HAS_UID        0x01
#define LX_FILE_METADATA_HAS_GID        0x02
#define LX_FILE_METADATA_HAS_MODE       0x04
#define LX_FILE_METADATA_HAS_DEVICE_ID  0x08
#define LX_FILE_CASE_SENSITIVE_DIR      0x10

typedef struct _FILE_DISPOSITION_INFORMATION_EX {
    ULONG Flags;
} FILE_DISPOSITION_INFORMATION_EX, *PFILE_DISPOSITION_INFORMATION_EX;

#ifndef FILE_DISPOSITION_DELETE
#define FILE_DISPOSITION_DELETE                    0x00000001
#endif
#ifndef FILE_DISPOSITION_POSIX_SEMANTICS
#define FILE_DISPOSITION_POSIX_SEMANTICS           0x00000002
#endif
#ifndef FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK
#define FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK 0x00000004
#endif

typedef struct _FILE_CASE_SENSITIVE_INFORMATION {
    ULONG Flags;
} FILE_CASE_SENSITIVE_INFORMATION, *PFILE_CASE_SENSITIVE_INFORMATION;

#endif

#if !defined(_MSC_VER) || NTDDI_VERSION < NTDDI_WIN10_RS5
typedef struct _FILE_LINK_INFORMATION_EX {
    union {
        BOOLEAN ReplaceIfExists;
        ULONG Flags;
    };
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_LINK_INFORMATION_EX, *PFILE_LINK_INFORMATION_EX;

C_ASSERT(FIELD_OFFSET(FILE_LINK_INFORMATION_EX, RootDirectory) == FIELD_OFFSET(FILE_LINK_INFORMATION, RootDirectory));
C_ASSERT(FIELD_OFFSET(FILE_LINK_INFORMATION_EX, FileName) == FIELD_OFFSET(FILE_LINK_INFORMATION, FileName));
#else
#define FILE_LINK_INFORMATION_EX FILE_LINK_INFORMATION
#endif

#ifndef FILE_LINK_REPLACE_IF_EXISTS
#define FILE_LINK_REPLACE_IF_EXISTS                       0x001
#endif
#ifndef FILE_LINK_POSIX_SEMANTICS
#define FILE_LINK_POSIX_SEMANTICS                         0x002
#endif
#ifndef FILE_LINK_SUPPRESS_STORAGE_RESERVE_INHERITANCE
#define FILE_LINK_SUPPRESS_STORAGE_RESERVE_INHERITANCE    0x008
#endif
#ifndef FILE_LINK_NO_INCREASE_AVAILABLE_SPACE
#define FILE_LINK_NO_INCREASE_AVAILABLE_SPACE             0x010
#endif
#ifndef FILE_LINK_NO_DECREASE_AVAILABLE_SPACE
#define FILE_LINK_NO_DECREASE_AVAILABLE_SPACE             0x020
#endif
#ifndef FILE_LINK_IGNORE_READONLY_ATTRIBUTE
#define FILE_LINK_IGNORE_READONLY_ATTRIBUTE               0x040
#endif
#ifndef FILE_LINK_FORCE_RESIZE_TARGET_SR
#define FILE_LINK_FORCE_RESIZE_TARGET_SR                  0x080
#endif
#ifndef FILE_LINK_FORCE_RESIZE_SOURCE_SR
#define FILE_LINK_FORCE_RESIZE_SOURCE_SR                  0x100
#endif
