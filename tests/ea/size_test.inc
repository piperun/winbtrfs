/* Boundaries and failed-update ownership, using the production dispatch path. */
enum size_shape { ONE, TWO, PADDED, SHRINK, FILTERED, UID_AND_DATA, BAD_GID, DELETE_ALL, MODE, DEVICE,
                  GID_AND_DATA, MODE_AND_DATA, BAD_MODE, BAD_UID, METADATA_ONLY, REPLACE, REPLACE_THEN_BAD };
typedef struct {
    const char* name;
    bool update, old;
    unsigned nodesize, total, fault;
    enum size_shape shape;
    NTSTATUS status;
} size_case;
static const size_case size_cases[] = {
    {"create-4k-below-limit", false, false, 4096, 3932, 0, ONE, STATUS_SUCCESS},
    {"create-4k-exact-limit", false, false, 4096, 3933, 0, ONE, STATUS_SUCCESS},
    {"create-4k-above-limit", false, false, 4096, 3934, 0, ONE, STATUS_EA_TOO_LARGE},
    {"create-16k-below-limit", false, false, 16384, 16220, 0, ONE, STATUS_SUCCESS},
    {"create-16k-exact-limit", false, false, 16384, 16221, 0, ONE, STATUS_SUCCESS},
    {"create-16k-above-limit", false, false, 16384, 16222, 0, ONE, STATUS_EA_TOO_LARGE},
    {"create-64k-below-limit", false, false, 65536, 65372, 0, ONE, STATUS_SUCCESS},
    {"create-64k-exact-limit", false, false, 65536, 65373, 0, ONE, STATUS_SUCCESS},
    {"create-64k-above-limit", false, false, 65536, 65374, 0, ONE, STATUS_EA_TOO_LARGE},
    {"create-65535-bytes", false, false, 65536, 65535, 0, ONE, STATUS_EA_TOO_LARGE},
    {"create-wraps-to-zero", false, false, 65536, 65536, 0, TWO, STATUS_EA_TOO_LARGE},
    {"create-wraps-to-one", false, false, 65536, 65537, 0, TWO, STATUS_EA_TOO_LARGE},
    {"create-multiple-large-records", false, false, 65536, 131069, 0, TWO, STATUS_EA_TOO_LARGE},
    {"create-padding-exact-limit", false, false, 4096, 3933, 0, PADDED, STATUS_SUCCESS},
    {"create-padding-above-limit", false, false, 4096, 3934, 0, PADDED, STATUS_EA_TOO_LARGE},
    {"create-large-duplicate-shrinks", false, false, 4096, 0, 0, SHRINK, STATUS_SUCCESS},
    {"create-large-lxss-filtered", false, false, 4096, 0, 0, FILTERED, STATUS_SUCCESS},
    {"create-unaligned-lxmod", false, false, 4096, 0, 0, MODE, STATUS_SUCCESS},
    {"create-unaligned-lxdev", false, false, 4096, 0, 0, DEVICE, STATUS_SUCCESS},
    {"set-4k-exact-limit", true, false, 4096, 3933, 0, ONE, STATUS_SUCCESS},
    {"set-4k-below-limit", true, false, 4096, 3932, 0, ONE, STATUS_SUCCESS},
    {"set-4k-above-limit", true, false, 4096, 3934, 0, ONE, STATUS_EA_TOO_LARGE},
    {"set-16k-exact-limit", true, false, 16384, 16221, 0, ONE, STATUS_SUCCESS},
    {"set-16k-below-limit", true, false, 16384, 16220, 0, ONE, STATUS_SUCCESS},
    {"set-16k-above-limit", true, false, 16384, 16222, 0, ONE, STATUS_EA_TOO_LARGE},
    {"set-64k-exact-limit", true, false, 65536, 65373, 0, ONE, STATUS_SUCCESS},
    {"set-64k-below-limit", true, false, 65536, 65372, 0, ONE, STATUS_SUCCESS},
    {"set-64k-above-limit", true, false, 65536, 65374, 0, ONE, STATUS_EA_TOO_LARGE},
    {"set-wraps-to-zero", true, true, 65536, 65536, 0, TWO, STATUS_EA_TOO_LARGE},
    {"set-padding-preserves-values", true, false, 4096, 100, 0, PADDED, STATUS_SUCCESS},
    {"set-large-duplicate-shrinks", true, false, 4096, 0, 0, SHRINK, STATUS_SUCCESS},
    {"set-large-lxss-filtered", true, false, 4096, 0, 0, FILTERED, STATUS_SUCCESS},
    {"set-failed-size-preserves-old", true, true, 4096, 3934, 0, ONE, STATUS_EA_TOO_LARGE},
    {"set-lxss-size-error-preserves-metadata", true, true, 4096, 3934, 0, UID_AND_DATA, STATUS_EA_TOO_LARGE},
    {"set-short-lxss-preserves-old-and-metadata", true, true, 4096, 0, 0, BAD_GID, STATUS_INVALID_PARAMETER},
    {"set-delete-all-releases-old", true, true, 4096, 0, 0, DELETE_ALL, STATUS_SUCCESS},
    {"set-old-item-allocation-failure", true, true, 4096, 100, 1, ONE, STATUS_INSUFFICIENT_RESOURCES},
    {"set-new-item-allocation-failure", true, true, 4096, 100, 2, ONE, STATUS_INSUFFICIENT_RESOURCES},
    {"set-output-allocation-failure", true, true, 4096, 100, 3, ONE, STATUS_INSUFFICIENT_RESOURCES},
    {"set-lxss-output-failure-preserves-metadata", true, true, 4096, 100, 4, UID_AND_DATA, STATUS_INSUFFICIENT_RESOURCES},
    {"set-gid-size-error-preserves-metadata", true, true, 4096, 3934, 0, GID_AND_DATA, STATUS_EA_TOO_LARGE},
    {"set-mode-size-error-preserves-metadata", true, true, 4096, 3934, 0, MODE_AND_DATA, STATUS_EA_TOO_LARGE},
    {"set-gid-output-failure-preserves-metadata", true, true, 4096, 100, 4, GID_AND_DATA, STATUS_INSUFFICIENT_RESOURCES},
    {"set-mode-output-failure-preserves-metadata", true, true, 4096, 100, 4, MODE_AND_DATA, STATUS_INSUFFICIENT_RESOURCES},
    {"set-invalid-mode-preserves-gid", true, true, 4096, 0, 0, BAD_MODE, STATUS_INVALID_PARAMETER},
    {"set-invalid-uid-preserves-mode", true, true, 4096, 0, 0, BAD_UID, STATUS_INVALID_PARAMETER},
    {"set-metadata-only-publishes-together", true, false, 4096, 0, 0, METADATA_ONLY, STATUS_SUCCESS},
    {"set-replacement-invalid-metadata-preserves-old", true, true, 4096, 0, 0, REPLACE_THEN_BAD, STATUS_INVALID_PARAMETER},
    {"set-replacement-allocation-failure-preserves-old", true, true, 4096, 0, 2, REPLACE, STATUS_INSUFFICIENT_RESOURCES},
    {"set-replacement-publishes-new-value", true, true, 4096, 0, 0, REPLACE, STATUS_SUCCESS},
    {"set-delete-item-allocation-failure", true, true, 4096, 0, 2, DELETE_ALL, STATUS_INSUFFICIENT_RESOURCES},
    {"create-limit-output-allocation-failure", false, false, 4096, 3933, 2, ONE, STATUS_INSUFFICIENT_RESOURCES},
    {"create-filtered-output-allocation-failure", false, false, 4096, 0, 3, FILTERED, STATUS_INSUFFICIENT_RESOURCES},
};

