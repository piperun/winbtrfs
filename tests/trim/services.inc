/* Deterministic lower-driver completion and allocation models. */
enum { MAX_DEVICES = 6, MAX_RANGES = 4, MAX_POOLS = 40 };
static struct { void* pointer; size_t bytes; } pools[MAX_POOLS];
static PIRP irps[MAX_DEVICES], queued[MAX_DEVICES];
static DEVICE_DATA_SET_RANGE expected_ranges[MAX_DEVICES][MAX_RANGES];
static unsigned range_counts[MAX_DEVICES], faults[MAX_DEVICES];
static unsigned live, live_irps, eligible, dmdsa_attempts, irp_attempts, batch_attempts;
static unsigned submitted, completed, pending, queue_count, signals, waits, timing;
static unsigned current_slot;
static bool executing, fail_batch;
static void* stripe_pool;
static KEVENT* active_event;
static NTSTATUS completion_status;
static device_extension* active_volume;

static void* allocate(int pool, size_t bytes) {
    if (executing && pool == NonPagedPool) {
        batch_attempts++;
        if (fail_batch) return NULL;
    } else if (executing) {
        current_slot = dmdsa_attempts++;
        CHECK(current_slot < eligible);
        if (faults[current_slot] == 1) return NULL;
    }
    void* pointer = malloc(bytes);
    CHECK(pointer);
    memset(pointer, 0xa5, bytes);
    for (unsigned i = 0; i < MAX_POOLS; i++) {
        if (!pools[i].pointer) {
            pools[i].pointer = pointer; pools[i].bytes = bytes; live++;
            if (executing && pool == NonPagedPool) stripe_pool = pointer;
            return pointer;
        }
    }
    CHECK(false);
    return NULL;
}
static void ExFreePool(void* pointer) {
    CHECK(pointer);
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (irps[i] && irps[i]->submitted && !irps[i]->completed)
            CHECK(pointer != irps[i]->AssociatedIrp.SystemBuffer && pointer != stripe_pool);
    for (unsigned i = 0; i < MAX_POOLS; i++) {
        if (pools[i].pointer == pointer) {
            pools[i].pointer = NULL; live--;
            if (pointer == stripe_pool) stripe_pool = NULL;
            free(pointer);
            return;
        }
    }
    CHECK(false);
}
static PIRP IoAllocateIrp(unsigned stack, bool charge) {
    CHECK(!charge && stack >= 4 && stack < 4 + MAX_DEVICES);
    irp_attempts++;
    if (faults[current_slot] == 2) return NULL;
    unsigned id = stack - 4;
    CHECK(!irps[id]);
    PIRP irp = calloc(1, sizeof(*irp));
    CHECK(irp);
    irp->device_id = id;
    irps[id] = irp; live_irps++;
    return irp;
}
static void IoFreeIrp(PIRP irp) {
    CHECK(irp && (!irp->submitted || irp->completed));
    CHECK(irps[irp->device_id] == irp);
    irps[irp->device_id] = NULL; live_irps--;
    free(irp);
}
static LONG InterlockedDecrement(LONG* value) { CHECK(*value > 0); return --*value; }
static void KeInitializeEvent(KEVENT* event, int type, bool state) {
    CHECK(type == NotificationEvent && !state && !active_event);
    event->signaled = false;
    active_event = event;
}
static void KeSetEvent(KEVENT* event, int boost, bool wait) {
    CHECK(event == active_event && !boost && !wait && !event->signaled);
    CHECK(dmdsa_attempts == eligible && pending == 0 && submitted == completed);
    CHECK(CONTAINING_RECORD(event, trim_context, Event)->left == 0);
    event->signaled = true; signals++;
}
static void IoSetCompletionRoutine(PIRP irp, COMPLETION completion, void* context, bool success, bool error, bool cancel) {
    CHECK(irp && completion && context && success && error && cancel);
    irp->completion = completion; irp->context = context;
}
static void complete_irp(PIRP irp) {
    CHECK(irp->submitted && !irp->completed && pending);
    irp->UserIosb->Status = completion_status;
    irp->completed = true; completed++; pending--;
    CHECK(irp->completion(NULL, irp, irp->context) == STATUS_MORE_PROCESSING_REQUIRED);
}
static NTSTATUS IoCallDriver(PDEVICE_OBJECT object, PIRP irp) {
    CHECK(object->id == irp->device_id && !active_volume->chunk_lock.held);
    CHECK(irp->stack.MajorFunction == IRP_MJ_DEVICE_CONTROL && irp->stack.FileObject);
    CHECK(irp->stack.Parameters.DeviceIoControl.IoControlCode == IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES);
    CHECK(irp->stack.Parameters.DeviceIoControl.OutputBufferLength == 0);
    CHECK(irp->Flags == IRP_BUFFERED_IO && !irp->UserBuffer && irp->UserIosb);
    DEVICE_MANAGE_DATA_SET_ATTRIBUTES* data = irp->AssociatedIrp.SystemBuffer;
    CHECK(data && data->Size == 28 && data->Action == DeviceDsmAction_Trim);
    CHECK(data->Flags == DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED && !data->ParameterBlockLength && !data->ParameterBlockOffset);
    CHECK(data->DataSetRangesOffset == 32 && data->DataSetRangesLength == 16 * range_counts[object->id]);
    CHECK(irp->stack.Parameters.DeviceIoControl.InputBufferLength == 32 + data->DataSetRangesLength);
    CHECK(memcmp((char*)data + 32, expected_ranges[object->id], data->DataSetRangesLength) == 0);
    irp->submitted = true; submitted++; pending++;
    if (timing == 0 || (timing == 3 && object->id % 2 == 0)) {
        complete_irp(irp);
        return completion_status;
    }
    CHECK(queue_count < MAX_DEVICES);
    queued[queue_count++] = irp;
    return STATUS_PENDING;
}
static NTSTATUS KeWaitForSingleObject(KEVENT* event, int reason, int mode, bool alert, void* timeout) {
    CHECK(event == active_event && reason == Executive && mode == KernelMode && !alert && !timeout);
    waits++;
    for (unsigned i = 0; i < queue_count; i++) {
        unsigned index = timing == 2 ? queue_count - i - 1 : i;
        complete_irp(queued[index]);
        queued[index] = NULL;
    }
    CHECK(!pending);
    if (!event->signaled) {
        fprintf(stderr, "%s: TRIM wait would hang after all %u submitted IRPs completed; left=%d\n",
                scenario, submitted, CONTAINING_RECORD(event, trim_context, Event)->left);
        exit(1);
    }
    CHECK(CONTAINING_RECORD(event, trim_context, Event)->left == 0);
    return STATUS_SUCCESS;
}
