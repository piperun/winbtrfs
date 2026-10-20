/* Deterministic storage, completion, checksum and pool service models. */
enum { MAX_DEVICES = 12, DISK_BYTES = 0x80000, MAX_POOLS = 64 };
static device_extension volume;
static chunk current_chunk;
static device devices[MAX_DEVICES];
static DEVICE_OBJECT objects[MAX_DEVICES];
static uint8_t *disk[MAX_DEVICES], *expected[MAX_DEVICES];
static uint64_t starts[MAX_DEVICES], bases[MAX_DEVICES], write_addresses[MAX_DEVICES];
static uint32_t lengths[MAX_DEVICES];
static PIRP irps[MAX_DEVICES], queued[MAX_DEVICES];
static struct { void* p; size_t size; } pools[MAX_POOLS];
static unsigned pool_live, irp_live, allocation_calls, read_count, completed, pending, queue_count;
static unsigned writes, recovery_logs, device_errors[4], timing;
static bool written[MAX_DEVICES], metadata, fail_write, fail_scratch, fail_context, allocation_failed;
static bool unrecoverable_case;
static unsigned unrecoverable_expected[MAX_DEVICES], unrecoverable_logs[MAX_DEVICES], corruption_events[MAX_DEVICES];
static uint64_t unrecoverable_addresses[MAX_DEVICES];
static int fail_read = -1;
static KEVENT* active_event;
static uint64_t request_offset;
static uint32_t request_size;

static uint32_t checksum(const void* data, size_t length) {
    const uint8_t* bytes = data;
    uint32_t result = 2166136261u;
    for (size_t i = 0; i < length; i++) result = (result ^ bytes[i]) * 16777619u;
    return result;
}

static size_t RtlCompareMemory(const void* a, const void* b, size_t size) {
    return memcmp(a, b, size) == 0 ? size : 0;
}

static bool check_sector_csum(device_extension* v, void* buf, void* csum) {
    CHECK(v == &volume && !metadata);
    uint32_t actual = checksum(buf, v->superblock.sectorsize);
    return memcmp(&actual, csum, sizeof(actual)) == 0;
}

static void get_tree_checksum(device_extension* v, struct btrfs_header* th, void* csum) {
    CHECK(v == &volume && metadata);
    uint32_t actual = checksum((uint8_t*)th + sizeof(th->csum), v->superblock.nodesize - sizeof(th->csum));
    memcpy(csum, &actual, sizeof(actual));
}

static bool check_tree_checksum(device_extension* v, struct btrfs_header* th) {
    uint32_t actual;
    get_tree_checksum(v, th, &actual);
    return memcmp(&actual, th->csum, sizeof(actual)) == 0;
}

static void do_calc_job(device_extension* v, uint8_t* data, uint32_t sectors, void* csum) {
    CHECK(v == &volume && !metadata);
    for (uint32_t i = 0; i < sectors; i++) {
        uint32_t value = checksum(data + i * v->superblock.sectorsize, v->superblock.sectorsize);
        memcpy((uint8_t*)csum + i * v->csum_size, &value, sizeof(value));
    }
}

static void log_error(device_extension* v, uint64_t address, uint64_t devid, bool is_metadata, bool recoverable, bool parity) {
    CHECK(v == &volume && devid >= 100 && devid < 100u + current_chunk.chunk_item->num_stripes);
    CHECK(address >= request_offset && address < request_offset + request_size);
    CHECK(is_metadata == metadata && !parity);
    unsigned i = (unsigned)(devid - 100);
    CHECK(devices[i].devobj != NULL);
    if (recoverable) {
        CHECK(!unrecoverable_case);
        recovery_logs++;
    } else {
        CHECK(unrecoverable_case && !metadata && unrecoverable_expected[i]);
        CHECK(address == unrecoverable_addresses[i]);
        unrecoverable_logs[i]++;
    }
}

static void log_device_error(device_extension* v, device* dev, int error) {
    CHECK(v == &volume && dev >= devices && dev < devices + MAX_DEVICES);
    CHECK(error == BTRFS_DEV_STAT_CORRUPTION_ERRS || error == BTRFS_DEV_STAT_READ_ERRS || error == BTRFS_DEV_STAT_WRITE_ERRS);
    CHECK(dev->devobj != NULL);
    if (error == BTRFS_DEV_STAT_CORRUPTION_ERRS) corruption_events[dev - devices]++;
    device_errors[error]++;
}

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK((pool == PagedPool || pool == NonPagedPool) && tag == ALLOC_TAG && size > 0);
    allocation_calls++;
    unsigned groups = current_chunk.chunk_item->num_stripes / current_chunk.chunk_item->sub_stripes;
    if ((fail_context && allocation_calls == 1) ||
        (fail_scratch && pool == NonPagedPool && size == groups * sizeof(uint32_t))) {
        allocation_failed = true; return NULL;
    }
    void* p = malloc(size); CHECK(p); memset(p, 0xa5, size);
    for (unsigned i = 0; i < MAX_POOLS; i++) {
        if (!pools[i].p) { pools[i].p = p; pools[i].size = size; pool_live++; return p; }
    }
    CHECK(false); return NULL;
}

static void ExFreePool(void* p) {
    CHECK(p && pending == 0);
    for (unsigned i = 0; i < MAX_POOLS; i++) {
        if (pools[i].p == p) { pools[i].p = NULL; pool_live--; free(p); return; }
    }
    CHECK(false);
}