typedef struct { const char* name; unsigned length; uint32_t special; } input_entry;
static unsigned char value_byte(unsigned i) { return (unsigned char)(i * 37 + 0x11); }
static void* build_entries(const input_entry* entries, unsigned count, size_t* size) {
    *size = 0;
    for (unsigned i = 0; i < count; i++) {
        size_t bytes = 9 + strlen(entries[i].name) + entries[i].length;
        *size += i + 1 == count ? bytes : align4(bytes);
    }
    unsigned char* buffer = calloc(1, *size);
    CHECK(buffer);
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        const input_entry* entry = &entries[i];
        FILE_FULL_EA_INFORMATION* ea = (void*)(buffer + offset);
        size_t bytes = 9 + strlen(entry->name) + entry->length;
        CHECK(entry->length <= UINT16_MAX);
        ea->NextEntryOffset = i + 1 == count ? 0 : (ULONG)align4(bytes);
        ea->Flags = i % 2 ? 0 : 0x80;
        ea->EaNameLength = (UCHAR)strlen(entry->name);
        ea->EaValueLength = (uint16_t)entry->length;
        memcpy(ea->EaName, entry->name, ea->EaNameLength + 1);
        unsigned char* data = (void*)(ea->EaName + ea->EaNameLength + 1);
        for (unsigned j = 0; j < entry->length; j++)
            data[j] = value_byte(j);
        if (entry->special) {
            memcpy(data, &entry->special, sizeof(entry->special));
            if (strcmp(entry->name, "$LXDEV") == 0) {
                uint32_t minor = 5678;
                memcpy(data + 4, &minor, sizeof(minor));
            }
        }
        offset += align4(bytes);
    }
    return buffer;
}

