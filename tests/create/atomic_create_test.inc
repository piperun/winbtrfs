/* ECP discovery and reparse work are service boundaries; file_create's
 * ownership, flags, failed-child cleanup and handle publication are real. */
static ATOMIC_CREATE_ECP_CONTEXT atomic_context;
static ECP_LIST atomic_list;
static REPARSE_DATA_BUFFER atomic_reparse;
static unsigned reparse_calls;

static NTSTATUS host_get_ecp_list(PIRP request, ECP_LIST** list) {
    CHECK(request == &irp);
    if (current->ecp_list_failure) return STATUS_IO_DEVICE_ERROR;
    *list=&atomic_list; return STATUS_SUCCESS;
}
static NTSTATUS host_get_next_ecp(ECP_LIST* list, void* previous, GUID* type, void** context, ULONG* size) {
    CHECK(list == &atomic_list);
    if (previous) { CHECK(previous == &atomic_context); return STATUS_NOT_FOUND; }
    *type=current->ecp_other_guid ? GUID_ECP_QUERY_ON_CREATE : GUID_ECP_ATOMIC_CREATE;
    *context=&atomic_context;
    *size=sizeof(atomic_context)-(current->ecp_short ? 1 : 0);
    return STATUS_SUCCESS;
}
static void prepare_atomic_create(void) {
    memset(&atomic_context,0,sizeof(atomic_context));
    atomic_context.Size=sizeof(atomic_context);
    atomic_context.InFlags=ATOMIC_CREATE_ECP_IN_FLAG_REPARSE_POINT_SPECIFIED;
    atomic_reparse.ReparseTag=0xa0000003; // Mount-point tag; payload validation is modeled.
    atomic_context.ReparseBuffer=&atomic_reparse;
    atomic_context.ReparseBufferLength=sizeof(atomic_reparse);
    if (current->ecp_ops) {
        atomic_context.InFlags |= ATOMIC_CREATE_ECP_IN_FLAG_OP_FLAGS_SPECIFIED;
        atomic_context.InOpFlags=ATOMIC_CREATE_ECP_IN_OP_FLAG_CASE_SENSITIVE_FLAGS_SPECIFIED;
        atomic_context.CaseSensitiveFlagsMask=FILE_CS_FLAG_CASE_SENSITIVE_DIR;
        atomic_context.InCaseSensitiveFlags=FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    }
}
static NTSTATUS host_set_reparse(fcb* node, PREPARSE_DATA_BUFFER data, unsigned size, void* a, void* b,
                                PIRP request, LIST_ENTRY* rollback) {
    CHECK(current->atomic_ecp && node && node->created && data == &atomic_reparse);
    CHECK(size == sizeof(atomic_reparse) && !a && !b && request == &irp && rollback);
    // set_reparse_point2/set_symlink call write_file2, which reads these
    // contexts and SectionObjectPointer from the IRP's FileObject.
    PFILE_OBJECT object=IoGetCurrentIrpStackLocation(request)->FileObject;
    CHECK(object && object->FsContext == node && object->FsContext2);
    CHECK(object->FsContext2->fileref->fcb == node);
    CHECK(object->SectionObjectPointer == &node->nonpaged->segment_object);
    reparse_calls++;
    if (hit(REPARSE_ERROR)) return STATUS_IO_DEVICE_ERROR;
    node->atts |= FILE_ATTRIBUTE_REPARSE_POINT;
    return STATUS_SUCCESS;
}
static void check_atomic_result(NTSTATUS status) {
    if (!current->atomic_ecp) { CHECK(!reparse_calls); return; }
    bool handled=NT_SUCCESS(status) && !current->ecp_short && !current->ecp_other_guid && !current->ecp_list_failure;
    CHECK(reparse_calls == (handled || current->fault == REPARSE_ERROR ? 1u : 0u));
    CHECK(!!(atomic_context.OutFlags & ATOMIC_CREATE_ECP_OUT_FLAG_REPARSE_POINT_SET) == handled);
    if (handled && current->ecp_ops) {
        CHECK(atomic_context.OutFlags & ATOMIC_CREATE_ECP_OUT_FLAG_OP_FLAGS_HONORED);
        CHECK(atomic_context.OutOpFlags & ATOMIC_CREATE_ECP_OUT_OP_FLAG_CASE_SENSITIVE_FLAGS_SET);
        CHECK(atomic_context.OutCaseSensitiveFlags == (current->directory ? FILE_CS_FLAG_CASE_SENSITIVE_DIR : 0));
    }
}