static PIRP IoAllocateIrp(unsigned stack, bool charge) {
    CHECK(stack > 0 && stack <= MAX_DEVICES && !charge);
    unsigned i = stack - 1; CHECK(!irps[i]);
    PIRP irp = calloc(1, sizeof(*irp)); CHECK(irp);
    irp->id = i; irps[i] = irp; irp_live++; return irp;
}

static void IoFreeIrp(PIRP irp) {
    CHECK(irp && irps[irp->id] == irp && (!irp->submitted || irp->completed));
    CHECK(!irp->MdlAddress && !irp->AssociatedIrp.SystemBuffer);
    irps[irp->id] = NULL; irp_live--; free(irp);
}

static void* IoAllocateMdl(void* p, ULONG size, bool secondary, bool quota, PIRP irp) {
    UNUSED(p); UNUSED(size); UNUSED(secondary); UNUSED(quota); UNUSED(irp); CHECK(false); return NULL;
}
static void MmProbeAndLockPages(void* mdl, unsigned mode, unsigned access) {
    UNUSED(mdl); UNUSED(mode); UNUSED(access); CHECK(false);
}
static void IoFreeMdl(void* mdl) { UNUSED(mdl); CHECK(false); }
static void MmUnlockPages(void* mdl) { UNUSED(mdl); CHECK(false); }

static LONG InterlockedDecrement(LONG* value) { CHECK(*value > 0); return --*value; }
static void KeInitializeEvent(KEVENT* event, unsigned type, bool state) {
    CHECK(type == NotificationEvent && !state && !active_event); active_event = event; event->signaled = false;
}
static void KeSetEvent(KEVENT* event, unsigned boost, bool wait) {
    CHECK(event == active_event && !boost && !wait && !event->signaled && pending == 0);
    event->signaled = true;
}
static void IoSetCompletionRoutine(PIRP irp, COMPLETION callback, void* context, bool success, bool error, bool cancel) {
    CHECK(callback && context && success && error && cancel);
    irp->completion = callback; irp->context = context;
}
static void complete_irp(PIRP irp) {
    CHECK(irp->submitted && !irp->completed && pending > 0);
    irp->completed = true; pending--; completed++;
    CHECK(irp->completion(&objects[irp->id], irp, irp->context) == STATUS_MORE_PROCESSING_REQUIRED);
}
static NTSTATUS IoCallDriver(PDEVICE_OBJECT object, PIRP irp) {
    unsigned i = object->id;
    CHECK(irp == irps[i] && !irp->submitted && object->Flags == 0);
    CHECK(irp->stack.MajorFunction == IRP_MJ_READ && irp->stack.FileObject == devices[i].fileobj);
    CHECK(irp->stack.Parameters.Read.Length == lengths[i]);
    CHECK((uint64_t)irp->stack.Parameters.Read.ByteOffset.QuadPart == bases[i] + starts[i]);
    CHECK(starts[i] + lengths[i] <= DISK_BYTES && irp->UserBuffer && irp->UserIosb);
    irp->IoStatus.Status = (int)i == fail_read ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
    irp->IoStatus.Information = NT_SUCCESS(irp->IoStatus.Status) ? lengths[i] : 0;
    if (NT_SUCCESS(irp->IoStatus.Status)) memcpy(irp->UserBuffer, disk[i] + starts[i], lengths[i]);
    irp->submitted = true; pending++; read_count++;
    if (!timing) { complete_irp(irp); return irp->IoStatus.Status; }
    CHECK(queue_count < MAX_DEVICES); queued[queue_count++] = irp; return STATUS_PENDING;
}
static NTSTATUS KeWaitForSingleObject(KEVENT* event, unsigned reason, unsigned mode, bool alert, void* timeout) {
    CHECK(event == active_event && reason == Executive && mode == KernelMode && !alert && !timeout);
    for (unsigned j = queue_count; j > 0; j--) complete_irp(queued[j - 1]);
    CHECK(pending == 0 && event->signaled); return STATUS_SUCCESS;
}

static NTSTATUS write_data_phys(PDEVICE_OBJECT object, void* file, uint64_t address, void* data, uint32_t size) {
    unsigned i = object->id;
    CHECK(!devices[i].readonly && file == devices[i].fileobj && !written[i] && pending == 0);
    CHECK(address >= bases[i] && address - bases[i] + size <= DISK_BYTES);
    CHECK(size == lengths[i] && size > 0);
    written[i] = true; write_addresses[i] = address; writes++;
    if (fail_write) return STATUS_IO_DEVICE_ERROR;
    memcpy(disk[i] + address - bases[i], data, size); return STATUS_SUCCESS;
}

static NTSTATUS scrub_extent_dup(device_extension* v, chunk* c, uint64_t off, void* sum, scrub_context* ctx) {
    UNUSED(v); UNUSED(c); UNUSED(off); UNUSED(sum); UNUSED(ctx); CHECK(false); return STATUS_INTERNAL_ERROR;
}
static NTSTATUS scrub_extent_raid0(device_extension* v, chunk* c, uint64_t off, uint32_t len, uint16_t stripe, void* sum, scrub_context* ctx) {
    UNUSED(v); UNUSED(c); UNUSED(off); UNUSED(len); UNUSED(stripe); UNUSED(sum); UNUSED(ctx); CHECK(false); return STATUS_INTERNAL_ERROR;
}
