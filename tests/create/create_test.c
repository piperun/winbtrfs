/* Native ownership regression: production creation, dirty insertion and
 * reaping routines, with explicit host models for kernel services. */
#include "host.h"

static const char* scenario;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",scenario,__LINE__,#c); exit(1); } } while (0)
static void host_log(const char* format, ...) { (void)format; }
enum fault_kind { NONE, POOL, FCB_ALLOC, NP_ALLOC, FILEREF_ALLOC, SECURITY, EA_ERROR,
                  EXTEND_ERROR, UTF8_ERROR, UPPERCASE_ERROR, TREE_ERROR, ACCESS_ERROR,
                  OPEN_ERROR, NAME_ERROR, REOPEN_ERROR };
typedef struct {
    const char* name;
    bool stream, missing_base, directory, paging, insensitive, preallocate, existing_entry, full, readonly;
    enum fault_kind fault;
    const char* allocator;
    unsigned index;
    NTSTATUS expected;
    const WCHAR *existing_name, *requested_name;
    bool hash_collision, reaped_entry, existing_directory, install_during_open;
} test_case;
static const test_case* current;
static device_extension volume;
static root subvol;
static fcb directory_fcb, base_fcb;
static fcb_nonpaged directory_np, base_np;
static file_ref directory_ref, base_ref;
static dir_child collision_dc;
static WCHAR *cached_name, *cached_name_uc;
static LIST_ENTRY *directory_hash[256], *directory_hash_uc[256], *base_hash[256], *base_hash_uc[256];
static fcb fcbs[4];
static fcb_nonpaged nonpaged[4];
static file_ref refs[4];
static bool leases[3][4];
static unsigned faults, fault_attempts, utf_calls, uppercase_calls, destroyed, created_resources, notifications;
static bool have_base, preparing;
static unsigned reopen_calls;
static file_ref* installed_ref;
static ACCESS_STATE access_state;
static SECURITY_CONTEXT security_context={&access_state};
static IRP irp={.RequestorMode=UserMode};
static struct { void* p; size_t size; bool live; } pools[64];
static unsigned pool_count;
static struct { const char* caller; unsigned calls; } allocators[16];
static unsigned allocator_count;

static void InitializeListHead(LIST_ENTRY* list) { list->Flink=list->Blink=list; }
static bool IsListEmpty(LIST_ENTRY* list) { return list->Flink == list; }
static void InsertHeadList(LIST_ENTRY* list, LIST_ENTRY* entry) {
    entry->Flink=list->Flink; entry->Blink=list; list->Flink->Blink=entry; list->Flink=entry;
}
static void InsertTailList(LIST_ENTRY* list, LIST_ENTRY* entry) { InsertHeadList(list->Blink,entry); }
static void RemoveEntryList(LIST_ENTRY* entry) { entry->Blink->Flink=entry->Flink; entry->Flink->Blink=entry->Blink; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* list) { LIST_ENTRY* entry=list->Flink; RemoveEntryList(entry); return entry; }
static bool hit(enum fault_kind kind) {
    if (preparing || current->fault != kind || ++fault_attempts != (current->index ? current->index : 1)) return false;
    faults++; return true;
}
static bool ExAcquireResourceExclusiveLite(ERESOURCE* lock, bool wait) { CHECK(wait); lock->held++; return true; }
static bool ExAcquireResourceSharedLite(ERESOURCE* lock, bool wait) { return ExAcquireResourceExclusiveLite(lock,wait); }
static void ExReleaseResourceLite(ERESOURCE* lock) { CHECK(lock->held); lock->held--; }
static bool ExIsResourceAcquiredExclusive(ERESOURCE* lock) { return lock->held != 0; }
static bool ExIsResourceAcquiredSharedLite(ERESOURCE* lock) { return lock->held != 0; }
static int32_t InterlockedIncrement(int32_t* n) { return ++*n; }
static int32_t InterlockedDecrement(int32_t* n) { CHECK(*n > 0); return --*n; }
static int64_t InterlockedIncrement64(int64_t* n) { return ++*n; }
static void increase_fileref_refcount(file_ref* ref) { InterlockedIncrement(&ref->refcount); }
#ifdef DEBUG_FCB_REFCOUNTS
#define free_fcb(f) _free_fcb(f,__func__)
#endif
#include "free_fcb.inc"
#include "free_fileref.inc"
#include "mark_fcb_dirty.inc"
#include "mark_fileref_dirty.inc"

