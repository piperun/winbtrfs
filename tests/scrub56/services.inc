/* Tracked Windows ownership, RAM disks and a controlled parity-verification seam. */
enum { MAX_DEVICES = 5, STRIPE_BYTES = 65536, BATCH_ROWS = 16, MAX_BATCHES = 3 };
enum fault { NO_FAULT, PAGE_LOCK, IRP_ALLOC, MDL_ALLOC, READ_ERROR, POOL_ALLOC, WRITE_ERROR };
typedef struct {
    bool raid6, direct, deferred, metadata;
    unsigned rows, repair_batches, repair_devices, missing;
    enum fault fault;
    unsigned fail_batch, fail_device, fail_allocation;
} scenario;
static scenario cfg;
static device_extension volume;
static chunk current_chunk;
static device devices[MAX_DEVICES], *device_ptrs[MAX_DEVICES];
static DEVICE_OBJECT objects[MAX_DEVICES];
static uint8_t *disk[MAX_DEVICES], *expected[MAX_DEVICES];
static unsigned disk_size, count, prepared[MAX_DEVICES], preparing;
static unsigned writes[MAX_BATCHES][MAX_DEVICES], wanted_writes[MAX_BATCHES][MAX_DEVICES];
static PIRP irps[MAX_DEVICES], queue[MAX_DEVICES];
static unsigned pending, queued, reads, completions, verified_rows, pool_live, mdl_live, mdl_used, range_locks;
static unsigned read_errors, write_errors, fault_hits, allocation_calls;
static jmp_buf page_exception;
static KEVENT* active_event;
static const uint64_t first_row = 3;
static struct { void* p; size_t size; } pools[64];
typedef struct { bool live, locked; void* buf; unsigned size, id, batch; } model_mdl;
static model_mdl mdls[MAX_DEVICES * MAX_BATCHES];

