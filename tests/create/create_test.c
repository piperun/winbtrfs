/* Native ownership regression: production creation, dirty insertion and
 * reaping routines, with explicit host models for kernel services. */
#include "host.h"

static const char* scenario;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",scenario,__LINE__,#c); exit(1); } } while (0)
static void host_log(const char* format, ...) { (void)format; }
enum fault_kind { NONE, POOL, FCB_ALLOC, NP_ALLOC, FILEREF_ALLOC, SECURITY, EA_ERROR,
                  EXTEND_ERROR, UTF8_ERROR, UPPERCASE_ERROR, TREE_ERROR, ACCESS_ERROR,
                  OPEN_ERROR, NAME_ERROR, REOPEN_ERROR, XATTR_ERROR };
typedef struct {
    const char* name;
    bool stream, missing_base, directory, paging, insensitive, preallocate, existing_entry, full, readonly;
    enum fault_kind fault;
    const char* allocator;
    unsigned index;
    NTSTATUS expected;
    const WCHAR *existing_name, *requested_name;
    bool hash_collision, reaped_entry, existing_directory, install_during_open, existing_stream;
    const WCHAR *stream_name, *cached_stream_name;
    bool reaped_stream, cold_stream, reopen_only, install_stream, bad_stream_key, short_stream_item;
    bool dispatch, loaded_related, relative_stream, data_suffix;
    const char* colliding_xattr;
    bool collider_after_target;
} test_case;
static const test_case* current;
static device_extension volume;
static root subvol;
static fcb directory_fcb, base_fcb, stream_fcb;
static fcb_nonpaged directory_np, base_np, stream_np;
static file_ref directory_ref, base_ref, stream_ref;
static dir_child collision_dc, stream_dc;
static WCHAR *cached_name, *cached_name_uc, *stream_cached_name, *stream_cached_uc;
static char* stream_utf8;
static fcb* warm_stream_fcb;
static file_ref* installed_stream_ref;
static unsigned stream_reads;
static const uint8_t stream_payload[]={'p','a','y','l','o','a','d'};
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
static SECURITY_CONTEXT security_context={.AccessState=&access_state,.DesiredAccess=FILE_WRITE_DATA};
static IRP irp={.RequestorMode=UserMode};
static struct { void* p; size_t size; bool live; const char* caller; } pools[64];
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
static void increase_fileref_refcount(file_ref* ref) {
    if (ref == &stream_ref) CHECK(base_np.dir_children_lock.held);
    InterlockedIncrement(&ref->refcount);
}
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
    pools[pool_count].p=p; pools[pool_count].size=size; pools[pool_count].caller=caller; pools[pool_count++].live=true;
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
    CHECK(vcb == &volume && name && !a && !b && request == &irp);
    CHECK(sensitive == !current->insensitive);
    if (hit(OPEN_ERROR)) return STATUS_IO_DEVICE_ERROR;
    if (parent) {
        CHECK(current->dispatch && !current->loaded_related);
        CHECK(pool == (current->paging ? NonPagedPool : PagedPool));
        file_ref* found=current->relative_stream ? &base_ref : &directory_ref;
        increase_fileref_refcount(found); *out=found; return STATUS_SUCCESS;
    }
    CHECK(pool == PagedPool && (related == &directory_ref || (current->dispatch && current->relative_stream && related == &base_ref)));
    if (current->missing_base) return STATUS_OBJECT_NAME_NOT_FOUND;
    CHECK(have_base); increase_fileref_refcount(&base_ref); *out=&base_ref; return STATUS_SUCCESS;
}
static NTSTATUS find_item(device_extension* vcb, root* r, traverse_ptr* tp, struct btrfs_key* key, bool exact, PIRP request) {
    CHECK(vcb == &volume && r == &subvol && !exact && request == &irp && key->type == BTRFS_XATTR_ITEM_KEY);
    if (hit(TREE_ERROR)) return STATUS_IO_DEVICE_ERROR;
    static struct { struct btrfs_key key; uint32_t size; } item;
    item.key=*key; item.size=current->full ? 4096 : stream_reads ? 32 : 0;
    if (stream_reads && current->bad_stream_key) item.key.offset++;
    if (stream_reads && current->short_stream_item) item.size=sizeof(stream_payload)-1;
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
static bool get_xattr(device_extension* vcb, root* r, uint64_t inode, char* name, uint32_t crc,
                       uint8_t** data, uint16_t* length, PIRP request) {
    CHECK(vcb == &volume && r == &subvol && inode == base_fcb.inode && request == &irp);
    CHECK(volume.fcb_lock.held && !base_np.dir_children_lock.held);
    CHECK(!memcmp(name,"user.",5) && strlen(name) == 5u+stream_dc.utf8.Length);
    CHECK(!memcmp(name+5,stream_dc.utf8.Buffer,stream_dc.utf8.Length));
    CHECK(crc == calc_crc32c(0xfffffffe,(uint8_t*)name,strlen(name)));
    stream_reads++;
    if (hit(XATTR_ERROR)) return false;
    *length=sizeof(stream_payload); *data=allocate(*length,"xattr"); memcpy(*data,stream_payload,*length);
    if (current->install_stream) {
        CHECK(warm_stream_fcb && !stream_dc.fileref);
        installed_stream_ref=create_fileref(vcb); CHECK(installed_stream_ref);
        installed_stream_ref->fcb=warm_stream_fcb; InterlockedIncrement(&warm_stream_fcb->refcount);
        installed_stream_ref->parent=&base_ref; increase_fileref_refcount(&base_ref);
        installed_stream_ref->dc=&stream_dc; stream_dc.fileref=installed_stream_ref;
        InsertTailList(&base_ref.children,&installed_stream_ref->list_entry);
    }
    return true;
}
#include "open_fcb_stream.inc"
#include "open_fileref_child.inc"
#include "file_create2.inc"
#include "create_stream.inc"
#include "dispatch_test.inc"

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
    if (current->existing_stream) {
        CHECK(have_base && !current->missing_base);
        if (!current->reaped_stream) {
            setup_fcb(&stream_fcb,&stream_np,base_fcb.type,base_fcb.inode,NULL,NULL);
            stream_fcb.ads=true; stream_fcb.adsdata.Length=sizeof(stream_payload);
            stream_ref.fcb=&stream_fcb; stream_ref.refcount=1; stream_ref.parent=&base_ref;
            InitializeListHead(&stream_ref.children); stream_ref.dc=&stream_dc;
            increase_fileref_refcount(&base_ref); InsertTailList(&base_ref.children,&stream_ref.list_entry);
        }
        const WCHAR* name=current->cached_stream_name ? current->cached_stream_name : (const WCHAR*)L"extra";
        unsigned length=0; while (name[length]) length++;
        CHECK(length && length < 64);
        stream_cached_name=malloc(length*sizeof(WCHAR)); stream_cached_uc=malloc(length*sizeof(WCHAR));
        CHECK(stream_cached_name && stream_cached_uc);
        for (unsigned i=0;i<length;i++) {
            stream_cached_name[i]=name[i]; stream_cached_uc[i]=name[i] >= 'a' && name[i] <= 'z' ? name[i]-32 : name[i];
        }
        stream_dc.name=(UNICODE_STRING){length*sizeof(WCHAR),length*sizeof(WCHAR),stream_cached_name};
        stream_dc.name_uc=(UNICODE_STRING){length*sizeof(WCHAR),length*sizeof(WCHAR),stream_cached_uc};
        stream_dc.index=0; stream_dc.size=sizeof(stream_payload); stream_dc.fileref=&stream_ref;
        stream_utf8=malloc(length); CHECK(stream_utf8);
        for (unsigned i=0;i<length;i++) { CHECK(name[i] < 128); stream_utf8[i]=(char)name[i]; }
        stream_dc.utf8=(ANSI_STRING){length,length,stream_utf8};
        InsertHeadList(&base_fcb.dir_children_index,&stream_dc.list_entry_index);
        if (current->reaped_stream) {
            preparing=true;
            // Construct and close a real cached stream, retaining a separate
            // FCB hold only in warm-cache cases. Production reapers clear dc.
            fcb* prior=create_fcb(&volume,PagedPool); CHECK(prior);
            prior->Vcb=&volume; prior->subvol=&subvol; prior->inode=base_fcb.inode;
            prior->hash=base_fcb.hash; prior->type=base_fcb.type; prior->ads=true;
            prior->adsxattr.Length=5+length; prior->adsxattr.MaximumLength=6+length;
            prior->adsxattr.Buffer=allocate(6+length,"fixture");
            memcpy(prior->adsxattr.Buffer,"user.",5); memcpy(prior->adsxattr.Buffer+5,stream_utf8,length);
            prior->adsxattr.Buffer[5+length]=0;
            prior->adshash=calc_crc32c(0xfffffffe,(uint8_t*)prior->adsxattr.Buffer,5+length);
            prior->adsdata.Length=prior->adsdata.MaximumLength=sizeof(stream_payload);
            prior->adsdata.Buffer=allocate(sizeof(stream_payload),"fixture");
            memcpy(prior->adsdata.Buffer,stream_payload,sizeof(stream_payload));
            prior->Header.FileSize.QuadPart=sizeof(stream_payload);
            add_fcb_to_subvol(prior); InsertTailList(&volume.all_fcbs,&prior->list_entry_all);
            if (!current->cold_stream) { warm_stream_fcb=prior; InterlockedIncrement(&prior->refcount); }
            file_ref* closed=create_fileref(&volume); CHECK(closed);
            closed->fcb=prior; closed->parent=&base_ref; increase_fileref_refcount(&base_ref);
            closed->dc=&stream_dc; stream_dc.fileref=closed;
            InsertTailList(&base_ref.children,&closed->list_entry);
            free_fileref(closed); reap_filerefs(&volume); reap_fcbs(&volume);
            CHECK(!stream_dc.fileref && base_ref.refcount == 1);
            CHECK(!warm_stream_fcb || warm_stream_fcb->refcount == 1);
            preparing=false;
        }
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
    if (current->dispatch && !current->insensitive) irp.stack.Flags |= SL_CASE_SENSITIVE;
}
#include "ads_cache_test.inc"

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
        if (dc == &collision_dc || dc == &stream_dc) continue;
        CHECK(!dc->fileref);
        ExFreePool(dc->utf8.Buffer); ExFreePool(dc->name.Buffer); ExFreePool(dc->name_uc.Buffer); ExFreePool(dc);
    }
}
static void run(void) {
    setup();
    setup_ads_cache();
    LIST_ENTRY *cache_head=subvol.fcbs.Flink, *cache_tail=subvol.fcbs.Blink;
    LIST_ENTRY* buckets[256]; memcpy(buckets,subvol.fcbs_ptrs,sizeof(buckets));
    const WCHAR* requested=current->requested_name ? current->requested_name : (const WCHAR*)L"alpha";
    WCHAR chars[64], stream_chars[64];
    const WCHAR* stream_requested=current->stream_name ? current->stream_name : (const WCHAR*)L"extra";
    unsigned stream_length=0;
    while (stream_requested[stream_length]) { CHECK(stream_length < 64); stream_chars[stream_length]=stream_requested[stream_length]; stream_length++; }
    unsigned length=0;
    while (requested[length]) { CHECK(length < 64); chars[length]=requested[length]; length++; }
    UNICODE_STRING name={length*sizeof(WCHAR),sizeof(chars),chars}, stream={stream_length*sizeof(WCHAR),sizeof(stream_chars),stream_chars};
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
    NTSTATUS status=current->dispatch ? dispatch_create(&name,&stream,&result,&parent,&rollback)
        : current->reopen_only ? open_fileref_child(&volume,&base_ref,&stream,!current->insensitive,true,true,
                                                                           current->paging ? NonPagedPool : PagedPool,&result,&irp)
        : current->stream ? create_stream(&volume,&result,&parent,&name,&stream,&irp,
                          current->directory ? FILE_DIRECTORY_FILE : 0,current->paging ? NonPagedPool : PagedPool,!current->insensitive,&rollback)
        : file_create2(&irp,&volume,&name,&directory_ref,current->directory ? FILE_DIRECTORY_FILE : 0,
                       current->fault == EA_ERROR ? &ea : NULL,current->fault == EA_ERROR ? sizeof(ea) : 0,&result,!current->insensitive,&rollback);
    CHECK(volume.fcb_lock.held == 1); volume.fcb_lock.held=0;
    if (status != current->expected)
        fprintf(stderr,"%s: status=%#x, expected=%#x, returned existing=%u\n",
                scenario,(unsigned)status,(unsigned)current->expected,result == &base_ref);
    CHECK(status == current->expected);
    check_ads_result(result);
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
    if (current->existing_stream) {
        unsigned entries=0;
        for (LIST_ENTRY* le=base_fcb.dir_children_index.Flink;le != &base_fcb.dir_children_index;le=le->Flink) entries++;
        CHECK(entries == (NT_SUCCESS(status) && !current->reopen_only ? 2u : 1u));
        CHECK(stream_dc.size == sizeof(stream_payload));
        if (!current->reaped_stream) CHECK(stream_dc.fileref == &stream_ref && stream_fcb.adsdata.Length == sizeof(stream_payload));
        else if (!result || (NT_SUCCESS(status) && !current->reopen_only)) CHECK(!stream_dc.fileref);
        const WCHAR* original=current->cached_stream_name ? current->cached_stream_name : (const WCHAR*)L"extra";
        CHECK(!memcmp(stream_dc.name.Buffer,original,stream_dc.name.Length));
        if (status == STATUS_OBJECT_NAME_COLLISION) {
            unsigned uppercase_leaks=0, dirty_streams=0;
            for (LIST_ENTRY* le=volume.dirty_fcbs.Flink;le != &volume.dirty_fcbs;le=le->Flink) {
                fcb* child=CONTAINING_RECORD(le,fcb,list_entry_dirty); if (child->ads) dirty_streams++;
            }
            for (unsigned i=0;i<pool_count;i++) if (pools[i].live && !strcmp(pools[i].caller,"uppercase")) uppercase_leaks++;
            if (base_np.dir_children_lock.held || uppercase_leaks || dirty_streams)
                fprintf(stderr,"%s: collision cleanup: directory lock=%u, uppercase buffers=%u, dirty discarded streams=%u\n",
                        scenario,base_np.dir_children_lock.held,uppercase_leaks,dirty_streams);
            CHECK(!base_np.dir_children_lock.held && !uppercase_leaks && !dirty_streams);
            CHECK(!notifications && !base_fcb.inode_item_changed && !base_fcb.dirty);
        }
    }
    if (!current->stream) CHECK(directory_fcb.inode_item.size == (NT_SUCCESS(status) ? name.Length : 0));
    CHECK(faults == (current->fault == NONE ? 0u : 1u));
    CHECK(!access_state.SubjectSecurityContext && !volume.fcb_lock.held);
    CHECK(!directory_np.resource.held && !directory_np.dir_children_lock.held && !base_np.resource.held && !base_np.dir_children_lock.held);
    if ((current->reaped_stream && status == STATUS_OBJECT_NAME_COLLISION) || (current->reopen_only && NT_SUCCESS(status))) {
        CHECK(result && result == stream_dc.fileref && result->parent == &base_ref && result->dc == &stream_dc);
        CHECK(result->refcount == (current->install_stream ? 2 : 1));
        if (current->install_stream) { CHECK(result == installed_stream_ref); free_fileref(installed_stream_ref); }
        if (warm_stream_fcb) CHECK(result->fcb == warm_stream_fcb);
        CHECK(result->fcb->ads && !result->fcb->deleted && !result->fcb->dirty && !result->dirty);
        CHECK(result->fcb->adsdata.Length == sizeof(stream_payload));
        CHECK(!memcmp(result->fcb->adsdata.Buffer,stream_payload,sizeof(stream_payload)));
        CHECK(result->fcb->adsxattr.Length == 5+stream_dc.utf8.Length);
        CHECK(!memcmp(result->fcb->adsxattr.Buffer+5,stream_utf8,stream_dc.utf8.Length));
        CHECK(result->fcb->Header.FileSize.QuadPart == sizeof(stream_payload));
        CHECK(IsListEmpty(&rollback)); free_fileref(result);
    } else if (NT_SUCCESS(status)) {
        CHECK(result && result->fcb->dirty && result->dirty && result->refcount == 2);
        CHECK(result->parent == (current->stream ? parent : &directory_ref));
        CHECK(!result->fcb->deleted && result->dc && result->dc->fileref == result);
        if (current->stream) { CHECK(result->fcb->ads); if (!current->dispatch) free_fileref(parent); }
        else {
            CHECK(result->fcb->type == (current->directory ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE));
            CHECK(result->dc->name.Length == name.Length && !memcmp(result->dc->name.Buffer,requested,name.Length));
        }
        free_fileref(result); // release the caller's returned fileref
    } else if (status == STATUS_OBJECT_NAME_COLLISION && current->stream) {
        CHECK(result == &stream_ref && result->refcount == 2 && result->fcb == &stream_fcb);
        CHECK(parent == &base_ref && base_ref.refcount == 2);
        CHECK(IsListEmpty(&rollback)); free_fileref(result);
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
    if (current->stream && !NT_SUCCESS(status)) {
        // A discarded ADS shares the existing stream's inode and xattr name.
        // A dirty deleted ADS would make flush_fcb delete that xattr.
        for (LIST_ENTRY* le=volume.dirty_fcbs.Flink;le != &volume.dirty_fcbs;le=le->Flink)
            CHECK(!CONTAINING_RECORD(le,fcb,list_entry_dirty)->ads);
    }
    // Model completed persistence, then run the actual cache reapers.
    drain_dirty(); reap_filerefs(&volume); reap_fcbs(&volume);
    if (directory_fcb.refcount != 1 || (have_base && base_fcb.refcount != 1))
        fprintf(stderr,"%s: parent FCB refs: directory=%d, base=%d (expected 1 for each fixture parent)\n",
                scenario,directory_fcb.refcount,have_base ? base_fcb.refcount : 1);
    CHECK(directory_fcb.refcount == 1 && (!have_base || base_fcb.refcount == 1));
    CHECK(directory_ref.refcount == (have_base && !current->reaped_entry ? 2 : 1) && (!have_base || base_ref.refcount == (current->existing_stream && !current->reaped_stream ? 2 : 1)));
    if (current->reaped_entry) {
        CHECK(!collision_dc.fileref && !base_fcb.fileref);
        bool opened=status == STATUS_OBJECT_NAME_COLLISION || current->fault == REOPEN_ERROR || current->fault == FILEREF_ALLOC;
        CHECK(reopen_calls == (opened ? 1u : 0u));
    }
    if (current->existing_stream && !current->reaped_stream) CHECK(stream_ref.refcount == 1 && stream_fcb.refcount == 1);
    if (current->reaped_stream) CHECK(!stream_dc.fileref);
    CHECK(subvol.fcbs.Flink == cache_head && subvol.fcbs.Blink == cache_tail);
    CHECK(!memcmp(buckets,subvol.fcbs_ptrs,sizeof(buckets)));
    if (warm_stream_fcb) { CHECK(warm_stream_fcb->refcount == 1); free_fcb(warm_stream_fcb); reap_fcbs(&volume); }
    release_ads_cache();
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
    free(cached_name); free(cached_name_uc); free(stream_cached_name); free(stream_cached_uc); free(stream_utf8);
    printf("PASS %s\n",scenario);
}
int main(int argc, char** argv) {
    static const test_case cases[]={
        #include "ads_cache_cases.inc"
        {.name="dispatch-stream-live",.dispatch=true,.stream=true,.existing_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-live-borrowed",.dispatch=true,.loaded_related=true,.stream=true,.existing_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-relative",.dispatch=true,.loaded_related=true,.relative_stream=true,.stream=true,.existing_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-relative-acquired",.dispatch=true,.relative_stream=true,.stream=true,.existing_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-new-relative-acquired",.dispatch=true,.relative_stream=true,.stream=true},
        {.name="dispatch-stream-insensitive",.dispatch=true,.stream=true,.existing_stream=true,.insensitive=true,.stream_name=L"ExTrA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-data-suffix",.dispatch=true,.stream=true,.existing_stream=true,.data_suffix=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-paging",.dispatch=true,.stream=true,.existing_stream=true,.paging=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-warm",.dispatch=true,.stream=true,.existing_stream=true,.reaped_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-cold",.dispatch=true,.stream=true,.existing_stream=true,.reaped_stream=true,.cold_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-cold-relative",.dispatch=true,.loaded_related=true,.relative_stream=true,.stream=true,.existing_stream=true,.reaped_stream=true,.cold_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-read-failure",.dispatch=true,.stream=true,.existing_stream=true,.reaped_stream=true,.fault=XATTR_ERROR,.expected=STATUS_INTERNAL_ERROR},
        {.name="dispatch-stream-uppercase-failure",.dispatch=true,.stream=true,.existing_stream=true,.fault=UPPERCASE_ERROR,.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="dispatch-stream-base-open-failure",.dispatch=true,.stream=true,.fault=OPEN_ERROR,.index=2,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="dispatch-stream-parent-open-failure",.dispatch=true,.stream=true,.fault=OPEN_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="dispatch-stream-new",.dispatch=true,.stream=true},
        {.name="dispatch-stream-new-borrowed",.dispatch=true,.loaded_related=true,.stream=true},
        {.name="dispatch-stream-new-relative",.dispatch=true,.loaded_related=true,.relative_stream=true,.stream=true},
        {.name="dispatch-stream-directory-base",.dispatch=true,.stream=true,.existing_stream=true,.existing_directory=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-stream-missing-base",.dispatch=true,.stream=true,.missing_base=true},
        {.name="dispatch-stream-missing-base-borrowed",.dispatch=true,.loaded_related=true,.stream=true,.missing_base=true},
        {.name="dispatch-file-collision",.dispatch=true,.existing_entry=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="dispatch-file-new",.dispatch=true},
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
        {.name="reaped-stream-warm",.stream=true,.existing_stream=true,.reaped_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="reaped-stream-warm-insensitive",.stream=true,.existing_stream=true,.reaped_stream=true,.insensitive=true,.stream_name=L"ExTrA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="reaped-stream-cold",.stream=true,.existing_stream=true,.reaped_stream=true,.cold_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="reaped-stream-cold-insensitive",.stream=true,.existing_stream=true,.reaped_stream=true,.cold_stream=true,.insensitive=true,.cached_stream_name=L"eXtRa",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="reaped-stream-paging",.stream=true,.existing_stream=true,.reaped_stream=true,.cold_stream=true,.paging=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="reaped-stream-directory-base",.stream=true,.existing_stream=true,.reaped_stream=true,.existing_directory=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="reaped-stream-uppercase-error",.stream=true,.existing_stream=true,.reaped_stream=true,.insensitive=true,.fault=UPPERCASE_ERROR,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="reaped-stream-xattr-name-error",.stream=true,.existing_stream=true,.reaped_stream=true,.fault=POOL,.allocator="open_fcb_stream",.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="reaped-stream-fcb-error",.stream=true,.existing_stream=true,.reaped_stream=true,.fault=FCB_ALLOC,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="reaped-stream-nonpaged-error",.stream=true,.existing_stream=true,.reaped_stream=true,.fault=NP_ALLOC,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="reaped-stream-read-error",.stream=true,.existing_stream=true,.reaped_stream=true,.fault=XATTR_ERROR,.expected=STATUS_INTERNAL_ERROR},
        {.name="reaped-stream-metadata-error",.stream=true,.existing_stream=true,.reaped_stream=true,.fault=TREE_ERROR,.index=2,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="reaped-stream-fileref-error",.stream=true,.existing_stream=true,.reaped_stream=true,.fault=FILEREF_ALLOC,.index=2,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="reaped-stream-publication",.stream=true,.existing_stream=true,.reaped_stream=true,.install_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="reaped-stream-distinct-sensitive",.stream=true,.existing_stream=true,.reaped_stream=true,.cold_stream=true,.stream_name=L"EXTRA"},
        {.name="reaped-stream-distinct-insensitive",.stream=true,.existing_stream=true,.reaped_stream=true,.insensitive=true,.stream_name=L"bravo"},
        {.name="open-stream-metadata-error",.stream=true,.existing_stream=true,.reaped_stream=true,.reopen_only=true,.fault=TREE_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
        {.name="open-stream-wrong-key",.stream=true,.existing_stream=true,.reaped_stream=true,.reopen_only=true,.bad_stream_key=true,.expected=STATUS_INTERNAL_ERROR},
        {.name="open-stream-short-item",.stream=true,.existing_stream=true,.reaped_stream=true,.reopen_only=true,.short_stream_item=true,.expected=STATUS_INTERNAL_ERROR},
        {.name="open-stream-publication",.stream=true,.existing_stream=true,.reaped_stream=true,.reopen_only=true,.install_stream=true},
        {.name="stream-collision-sensitive",.stream=true,.existing_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="stream-collision-insensitive",.stream=true,.existing_stream=true,.insensitive=true,.stream_name=L"ExTrA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="stream-collision-cached-upper",.stream=true,.existing_stream=true,.insensitive=true,.cached_stream_name=L"EXTRA",.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="stream-collision-nonpaged",.stream=true,.existing_stream=true,.paging=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="stream-collision-directory-base",.stream=true,.existing_stream=true,.existing_directory=true,.expected=STATUS_OBJECT_NAME_COLLISION},
        {.name="stream-distinct-sensitive",.stream=true,.existing_stream=true,.stream_name=L"EXTRA"},
        {.name="stream-distinct-insensitive",.stream=true,.existing_stream=true,.insensitive=true,.stream_name=L"bravo"},
        {.name="stream-distinct-longer",.stream=true,.existing_stream=true,.insensitive=true,.stream_name=L"extralonger"},
        {.name="stream-distinct-shorter",.stream=true,.existing_stream=true,.insensitive=true,.stream_name=L"ext"},
        {.name="stream-collision-uppercase-failure",.stream=true,.existing_stream=true,.fault=UPPERCASE_ERROR,.index=1,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-collision-name-failure",.stream=true,.existing_stream=true,.fault=POOL,.allocator="create_stream",.index=4,.expected=STATUS_INSUFFICIENT_RESOURCES},
        {.name="stream-collision-fileref-failure",.stream=true,.existing_stream=true,.fault=FILEREF_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
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
