/* Production file creation dispatch with host-only Windows services. */
#include "create_ecp.inc"
static NTSTATUS (*fFsRtlGetEcpListFromIrp)(PIRP,ECP_LIST**) = NULL;
static NTSTATUS (*fFsRtlGetNextExtraCreateParameter)(ECP_LIST*,void*,GUID*,void**,ULONG*) = NULL;
static unsigned share_calls;
static bool is_subvol_readonly(root* r, PIRP request) { CHECK(r == &subvol && request == &irp); return false; }
static bool FsRtlAreNamesEqual(const UNICODE_STRING* a, const UNICODE_STRING* b, bool fold, void* table) {
    CHECK(fold && !table); if (a->Length != b->Length) return false;
    for (unsigned i=0;i<a->Length/sizeof(WCHAR);i++) {
        WCHAR x=a->Buffer[i], y=b->Buffer[i];
        if (x >= 'a' && x <= 'z') x-=32;
        if (y >= 'a' && y <= 'z') y-=32;
        if (x != y) return false;
    }
    return true;
}
static NTSTATUS IoCheckEaBufferValidity(void* data, ULONG size, ULONG* offset) {
    (void)data; (void)size; (void)offset; CHECK(false); return STATUS_INVALID_PARAMETER;
}
static void IoSetShareAccess(unsigned desired, unsigned share, PFILE_OBJECT object, int* access) {
    CHECK(desired == security_context.DesiredAccess && share == irp.stack.Parameters.Create.ShareAccess && object && access);
    share_calls++;
}
static void RtlInitUnicodeString(UNICODE_STRING* out, const WCHAR* text) { CHECK(!text); memset(out,0,sizeof(*out)); }
#define called_from_lxss() false
static NTSTATUS set_reparse_point2(fcb* node, PREPARSE_DATA_BUFFER data, unsigned size, void* a, void* b, PIRP request, LIST_ENTRY* rollback) {
    (void)node; (void)data; (void)size; (void)a; (void)b; (void)request; (void)rollback;
    CHECK(false); return STATUS_INVALID_PARAMETER;
}
#include "file_create.inc"
static NTSTATUS dispatch_create(UNICODE_STRING* name, UNICODE_STRING* stream, file_ref** out, file_ref** parent, LIST_ENTRY* rollback) {
    WCHAR path[160]; unsigned length=0;
    if (!current->relative_stream) {
        memcpy(path,name->Buffer,name->Length); length=name->Length/sizeof(WCHAR);
    }
    if (current->stream) {
        path[length++]=':'; memcpy(path+length,stream->Buffer,stream->Length); length+=stream->Length/sizeof(WCHAR);
    }
    if (current->data_suffix) {
        const WCHAR suffix[]={':','$','d','a','t','a'}; memcpy(path+length,suffix,sizeof(suffix)); length+=sizeof(suffix)/sizeof(WCHAR);
    }
    CHECK(length < 160);
    UNICODE_STRING filename={length*sizeof(WCHAR),sizeof(path),path};
    FILE_OBJECT object={0};
    file_ref* related=current->relative_stream ? &base_ref : &directory_ref;
    NTSTATUS status=file_create(&irp,&volume,&object,related,current->loaded_related,&filename,1,0,out,rollback);
    if (status == STATUS_OBJECT_NAME_COLLISION) {
        if (!*out) fprintf(stderr,"%s: collision fileref missing; directory refs=%d, base refs=%d\n",scenario,directory_ref.refcount,base_ref.refcount);
        CHECK(*out && (*out)->fcb && !object.FsContext && !object.FsContext2 && !share_calls);
        *parent=(*out)->parent;
    } else if (NT_SUCCESS(status)) {
        CHECK(!*out && object.FsContext2 && object.FsContext == object.FsContext2->fileref->fcb && share_calls == 1);
        *out=object.FsContext2->fileref; *parent=(*out)->parent;
        CHECK((*out)->open_count == 1 && volume.open_files == 1);
        // Model handle closure; run() releases the CCB's fileref hold once.
        InterlockedDecrement(&(*out)->open_count); InterlockedDecrement(&volume.open_files);
        ExFreePool(object.FsContext2);
    } else CHECK(!*out && !object.FsContext && !object.FsContext2 && !share_calls);
    if (current->stream) {
        int expected_base=1+(current->existing_stream && !current->reaped_stream ? 1 : 0);
        if (NT_SUCCESS(status) || (status == STATUS_OBJECT_NAME_COLLISION && current->reaped_stream)) expected_base++;
        int expected_directory=(have_base ? 2 : 1)+(current->missing_base && NT_SUCCESS(status) ? 1 : 0);
        if ((have_base && base_ref.refcount != expected_base) || directory_ref.refcount != expected_directory)
            fprintf(stderr,"%s: returned parent refs: directory=%d, base=%d (expected %d, %d)\n",
                    scenario,directory_ref.refcount,base_ref.refcount,expected_directory,expected_base);
        if (have_base) CHECK(base_ref.refcount == expected_base);
        CHECK(directory_ref.refcount == expected_directory);
    }
    return status;
}