static void run_size(const test_case* test) {
    const size_case* c = &size_cases[test->n];
    /* Windows EA storage: header 101, item 25, dir item 30, name 7. */
    CHECK(sizeof(struct btrfs_header) == 101 && sizeof(struct btrfs_item) == 25);
    CHECK(sizeof(struct btrfs_dir_item) == 30);
    CHECK(101 + 25 + 30 + strlen("user.EA") == 163);
    input_entry entries[3] = {{"a", c->total >= 10 ? c->total - 10 : 3, 0}};
    unsigned count = 1;
    switch (c->shape) {
        case ONE: break;
        case TWO:
            count = 2;
            entries[0].length = c->total > 100000 ? 65522 : 32754;
            entries[1] = (input_entry){"b", c->total - (unsigned)align4(10 + entries[0].length) - 10, 0};
            break;
        case PADDED:
            count = 2;
            entries[0].length = 3;
            entries[1] = (input_entry){"b", c->total - 26, 0};
            break;
        case SHRINK:
            count = 2;
            entries[0].length = 65525;
            entries[1] = (input_entry){"A", 3, 0};
            break;
        case FILTERED: case UID_AND_DATA: case BAD_GID: case GID_AND_DATA: case MODE_AND_DATA:
            count = 2;
            entries[0] = (input_entry){"$LXUID", c->shape == FILTERED ? 65535 : 4, 1234};
            if (c->shape == GID_AND_DATA)
                entries[0].name = "$LXGID";
            else if (c->shape == MODE_AND_DATA)
                entries[0] = (input_entry){"$LXMOD", 4, __S_IFREG | 0600};
            entries[1] = c->shape == BAD_GID ? (input_entry){"$LXGID", 3, 0} :
                         (input_entry){"a", c->shape == FILTERED ? 3 : c->total - 10, 0};
            break;
        case BAD_MODE:
            count = 2;
            entries[0] = (input_entry){"$LXGID", 4, 1234};
            entries[1] = (input_entry){"$LXMOD", 3, 0};
            break;
        case BAD_UID:
            count = 2;
            entries[0] = (input_entry){"$LXMOD", 4, __S_IFREG | 0600};
            entries[1] = (input_entry){"$LXUID", 3, 0};
            break;
        case METADATA_ONLY:
            count = 3;
            entries[0] = (input_entry){"$LXUID", 4, 1234};
            entries[1] = (input_entry){"$LXGID", 4, 5678};
            entries[2] = (input_entry){"$LXMOD", 4, __S_IFCHR | 0600};
            break;
        case REPLACE: case REPLACE_THEN_BAD:
            entries[0] = (input_entry){"OLD", 5, 0};
            if (c->shape == REPLACE_THEN_BAD) {
                count = 2;
                entries[1] = (input_entry){"$LXGID", 3, 0};
            }
            break;
        case DELETE_ALL:
            count = 2;
            entries[0] = (input_entry){"OLD", 0, 0};
            entries[1] = (input_entry){"GHOST", 0, 0};
            break;
        case MODE: entries[0] = (input_entry){"$LXMOD", 4, __S_IFREG | 0600}; break;
        case DEVICE: entries[0] = (input_entry){"$LXDEV", 8, 1234}; break;
    }
    size_t length;
    void* input = build_entries(entries, count, &length);
    ULONG bad;
    CHECK(IoCheckEaBufferValidity(input, (ULONG)length, &bad) == STATUS_SUCCESS);
    device_extension volume = {.type = VCB_TYPE_FS, .superblock = {.nodesize = c->nodesize, .generation = 7}};
    ERESOURCE resource = {0};
    fcb node = {.type = c->shape == DEVICE ? BTRFS_FT_CHRDEV : BTRFS_FT_REG_FILE,
                .sd_deleted = true, .Vcb = &volume, .Header.Resource = &resource};
    node.inode_item.uid = 99;
    node.inode_item.gid = 98;
    node.inode_item.mode = __S_IFREG | 0644;
    file_ref ref = {.fcb = &node};
    ccb context = {.fileref = &ref, .access = FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES};
    FILE_OBJECT object = {.FsContext = &node, .FsContext2 = &context};
    IRP irp = {.RequestorMode = UserMode, .buffer = input,
               .stack = {.FileObject = &object, .Parameters.SetEa.Length = (ULONG)length}};
    DEVICE_OBJECT device = {.DeviceExtension = &volume};
    char* old_buffer = NULL;
    void* old_snapshot = NULL;
    size_t old_size = 0;
    if (c->old) {
        input_entry old_entry = {"OLD", 3, 0};
        old_snapshot = build_entries(&old_entry, 1, &old_size);
        old_buffer = allocate(old_size);
        memcpy(old_buffer, old_snapshot, old_size);
        node.ea_xattr = (ANSI_STRING){(uint16_t)old_size, (uint16_t)old_size, old_buffer};
        node.ealen = 15;
    }
    struct btrfs_inode_item old_inode = node.inode_item;
    ANSI_STRING old_ea = node.ea_xattr;
    ULONG old_ealen = node.ealen;
    attempts = 0;
    fail_at = c->fault;
    NTSTATUS status = c->update ? drv_set_ea(&device, &irp) : file_create_parse_ea(&node, input);
    CHECK(status == c->status);
    if (c->fault)
        CHECK(attempts == c->fault);
    CHECK(!resource.held && !filesystem_depth);
    CHECK(completions == (unsigned)c->update);
    if (c->update)
        CHECK(irp.IoStatus.Status == status && irp.IoStatus.Information == 0);
    if (status != STATUS_SUCCESS) {
        CHECK(node.ea_xattr.Buffer == old_ea.Buffer && node.ea_xattr.Length == old_ea.Length);
        CHECK(node.ea_xattr.MaximumLength == old_ea.MaximumLength && node.ealen == old_ealen);
        CHECK(live == (unsigned)c->old && !node.ea_changed && !dirty_calls && !notifications);
        if (c->old)
            CHECK(memcmp(node.ea_xattr.Buffer, old_snapshot, old_size) == 0);
        if (c->update)
            CHECK(memcmp(&node.inode_item, &old_inode, sizeof(old_inode)) == 0 &&
                  !node.inode_item_changed && !node.sd_dirty && node.sd_deleted);
    } else {
        CHECK(dirty_calls == (unsigned)c->update && notifications == (unsigned)c->update);
        if (c->update)
            CHECK(node.ea_changed && node.inode_item_changed && node.inode_item.sequence == 1 &&
                  node.inode_item.transid == 7 && node.inode_item.ctime.sec == 42);
        if (c->shape == DELETE_ALL || c->shape == MODE || c->shape == DEVICE || c->shape == METADATA_ONLY) {
            CHECK(!node.ea_xattr.Buffer && !node.ea_xattr.Length && !node.ealen && !live);
        } else {
            CHECK(node.ea_xattr.Buffer && node.ea_changed && live == 1);
            size_t offset = 0, end = 0;
            unsigned seen = 0;
            for (;;) {
                CHECK(offset + 8 <= node.ea_xattr.Length && offset % 4 == 0);
                FILE_FULL_EA_INFORMATION* ea = (void*)(node.ea_xattr.Buffer + offset);
                unsigned index = c->shape == SHRINK || c->shape == FILTERED ? 1 : seen;
                CHECK(index < count);
                const input_entry* want = &entries[index];
                CHECK(ea->EaNameLength == strlen(want->name) && ea->EaValueLength == want->length);
                CHECK(ea->EaName[ea->EaNameLength] == 0);
                for (unsigned j = 0; j < ea->EaNameLength; j++)
                    CHECK((unsigned char)ea->EaName[j] == uppercase((unsigned char)want->name[j]));
                for (unsigned j = 0; j < want->length; j++)
                    CHECK((unsigned char)ea->EaName[ea->EaNameLength + 1 + j] == value_byte(j));
                end = offset + 9 + ea->EaNameLength + ea->EaValueLength;
                seen++;
                if (!ea->NextEntryOffset)
                    break;
                CHECK(ea->NextEntryOffset == align4(end - offset));
                offset += ea->NextEntryOffset;
            }
            CHECK(seen == (c->shape == SHRINK || c->shape == FILTERED ? 1 : count));
            CHECK(end == node.ea_xattr.Length && allocation_size(node.ea_xattr.Buffer) == end);
            CHECK(end + 163 <= c->nodesize);
        }
        if (c->shape == FILTERED)
            CHECK(node.inode_item.uid == 1234 && node.sd_dirty && !node.sd_deleted);
        if (c->shape == MODE)
            CHECK(node.inode_item.mode == (__S_IFREG | 0600));
        if (c->shape == DEVICE)
            CHECK(node.inode_item.rdev == (((uint64_t)1234 << 20) | 5678));
        if (c->shape == METADATA_ONLY)
            CHECK(node.inode_item.uid == 1234 && node.inode_item.gid == 5678 &&
                  node.inode_item.mode == (__S_IFREG | 0600) && node.sd_dirty && !node.sd_deleted);
    }
    if (node.ea_xattr.Buffer)
        ExFreePool(node.ea_xattr.Buffer);
    CHECK(!live);
    free(old_snapshot);
    free(input);
}

static void define_size_cases(void) {
    for (unsigned i = 0; i < sizeof(size_cases) / sizeof(size_cases[0]); i++) {
        add_case(size_cases[i].name, PAIR, i, 0, 0);
        cases[case_count - 1].run = run_size;
    }
}
