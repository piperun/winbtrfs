/* Native Windows service boundary for the production hard-link queries. */
#ifndef HARDLINK_TEST_HOST_H
#define HARDLINK_TEST_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG;
typedef uint16_t WCHAR;
typedef struct { unsigned char Identifier[16]; } FILE_ID_128;
typedef struct { uint16_t Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned held; } ERESOURCE;
typedef struct { uint64_t id; } root;
typedef struct device_extension device_extension;
typedef struct {
    device_extension* Vcb;
    root* subvol;
    uint64_t inode;
    bool ads;
    LIST_ENTRY hardlinks;
    struct { ERESOURCE* Resource; } Header;
} fcb;
typedef struct { uint64_t index; UNICODE_STRING name; } dir_child;
typedef struct file_ref {
    fcb* fcb;
    struct file_ref* parent;
    dir_child* dc;
    bool deleted;
    LONG refcount;
    LIST_ENTRY children, list_entry;
} file_ref;
struct device_extension { ERESOURCE fileref_lock; file_ref* root_fileref; };
typedef struct { LIST_ENTRY list_entry; uint64_t parent, index; UNICODE_STRING name; } hardlink;
typedef struct { unsigned unused; } IRP, *PIRP;
typedef struct {
    ULONG NextEntryOffset;
    int64_t ParentFileId;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_LINK_ENTRY_INFORMATION;
typedef struct {
    ULONG BytesNeeded, EntriesReturned;
    FILE_LINK_ENTRY_INFORMATION Entry;
} FILE_LINKS_INFORMATION;
_Static_assert(sizeof(FILE_LINK_ENTRY_INFORMATION) == 24, "Windows x64 link entry size");
_Static_assert(offsetof(FILE_LINK_ENTRY_INFORMATION, ParentFileId) == 8, "Windows x64 file ID offset");
_Static_assert(offsetof(FILE_LINK_ENTRY_INFORMATION, FileName) == 20, "Windows x64 name offset");
_Static_assert(offsetof(FILE_LINKS_INFORMATION, Entry) == 8, "Windows link list header");

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define NT_SUCCESS(s) ((s) >= 0)
#define BTRFS_FIRST_FREE_OBJECTID 256
#define _Inout_
#define TRACE(...) ((void)0)
#define ERR(...) debug_log(__VA_ARGS__)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define sector_align(n,a) (((n) + (a) - 1) & ~((a) - 1))
static void debug_log(const char* format, ...) { (void)format; }
static void InitializeListHead(LIST_ENTRY* head) { head->Flink = head->Blink = head; }
static bool IsListEmpty(const LIST_ENTRY* head) { return head->Flink == head; }
static void InsertTailList(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Blink = head->Blink;
    entry->Flink = head;
    head->Blink->Flink = entry;
    head->Blink = entry;
}
#endif
