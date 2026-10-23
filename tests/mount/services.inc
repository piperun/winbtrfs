/* Ownership and lifetime models; all allocated objects are tracked. */
static device_extension volume;
static DEVICE_OBJECT new_device;
static root subvolume;
static unsigned allocations, resource_live, lookaside_deletions, device_deletions, freed_fcbs;
static bool initialized_lists;
static unsigned cache_mutations;
static NTSTATUS batch_failure;
static NTSTATUS commit_batch_list(device_extension* Vcb, LIST_ENTRY* list, void* Irp) {
    (void)Vcb; (void)list; (void)Irp; return batch_failure;
}
static struct { void* p; LOOKASIDE* owner; } owned[256];

static void* allocate(size_t size, LOOKASIDE* owner) {
    CHECK(size && (!owner || owner->live));
    void* p = calloc(1, size); CHECK(p);
    for (unsigned i = 0; i < 256; i++) if (!owned[i].p) {
        owned[i].p = p; owned[i].owner = owner; allocations++;
        if (owner) owner->outstanding++;
        return p;
    }
    CHECK(false); return NULL;
}
static void release(void* p, LOOKASIDE* owner) {
    CHECK(p);
    if (owner && !owner->live) {
        fprintf(stderr, "FCB allocation returned to a deleted %s lookaside list\n", owner->nonpaged ? "nonpaged" : "paged");
        CHECK(false);
    }
    for (unsigned i = 0; i < 256; i++) if (owned[i].p == p) {
        CHECK(owned[i].owner == owner);
        owned[i].p = NULL; allocations--; free(p);
        if (owner) { CHECK(owner->outstanding); owner->outstanding--; owner->returned++; }
        return;
    }
    CHECK(false);
}
/* Pool allocations contain old bytes until production explicitly initializes them. */
static void* ExAllocatePoolWithTag(int pool, size_t size, unsigned tag) {
    (void)pool; (void)tag;
    void* p = allocate(size, NULL); memset(p, 0xa5, size); return p;
}
static void ExFreePool(void* p) { release(p, NULL); }
static unsigned notify_uninitializations;
static void FsRtlNotifyInitializeSync(void** sync) { CHECK(!*sync); *sync = allocate(32, NULL); }
void FsRtlNotifyUninitializeSync(void** sync) {
    CHECK(*sync); ExFreePool(*sync); *sync = NULL; notify_uninitializations++;
}
static void ExFreeToNPagedLookasideList(LOOKASIDE* list, void* p) {
    CHECK(list->nonpaged); release(p, list);
}
static void ExFreeToPagedLookasideList(LOOKASIDE* list, void* p) {
    CHECK(!list->nonpaged); release(p, list);
}
static void delete_lookaside(LOOKASIDE* list, bool nonpaged) {
    CHECK(list->live && list->nonpaged == nonpaged);
    list->live = false; lookaside_deletions++;
}
static void ExDeletePagedLookasideList(LOOKASIDE* list) { delete_lookaside(list, false); }
static void ExDeleteNPagedLookasideList(LOOKASIDE* list) { delete_lookaside(list, true); }
static void init_resource(ERESOURCE* r) { CHECK(!r->live); r->live = true; resource_live++; }
static void ExAcquireResourceExclusiveLite(ERESOURCE* r, bool wait) {
    CHECK(wait && r->live && !r->held); r->held++;
}
static LONG InterlockedIncrement(LONG* count) { CHECK(*count >= 0); return ++*count; }
static void ExReleaseResourceLite(ERESOURCE* r) { CHECK(r->live && r->held); r->held--; }
static void ExDeleteResourceLite(ERESOURCE* r) { CHECK(r->live && !r->held); r->live = false; resource_live--; }
static LONG InterlockedDecrement(LONG* count) { CHECK(*count > 0); return --*count; }
/* The root stream file; dropping mount's reference leaves the FCB it points to with the cache manager. */
static int root_stream_file;
static unsigned root_file_dereferences;
static void ObDereferenceObject(void* object) { CHECK(object == &root_stream_file); root_file_dereferences++; }
static void FsRtlTeardownPerStreamContexts(int* header) { CHECK(*header == 1); *header = 0; freed_fcbs++; }
static void FsRtlUninitializeFileLock(int* lock) { CHECK(*lock == 1); *lock = 0; }
static void FsRtlUninitializeOplock(int* oplock) { CHECK(*oplock == 1); *oplock = 0; }
/* Allocations and resources a case expects to outlive the cleanup. */
static unsigned kept_allocations, kept_resources;
static void IoDeleteDevice(PDEVICE_OBJECT dev) {
    CHECK(dev == &new_device && !dev->deleted && dev->DeviceExtension == &volume);
    CHECK(resource_live == kept_resources && allocations == kept_allocations);
    CHECK(lookaside_deletions == (initialized_lists ? 8u : 0u));
    dev->deleted = true; device_deletions++;
}

