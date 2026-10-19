/* Regression for WinBtrfs mknod inode ownership. Real source routines are
 * extracted by check_mknod.py; Windows object/metadata services are mocked. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

#define _In_
#define _In_opt_
#define _In_reads_bytes_(n)
#define _Requires_exclusive_lock_held_(x)
#define __stdcall
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
#define PagedPool 1
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

typedef int32_t NTSTATUS;
typedef uint16_t WCHAR, USHORT;
typedef uint32_t ULONG;
typedef unsigned char BOOLEAN;
typedef void* PSID;
typedef struct { int RequestorMode; } IRP, *PIRP;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct { USHORT Length, MaximumLength; char* Buffer; } ANSI_STRING, *PANSI_STRING;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned held; } ERESOURCE;
typedef int SECURITY_SUBJECT_CONTEXT;
typedef struct _device_extension device_extension;
typedef struct _file_ref file_ref;
typedef struct _fcb fcb;
typedef struct _root {
    uint64_t id;
    int64_t lastinode;
    unsigned fcbs_version;
    struct btrfs_root_item root_item;
    LIST_ENTRY fcbs, list_entry;
    LIST_ENTRY* fcbs_ptrs[256];
} root;
typedef struct _dir_child {
    struct btrfs_key key;
    uint64_t index;
    uint8_t type;
    ANSI_STRING utf8;
    UNICODE_STRING name, name_uc;
    uint32_t hash, hash_uc;
    file_ref* fileref;
    LIST_ENTRY list_entry_index, list_entry_hash, list_entry_hash_uc;
} dir_child;
typedef struct { ERESOURCE dir_children_lock; } fcb_nonpaged;
struct _fcb {
    device_extension* Vcb;
    root* subvol;
    uint8_t type;
    uint64_t inode;
    uint32_t hash, atts;
    int32_t refcount;
    struct btrfs_inode_item inode_item;
    bool created, sd_dirty, inode_item_changed, prop_compression_changed, atts_changed;
    unsigned prop_compression;
    void* sd;
    fcb_nonpaged* nonpaged;
    file_ref* fileref;
    LIST_ENTRY list_entry, list_entry_all, dir_children_index, dir_children_hash, dir_children_hash_uc;
    LIST_ENTRY **hash_ptrs, **hash_ptrs_uc;
    struct {
        ERESOURCE* Resource;
        unsigned IsFastIoPossible;
        LARGE_INTEGER AllocationSize, FileSize, ValidDataLength;
    } Header;
};
struct _file_ref {
    fcb* fcb;
    file_ref* parent;
    dir_child* dc;
    bool created;
    int32_t refcount;
    LIST_ENTRY children, list_entry;
};
typedef struct { file_ref* fileref; ULONG access; bool user_set_change_time, user_set_write_time; } ccb;
typedef struct { fcb* FsContext; ccb* FsContext2; } FILE_OBJECT, *PFILE_OBJECT;
struct _device_extension {
    ERESOURCE fileref_lock, fcb_lock;
    bool readonly;
    fcb* volume_fcb;
    struct { uint64_t generation; } superblock;
    LIST_ENTRY all_fcbs, roots;
};
typedef struct { struct { struct btrfs_key key; } *item; } traverse_ptr;
typedef struct { unsigned type; struct { fcb* parent; dir_child* dc; } dir_child; LIST_ENTRY list_entry; } rollback_item;
#define ROLLBACK_ADD_DIR_CHILD 1
#include "protocol.inc"
typedef struct { uint64_t inode; uint8_t type; uint64_t rdev; USHORT namelen; WCHAR name[16]; } request_buffer;
_Static_assert(offsetof(btrfs_mknod, name) == offsetof(request_buffer, name), "wire request layout");

static const char* scenario;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",scenario,__LINE__,#c); exit(1); } } while (0)
enum fault_kind { NONE, NAME_INVALID, SECURITY, FCB_ALLOC, FILEREF_ALLOC, POOL_ALLOC, UPPERCASE, TREE_ERROR };
typedef struct {
    const char* name;
    uint64_t inode;
    uint8_t type;
    int64_t lastinode;
    enum fault_kind fault;
    const char* allocator;
    unsigned allocation_index;
    NTSTATUS expected;
    bool disk_collision, cache_collision, other_root_collision, name_collision, deny_privilege, deny_access;
} test_case;
static const test_case* current;
static device_extension volume;
static root parent_root, other_root;
static fcb parent_fcb, child_fcb, occupied_fcb;
static fcb_nonpaged parent_np, child_np;
static ERESOURCE parent_resource, child_resource;
static file_ref parent_ref, child_ref;
static ccb parent_ccb;
static FILE_OBJECT parent_file;
static IRP request_irp = { .RequestorMode = 1 };
static dir_child name_collision;
static LIST_ENTRY *parent_hash[256], *parent_hash_uc[256];
static unsigned faults, fcb_calls, reaps, fileref_calls, tree_calls, notifications, privilege_calls;
static unsigned mknod_allocs, child_allocs;
static bool inserted;
static struct { void* p; size_t size; bool live, temporary; } pools[32];
static unsigned pool_count;

static void InitializeListHead(LIST_ENTRY* list) { list->Flink = list->Blink = list; }
static bool IsListEmpty(LIST_ENTRY* list) { return list->Flink == list; }
static void InsertHeadList(LIST_ENTRY* list, LIST_ENTRY* entry) {
    entry->Flink=list->Flink; entry->Blink=list; list->Flink->Blink=entry; list->Flink=entry;
}
static void InsertTailList(LIST_ENTRY* list, LIST_ENTRY* entry) { InsertHeadList(list->Blink,entry); }
static bool hit(enum fault_kind kind) { if (current->fault != kind) return false; faults++; return true; }
static bool ExAcquireResourceExclusiveLite(ERESOURCE* lock, bool wait) { CHECK(wait); lock->held++; return true; }
static bool ExAcquireResourceSharedLite(ERESOURCE* lock, bool wait) { return ExAcquireResourceExclusiveLite(lock,wait); }
static void ExReleaseResourceLite(ERESOURCE* lock) { CHECK(lock->held); lock->held--; }
static bool ExIsResourceAcquiredExclusive(ERESOURCE* lock) { return lock->held != 0; }
static bool ExIsResourceAcquiredSharedLite(ERESOURCE* lock) { return lock->held != 0; }
static void* allocate(size_t size, const char* caller) {
    unsigned index = !strcmp(caller,"mknod") ? ++mknod_allocs : !strcmp(caller,"add_dir_child") ? ++child_allocs : 0;
    if (current->fault == POOL_ALLOC && !strcmp(caller,current->allocator) && index == current->allocation_index) {
        faults++; return NULL;
    }
    CHECK(pool_count < 32);
    void* p=calloc(1,size);
    CHECK(p);
    pools[pool_count].p=p; pools[pool_count].size=size; pools[pool_count].live=true;
    pools[pool_count++].temporary=!strcmp(caller,"mknod") && index == 1;
    return p;
}
#define ExAllocatePoolWithTag(p,n,t) allocate(n,__func__)
static void ExFreePool(void* p) {
    for (unsigned i=0;i<pool_count;i++) if (pools[i].p == p) {
        CHECK(pools[i].live); pools[i].live=false; memset(p,0xdd,pools[i].size); return;
    }
    CHECK(false);
}
static int32_t InterlockedIncrement(int32_t* n) { return ++*n; }
static int64_t InterlockedIncrement64(int64_t* n) { return ++*n; }
static bool is_subvol_readonly(root* r, PIRP irp) { CHECK(r == &parent_root && irp == &request_irp); return false; }
static unsigned RtlConvertLongToLuid(unsigned value) { return value; }
static bool SeSinglePrivilegeCheck(unsigned privilege, int mode) {
    CHECK(privilege == SE_MANAGE_VOLUME_PRIVILEGE && mode == 1); privilege_calls++; return !current->deny_privilege;
}
static NTSTATUS check_file_name_valid(PUNICODE_STRING name, bool posix, bool stream) {
    CHECK(name && !posix && !stream); return hit(NAME_INVALID) ? STATUS_OBJECT_NAME_INVALID : STATUS_SUCCESS;
}
static NTSTATUS utf16_to_utf8(char* out, ULONG capacity, ULONG* len, const WCHAR* in, USHORT bytes) {
    *len=bytes/sizeof(WCHAR); CHECK(!out || capacity >= *len);
    if (out) for (ULONG i=0;i<*len;i++) out[i]=(char)in[i];
    return STATUS_SUCCESS;
}
static NTSTATUS RtlUpcaseUnicodeString(PUNICODE_STRING out, const UNICODE_STRING* in, bool own) {
    CHECK(own); if (hit(UPPERCASE)) return STATUS_INSUFFICIENT_RESOURCES;
    *out=*in; out->Buffer=allocate(in->Length,"uppercase"); memcpy(out->Buffer,in->Buffer,in->Length); return STATUS_SUCCESS;
}
static size_t RtlCompareMemory(const void* a, const void* b, size_t size) { return memcmp(a,b,size) == 0 ? size : 0; }
static void insert_dir_child_into_hash_lists(fcb* parent, dir_child* dc) {
    CHECK(parent == &parent_fcb);
    InsertTailList(&parent->dir_children_hash,&dc->list_entry_hash);
    parent->hash_ptrs[dc->hash >> 24]=&dc->list_entry_hash;
}
static void KeQuerySystemTime(LARGE_INTEGER* time) { time->QuadPart=123; }
static void win_time_to_unix(LARGE_INTEGER time, struct btrfs_timespec* now) { (void)time; memset(now,0,sizeof(*now)); }
static fcb* create_fcb(device_extension* vcb, int pool) { CHECK(vcb == &volume && pool == PagedPool); fcb_calls++; return hit(FCB_ALLOC) ? NULL : &child_fcb; }
static void reap_fcb(fcb* child) {
    CHECK(child == &child_fcb && reaps++ == 0);
    if (child->hash_ptrs) ExFreePool(child->hash_ptrs);
    if (child->hash_ptrs_uc) ExFreePool(child->hash_ptrs_uc);
}
static file_ref* create_fileref(device_extension* vcb) { CHECK(vcb == &volume); fileref_calls++; return hit(FILEREF_ALLOC) ? NULL : &child_ref; }
static void free_fileref(file_ref* ref) { CHECK(ref == &child_ref); reap_fcb(ref->fcb); }
static uint32_t inherit_mode(fcb* parent, bool directory) { CHECK(parent == &parent_fcb); (void)directory; return 0755; }
static unsigned fast_io_possible(fcb* child) { CHECK(child == &child_fcb); return 1; }
static void SeCaptureSubjectContext(SECURITY_SUBJECT_CONTEXT* context) { *context=1; }
static void SeReleaseSubjectContext(SECURITY_SUBJECT_CONTEXT* context) { CHECK(*context == 1); *context=0; }
static void* IoGetFileObjectGenericMapping(void) { return NULL; }
static NTSTATUS SeAssignSecurityEx(void* parent, void* explicit_sd, void** out, void* guid, bool directory,
                                    unsigned flags, SECURITY_SUBJECT_CONTEXT* context, void* mapping, int pool) {
    (void)parent; (void)directory;
    CHECK(!explicit_sd && !guid && flags == SEF_SACL_AUTO_INHERIT && *context == 1 && !mapping && pool == PagedPool);
    if (hit(SECURITY)) return STATUS_ACCESS_DENIED;
    *out=&child_fcb; return STATUS_SUCCESS;
}
static NTSTATUS RtlGetOwnerSecurityDescriptor(void* sd, PSID* owner, BOOLEAN* defaulted) { CHECK(sd); *owner=&child_fcb; *defaulted=false; return STATUS_SUCCESS; }
static uint32_t sid_to_uid(PSID owner) { CHECK(owner == &child_fcb); return 1000; }
static void find_gid(fcb* child, fcb* parent, SECURITY_SUBJECT_CONTEXT* context) { CHECK(child == &child_fcb && parent == &parent_fcb && *context == 1); child->inode_item.gid=1000; }
static void acquire_fcb_lock_exclusive(device_extension* vcb) { ExAcquireResourceExclusiveLite(&vcb->fcb_lock,true); }
static void release_fcb_lock(device_extension* vcb) { ExReleaseResourceLite(&vcb->fcb_lock); }
static void increase_fileref_refcount(file_ref* ref) { ref->refcount++; }
static void mark_fcb_dirty(fcb* child) { CHECK(child == &parent_fcb || child == &child_fcb); }
static void mark_fileref_dirty(file_ref* ref) { CHECK(ref == &child_ref); }
static void send_notification_fileref(file_ref* ref, ULONG filter, ULONG action, PUNICODE_STRING stream) {
    CHECK(ref == &child_ref && (filter == FILE_NOTIFY_CHANGE_FILE_NAME || filter == FILE_NOTIFY_CHANGE_DIR_NAME) && action == FILE_ACTION_ADDED && !stream); notifications++;
}
static void queue_notification_fcb(file_ref* ref, ULONG filter, ULONG action, PUNICODE_STRING stream) {
    CHECK(ref == &parent_ref && filter == FILE_NOTIFY_CHANGE_LAST_WRITE && action == FILE_ACTION_MODIFIED && !stream); notifications++;
}
static NTSTATUS find_item(device_extension* vcb, root* r, traverse_ptr* tp, struct btrfs_key* key, bool exact, PIRP irp) {
    CHECK(vcb == &volume && r == &parent_root && !exact && irp == &request_irp);
    CHECK(key->objectid == current->inode && key->type == BTRFS_INODE_ITEM_KEY);
    tree_calls++;
    if (hit(TREE_ERROR)) return STATUS_IO_DEVICE_ERROR;
    static struct { struct btrfs_key key; } item;
    item.key=*key;
    if (!current->disk_collision) item.key.objectid--;
    tp->item=(void*)&item;
    return STATUS_SUCCESS;
}

uint32_t calc_crc32c_sw(uint32_t, uint8_t*, uint32_t);
static uint32_t (*calc_crc32c)(uint32_t,uint8_t*,uint32_t)=calc_crc32c_sw;
#include "crc32c.inc"
#include "find_file_in_dir.inc"
#include "check_inode_used.inc"
#include "add_dir_child.inc"
#include "add_fcb_to_subvol.inc"
#include "mknod.inc"

static void setup(void) {
    parent_fcb=(fcb){.Vcb=&volume,.subvol=&parent_root,.type=BTRFS_FT_DIR,.refcount=1,.nonpaged=&parent_np,.hash_ptrs=parent_hash,.hash_ptrs_uc=parent_hash_uc};
    parent_fcb.Header.Resource=&parent_resource;
    child_fcb.nonpaged=&child_np; child_fcb.Header.Resource=&child_resource;
    parent_root.id=5; parent_root.lastinode=current->lastinode ? current->lastinode : 1000;
    other_root.id=6; other_root.lastinode=2000;
    InitializeListHead(&parent_root.fcbs); InitializeListHead(&other_root.fcbs);
    InitializeListHead(&parent_fcb.dir_children_index); InitializeListHead(&parent_fcb.dir_children_hash); InitializeListHead(&parent_fcb.dir_children_hash_uc);
    parent_ref.fcb=&parent_fcb; parent_ref.refcount=1; child_ref.refcount=1;
    InitializeListHead(&parent_ref.children); InitializeListHead(&child_ref.children);
    parent_ccb.fileref=&parent_ref; parent_ccb.access=current->deny_access ? 0 : FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY;
    parent_file=(FILE_OBJECT){&parent_fcb,&parent_ccb};
    volume.superblock.generation=42;
    InitializeListHead(&volume.all_fcbs); InitializeListHead(&volume.roots);
    if (current->cache_collision || current->other_root_collision) {
        occupied_fcb.inode=current->inode;
        occupied_fcb.hash=calc_crc32c(0xffffffff,(uint8_t*)&occupied_fcb.inode,sizeof(uint64_t));
        occupied_fcb.subvol=current->other_root_collision ? &other_root : &parent_root;
        add_fcb_to_subvol(&occupied_fcb);
    }
    if (current->name_collision) {
        static WCHAR name[]={'a','l','p','h','a'};
        name_collision.name=(UNICODE_STRING){10,10,name}; name_collision.key.objectid=777;
        name_collision.key.type=BTRFS_INODE_ITEM_KEY;
        name_collision.hash=calc_crc32c(0xffffffff,(uint8_t*)name,sizeof(name));
        InsertTailList(&parent_fcb.dir_children_hash,&name_collision.list_entry_hash);
        parent_hash[name_collision.hash >> 24]=&name_collision.list_entry_hash;
    }
}
static void run(void) {
    setup();
    if (!strcmp(scenario,"lookup-not-found")) {
        WCHAR chars[]={'a','l','p','h','a'};
        UNICODE_STRING name={sizeof(chars),sizeof(chars),chars};
        root* found_root=&other_root;
        uint64_t found_inode=0x11223344;
        dir_child* found_child=&name_collision;
        CHECK(find_file_in_dir(&name,&parent_fcb,&found_root,&found_inode,&found_child,true) == STATUS_OBJECT_NAME_NOT_FOUND);
        CHECK(found_root == &other_root && found_inode == 0x11223344 && found_child == &name_collision);
        CHECK(!parent_np.dir_children_lock.held);
        printf("PASS %s\n",scenario);
        return;
    }
    request_buffer request={.inode=current->inode,.type=current->type,.rdev=0x12345,.namelen=10,.name={'a','l','p','h','a'}};
    NTSTATUS status=mknod(&volume,&parent_file,&request,sizeof(request),&request_irp);
    CHECK(status == current->expected);
    CHECK(faults == (current->fault == NONE ? 0u : 1u));
    CHECK(privilege_calls == (current->inode != 0 && !current->deny_access ? 1u : 0u));
    CHECK(!volume.fcb_lock.held && !volume.fileref_lock.held && !parent_resource.held && !parent_np.dir_children_lock.held);
    if (NT_SUCCESS(status)) {
        uint64_t previous=current->lastinode ? (uint64_t)current->lastinode : 1000;
        uint64_t expected=current->inode ? current->inode : previous + 1;
        if (child_fcb.inode != expected)
            fprintf(stderr,"%s: inode=%#llx, expected=%#llx\n",scenario,(unsigned long long)child_fcb.inode,(unsigned long long)expected);
        CHECK(child_fcb.inode == expected);
        CHECK(child_fcb.hash == calc_crc32c(0xffffffff,(uint8_t*)&child_fcb.inode,sizeof(uint64_t)));
        CHECK(child_fcb.subvol == &parent_root);
        CHECK(parent_root.lastinode == (int64_t)(expected > previous ? expected : previous));
        CHECK(child_ref.dc && child_ref.dc->key.objectid == expected && child_ref.dc->key.type == BTRFS_INODE_ITEM_KEY);
        CHECK(parent_root.fcbs_ptrs[child_fcb.hash >> 24] == &child_fcb.list_entry);
        CHECK(check_inode_used(&volume,&parent_root,expected,child_fcb.hash,&request_irp) == STATUS_SUCCESS);
        CHECK(child_fcb.created && child_ref.created && child_fcb.type == current->type && notifications == 2);
        CHECK(child_ref.dc->utf8.Length == 5 && !memcmp(child_ref.dc->utf8.Buffer,"alpha",5));
        CHECK(parent_fcb.inode_item.size == 10 && parent_root.fcbs_version == 1 && reaps == 0);
        inserted=true;
    } else {
        CHECK(notifications == 0 && IsListEmpty(&parent_ref.children) && IsListEmpty(&volume.all_fcbs));
        /* Existing bug outside inode initialization: mknod does not reap its
         * new FCB on an explicit-inode rejection or metadata lookup error.
         * Keep this visible rather than treating fixture cleanup as a fix. */
        bool known_unreaped=current->cache_collision || current->disk_collision || current->fault == TREE_ERROR;
        CHECK(reaps == (fcb_calls && current->fault != FCB_ALLOC && !known_unreaped ? 1u : 0u));
        if (known_unreaped) CHECK(fcb_calls == 1);
    }
    CHECK(tree_calls == (current->inode && current->inode <= (uint64_t)(current->lastinode ? current->lastinode : 1000) && !current->deny_privilege && !current->deny_access &&
                         !current->cache_collision && current->fault != NAME_INVALID && current->fault != SECURITY &&
                         current->fault != FCB_ALLOC && !current->name_collision ? 1u : 0u));
    for (unsigned i=0;i<pool_count;i++) {
        if (pools[i].temporary) CHECK(!pools[i].live);
        if (!inserted) CHECK(!pools[i].live);
        free(pools[i].p);
    }
    printf("PASS %s\n",scenario);
}
int main(int argc, char** argv) {
    static const test_case cases[]={
        {.name="lookup-not-found",.type=BTRFS_FT_REG_FILE},
        {.name="auto-regular",.type=BTRFS_FT_REG_FILE},
        {.name="auto-directory",.type=BTRFS_FT_DIR},
        {.name="auto-character",.type=BTRFS_FT_CHRDEV},
        {.name="auto-block",.type=BTRFS_FT_BLKDEV},
        {.name="auto-fifo",.type=BTRFS_FT_FIFO},
        {.name="auto-socket",.type=BTRFS_FT_SOCK},
        {.name="auto-symlink",.type=BTRFS_FT_SYMLINK},
        {.name="auto-wide-inode",.type=BTRFS_FT_REG_FILE,.lastinode=0x10000000000},
        {.name="explicit-above-last",.inode=2000,.type=BTRFS_FT_REG_FILE},
        {.name="explicit-below-last",.inode=500,.type=BTRFS_FT_REG_FILE},
        {.name="explicit-equal-last",.inode=1000,.type=BTRFS_FT_REG_FILE},
        {.name="explicit-wide-inode",.inode=0x10000000001,.lastinode=0x10000000010,.type=BTRFS_FT_REG_FILE},
        {.name="explicit-directory",.inode=500,.type=BTRFS_FT_DIR},
        {.name="explicit-other-root",.inode=500,.type=BTRFS_FT_REG_FILE,.other_root_collision=true},
        {.name="explicit-cache-collision",.inode=500,.type=BTRFS_FT_REG_FILE,.cache_collision=true,.expected=STATUS_INVALID_PARAMETER},
        {.name="explicit-disk-collision",.inode=500,.type=BTRFS_FT_REG_FILE,.disk_collision=true,.expected=STATUS_INVALID_PARAMETER},
        {.name="explicit-tree-error",.inode=500,.type=BTRFS_FT_REG_FILE,.fault=TREE_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="explicit-denied",.inode=500,.type=BTRFS_FT_REG_FILE,.deny_privilege=true,.expected=STATUS_PRIVILEGE_NOT_HELD},
        {.name="parent-access-denied",.type=BTRFS_FT_DIR,.deny_access=true,.expected=STATUS_ACCESS_DENIED},
        {.name="name-collision",.type=BTRFS_FT_REG_FILE,.name_collision=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="invalid-name",.type=BTRFS_FT_REG_FILE,.fault=NAME_INVALID,.expected=STATUS_OBJECT_NAME_INVALID},
        {.name="temporary-allocation",.type=BTRFS_FT_REG_FILE,.fault=POOL_ALLOC,.allocator="mknod",.allocation_index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="fcb-allocation",.type=BTRFS_FT_REG_FILE,.fault=FCB_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="security-failure",.type=BTRFS_FT_REG_FILE,.fault=SECURITY,.expected=STATUS_ACCESS_DENIED},
        {.name="directory-hash-allocation",.type=BTRFS_FT_DIR,.fault=POOL_ALLOC,.allocator="mknod",.allocation_index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="directory-uppercase-hash-allocation",.type=BTRFS_FT_DIR,.fault=POOL_ALLOC,.allocator="mknod",.allocation_index=3,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="fileref-allocation",.type=BTRFS_FT_REG_FILE,.fault=FILEREF_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="child-allocation",.type=BTRFS_FT_REG_FILE,.fault=POOL_ALLOC,.allocator="add_dir_child",.allocation_index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="child-name-allocation",.type=BTRFS_FT_REG_FILE,.fault=POOL_ALLOC,.allocator="add_dir_child",.allocation_index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="child-unicode-allocation",.type=BTRFS_FT_REG_FILE,.fault=POOL_ALLOC,.allocator="add_dir_child",.allocation_index=3,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="uppercase-failure",.type=BTRFS_FT_REG_FILE,.fault=UPPERCASE,.expected=STATUS_INSUFFICIENT_RESOURCES},
    };
    if (argc == 2 && !strcmp(argv[1],"--list")) {
        for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) puts(cases[i].name);
        return 0;
    }
    if (argc != 2) return 2;
    scenario=argv[1];
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) if (!strcmp(scenario,cases[i].name)) {
        current=&cases[i]; run(); return 0;
    }
    return 2;
}