static void* allocate(size_t size, const char* caller) {
    unsigned i;
    for (i=0;i<allocator_count;i++) if (!strcmp(allocators[i].caller,caller)) break;
    if (i == allocator_count) {
        CHECK(allocator_count < 16); allocators[allocator_count++].caller=caller;
    }
    unsigned count=++allocators[i].calls;
    if (current->fault == POOL && !strcmp(current->allocator,caller) && count == current->index) {
        faults++; return NULL;
    }
    if (!strcmp(caller,"create_fcb") && hit(FCB_ALLOC)) return NULL;
    CHECK(pool_count < 64);
    void* p=calloc(1,size); CHECK(p);
    pools[pool_count].p=p; pools[pool_count].size=size; pools[pool_count++].live=true;
    return p;
}
#define ExAllocatePoolWithTag(p,n,t) allocate(n,__func__)
static void ExFreePool(void* p) {
    CHECK(p);
    for (unsigned i=0;i<pool_count;i++) if (pools[i].p == p) {
        CHECK(pools[i].live); pools[i].live=false; memset(p,0xdd,pools[i].size); return;
    }
    CHECK(false);
}
static void* object(unsigned kind, unsigned i) {
    return kind == 0 ? (void*)&fcbs[i] : kind == 1 ? (void*)&nonpaged[i] : (void*)&refs[i];
}
static void* lookaside_allocate(LOOKASIDE* list) {
    enum fault_kind fault=list->kind == 0 ? FCB_ALLOC : list->kind == 1 ? NP_ALLOC : FILEREF_ALLOC;
    if (hit(fault)) return NULL;
    for (unsigned i=0;i<4;i++) if (!leases[list->kind][i]) {
        leases[list->kind][i]=true; list->allocations++; return object(list->kind,i);
    }
    CHECK(false); return NULL;
}
static void* ExAllocateFromPagedLookasideList(LOOKASIDE* list) { CHECK(list->kind != 1); return lookaside_allocate(list); }
static void* ExAllocateFromNPagedLookasideList(LOOKASIDE* list) { CHECK(list->kind == 1); return lookaside_allocate(list); }
static void lookaside_free(LOOKASIDE* list, void* p) {
    for (unsigned i=0;i<4;i++) if (object(list->kind,i) == p) {
        CHECK(leases[list->kind][i]); leases[list->kind][i]=false; list->returns++;
        if (list->kind == 0) {
            CHECK(!fcbs[i].Header.contexts && !fcbs[i].lock && !fcbs[i].oplock);
        }
        return;
    }
    CHECK(false);
}
static void ExFreeToPagedLookasideList(LOOKASIDE* list, void* p) { CHECK(list->kind != 1); lookaside_free(list,p); }
static void ExFreeToNPagedLookasideList(LOOKASIDE* list, void* p) { CHECK(list->kind == 1); lookaside_free(list,p); }
static void ExInitializeResourceLite(ERESOURCE* r) { CHECK(!r->initialized); r->initialized=true; created_resources++; }
static void ExDeleteResourceLite(ERESOURCE* r) { CHECK(r->initialized && !r->held); r->initialized=false; destroyed++; }
static void ExInitializeFastMutex(int* mutex) { *mutex=1; }
static void FsRtlSetupAdvancedHeader(struct host_header* header, int* mutex) { CHECK(*mutex == 1 && !header->contexts); header->contexts=true; }
static void FsRtlTeardownPerStreamContexts(struct host_header* header) { CHECK(header->contexts); header->contexts=false; }
static void FsRtlInitializeFileLock(int* lock, void* a, void* b) { CHECK(!*lock && !a && !b); *lock=1; }
static void FsRtlUninitializeFileLock(int* lock) { CHECK(*lock == 1); *lock=0; }
static int* fcb_oplock(fcb* child) { return &child->oplock; }
static void FsRtlInitializeOplock(int* lock) { CHECK(!*lock); *lock=1; }
static void FsRtlUninitializeOplock(int* lock) { CHECK(*lock == 1); *lock=0; }
#include "create_fcb.inc"
#include "create_fileref.inc"
#include "reap_fcb.inc"
#include "reap_fileref.inc"
#include "reap_filerefs.inc"
#include "reap_fcbs.inc"

