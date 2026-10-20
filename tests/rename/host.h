/* Native Windows service boundary for rename ownership regressions. */
#ifndef RENAME_TEST_HOST_H
#define RENAME_TEST_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"
typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG, ACCESS_MASK;
typedef uint16_t WCHAR, USHORT;
typedef uint8_t BOOLEAN;
typedef void* HANDLE;
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct { USHORT Length, MaximumLength; char* Buffer; } ANSI_STRING, *PANSI_STRING, *PSTRING;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned held; } ERESOURCE;
typedef struct { uint64_t id; } root;
typedef struct { ERESOURCE dir_children_lock; } fcb_nonpaged;
typedef struct device_extension device_extension;
typedef struct {
    device_extension* Vcb;
    root* subvol;
    uint64_t inode;
    ULONG type, atts;
    bool ads, inode_item_changed;
    struct btrfs_inode_item inode_item;
    struct { ERESOURCE* Resource; } Header;
    fcb_nonpaged* nonpaged;
    void* sd;
} fcb;
typedef struct { ANSI_STRING utf8; UNICODE_STRING name, name_uc; uint32_t hash, hash_uc; } dir_child;
typedef struct file_ref {
    fcb* fcb;
    struct file_ref* parent;
    dir_child* dc;
    LONG refcount, open_count;
    bool created, deleted, delete_on_close, posix_delete;
    ANSI_STRING oldutf8;
} file_ref;
typedef struct _ccb { file_ref* fileref; bool case_sensitive, user_set_change_time; } ccb;
struct device_extension {
    ERESOURCE tree_lock, fileref_lock;
    fcb* dummy_fcb;
    struct { uint64_t generation; } superblock;
    void* NotifySync;
    LIST_ENTRY DirNotifyList;
};
typedef struct { void* DataSectionObject; } SECTION_OBJECT_POINTERS;
typedef struct { fcb* FsContext; ccb* FsContext2; SECTION_OBJECT_POINTERS* SectionObjectPointer; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct { struct { void* SystemBuffer; } AssociatedIrp; int RequestorMode; } IRP, *PIRP;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { NTSTATUS Status; } IO_STATUS_BLOCK;
typedef unsigned SECURITY_SUBJECT_CONTEXT;
#define _Inout_
#define _In_
#define _In_reads_bytes_(n)
#define __stdcall
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define STATUS_OBJECT_NAME_INVALID ((NTSTATUS)0xc0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xc0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xc000003a)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_REPARSE ((NTSTATUS)0x104)
#define FILE_ATTRIBUTE_READONLY 1
#define FILE_ADD_FILE 2
#define FILE_ADD_SUBDIRECTORY 4
#define DELETE 0x10000
#define FILE_NOTIFY_CHANGE_FILE_NAME 1
#define FILE_NOTIFY_CHANGE_DIR_NAME 2
#define FILE_NOTIFY_CHANGE_LAST_WRITE 16
#define FILE_ACTION_MODIFIED 3
#define FILE_ACTION_RENAMED_OLD_NAME 4
#define FILE_ACTION_RENAMED_NEW_NAME 5
#define PagedPool 1
#define ALLOC_TAG 0
#define UserMode 1
#define TRACE(...) host_log(__VA_ARGS__)
#define ERR(...) host_log(__VA_ARGS__)
#define WARN(...) host_log(__VA_ARGS__)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
#define ExAllocatePoolWithTag(pool,n,tag) allocate(n)
#define increase_fileref_refcount(fr) InterlockedIncrement(&(fr)->refcount)
static void host_log(const char* format, ...) { (void)format; }
static void InitializeListHead(LIST_ENTRY* head) { head->Flink = head->Blink = head; }
static LONG InterlockedIncrement(LONG* value) { return ++*value; }
static LONG InterlockedDecrement(LONG* value) { return --*value; }
#endif