/* Subvolumes of the dummy and root FCBs: the root tree and the default subvolume once roots are loaded. */
static root *dummy_subvol = &subvolume, *root_subvol = &subvolume;

static void init_fcb(fcb* f, int pool_type, LONG refcount, uint32_t hash, root* subvol) {
    f->Vcb = &volume; f->pool_type = pool_type; f->refcount = refcount;
    f->hash = hash; f->subvol = subvol;
    f->nonpaged = allocate(sizeof(*f->nonpaged), &volume.fcb_np_lookaside);
    init_resource(&f->nonpaged->resource); init_resource(&f->nonpaged->paging_resource);
    init_resource(&f->nonpaged->dir_children_lock);
    InitializeListHead(&f->extents); InitializeListHead(&f->hardlinks);
    InitializeListHead(&f->xattrs); InitializeListHead(&f->dir_children_index);
    f->Header = f->lock = f->oplock = 1;
}

static fcb* make_fcb(root* subvol, bool payload) {
    fcb* f = allocate(sizeof(*f), NULL);
    init_fcb(f, NonPagedPool, 1, 0xabc00000, subvol);
    if (payload) {
        f->sd = allocate(32, NULL); f->ea_xattr.Buffer = allocate(48, NULL);
        f->hash_ptrs = allocate(sizeof(LIST_ENTRY*) * 256, NULL);
        f->hash_ptrs_uc = allocate(sizeof(LIST_ENTRY*) * 256, NULL);
    }
    return f;
}

/* Metadata mount loads before it creates its FCBs, in load order; each stage keeps the earlier ones. */
enum { LOADED_NONE, LOADED_SYSTEM_CHUNKS, LOADED_CHUNK_TREE, LOADED_ROOTS, LOADED_CACHES };
static unsigned loaded_trees, loaded_items;

