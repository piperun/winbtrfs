/* Service models for namespace lookup, permissions and notifications. */
static unsigned subject_contexts, clear_calls, rollback_calls, path_calls, notifications, move_calls;
static file_ref *source_ref, *destination, *collision, *old_parent_hold;
static bool deny_access, children_open, cross_volume, fail_move;
static bool deny_delete, fail_delete, fail_utf8_size, fail_utf8_fill, fail_cache_flush;
static NTSTATUS parent_lookup_status;
static unsigned fail_path_call;

static void ExAcquireResourceSharedLite(ERESOURCE* resource, bool wait) {
    CHECK(wait && !resource->held);
    resource->held = 1;
}
static void ExAcquireResourceExclusiveLite(ERESOURCE* resource, bool wait) {
    CHECK(wait && !resource->held);
    resource->held = 2;
}
static void ExReleaseResourceLite(ERESOURCE* resource) { CHECK(resource->held); resource->held = 0; }
static void SeCaptureSubjectContext(SECURITY_SUBJECT_CONTEXT* context) { *context = 1; subject_contexts++; }
static void SeReleaseSubjectContext(SECURITY_SUBJECT_CONTEXT* context) { CHECK(*context == 1 && subject_contexts); subject_contexts--; }
static bool SeAccessCheck(void* sd, SECURITY_SUBJECT_CONTEXT* context, bool locked, ACCESS_MASK desired,
                         ACCESS_MASK previous, void* privileges, void* mapping, int mode,
                         ACCESS_MASK* access, NTSTATUS* status) {
    (void)sd; (void)locked; (void)desired; (void)previous; (void)privileges; (void)mapping; (void)mode;
    CHECK(*context == 1);
    *access = desired;
    bool denied = deny_access || (deny_delete && desired == DELETE);
    *status = denied ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
    return !denied;
}
static void* IoGetFileObjectGenericMapping(void) { return NULL; }
static void CcFlushCache(SECTION_OBJECT_POINTERS* section, void* offset, ULONG length, IO_STATUS_BLOCK* iosb) {
    (void)offset; (void)length;
    CHECK(section && section->DataSectionObject);
    iosb->Status = fail_cache_flush ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
static bool has_open_children(file_ref* ref) { return children_open && ref == source_ref; }
static NTSTATUS delete_fileref(file_ref* ref, void* object, bool posix, PIRP irp, LIST_ENTRY* rollback) {
    (void)object; (void)posix; CHECK(ref == collision && irp && rollback);
    if (fail_delete)
        return STATUS_UNSUCCESSFUL;
    ref->deleted = true;
    return STATUS_SUCCESS;
}
static void clear_rollback(LIST_ENTRY* list) { CHECK(list->Flink == list); clear_calls++; }
static void do_rollback(device_extension* volume, LIST_ENTRY* list) { CHECK(volume && list->Flink == list); rollback_calls++; }
static NTSTATUS open_fileref(device_extension* volume, file_ref** out, UNICODE_STRING* name,
                            file_ref* related, bool parent, void* final, void* parsed, int pool,
                            bool sensitive, PIRP irp) {
    (void)final; (void)parsed; (void)sensitive;
    CHECK(volume && name && irp && pool == PagedPool);
    if (parent) {
        CHECK(!related);
        if (!NT_SUCCESS(parent_lookup_status) || parent_lookup_status == STATUS_REPARSE)
            return parent_lookup_status;
        *out = destination;
        increase_fileref_refcount(destination);
        return STATUS_SUCCESS;
    }
    if (!collision)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    *out = collision;
    increase_fileref_refcount(collision);
    return STATUS_SUCCESS;
}
static NTSTATUS utf16_to_utf8(char* out, ULONG capacity, ULONG* length, WCHAR* text, ULONG bytes) {
    if ((out && fail_utf8_fill) || (!out && fail_utf8_size))
        return STATUS_UNSUCCESSFUL;
    ULONG needed = 0;
    for (unsigned i = 0; i < bytes / 2; i++) {
        unsigned c = text[i];
        unsigned n = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (out) {
            CHECK(needed + n <= capacity);
            if (n == 1)
                out[needed] = (char)c;
            else if (n == 2) {
                out[needed] = (char)(0xc0 | (c >> 6));
                out[needed + 1] = (char)(0x80 | (c & 63));
            } else {
                out[needed] = (char)(0xe0 | (c >> 12));
                out[needed + 1] = (char)(0x80 | ((c >> 6) & 63));
                out[needed + 2] = (char)(0x80 | (c & 63));
            }
        }
        needed += n;
    }
    *length = needed;
    return STATUS_SUCCESS;
}
static NTSTATUS RtlUpcaseUnicodeString(UNICODE_STRING* out, UNICODE_STRING* in, bool alloc) {
    CHECK(alloc);
    out->Buffer = allocate(in->Length);
    if (!out->Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    out->Length = out->MaximumLength = in->Length;
    for (unsigned i = 0; i < in->Length / 2; i++)
        out->Buffer[i] = in->Buffer[i] >= 'a' && in->Buffer[i] <= 'z' ? in->Buffer[i] - 32 : in->Buffer[i];
    return STATUS_SUCCESS;
}
static NTSTATUS fileref_get_filename(file_ref* ref, UNICODE_STRING* out, USHORT* offset, ULONG* needed) {
    CHECK(ref == source_ref);
    if (++path_calls == fail_path_call)
        return STATUS_UNSUCCESSFUL;
    *offset = 0;
    *needed = ref->dc->name.Length;
    if (out->MaximumLength < *needed)
        return STATUS_BUFFER_OVERFLOW;
    out->Length = (USHORT)*needed;
    memcpy(out->Buffer, ref->dc->name.Buffer, *needed);
    return STATUS_SUCCESS;
}
static void remove_dir_child_from_hash_lists(fcb* node, dir_child* child) { CHECK(node->nonpaged->dir_children_lock.held == 2 && child); }
static void insert_dir_child_into_hash_lists(fcb* node, dir_child* child) { CHECK(node->nonpaged->dir_children_lock.held == 2 && child); }
static void mark_fileref_dirty(file_ref* ref) { CHECK(ref == source_ref); }
static void mark_fcb_dirty(fcb* node) { CHECK(node && node->inode_item_changed); }
static bool is_subvol_readonly(root* tree, PIRP irp) { CHECK(tree && irp); return false; }
static void KeQuerySystemTime(LARGE_INTEGER* time) { time->QuadPart = 42; }
static void win_time_to_unix(LARGE_INTEGER time, struct btrfs_timespec* now) { now->sec = time.QuadPart; now->nsec = 0; }
static void send_notification_fileref(file_ref* ref, ULONG filter, ULONG action, void* stream) {
    CHECK(ref == source_ref->parent && filter == FILE_NOTIFY_CHANGE_LAST_WRITE && action == FILE_ACTION_MODIFIED && !stream);
    notifications++;
}
static void FsRtlNotifyFilterReportChange(void* sync, LIST_ENTRY* list, PSTRING name, USHORT offset,
    void* stream, void* parent, ULONG filter, ULONG action, void* target, void* context) {
    (void)sync; (void)offset; (void)stream; (void)parent; (void)filter; (void)target; (void)context;
    CHECK(list && name->Length && (action == FILE_ACTION_RENAMED_OLD_NAME || action == FILE_ACTION_RENAMED_NEW_NAME));
    notifications++;
}
/* Cross-directory helpers consume the destination hold on success. Their
 * namespace mutations are outside this dispatch/reference regression. */
static NTSTATUS move_across_directories(file_ref* ref, ccb* context, file_ref* related,
                                       ANSI_STRING* utf8, UNICODE_STRING* name, PIRP irp) {
    CHECK(ref == source_ref && context && related == destination && utf8 && name && irp);
    CHECK(utf8->Buffer == request_utf8 && request_frees == 0);
    move_calls++;
    if (fail_move)
        return STATUS_INSUFFICIENT_RESOURCES;
    old_parent_hold = ref->parent;
    ref->parent = related;
    return STATUS_SUCCESS;
}
static NTSTATUS move_across_subvols(file_ref* ref, ccb* context, file_ref* related,
                                   ANSI_STRING* utf8, UNICODE_STRING* name, PIRP irp) {
    CHECK(cross_volume);
    return move_across_directories(ref, context, related, utf8, name, irp);
}
static NTSTATUS rename_stream(device_extension* volume, file_ref* ref, ccb* context,
    FILE_RENAME_INFORMATION_EX* request, ULONG flags, PIRP irp, LIST_ENTRY* rollback) {
    (void)volume; (void)ref; (void)context; (void)request; (void)flags; (void)irp; (void)rollback;
    return STATUS_SUCCESS;
}
static NTSTATUS rename_file_to_stream(device_extension* volume, file_ref* ref, ccb* context,
    FILE_RENAME_INFORMATION_EX* request, ULONG flags, PIRP irp, LIST_ENTRY* rollback) {
    return rename_stream(volume, ref, context, request, flags, irp, rollback);
}