static NTSTATUS utf16_to_utf8(char* out, ULONG capacity, ULONG* len, const WCHAR* in, USHORT bytes) {
    utf_calls++;
    if (current->fault == UTF8_ERROR && utf_calls == current->index) { faults++; return STATUS_INVALID_PARAMETER; }
    *len=bytes/sizeof(WCHAR); CHECK(!out || capacity >= *len);
    if (out) for (ULONG i=0;i<*len;i++) { CHECK(in[i] < 128); out[i]=(char)in[i]; }
    return STATUS_SUCCESS;
}
static NTSTATUS RtlUpcaseUnicodeString(PUNICODE_STRING out, const UNICODE_STRING* in, bool own) {
    CHECK(own); uppercase_calls++;
    if (current->fault == UPPERCASE_ERROR && uppercase_calls == current->index) { faults++; return STATUS_INSUFFICIENT_RESOURCES; }
    *out=*in; out->Buffer=allocate(in->Length,"uppercase");
    for (unsigned i=0;i<in->Length/sizeof(WCHAR);i++) {
        WCHAR c=in->Buffer[i]; out->Buffer[i]=c >= 'a' && c <= 'z' ? c-32 : c;
    }
    return STATUS_SUCCESS;
}
static size_t RtlCompareMemory(const void* a, const void* b, size_t n) { return memcmp(a,b,n) ? 0 : n; }
static void insert_dir_child_into_hash_lists(fcb* parent, dir_child* dc) {
    InsertTailList(&parent->dir_children_hash,&dc->list_entry_hash);
    InsertTailList(&parent->dir_children_hash_uc,&dc->list_entry_hash_uc);
    parent->hash_ptrs[dc->hash >> 24]=&dc->list_entry_hash;
    parent->hash_ptrs_uc[dc->hash_uc >> 24]=&dc->list_entry_hash_uc;
}
static void KeQuerySystemTime(LARGE_INTEGER* time) { time->QuadPart=123; }
static void win_time_to_unix(LARGE_INTEGER time, struct btrfs_timespec* now) { (void)time; memset(now,0,sizeof(*now)); }
static uint32_t inherit_mode(fcb* parent, bool directory) { CHECK(parent); (void)directory; return 0755; }
static unsigned fast_io_possible(fcb* child) { CHECK(child->Vcb == &volume); return 1; }
static void acquire_fcb_lock_exclusive(device_extension* vcb) { ExAcquireResourceExclusiveLite(&vcb->fcb_lock,true); }
static void release_fcb_lock(device_extension* vcb) { ExReleaseResourceLite(&vcb->fcb_lock); }
static NTSTATUS fcb_get_new_sd(fcb* child, file_ref* parent, ACCESS_STATE* state) {
    CHECK(parent == &directory_ref && state == &access_state);
    if (hit(SECURITY)) return STATUS_ACCESS_DENIED;
    child->sd=allocate(64,"security"); return STATUS_SUCCESS;
}
static NTSTATUS file_create_parse_ea(fcb* child, FILE_FULL_EA_INFORMATION* ea) {
    CHECK(ea); if (hit(EA_ERROR)) return STATUS_INVALID_PARAMETER;
    child->ea_xattr.Buffer=allocate(8,"ea"); return STATUS_SUCCESS;
}
static bool write_fcb_compressed(fcb* child) { CHECK(child); return false; }
static NTSTATUS extend_file(fcb* child, file_ref* ref, int64_t size, bool prealloc, void* irp_arg, LIST_ENTRY* rollback) {
    CHECK(ref->fcb == child && size == 8192 && prealloc && !irp_arg && rollback);
    return hit(EXTEND_ERROR) ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
static NTSTATUS check_file_name_valid(PUNICODE_STRING name, bool posix, bool stream) {
    CHECK(name && !posix); (void)stream;
    return hit(NAME_ERROR) ? STATUS_OBJECT_NAME_INVALID : STATUS_SUCCESS;
}
static void SeLockSubjectContext(int* context) { CHECK(!*context); *context=1; }
static void SeUnlockSubjectContext(int* context) { CHECK(*context == 1); *context=0; }
static void* IoGetFileObjectGenericMapping(void) { return NULL; }
static bool SeAccessCheck(void* sd, int* context, bool locked, unsigned desired, unsigned previous, void* privilege,
                          void* mapping, int mode, ACCESS_MASK* granted, NTSTATUS* status) {
    (void)sd; CHECK(*context == 1 && locked && desired && !previous && !privilege && !mapping && mode == UserMode);
    if (hit(ACCESS_ERROR)) { *status=STATUS_ACCESS_DENIED; return false; }
    *granted=desired; *status=STATUS_SUCCESS; return true;
}
static NTSTATUS open_fileref(device_extension* vcb, file_ref** out, PUNICODE_STRING name, file_ref* related, bool parent,
                             void* a, void* b, POOL_TYPE pool, bool sensitive, PIRP request) {
    CHECK(vcb == &volume && name && related == &directory_ref && !parent && !a && !b && pool == PagedPool && request == &irp);
    CHECK(sensitive == !current->insensitive);
    if (hit(OPEN_ERROR)) return STATUS_IO_DEVICE_ERROR;
    if (current->missing_base) return STATUS_OBJECT_NAME_NOT_FOUND;
    CHECK(have_base); increase_fileref_refcount(&base_ref); *out=&base_ref; return STATUS_SUCCESS;
}
static NTSTATUS find_item(device_extension* vcb, root* r, traverse_ptr* tp, struct btrfs_key* key, bool exact, PIRP request) {
    CHECK(vcb == &volume && r == &subvol && !exact && request == &irp && key->type == BTRFS_XATTR_ITEM_KEY);
    if (hit(TREE_ERROR)) return STATUS_IO_DEVICE_ERROR;
    static struct { struct btrfs_key key; uint32_t size; } item;
    item.key=*key; item.size=current->full ? 4096 : 0;
    tp->item=(void*)&item; return STATUS_SUCCESS;
}
static int keycmp(struct btrfs_key a, struct btrfs_key b) { return a.objectid != b.objectid || a.type != b.type || a.offset != b.offset; }
static void send_notification_fileref(file_ref* ref, ULONG filter, ULONG action, PUNICODE_STRING stream) {
    CHECK(ref && filter && action); (void)stream; notifications++;
}
static void queue_notification_fcb(file_ref* ref, ULONG filter, ULONG action, PUNICODE_STRING stream) {
    CHECK(ref && filter && action && !stream); notifications++;
}
uint32_t calc_crc32c_sw(uint32_t, uint8_t*, uint32_t);
static uint32_t (*calc_crc32c)(uint32_t,uint8_t*,uint32_t)=calc_crc32c_sw;
#include "crc32c.inc"
#include "add_dir_child.inc"
#include "add_fcb_to_subvol.inc"
#include "find_file_in_dir.inc"
// Disk/FCB service boundary. Namespace lookup, fileref publication and
// cleanup use production code below.
static NTSTATUS open_fcb(device_extension* vcb, root* r, uint64_t inode, uint8_t type, PANSI_STRING utf8,
                         bool hardlink, fcb* parent, fcb** out, POOL_TYPE pool, PIRP request) {
    CHECK(vcb == &volume && r == &subvol && inode == base_fcb.inode && type == base_fcb.type);
    CHECK(utf8 == &collision_dc.utf8 && !hardlink && parent == &directory_fcb && request == &irp);
    CHECK(pool == (current->paging ? NonPagedPool : PagedPool));
    CHECK(volume.fcb_lock.held && !directory_np.dir_children_lock.held);
    reopen_calls++;
    if (hit(REOPEN_ERROR)) return STATUS_IO_DEVICE_ERROR;
    if (current->install_during_open) {
        installed_ref=create_fileref(vcb); CHECK(installed_ref);
        installed_ref->fcb=&base_fcb; InterlockedIncrement(&base_fcb.refcount);
        installed_ref->parent=&directory_ref; increase_fileref_refcount(&directory_ref);
        installed_ref->dc=&collision_dc; collision_dc.fileref=installed_ref;
        InsertTailList(&directory_ref.children,&installed_ref->list_entry);
    }
    InterlockedIncrement(&base_fcb.refcount); *out=&base_fcb; return STATUS_SUCCESS;
}
static NTSTATUS open_fcb_stream(device_extension* vcb, dir_child* dc, fcb* parent, fcb** out, PIRP request) {
    (void)vcb; (void)dc; (void)parent; (void)out; (void)request;
    CHECK(false); return STATUS_UNSUCCESSFUL; // not exercised by ordinary creation
}
#include "open_fileref_child.inc"
#include "file_create2.inc"
#include "create_stream.inc"

static void setup_fcb(fcb* node, fcb_nonpaged* np, uint8_t type, uint64_t inode, LIST_ENTRY** hash, LIST_ENTRY** uc) {
    node->Vcb=&volume; node->subvol=&subvol; node->type=type; node->inode=inode; node->refcount=1;
    node->nonpaged=np; node->Header.Resource=&np->resource; node->hash_ptrs=hash; node->hash_ptrs_uc=uc;
    node->hash=calc_crc32c(0xffffffff,(uint8_t*)&inode,sizeof(inode));
    InitializeListHead(&node->dir_children_index); InitializeListHead(&node->dir_children_hash); InitializeListHead(&node->dir_children_hash_uc);
    add_fcb_to_subvol(node); InsertTailList(&volume.all_fcbs,&node->list_entry_all);
}
static void setup(void) {
    volume.fcb_lookaside.kind=0; volume.fcb_np_lookaside.kind=1; volume.fileref_lookaside.kind=2;
    volume.superblock.generation=42; volume.superblock.nodesize=4096;
    InitializeListHead(&volume.all_fcbs); InitializeListHead(&volume.dirty_fcbs); InitializeListHead(&volume.dirty_filerefs);
    InitializeListHead(&subvol.fcbs); subvol.id=5; subvol.lastinode=1000;
    setup_fcb(&directory_fcb,&directory_np,BTRFS_FT_DIR,256,directory_hash,directory_hash_uc);
    directory_ref.fcb=&directory_fcb; directory_ref.refcount=1; InitializeListHead(&directory_ref.children);
    volume.root_fileref=&directory_ref;
    have_base=(current->stream && !current->missing_base) || current->existing_entry;
    if (have_base) {
        setup_fcb(&base_fcb,&base_np,current->existing_directory ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE,500,base_hash,base_hash_uc);
        base_ref.fcb=&base_fcb; base_ref.refcount=1; base_ref.parent=&directory_ref; InitializeListHead(&base_ref.children);
        if (!current->reaped_entry) {
            InsertTailList(&directory_ref.children,&base_ref.list_entry); increase_fileref_refcount(&directory_ref);
        }
        if (current->readonly) base_fcb.atts=FILE_ATTRIBUTE_READONLY;
    }
    if (current->existing_entry) {
        const WCHAR* existing=current->existing_name ? current->existing_name : (const WCHAR*)L"alpha";
        unsigned length=0; while (existing[length]) length++;
        CHECK(length && length < 64);
        cached_name=malloc(length*sizeof(WCHAR)); cached_name_uc=malloc(length*sizeof(WCHAR));
        CHECK(cached_name && cached_name_uc);
        for (unsigned i=0;i<length;i++) {
            cached_name[i]=existing[i];
            cached_name_uc[i]=existing[i] >= 'a' && existing[i] <= 'z' ? existing[i]-32 : existing[i];
        }
        collision_dc.name=(UNICODE_STRING){length*sizeof(WCHAR),length*sizeof(WCHAR),cached_name};
        collision_dc.name_uc=(UNICODE_STRING){length*sizeof(WCHAR),length*sizeof(WCHAR),cached_name_uc};
        collision_dc.index=2; collision_dc.fileref=&base_ref;
        collision_dc.key.objectid=base_fcb.inode; collision_dc.key.type=BTRFS_INODE_ITEM_KEY;
        collision_dc.type=base_fcb.type;
        base_ref.dc=&collision_dc;
        collision_dc.hash=calc_crc32c(0xffffffff,(uint8_t*)cached_name,collision_dc.name.Length);
        collision_dc.hash_uc=calc_crc32c(0xffffffff,(uint8_t*)cached_name_uc,collision_dc.name_uc.Length);
        InsertTailList(&directory_fcb.dir_children_index,&collision_dc.list_entry_index);
        insert_dir_child_into_hash_lists(&directory_fcb,&collision_dc);
    }
    if (current->reaped_entry) {
        // Reap a real, closed fileref while its directory entry remains cached.
        // The fixture keeps an independent FCB hold, as another hardlink could.
        preparing=true;
        file_ref* closed=create_fileref(&volume); CHECK(closed);
        closed->fcb=&base_fcb; InterlockedIncrement(&base_fcb.refcount);
        closed->parent=&directory_ref; increase_fileref_refcount(&directory_ref);
        closed->dc=&collision_dc; collision_dc.fileref=closed;
        if (current->existing_directory) base_fcb.fileref=closed;
        InsertTailList(&directory_ref.children,&closed->list_entry);
        free_fileref(closed); reap_filerefs(&volume);
        CHECK(!collision_dc.fileref && !base_fcb.fileref && base_fcb.refcount == 1);
        CHECK(directory_ref.refcount == 1 && IsListEmpty(&directory_ref.children));
        preparing=false;
    }
    irp.stack.Parameters.Create.SecurityContext=&security_context;
    irp.stack.Parameters.Create.FileAttributes=FILE_ATTRIBUTE_NORMAL;
    if (current->paging) irp.stack.Flags=SL_OPEN_PAGING_FILE;
    if (current->preallocate) irp.Overlay.AllocationSize.QuadPart=8192;
}
static void drain_dirty(void) {
    while (!IsListEmpty(&volume.dirty_fcbs)) {
        fcb* node=CONTAINING_RECORD(RemoveHeadList(&volume.dirty_fcbs),fcb,list_entry_dirty);
        CHECK(node->dirty); node->dirty=false; free_fcb(node);
    }
    while (!IsListEmpty(&volume.dirty_filerefs)) {
        file_ref* ref=CONTAINING_RECORD(RemoveHeadList(&volume.dirty_filerefs),file_ref,list_entry_dirty);
        CHECK(ref->dirty); ref->dirty=false; free_fileref(ref);
    }
}
static void release_cached_entries(fcb* parent) {
    while (!IsListEmpty(&parent->dir_children_index)) {
        dir_child* dc=CONTAINING_RECORD(RemoveHeadList(&parent->dir_children_index),dir_child,list_entry_index);
        if (dc == &collision_dc) continue;
        CHECK(!dc->fileref);
        ExFreePool(dc->utf8.Buffer); ExFreePool(dc->name.Buffer); ExFreePool(dc->name_uc.Buffer); ExFreePool(dc);
    }
}
static void run(void) {
    setup();
    LIST_ENTRY *cache_head=subvol.fcbs.Flink, *cache_tail=subvol.fcbs.Blink;
    LIST_ENTRY* buckets[256]; memcpy(buckets,subvol.fcbs_ptrs,sizeof(buckets));
    const WCHAR* requested=current->requested_name ? current->requested_name : (const WCHAR*)L"alpha";
    WCHAR chars[64], stream_chars[]={'e','x','t','r','a'};
    unsigned length=0;
    while (requested[length]) { CHECK(length < 64); chars[length]=requested[length]; length++; }
    UNICODE_STRING name={length*sizeof(WCHAR),sizeof(chars),chars}, stream={sizeof(stream_chars),sizeof(stream_chars),stream_chars};
    if (current->hash_collision) {
        WCHAR upper[64];
        for (unsigned i=0;i<length;i++) upper[i]=chars[i] >= 'a' && chars[i] <= 'z' ? chars[i]-32 : chars[i];
        CHECK(collision_dc.hash_uc == calc_crc32c(0xffffffff,(uint8_t*)upper,name.Length));
        CHECK(collision_dc.name_uc.Length != name.Length || memcmp(cached_name_uc,upper,name.Length));
    }
    LIST_ENTRY rollback; InitializeListHead(&rollback);
    FILE_FULL_EA_INFORMATION ea={0};
    file_ref *result=NULL, *parent=&directory_ref;
    volume.fcb_lock.held=1; // caller holds this across creation and child lookup
    NTSTATUS status=current->stream ? create_stream(&volume,&result,&parent,&name,&stream,&irp,
                          current->directory ? FILE_DIRECTORY_FILE : 0,current->paging ? NonPagedPool : PagedPool,!current->insensitive,&rollback)
        : file_create2(&irp,&volume,&name,&directory_ref,current->directory ? FILE_DIRECTORY_FILE : 0,
                       current->fault == EA_ERROR ? &ea : NULL,current->fault == EA_ERROR ? sizeof(ea) : 0,&result,!current->insensitive,&rollback);
    CHECK(volume.fcb_lock.held == 1); volume.fcb_lock.held=0;
    if (status != current->expected)
        fprintf(stderr,"%s: status=%#x, expected=%#x, returned existing=%u\n",
                scenario,(unsigned)status,(unsigned)current->expected,result == &base_ref);
    CHECK(status == current->expected);
    if (current->existing_entry) {
        unsigned entries=0;
        for (LIST_ENTRY* le=directory_fcb.dir_children_index.Flink;le != &directory_fcb.dir_children_index;le=le->Flink) entries++;
        CHECK(entries == (NT_SUCCESS(status) ? 2u : 1u));
        if (!current->reaped_entry) CHECK(collision_dc.fileref == &base_ref);
        else if (status != STATUS_OBJECT_NAME_COLLISION) CHECK(!collision_dc.fileref);
        if (current->reaped_entry && !NT_SUCCESS(status)) {
            // No matching cached name may reach directory-entry insertion,
            // even when a later allocation happens to produce the same error.
            for (unsigned i=0;i<allocator_count;i++) CHECK(strcmp(allocators[i].caller,"add_dir_child"));
        }
        const WCHAR* existing=current->existing_name ? current->existing_name : (const WCHAR*)L"alpha";
        CHECK(!memcmp(collision_dc.name.Buffer,existing,collision_dc.name.Length));
        if (!NT_SUCCESS(status)) {
            CHECK(directory_fcb.dir_children_hash.Flink == &collision_dc.list_entry_hash && collision_dc.list_entry_hash.Flink == &directory_fcb.dir_children_hash);
            CHECK(directory_fcb.dir_children_hash_uc.Flink == &collision_dc.list_entry_hash_uc && collision_dc.list_entry_hash_uc.Flink == &directory_fcb.dir_children_hash_uc);
        }
    }
    if (!current->stream) CHECK(directory_fcb.inode_item.size == (NT_SUCCESS(status) ? name.Length : 0));
    CHECK(faults == (current->fault == NONE ? 0u : 1u));
    CHECK(!access_state.SubjectSecurityContext && !volume.fcb_lock.held);
    CHECK(!directory_np.resource.held && !directory_np.dir_children_lock.held && !base_np.resource.held && !base_np.dir_children_lock.held);
    if (NT_SUCCESS(status)) {
        CHECK(result && result->fcb->dirty && result->dirty && result->refcount == 2);
        CHECK(result->parent == (current->stream ? parent : &directory_ref));
        CHECK(!result->fcb->deleted && result->dc && result->dc->fileref == result);
        if (current->stream) { CHECK(result->fcb->ads); free_fileref(parent); }
        else {
            CHECK(result->fcb->type == (current->directory ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE));
            CHECK(result->dc->name.Length == name.Length && !memcmp(result->dc->name.Buffer,requested,name.Length));
        }
        free_fileref(result); // release the caller's returned fileref
    } else if (status == STATUS_OBJECT_NAME_COLLISION) {
        CHECK(result && result == collision_dc.fileref && result->fcb == &base_fcb);
        CHECK(result->parent == &directory_ref && result->dc == &collision_dc);
        if (current->reaped_entry) {
            CHECK(result->refcount == (current->install_during_open ? 2 : 1));
            if (current->existing_directory) CHECK(base_fcb.fileref == result);
            if (installed_ref) { CHECK(result == installed_ref); free_fileref(installed_ref); }
        } else CHECK(result == &base_ref && result->refcount == 2);
        CHECK(IsListEmpty(&rollback));
        free_fileref(result);
    } else CHECK(!result);
    if (!current->stream && !NT_SUCCESS(status)) {
        for (LIST_ENTRY* le=volume.dirty_fcbs.Flink;le != &volume.dirty_fcbs;le=le->Flink) {
            fcb* attempted=CONTAINING_RECORD(le,fcb,list_entry_dirty);
            if (attempted != &directory_fcb) CHECK(attempted->deleted);
        }
    }
    // Model completed persistence, then run the actual cache reapers.
    drain_dirty(); reap_filerefs(&volume); reap_fcbs(&volume);
    if (directory_fcb.refcount != 1 || (have_base && base_fcb.refcount != 1))
        fprintf(stderr,"%s: parent FCB refs: directory=%d, base=%d (expected 1 for each fixture parent)\n",
                scenario,directory_fcb.refcount,have_base ? base_fcb.refcount : 1);
    CHECK(directory_fcb.refcount == 1 && (!have_base || base_fcb.refcount == 1));
    CHECK(directory_ref.refcount == (have_base && !current->reaped_entry ? 2 : 1) && (!have_base || base_ref.refcount == 1));
    if (current->reaped_entry) {
        CHECK(!collision_dc.fileref && !base_fcb.fileref);
        bool opened=status == STATUS_OBJECT_NAME_COLLISION || current->fault == REOPEN_ERROR || current->fault == FILEREF_ALLOC;
        CHECK(reopen_calls == (opened ? 1u : 0u));
    }
    CHECK(subvol.fcbs.Flink == cache_head && subvol.fcbs.Blink == cache_tail);
    CHECK(!memcmp(buckets,subvol.fcbs_ptrs,sizeof(buckets)));
    CHECK(volume.fcb_lookaside.allocations == volume.fcb_lookaside.returns);
    CHECK(volume.fcb_np_lookaside.allocations == volume.fcb_np_lookaside.returns);
    CHECK(volume.fileref_lookaside.allocations == volume.fileref_lookaside.returns);
    CHECK(created_resources == destroyed);
    for (unsigned kind=0;kind<3;kind++) for (unsigned i=0;i<4;i++) CHECK(!leases[kind][i]);
    // Parent namespace entries remain cached after child FCB eviction.
    release_cached_entries(&directory_fcb); if (have_base) release_cached_entries(&base_fcb);
    while (!IsListEmpty(&rollback)) {
        rollback_item* item=CONTAINING_RECORD(RemoveHeadList(&rollback),rollback_item,list_entry);
        ExFreePool(item);
    }
    for (unsigned i=0;i<pool_count;i++) { CHECK(!pools[i].live); free(pools[i].p); }
    CHECK(!volume.dirty_fcbs_lock.held && !volume.dirty_filerefs_lock.held);
    free(cached_name); free(cached_name_uc);
    printf("PASS %s\n",scenario);
}
int main(int argc, char** argv) {
    static const test_case cases[]={
        {.name="file-success"}, {.name="directory-success",.directory=true},
        {.name="file-casefold",.insensitive=true}, {.name="file-paging",.paging=true},
        {.name="file-preallocate",.preallocate=true},
        {.name="file-utf8-size",.fault=UTF8_ERROR,.index=1,.expected=STATUS_INVALID_PARAMETER},
        {.name="file-utf8-copy",.fault=UTF8_ERROR,.index=2,.expected=STATUS_INVALID_PARAMETER},
        {.name="file-name-allocation",.fault=POOL,.allocator="file_create2",.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-fcb-allocation",.fault=FCB_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-nonpaged-allocation",.fault=NP_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-security",.fault=SECURITY,.expected=STATUS_ACCESS_DENIED},
        {.name="directory-security",.directory=true,.fault=SECURITY,.expected=STATUS_ACCESS_DENIED},
        {.name="file-ea",.fault=EA_ERROR,.expected=STATUS_INVALID_PARAMETER},
        {.name="file-fileref",.fault=FILEREF_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-extend",.preallocate=true,.fault=EXTEND_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="directory-hash",.directory=true,.fault=POOL,.allocator="file_create2",.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="directory-uppercase-hash",.directory=true,.fault=POOL,.allocator="file_create2",.index=3,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-casefold-failure",.insensitive=true,.fault=UPPERCASE_ERROR,.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-rollback-allocation",.fault=POOL,.allocator="add_dir_child",.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-entry-allocation",.fault=POOL,.allocator="add_dir_child",.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-entry-utf8",.fault=POOL,.allocator="add_dir_child",.index=3,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-entry-name",.fault=POOL,.allocator="add_dir_child",.index=4,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-entry-uppercase",.fault=UPPERCASE_ERROR,.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="file-collision",.existing_entry=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="insensitive-lower-lower",.existing_entry=true,.insensitive=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="insensitive-lower-upper",.existing_entry=true,.insensitive=true,.requested_name=L"ALPHA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="insensitive-lower-mixed",.existing_entry=true,.insensitive=true,.requested_name=L"AlPhA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="insensitive-mixed-lower",.existing_entry=true,.insensitive=true,.existing_name=L"aLpHa",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="insensitive-upper-lower",.existing_entry=true,.insensitive=true,.existing_name=L"ALPHA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="insensitive-directory",.existing_entry=true,.insensitive=true,.directory=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="insensitive-paging",.existing_entry=true,.insensitive=true,.paging=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="sensitive-case-distinct",.existing_entry=true,.requested_name=L"ALPHA"},
        {.name="insensitive-distinct",.existing_entry=true,.insensitive=true,.requested_name=L"bravo"},
        {.name="insensitive-conversion-error",.existing_entry=true,.insensitive=true,.fault=UPPERCASE_ERROR,.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="insensitive-hash-collision",.existing_entry=true,.insensitive=true,.existing_name=L"572BSHK",.requested_name=L"AFNB7ZH",.hash_collision=true},
        {.name="insensitive-hash-collision-length",.existing_entry=true,.insensitive=true,.existing_name=L"6KKS16",.requested_name=L"IM921VL31U",.hash_collision=true},
        {.name="cached-sensitive",.existing_entry=true,.reaped_entry=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-insensitive",.existing_entry=true,.reaped_entry=true,.insensitive=true,.requested_name=L"AlPhA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-directory",.existing_entry=true,.reaped_entry=true,.existing_directory=true,.directory=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-directory-as-file",.existing_entry=true,.reaped_entry=true,.existing_directory=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-file-as-directory",.existing_entry=true,.reaped_entry=true,.directory=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-paging",.existing_entry=true,.reaped_entry=true,.paging=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-preallocate",.existing_entry=true,.reaped_entry=true,.preallocate=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-open-error",.existing_entry=true,.reaped_entry=true,.fault=REOPEN_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="cached-insensitive-open-error",.existing_entry=true,.reaped_entry=true,.insensitive=true,.fault=REOPEN_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="cached-fileref-error",.existing_entry=true,.reaped_entry=true,.fault=FILEREF_ALLOC,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="cached-uppercase-error",.existing_entry=true,.reaped_entry=true,.insensitive=true,.fault=UPPERCASE_ERROR,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="cached-installed-during-open",.existing_entry=true,.reaped_entry=true,.install_during_open=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="cached-sensitive-distinct",.existing_entry=true,.reaped_entry=true,.requested_name=L"ALPHA"},
        {.name="cached-insensitive-distinct",.existing_entry=true,.reaped_entry=true,.insensitive=true,.requested_name=L"bravo"},
        {.name="cached-hash-collision",.existing_entry=true,.reaped_entry=true,.insensitive=true,.existing_name=L"572BSHK",.requested_name=L"AFNB7ZH",.hash_collision=true},
        {.name="cached-hash-collision-length",.existing_entry=true,.reaped_entry=true,.insensitive=true,.existing_name=L"6KKS16",.requested_name=L"IM921VL31U",.hash_collision=true},
        {.name="stream-success",.stream=true}, {.name="stream-casefold",.stream=true,.insensitive=true},
        {.name="stream-nonpaged",.stream=true,.paging=true},
        {.name="stream-missing-base",.stream=true,.missing_base=true},
        {.name="stream-missing-base-security",.stream=true,.missing_base=true,.fault=SECURITY,.expected=STATUS_ACCESS_DENIED},
        {.name="stream-missing-base-fcb",.stream=true,.missing_base=true,.fault=FCB_ALLOC,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-missing-base-fileref",.stream=true,.missing_base=true,.fault=FILEREF_ALLOC,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-missing-base-utf8",.stream=true,.missing_base=true,.fault=UTF8_ERROR,.index=3,.expected=STATUS_INVALID_PARAMETER},
        {.name="stream-missing-base-metadata",.stream=true,.missing_base=true,.fault=TREE_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="stream-missing-base-full",.stream=true,.missing_base=true,.full=true,.expected=STATUS_DISK_FULL},
        {.name="stream-missing-base-uppercase",.stream=true,.missing_base=true,.fault=UPPERCASE_ERROR,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-open",.stream=true,.fault=OPEN_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="stream-invalid-name",.stream=true,.fault=NAME_ERROR,.expected=STATUS_OBJECT_NAME_INVALID},
        {.name="stream-directory-rejected",.stream=true,.directory=true,.expected=STATUS_INVALID_PARAMETER},
        {.name="stream-readonly",.stream=true,.readonly=true,.expected=STATUS_ACCESS_DENIED},
        {.name="stream-access",.stream=true,.fault=ACCESS_ERROR,.expected=STATUS_ACCESS_DENIED},
        {.name="stream-fcb",.stream=true,.fault=FCB_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-nonpaged-fcb",.stream=true,.fault=NP_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-utf8-size",.stream=true,.fault=UTF8_ERROR,.index=1,.expected=STATUS_INVALID_PARAMETER},
        {.name="stream-xattr-name",.stream=true,.fault=POOL,.allocator="create_stream",.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-utf8-copy",.stream=true,.fault=UTF8_ERROR,.index=2,.expected=STATUS_INVALID_PARAMETER},
        {.name="stream-metadata",.stream=true,.fault=TREE_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="stream-full",.stream=true,.full=true,.expected=STATUS_DISK_FULL},
        {.name="stream-fileref",.stream=true,.fault=FILEREF_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-entry",.stream=true,.fault=POOL,.allocator="create_stream",.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-entry-utf8",.stream=true,.fault=POOL,.allocator="create_stream",.index=3,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-entry-name",.stream=true,.fault=POOL,.allocator="create_stream",.index=4,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-entry-uppercase",.stream=true,.fault=UPPERCASE_ERROR,.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
    };
    if (argc == 2 && !strcmp(argv[1],"--list")) {
        for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) puts(cases[i].name);
        return 0;
    }
    if (argc != 2) return 2;
    scenario=argv[1];
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) if (!strcmp(scenario,cases[i].name)) { current=&cases[i]; run(); return 0; }
    return 2;
}
