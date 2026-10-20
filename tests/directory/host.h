/* Host boundary for the unchanged production directory query functions. */
#ifndef DIRECTORY_TEST_HOST_H
#define DIRECTORY_TEST_HOST_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t NTSTATUS,LONG; typedef uint32_t ULONG; typedef __WCHAR_TYPE__ WCHAR; typedef int8_t CCHAR;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { unsigned char Identifier[16]; } FILE_ID_128;
typedef struct { uint16_t Length,MaximumLength; WCHAR* Buffer; } UNICODE_STRING;
typedef struct LIST_ENTRY { struct LIST_ENTRY* Flink; struct LIST_ENTRY* Blink; } LIST_ENTRY;
struct btrfs_key {uint64_t objectid, offset; unsigned type;};
struct btrfs_inode_item {uint64_t size,nbytes,otime,atime,mtime,ctime;};
typedef struct root {uint64_t id,parent; LIST_ENTRY list_entry;} root;
typedef struct device_extension device_extension;
typedef struct {unsigned unused;} ERESOURCE;
typedef struct {ERESOURCE dir_children_lock;} fcb_nonpaged;
typedef struct fcb {uint64_t inode; root* subvol; device_extension* Vcb; struct btrfs_inode_item inode_item; ULONG atts,ealen; fcb_nonpaged* nonpaged; LIST_ENTRY dir_children_index,dir_children_hash,dir_children_hash_uc; LIST_ENTRY* hash_ptrs[256];LIST_ENTRY* hash_ptrs_uc[256];} fcb;
typedef struct file_ref {struct file_ref* parent; fcb* fcb;} file_ref;
typedef struct dir_child {file_ref* fileref; bool root_dir; LIST_ENTRY list_entry_index,list_entry_hash,list_entry_hash_uc; uint64_t index; struct btrfs_key key; UNICODE_STRING name,name_uc; uint8_t type;uint32_t hash,hash_uc;} dir_child;
typedef struct {file_ref* fileref; bool lxss,case_sensitive,has_wildcard,specific_file; ULONG access; uint64_t query_dir_offset; UNICODE_STRING query_string;} ccb;
struct device_extension { ERESOURCE tree_lock; LIST_ENTRY roots; fcb* dummy_fcb; struct {uint32_t sectorsize;} superblock;};
enum _FILE_INFORMATION_CLASS { FileBothDirectoryInformation=3,FileDirectoryInformation=1,FileFullDirectoryInformation=2,FileIdBothDirectoryInformation=37,FileIdFullDirectoryInformation=38,FileNamesInformation=12,FileIdExtdDirectoryInformation=60,FileIdExtdBothDirectoryInformation=63};
typedef struct {void* FsContext;void* FsContext2;} FILE_OBJECT;
typedef struct { struct {struct {enum _FILE_INFORMATION_CLASS FileInformationClass; UNICODE_STRING* FileName;ULONG Length;} QueryDirectory;} Parameters; FILE_OBJECT* FileObject;ULONG Flags;} IO_STACK_LOCATION,*PIO_STACK_LOCATION;
typedef struct {IO_STACK_LOCATION stack;int RequestorMode;void* MdlAddress;void* buffer;struct {uintptr_t Information;} IoStatus;} IRP,*PIRP;
#include "types.inc"
#include "entry.inc"
#define DIRECTORY_FIELDS ULONG NextEntryOffset,FileIndex; LARGE_INTEGER CreationTime,LastAccessTime,LastWriteTime,ChangeTime,EndOfFile,AllocationSize; ULONG FileAttributes,FileNameLength

typedef struct {DIRECTORY_FIELDS; WCHAR FileName[1];} FILE_DIRECTORY_INFORMATION;
typedef struct {DIRECTORY_FIELDS; ULONG EaSize; WCHAR FileName[1];} FILE_FULL_DIR_INFORMATION;
typedef struct {DIRECTORY_FIELDS; ULONG EaSize; CCHAR ShortNameLength; WCHAR ShortName[12]; WCHAR FileName[1];} FILE_BOTH_DIR_INFORMATION;
typedef struct {DIRECTORY_FIELDS; ULONG EaSize; CCHAR ShortNameLength; WCHAR ShortName[12]; LARGE_INTEGER FileId; WCHAR FileName[1];} FILE_ID_BOTH_DIR_INFORMATION;
typedef struct {DIRECTORY_FIELDS; ULONG EaSize; LARGE_INTEGER FileId; WCHAR FileName[1];} FILE_ID_FULL_DIR_INFORMATION;
typedef struct {ULONG NextEntryOffset,FileIndex,FileNameLength;WCHAR FileName[1];} FILE_NAMES_INFORMATION;
_Static_assert(offsetof(FILE_DIRECTORY_INFORMATION,FileName)==64,"Windows directory header");
_Static_assert(offsetof(FILE_BOTH_DIR_INFORMATION,FileName)==94,"Windows both header");
_Static_assert(offsetof(FILE_ID_BOTH_DIR_INFORMATION,FileName)==104,"Windows ID both header");
_Static_assert(offsetof(FILE_ID_FULL_DIR_INFORMATION,FileName)==80,"Windows ID full header");
_Static_assert(offsetof(FILE_ID_EXTD_DIR_INFORMATION,FileName)==88,"Windows extended header");
_Static_assert(offsetof(FILE_ID_EXTD_BOTH_DIR_INFORMATION,FileName)==114,"Windows extended both header");
typedef struct {struct {struct btrfs_key key; unsigned size;void* data;} *item;} traverse_ptr;
#define STATUS_SUCCESS 0
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xc0000002)
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006)
#define NT_SUCCESS(s) ((s)>=0)
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_SPARSE_FILE 0x200
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define BTRFS_FT_DIR 2
#define BTRFS_FT_SYMLINK 7
#define BTRFS_ROOT_ITEM_KEY 132
#define BTRFS_INODE_ITEM_KEY 1
#define BTRFS_FIRST_FREE_OBJECTID 256
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
static void trace(const char* format, ...){(void)format;}
#define TRACE(...) trace(__VA_ARGS__)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset(p,0,n)
#define min(a,b) ((a)<(b)?(a):(b))
#define sector_align(n,a) (((n)+(a)-1)&~((uint64_t)(a)-1))
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP i){return &i->stack;}
static void KeQuerySystemTime(LARGE_INTEGER* t){t->QuadPart=0;}
static void win_time_to_unix(LARGE_INTEGER t,uint64_t* x){*x=t.QuadPart;}
static uint64_t unix_time_to_win(const uint64_t* t){return *t;}
static NTSTATUS find_item(device_extension* v,root* r,traverse_ptr* t,struct btrfs_key* k,bool b,PIRP i){(void)v;(void)r;(void)t;(void)k;(void)b;(void)i;abort();}
static ULONG get_file_attributes(device_extension* v,root* r,uint64_t n,uint8_t t,bool d,bool b,PIRP i){(void)v;(void)r;(void)n;(void)t;(void)d;(void)b;(void)i;abort();}
static ULONG get_ea_len(device_extension* v,root* r,uint64_t n,PIRP i){(void)v;(void)r;(void)n;(void)i;abort();}
static ULONG get_reparse_tag(device_extension* v,root* r,uint64_t n,uint8_t t,ULONG a,bool b,PIRP i){(void)v;(void)r;(void)n;(void)t;(void)a;(void)b;(void)i;return 0;}
static uint64_t make_file_id(root* r,uint64_t n){return n^r->id;}

#endif
