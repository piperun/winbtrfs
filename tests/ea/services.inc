/* Host boundary for the real EA dispatch functions, with balanced-state checks. */
static unsigned filesystem_depth, completions, dirty_calls, notifications;
static void FsRtlEnterFileSystem(void) { CHECK(filesystem_depth++ == 0); }
static void FsRtlExitFileSystem(void) { CHECK(filesystem_depth-- == 1); }
static bool is_top_level(PIRP irp) { CHECK(irp); return true; }
static void IoSetTopLevelIrp(void* irp) { CHECK(!irp); }
static void* map_user_buffer(PIRP irp, int priority) { (void)priority; return irp->buffer; }
static void IoCompleteRequest(PIRP irp, int boost) { CHECK(irp && !boost); completions++; }
static void ExAcquireResourceExclusiveLite(ERESOURCE* resource, bool wait) {
    CHECK(wait && resource->held++ == 0);
}
static void ExReleaseResourceLite(ERESOURCE* resource) { CHECK(resource->held-- == 1); }
static void KeQuerySystemTime(LARGE_INTEGER* time) { time->QuadPart = 42; }
static void win_time_to_unix(LARGE_INTEGER time, struct btrfs_timespec* now) {
    now->sec = time.QuadPart;
    now->nsec = 0;
}
static void mark_fcb_dirty(fcb* node) { CHECK(node->Header.Resource->held == 1); dirty_calls++; }
static void send_notification_fileref(file_ref* ref, ULONG filter, ULONG action, void* name) {
    CHECK(ref && filter == FILE_NOTIFY_CHANGE_EA && action == FILE_ACTION_MODIFIED && !name);
    notifications++;
}

/* Inputs are fixtures; enforce their chain bounds without replacing the parser. */
static NTSTATUS IoCheckEaBufferValidity(FILE_FULL_EA_INFORMATION* ea, ULONG length, ULONG* bad) {
    size_t offset = 0;
    for (;;) {
        *bad = (ULONG)offset;
        if (offset > length || length - offset < 8)
            return STATUS_INVALID_PARAMETER;
        FILE_FULL_EA_INFORMATION* item = (void*)((char*)ea + offset);
        size_t size = 8 + item->EaNameLength + 1 + item->EaValueLength;
        if (size > length - offset || item->EaName[item->EaNameLength] != 0)
            return STATUS_INVALID_PARAMETER;
        if (!item->NextEntryOffset)
            return STATUS_SUCCESS;
        if (item->NextEntryOffset < size || item->NextEntryOffset % 4)
            return STATUS_INVALID_PARAMETER;
        offset += item->NextEntryOffset;
    }
}
