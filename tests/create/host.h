/* Minimal Windows type model for native creation ownership tests. */
#ifndef CREATE_TEST_HOST_H
#define CREATE_TEST_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

#define _In_
#define _Inout_
#define _In_opt_
#define _Out_
#define _In_reads_bytes_opt_(n)
#define _Requires_lock_held_(x)
#define _In_reads_bytes_(n)
#define _Requires_exclusive_lock_held_(x)
#define __stdcall
#define TRACE(...) host_log(__VA_ARGS__)
#define ERR(...) host_log(__VA_ARGS__)
#define WARN(...) host_log(__VA_ARGS__)
#define PagedPool 1
#define NonPagedPool 0
#define BTRFS_NODE_TYPE_FCB 0x501
#define ALLOC_TAG 0
#define FILE_ADD_FILE 2
#define FILE_ADD_SUBDIRECTORY 4
#define FILE_ATTRIBUTE_HIDDEN 2
#define FILE_ATTRIBUTE_DIRECTORY 16
#define FILE_NOTIFY_CHANGE_FILE_NAME 1
#define FILE_NOTIFY_CHANGE_DIR_NAME 2
#define FILE_NOTIFY_CHANGE_LAST_WRITE 16
#define FILE_ACTION_ADDED 1
#define FILE_ACTION_MODIFIED 3
#define SE_MANAGE_VOLUME_PRIVILEGE 28
#define SEF_SACL_AUTO_INHERIT 2
#define PropCompression_None 0
#define UID_NOBODY 65534
#define GID_NOBODY 65534
#define __S_IFDIR 0040000
#define __S_IFCHR 0020000
#define __S_IFBLK 0060000
#define __S_IFIFO 0010000
#define __S_IFSOCK 0140000
#define __S_IFLNK 0120000
#define __S_IFREG 0100000
#define S_IXUSR 0000100
#define S_IXGRP 0000010
#define S_IXOTH 0000001
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS ((int32_t)0)
#define STATUS_UNSUCCESSFUL ((int32_t)0xc0000001)
#define STATUS_INVALID_PARAMETER ((int32_t)0xc000000d)
#define STATUS_ACCESS_DENIED ((int32_t)0xc0000022)
#define STATUS_OBJECT_NAME_INVALID ((int32_t)0xc0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND ((int32_t)0xc0000034)
#define STATUS_OBJECT_PATH_NOT_FOUND ((int32_t)0xc000003a)
#define STATUS_OBJECT_NAME_COLLISION ((int32_t)0xc0000035)
#define STATUS_PRIVILEGE_NOT_HELD ((int32_t)0xc0000061)
#define STATUS_INSUFFICIENT_RESOURCES ((int32_t)0xc000009a)
#define STATUS_MEDIA_WRITE_PROTECTED ((int32_t)0xc00000a2)
#define STATUS_INTERNAL_ERROR ((int32_t)0xc00000e5)
#define STATUS_IO_DEVICE_ERROR ((int32_t)0xc0000185)
#define STATUS_NOT_FOUND ((int32_t)0xc0000225)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define major(r) ((((r) >> 8) & 0xfff) | ((uint32_t)((r) >> 32) & ~0xfff))
#define minor(r) (((r) & 0xff) | ((uint32_t)((r) >> 12) & ~0xff))