static root* make_root(void) {
    root* r = allocate(sizeof(*r), NULL);
    r->nonpaged = allocate(sizeof(*r->nonpaged), NULL);
    init_resource(&r->nonpaged->load_tree_lock);
    InitializeListHead(&r->fcbs);
    InsertTailList(&volume.roots, &r->list_entry);
    return r;
}
/* Each tree gets its own hash bucket, inserted in hash order like the driver's hash list. */
static tree* make_tree(root* r, uint8_t level, tree_data* paritem, unsigned items) {
    tree* t = allocate(sizeof(*t), NULL);
    t->nonpaged = allocate(16, NULL); t->buf = allocate(64, NULL);
    t->Vcb = &volume; t->root = r; t->header.level = level; t->hash = ++loaded_trees << 24;
    InitializeListHead(&t->itemlist);
    for (unsigned i = 0; i < items; i++) {
        tree_data* td = allocate(sizeof(*td), &volume.tree_data_lookaside);
        if (level == 0 && i == 0) { td->inserted = true; td->size = 24; td->data = allocate(24, NULL); }
        InsertTailList(&t->itemlist, &td->list_entry); loaded_items++;
    }
    InsertTailList(&volume.trees, &t->list_entry);
    InsertTailList(&volume.trees_hash, &t->list_entry_hash);
    volume.trees_ptrs[t->hash >> 24] = &t->list_entry_hash;
    if (paritem) {
        t->paritem = paritem; paritem->treeholder.tree = t;
    } else r->treeholder.tree = t;
    return t;
}
static void make_two_level_tree(root* r) {
    tree* top = make_tree(r, 1, NULL, 2);
    for (LIST_ENTRY* le = top->itemlist.Flink; le != &top->itemlist; le = le->Flink) {
        tree* leaf = make_tree(r, 0, CONTAINING_RECORD(le, tree_data, list_entry), 2);
        leaf->parent = top;
    }
}
static chunk* make_chunk(void) {
    chunk* c = allocate(sizeof(*c), NULL);
    c->chunk_item = allocate(80, NULL); c->devices = allocate(2 * sizeof(device*), NULL);
    InitializeListHead(&c->space); InitializeListHead(&c->deleting);
    InitializeListHead(&c->changed_extents);
    if (cache_mutations & 2) for (unsigned i = 0; i < 2; i++) {
        changed_extent* ce = allocate(sizeof(*ce), NULL);
        InitializeListHead(&ce->refs); InitializeListHead(&ce->old_refs);
        InsertTailList(&c->changed_extents, &ce->list_entry);
        for (unsigned j = 0; j <= i; j++)
            InsertTailList(&ce->refs, &((changed_extent_ref*)allocate(sizeof(changed_extent_ref), NULL))->list_entry);
        for (unsigned j = 0; j < i; j++)
            InsertTailList(&ce->old_refs, &((changed_extent_ref*)allocate(sizeof(changed_extent_ref), NULL))->list_entry);
    }
    for (unsigned i = 0; i < 2; i++) InsertTailList(&c->space, &((space*)allocate(sizeof(space), NULL))->list_entry);
    InsertTailList(&c->deleting, &((space*)allocate(sizeof(space), NULL))->list_entry);
    init_resource(&c->lock); init_resource(&c->changed_extents_lock);
    init_resource(&c->range_locks_lock); init_resource(&c->partial_stripes_lock);
    InsertTailList(&volume.chunks, &c->list_entry);
    return c;
}
/* Free-space cache FCBs come from the paged FCB lookaside and live in the root tree's subvolume. */
static fcb* make_cache_fcb(root* r, uint32_t hash, LONG refcount) {
    fcb* f = allocate(sizeof(*f), &volume.fcb_lookaside);
    init_fcb(f, PagedPool, refcount, hash, r);
    InsertTailList(&r->fcbs, &f->list_entry); r->fcbs_ptrs[hash >> 24] = &f->list_entry;
    InsertTailList(&volume.all_fcbs, &f->list_entry_all);
    return f;
}
static void make_metadata(unsigned loaded) {
    if (loaded < LOADED_SYSTEM_CHUNKS) return;
    InitializeListHead(&volume.roots);
    root* chunk_root = make_root();
    InitializeListHead(&volume.sys_chunks);
    for (unsigned i = 0; i < 2; i++) {
        sys_chunk* sc = allocate(sizeof(*sc), NULL); sc->data = allocate(97, NULL);
        InsertTailList(&volume.sys_chunks, &sc->list_entry);
    }
    if (loaded < LOADED_CHUNK_TREE) return;
    CHECK(initialized_lists);
    InitializeListHead(&volume.chunks); InitializeListHead(&volume.trees);
    InitializeListHead(&volume.trees_hash); InitializeListHead(&volume.all_fcbs);
    InitializeListHead(&volume.dirty_fcbs);
    make_tree(chunk_root, 0, NULL, 3);
    chunk* first = make_chunk(); chunk* second = make_chunk();
    if (loaded < LOADED_ROOTS) return;
    root* root_root = dummy_subvol = make_root(); make_two_level_tree(root_root);
    make_two_level_tree(make_root());
    root_subvol = make_root(); make_two_level_tree(root_subvol);
    if (loaded < LOADED_CACHES) return;
    first->cache = make_cache_fcb(root_root, 0x01000000, 1);
    /* A cache whose load failed was released with free_fcb but stays listed until reaped. */
    make_cache_fcb(root_root, 0x02000000, 0);
    /* Two free-space headers naming one inode: open_fcb returns the same FCB with a second reference. */
    chunk* third = make_chunk();
    second->cache = third->cache = make_cache_fcb(root_root, 0x03000000, 2);
    if (cache_mutations & 1) for (LIST_ENTRY* le = volume.all_fcbs.Flink; le != &volume.all_fcbs; le = le->Flink) {
        fcb* f = CONTAINING_RECORD(le, fcb, list_entry_all);
        mark_fcb_dirty(f); LONG marked = f->refcount; mark_fcb_dirty(f); CHECK(f->refcount == marked);
    }
}
/* The production AVL/root invariant is exercised by changed-extents checks. */
static void initialize_changed_extents(chunk* c) {
    CHECK(IsListEmpty(&c->changed_extents));
    InitializeListHead(&c->changed_extents);
}

void trim_destroy(device_extension* Vcb) {CHECK(Vcb==&volume);}