static bool inject(enum fault fault, unsigned batch, unsigned id) {
    if (cfg.fault != fault || cfg.fail_batch != batch || cfg.fail_device != id) return false;
    fault_hits++; return true;
}

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag) {
    CHECK((pool == PagedPool || pool == NonPagedPool) && tag == ALLOC_TAG && size);
    allocation_calls++;
    if (cfg.fault == POOL_ALLOC && allocation_calls == cfg.fail_allocation) {
        fault_hits++; return NULL;
    }
    void* p = malloc(size); CHECK(p); memset(p, 0xa5, size);
    for (unsigned i = 0; i < 64; i++) if (!pools[i].p) {
        pools[i].p = p; pools[i].size = size; pool_live++; return p;
    }
    CHECK(false); return NULL;
}
static void ExFreePool(void* p) {
    CHECK(p && pending == 0);
    for (unsigned i = 0; i < mdl_used; i++) CHECK(!mdls[i].live || mdls[i].buf != p);
    for (unsigned i = 0; i < 64; i++) if (pools[i].p == p) {
        pools[i].p = NULL; pool_live--; free(p); return;
    }
    CHECK(false);
}
static uint64_t sector_align(uint64_t size, uint64_t align) { return (size + align - 1) / align * align; }
static void RtlInitializeBitMap(RTL_BITMAP* b, ULONG* p, ULONG bits) { b->Buffer = p; b->SizeOfBitMap = bits; }
static void RtlClearAllBits(RTL_BITMAP* b) { memset(b->Buffer, 0, ((b->SizeOfBitMap + 31) / 32) * sizeof(ULONG)); }
static void RtlSetBits(RTL_BITMAP* b, ULONG start, ULONG bits) {
    CHECK(start <= b->SizeOfBitMap && bits <= b->SizeOfBitMap - start);
    for (ULONG i = start; i < start + bits; i++) b->Buffer[i / 32] |= 1u << (i % 32);
}
static NTSTATUS find_item(device_extension* v, void* root, traverse_ptr* tp, struct btrfs_key* key, bool ignore, void* irp) {
    static tree_item sentinel;
    CHECK(v == &volume && root == v->extent_root && !ignore && !irp);
    CHECK(key->objectid == current_chunk.offset + first_row * (count - (cfg.raid6 ? 2 : 1)) * STRIPE_BYTES);
    sentinel.key.objectid = UINT64_MAX; tp->item = &sentinel; return STATUS_SUCCESS;
}
static bool find_next_item(device_extension* v, traverse_ptr* tp, traverse_ptr* next, bool ignore, void* irp) {
    UNUSED(v); UNUSED(tp); UNUSED(next); UNUSED(ignore); UNUSED(irp); CHECK(false); return false;
}
static NTSTATUS chunk_lock_range(device_extension* v, chunk* c, uint64_t off, uint64_t length) {
    CHECK(v == &volume && c == &current_chunk && !range_locks);
    CHECK(off == c->offset + first_row * (count - (cfg.raid6 ? 2 : 1)) * STRIPE_BYTES);
    CHECK(length == (uint64_t)cfg.rows * (count - (cfg.raid6 ? 2 : 1)) * STRIPE_BYTES);
    range_locks++; return STATUS_SUCCESS;
}
static void chunk_unlock_range(device_extension* v, chunk* c, uint64_t off, uint64_t length) {
    CHECK(range_locks == 1); range_locks--;
    UNUSED(v); UNUSED(c); UNUSED(off); UNUSED(length);
}
static PIRP IoAllocateIrp(unsigned stack, bool quota) {
    CHECK(stack && stack <= count && !quota);
    unsigned id = stack - 1, batch = prepared[id]++;
    CHECK(!irps[id] && batch < MAX_BATCHES); preparing = id;
    if (inject(IRP_ALLOC, batch, id)) return NULL;
    PIRP irp = calloc(1, sizeof(*irp)); CHECK(irp);
    irp->id = id; irp->batch = batch; irps[id] = irp; return irp;
}
static void IoFreeIrp(PIRP irp) {
    unsigned id;
    for (id = 0; id < count && irps[id] != irp; id++) {}
    CHECK(id < count); /* Reject an uninitialized stripe pointer without dereferencing it. */
    CHECK(!irp->submitted || irp->completed);
    CHECK(!irp->AssociatedIrp.SystemBuffer);
    irps[id] = NULL; free(irp);
}
static model_mdl* mdl_record(void* p) {
    for (unsigned i = 0; i < mdl_used; i++) if (p == &mdls[i]) return &mdls[i];
    CHECK(false); return NULL;
}
static void* IoAllocateMdl(void* buf, ULONG size, bool secondary, bool quota, PIRP irp) {
    CHECK(buf && size && !secondary && !quota && !irp && mdl_used < MAX_DEVICES * MAX_BATCHES);
    unsigned batch = prepared[preparing] - 1;
    if (inject(MDL_ALLOC, batch, preparing)) return NULL;
    model_mdl* mdl = &mdls[mdl_used++];
    *mdl = (model_mdl){ .live = true, .buf = buf, .size = size, .id = preparing, .batch = batch };
    mdl_live++; return mdl;
}
static void MmProbeAndLockPages(void* p, unsigned mode, unsigned access) {
    model_mdl* mdl = mdl_record(p);
    CHECK(mdl->live && !mdl->locked && mode == KernelMode && access == IoWriteAccess);
    if (inject(PAGE_LOCK, mdl->batch, mdl->id)) longjmp(page_exception, 1);
    mdl->locked = true;
}
static void MmUnlockPages(void* p) {
    model_mdl* mdl = mdl_record(p);
    CHECK(mdl->live && mdl->locked); mdl->locked = false;
}
static void IoFreeMdl(void* p) {
    model_mdl* mdl = mdl_record(p);
    CHECK(mdl->live && !mdl->locked); mdl->live = false; mdl_live--;
}
static LONG InterlockedDecrement(LONG* value) { CHECK(*value > 0); return --*value; }
static void KeInitializeEvent(KEVENT* event, unsigned type, bool state) {
    CHECK(type == NotificationEvent && !state && pending == 0);
    active_event = event; queued = 0; event->signaled = false;
}
static void KeSetEvent(KEVENT* event, unsigned boost, bool wait) {
    CHECK(event == active_event && !boost && !wait && !event->signaled && !pending);
    event->signaled = true;
}
static void IoSetCompletionRoutine(PIRP irp, COMPLETION cb, void* context, bool success, bool error, bool cancel) {
    CHECK(cb && context && success && error && cancel); irp->completion = cb; irp->context = context;
}
static void complete_irp(PIRP irp) {
    CHECK(irp->submitted && !irp->completed && pending);
    pending--; completions++; irp->completed = true;
    CHECK(irp->completion(&objects[irp->id], irp, irp->context) == STATUS_MORE_PROCESSING_REQUIRED);
}
static NTSTATUS IoCallDriver(PDEVICE_OBJECT object, PIRP irp) {
    unsigned id = object->id;
    CHECK(irp == irps[id] && !irp->submitted && irp->stack.MajorFunction == IRP_MJ_READ);
    CHECK(irp->stack.FileObject == devices[id].fileobj && irp->UserIosb);
    uint64_t off = (uint64_t)irp->stack.Parameters.Read.ByteOffset.QuadPart - current_chunk.chunk_item->stripe[id].offset;
    unsigned size = irp->stack.Parameters.Read.Length;
    CHECK(off == (first_row + irp->batch * BATCH_ROWS) * STRIPE_BYTES && off + size <= disk_size);
    CHECK(size == min(BATCH_ROWS, cfg.rows - irp->batch * BATCH_ROWS) * STRIPE_BYTES);
    void* buf = irp->UserBuffer;
    if (cfg.direct) {
        model_mdl* mdl = mdl_record(irp->MdlAddress);
        CHECK(mdl->live && mdl->locked && mdl->size == size); buf = mdl->buf;
    }
    CHECK(buf);
    bool fail = inject(READ_ERROR, irp->batch, id);
    memcpy(buf, disk[id] + off, fail ? size / 2 : size);
    irp->IoStatus = (IO_STATUS_BLOCK){ fail ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS, fail ? size / 2 : size };
    irp->submitted = true; reads++; pending++;
    if (!cfg.deferred) complete_irp(irp);
    else { CHECK(queued < count); queue[queued++] = irp; }
    return STATUS_PENDING;
}
static NTSTATUS KeWaitForSingleObject(KEVENT* event, unsigned reason, unsigned mode, bool alert, void* timeout) {
    CHECK(event == active_event && reason == Executive && mode == KernelMode && !alert && !timeout);
    for (unsigned i = queued; i > 0; i--) complete_irp(queue[i - 1]);
    CHECK(!pending && event->signaled); return STATUS_SUCCESS;
}
static void log_device_error(device_extension* v, device* dev, int error) {
    CHECK(v == &volume && dev == &devices[cfg.fail_device]);
    if (error == BTRFS_DEV_STAT_READ_ERRS) { CHECK(cfg.fault == READ_ERROR); read_errors++; }
    else { CHECK(error == BTRFS_DEV_STAT_WRITE_ERRS && cfg.fault == WRITE_ERROR); write_errors++; }
}
static NTSTATUS write_data_phys(PDEVICE_OBJECT obj, void* file, uint64_t address, void* data, uint32_t size) {
    unsigned id = obj->id;
    uint64_t off = address - current_chunk.chunk_item->stripe[id].offset;
    CHECK(file == devices[id].fileobj && off >= first_row * STRIPE_BYTES && off + size <= disk_size && !pending);
    unsigned batch = (off / STRIPE_BYTES - first_row) / BATCH_ROWS;
    CHECK(batch < MAX_BATCHES);
    if (++writes[batch][id] != wanted_writes[batch][id]) {
        fprintf(stderr, "unexpected repair write: batch %u, device %u\n", batch, id); CHECK(false);
    }
    CHECK(size == min(BATCH_ROWS, cfg.rows - batch * BATCH_ROWS) * STRIPE_BYTES);
    if (inject(WRITE_ERROR, batch, id)) return STATUS_IO_DEVICE_ERROR;
    memcpy(disk[id] + off, data, size); return STATUS_SUCCESS;
}
static void verify_stripe(device_extension* v, chunk* c, scrub_context_raid56* context,
                          uint64_t start, uint64_t bit_start, uint64_t num, uint16_t missing) {
    CHECK(v == &volume && c == &current_chunk && start == first_row && !pending && reads == completions);
    unsigned absent = 0;
    uint64_t off = (bit_start + num) * STRIPE_BYTES;
    unsigned batch = (bit_start - first_row) / BATCH_ROWS;
    for (unsigned id = 0; id < count; id++) {
        if (!devices[id].devobj) { absent++; continue; }
        CHECK(memcmp(context->stripes[id].buf + num * STRIPE_BYTES, disk[id] + off, STRIPE_BYTES) == 0);
        if (num == 0 && (cfg.repair_batches & (1u << batch)) && (cfg.repair_devices & (1u << id))) {
            context->stripes[id].buf[0] ^= 0x5a;
            if (cfg.fault != WRITE_ERROR || batch != cfg.fail_batch || id < cfg.fail_device)
                expected[id][off] ^= 0x5a;
            context->stripes[id].rewrite = true;
            wanted_writes[batch][id]++;
        }
    }
    CHECK(missing == absent); verified_rows++;
}
static void scrub_raid5_stripe(device_extension* v, chunk* c, scrub_context_raid56* ctx, uint64_t start,
                              uint64_t bits, uint64_t num, uint16_t missing) {
    CHECK(!cfg.raid6); verify_stripe(v, c, ctx, start, bits, num, missing);
}
static NTSTATUS scrub_raid6_stripe(device_extension* v, chunk* c, scrub_context_raid56* ctx, uint64_t start,
                                  uint64_t bits, uint64_t num, uint16_t missing) {
    CHECK(cfg.raid6); verify_stripe(v, c, ctx, start, bits, num, missing); return STATUS_SUCCESS;
}