typedef int32_t NTSTATUS, LONG;
typedef uint32_t ACCESS_MASK;
typedef uint16_t WCHAR, USHORT;
typedef uint32_t ULONG;
typedef unsigned char BOOLEAN;
typedef void* PSID;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct { USHORT Length, MaximumLength; char* Buffer; } ANSI_STRING, *PANSI_STRING;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned held; bool initialized; } ERESOURCE;
typedef struct { unsigned allocations, returns, kind; } LOOKASIDE;
typedef int POOL_TYPE;
typedef int SECURITY_SUBJECT_CONTEXT;
typedef struct _device_extension device_extension;
typedef struct _file_ref file_ref;
typedef struct _fcb fcb;
typedef struct { ERESOURCE load_tree_lock; } root_nonpaged;
typedef struct _root {
    uint64_t id, parent;
    int64_t lastinode;
    unsigned fcbs_version;
    struct btrfs_root_item root_item;
    LIST_ENTRY fcbs, list_entry;
    LIST_ENTRY* fcbs_ptrs[256];
    bool dropped;
    root_nonpaged* nonpaged;
} root;
typedef struct _dir_child {
    struct btrfs_key key;
    uint64_t index;
    uint64_t size;
    uint8_t type;
    bool root_dir;
    ANSI_STRING utf8;
    UNICODE_STRING name, name_uc;
    uint32_t hash, hash_uc;
    file_ref* fileref;
    LIST_ENTRY list_entry_index, list_entry_hash, list_entry_hash_uc;
} dir_child;
typedef struct _fcb_nonpaged { ERESOURCE resource, paging_resource, dir_children_lock; int HeaderMutex, segment_object; } fcb_nonpaged;
typedef struct { void* csum; LIST_ENTRY list_entry; } extent;
typedef struct { UNICODE_STRING name; ANSI_STRING utf8; LIST_ENTRY list_entry; } hardlink;
typedef struct { LIST_ENTRY list_entry; } xattr;
struct _fcb {
    device_extension* Vcb;
    root* subvol;
    uint8_t type;
    uint64_t inode;
    uint32_t hash, atts;
    int32_t refcount;
    struct btrfs_inode_item inode_item;
    bool created, sd_dirty, inode_item_changed, prop_compression_changed, atts_changed;
    bool ads, dirty, case_sensitive;
    int share_access;
    unsigned prop_compression, adshash, adsmaxlen;
    bool deleted;
    void* sd;
    fcb_nonpaged* nonpaged;
    int pool_type, lock, oplock;
    ANSI_STRING adsxattr, reparse_xattr, ea_xattr, adsdata;
    LIST_ENTRY extents, hardlinks, xattrs;
    file_ref* fileref;
    LIST_ENTRY list_entry, list_entry_all, dir_children_index, dir_children_hash, dir_children_hash_uc;
    LIST_ENTRY list_entry_dirty;
    LIST_ENTRY **hash_ptrs, **hash_ptrs_uc;
    struct host_header {
        ERESOURCE* Resource;
        ERESOURCE* PagingIoResource;
        unsigned NodeTypeCode, NodeByteSize;
        unsigned IsFastIoPossible, Flags2;
        bool contexts;
        LARGE_INTEGER AllocationSize, FileSize, ValidDataLength;
    } Header;
};
struct _file_ref {
    fcb* fcb;
    file_ref* parent;
    dir_child* dc;
    bool created, dirty, deleted;
    int32_t open_count;
    int32_t refcount;
    LIST_ENTRY children, list_entry;
    LIST_ENTRY list_entry_dirty;
    LIST_ENTRY list_entry_reap;
    ANSI_STRING oldutf8;
};
typedef struct {
    file_ref* fileref; ULONG access; bool user_set_change_time, user_set_write_time;
    unsigned NodeType, NodeSize, disposition, options, query_dir_offset;
    UNICODE_STRING query_string;
    bool has_wildcard, specific_file, case_sensitive, reserving, lxss;
} ccb;
typedef struct { fcb* FsContext; ccb* FsContext2; void* SectionObjectPointer; } FILE_OBJECT, *PFILE_OBJECT;
struct _device_extension {
    ERESOURCE fileref_lock, fcb_lock;
    ERESOURCE dirty_fcbs_lock, dirty_filerefs_lock;
    bool readonly;
    bool need_write;
    int32_t open_files;
    fcb* volume_fcb;
    fcb* dummy_fcb;
    struct { bool nodatacow; } options;
    file_ref* root_fileref;
    struct { uint64_t generation; uint32_t nodesize; } superblock;
    LIST_ENTRY all_fcbs, roots;
    LIST_ENTRY dirty_fcbs, dirty_filerefs;
    LOOKASIDE fcb_lookaside, fcb_np_lookaside, fileref_lookaside;
};
typedef struct { struct { struct btrfs_key key; uint32_t size; } *item; } traverse_ptr;
typedef struct { unsigned type; struct { fcb* parent; dir_child* dc; } dir_child; LIST_ENTRY list_entry; } rollback_item;
#define ROLLBACK_ADD_DIR_CHILD 1
typedef struct { int SubjectSecurityContext; } ACCESS_STATE;
typedef struct { ACCESS_STATE* AccessState; unsigned DesiredAccess; } SECURITY_CONTEXT;
typedef struct { unsigned Flags; PFILE_OBJECT FileObject; struct { struct { unsigned FileAttributes, EaLength, ShareAccess; SECURITY_CONTEXT* SecurityContext; } Create; } Parameters; } IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct { int RequestorMode; IO_STACK_LOCATION stack; struct { LARGE_INTEGER AllocationSize; } Overlay; struct { void* SystemBuffer; } AssociatedIrp; } IRP, *PIRP;
typedef struct { unsigned unused; } FILE_FULL_EA_INFORMATION;
#define IoGetCurrentIrpStackLocation(irp) (&(irp)->stack)
typedef int64_t LONGLONG, USN;
typedef struct { ULONG Data1; USHORT Data2, Data3; uint8_t Data4[8]; } GUID;
typedef struct { ULONG ReparseTag; USHORT ReparseDataLength, Reserved; } REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;
typedef struct { int unused; } ECP_LIST;
#define STATUS_CANNOT_DELETE ((int32_t)0xc0000121)
#define FILE_DELETE_ON_CLOSE 0x1000
#define BTRFS_NODE_TYPE_CCB 0x502
#define FILE_CS_FLAG_CASE_SENSITIVE_DIR 1
#define IO_REPARSE_TAG_SYMLINK 0xa000000c
#define SL_CASE_SENSITIVE 0x20
#define S_IFDIR __S_IFDIR
#define S_IFREG __S_IFREG
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_ATTRIBUTE_READONLY 1
#define FILE_DIRECTORY_FILE 1
#define SL_OPEN_PAGING_FILE 2
#define SL_FORCE_ACCESS_CHECK 4
#define SL_IGNORE_READONLY_ATTRIBUTE 8
#define FSRTL_FLAG2_IS_PAGING_FILE 1
#define UserMode 1
#define FILE_WRITE_DATA 2
#define FILE_NOTIFY_CHANGE_STREAM_NAME 0x200
#define FILE_ACTION_ADDED_STREAM 6
#define STATUS_DISK_FULL ((int32_t)0xc000007f)

#endif
