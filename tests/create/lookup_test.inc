/* Direct production directory lookup, including validation after upcasing. */
static void run_lookup(void) {
    setup();
    const WCHAR* text=current->requested_name ? current->requested_name : (const WCHAR*)L"alpha";
    unsigned length=0; while (text[length]) length++;
    UNICODE_STRING name={length*sizeof(WCHAR),length*sizeof(WCHAR),(WCHAR*)text};
    root* found_root=NULL; uint64_t inode=0; dir_child* dc=NULL;
    if (current->caller_holds_dirlock) ExAcquireResourceSharedLite(&directory_np.dir_children_lock,true);
    NTSTATUS status=find_file_in_dir(&name,&directory_fcb,&found_root,&inode,&dc,!current->insensitive);
    CHECK(status == current->expected && faults == (current->fault == NONE ? 0u : 1u));
    CHECK(directory_np.dir_children_lock.held == (current->caller_holds_dirlock ? 1u : 0u));
    if (current->caller_holds_dirlock) ExReleaseResourceLite(&directory_np.dir_children_lock);
    if (NT_SUCCESS(status)) CHECK(dc == &collision_dc && found_root == &subvol && inode == base_fcb.inode);
    else CHECK(!dc && !found_root && !inode);
    for (unsigned i=0;i<pool_count;i++) {
        if (pools[i].live) fprintf(stderr,"%s: lookup returned with %zu-byte %s buffer still allocated\n",scenario,pools[i].size,pools[i].caller);
        CHECK(!pools[i].live); free(pools[i].p);
    }
    CHECK(directory_ref.refcount == (have_base && !current->reaped_entry ? 2 : 1));
    CHECK(volume.fcb_lookaside.allocations == volume.fcb_lookaside.returns);
    CHECK(volume.fileref_lookaside.allocations == volume.fileref_lookaside.returns);
    free(cached_name); free(cached_name_uc);
    printf("PASS %s\n",scenario);
}
