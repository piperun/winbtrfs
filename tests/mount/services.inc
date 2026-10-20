/* Ownership and lifetime models; all allocated objects are tracked. */
static device_extension volume;
static DEVICE_OBJECT new_device;
static root subvolume;
static unsigned allocations, resource_live, lookaside_deletions, device_deletions, freed_fcbs;
static bool initialized_lists;
static struct { void* p; LOOKASIDE* owner; } owned[64];

static void* allocate(size_t size, LOOKASIDE* owner) {
    CHECK(size && (!owner || owner->live));
    void* p = calloc(1, size); CHECK(p);
    for (unsigned i = 0; i < 64; i++) if (!owned[i].p) {
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
    for (unsigned i = 0; i < 64; i++) if (owned[i].p == p) {
        CHECK(owned[i].owner == owner);
        owned[i].p = NULL; allocations--; free(p);
        if (owner) { CHECK(owner->outstanding); owner->outstanding--; owner->returned++; }
        return;
    }
    CHECK(false);
}
static void ExFreePool(void* p) { release(p, NULL); }
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
static void ExReleaseResourceLite(ERESOURCE* r) { CHECK(r->live && r->held); r->held--; }
static void ExDeleteResourceLite(ERESOURCE* r) { CHECK(r->live && !r->held); r->live = false; resource_live--; }
static LONG InterlockedDecrement(LONG* count) { CHECK(*count > 0); return --*count; }
static void ObDereferenceObject(void* object) { (void)object; CHECK(false); }
static void FsRtlTeardownPerStreamContexts(int* header) { CHECK(*header == 1); *header = 0; freed_fcbs++; }
static void FsRtlUninitializeFileLock(int* lock) { CHECK(*lock == 1); *lock = 0; }
static void FsRtlUninitializeOplock(int* oplock) { CHECK(*oplock == 1); *oplock = 0; }
static void IoDeleteDevice(PDEVICE_OBJECT dev) {
    CHECK(dev == &new_device && !dev->deleted && dev->DeviceExtension == &volume);
    CHECK(!resource_live && !allocations);
    CHECK(lookaside_deletions == (initialized_lists ? 8u : 0u));
    dev->deleted = true; device_deletions++;
}

static fcb* make_fcb(bool has_subvol, bool payload) {
    fcb* f = allocate(sizeof(*f), NULL);
    f->Vcb = &volume; f->pool_type = NonPagedPool; f->refcount = 1;
    f->hash = 0xabc00000; f->subvol = has_subvol ? &subvolume : NULL;
    f->nonpaged = allocate(sizeof(*f->nonpaged), &volume.fcb_np_lookaside);
    init_resource(&f->nonpaged->resource); init_resource(&f->nonpaged->paging_resource);
    init_resource(&f->nonpaged->dir_children_lock);
    InitializeListHead(&f->extents); InitializeListHead(&f->hardlinks);
    InitializeListHead(&f->xattrs); InitializeListHead(&f->dir_children_index);
    f->Header = f->lock = f->oplock = 1;
    if (payload) {
        f->sd = allocate(32, NULL); f->ea_xattr.Buffer = allocate(48, NULL);
        f->hash_ptrs = allocate(sizeof(LIST_ENTRY*) * 256, NULL);
        f->hash_ptrs_uc = allocate(sizeof(LIST_ENTRY*) * 256, NULL);
    }
    return f;
}
