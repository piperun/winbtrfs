/* Denying base-file creation must release the temporary filename before
 * returning, without creating an inode, publishing a child or sending events. */
static void check_missing_base_denial(file_ref* result, const UNICODE_STRING* name, NTSTATUS status, LIST_ENTRY* rollback) {
    if (!current->stream || !current->missing_base) return;
    if ((current->fault == POOL && !strcmp(current->allocator,"create_stream") && current->index == 1) ||
        (current->fault == NAME_ERROR && current->index == 2))
        CHECK(!access_checks);
    if (current->fault != ACCESS_ERROR || current->index > 1) return;
    CHECK(status == STATUS_ACCESS_DENIED && !result && access_checks == 1);
    CHECK(!access_state.SubjectSecurityContext && !directory_np.resource.held && !directory_np.dir_children_lock.held);
    CHECK(!volume.fcb_lookaside.allocations && !volume.fileref_lookaside.allocations);
    CHECK(IsListEmpty(&directory_fcb.dir_children_index) && IsListEmpty(rollback));
    CHECK(IsListEmpty(&volume.dirty_fcbs) && IsListEmpty(&volume.dirty_filerefs) && !notifications);
    unsigned copies=0;
    for (unsigned i=0;i<pool_count;i++) if (!strcmp(pools[i].caller,"create_stream")) {
        copies++;
        CHECK(pools[i].size == name->Length);
        if (pools[i].live)
            fprintf(stderr,"%s: access denied with %zu-byte copied filename still allocated\n",scenario,pools[i].size);
        CHECK(!pools[i].live);
    }
    CHECK(copies == 1);
}
