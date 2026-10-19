/* Real xattr hash collisions on the same inode, with independently held
 * cached FCBs. The normal run() checks the reopened stream and reaps it. */
static fcb* colliding_fcb;
static bool same_xattr;
static const char other_payload[]="other-stream";

static void setup_ads_cache(void) {
    if (!current->colliding_xattr) return;
    CHECK(current->reaped_stream && !stream_dc.fileref);
    preparing=true;
    colliding_fcb=create_fcb(&volume,PagedPool); CHECK(colliding_fcb);
    colliding_fcb->Vcb=&volume; colliding_fcb->subvol=&subvol;
    colliding_fcb->inode=base_fcb.inode; colliding_fcb->hash=base_fcb.hash;
    colliding_fcb->type=base_fcb.type; colliding_fcb->ads=true;
    unsigned length=strlen(current->colliding_xattr);
    colliding_fcb->adsxattr.Length=5+length;
    // Allocation capacity is not part of the xattr's identity.
    colliding_fcb->adsxattr.MaximumLength=14+length;
    colliding_fcb->adsxattr.Buffer=allocate(14+length,"fixture");
    memcpy(colliding_fcb->adsxattr.Buffer,"user.",5);
    memcpy(colliding_fcb->adsxattr.Buffer+5,current->colliding_xattr,length+1);
    colliding_fcb->adshash=calc_crc32c(0xfffffffe,(uint8_t*)colliding_fcb->adsxattr.Buffer,5+length);
    uint32_t requested_hash=calc_crc32c(0xfffffffe,(uint8_t*)"user.",5);
    requested_hash=calc_crc32c(requested_hash,(uint8_t*)stream_utf8,stream_dc.utf8.Length);
    CHECK(colliding_fcb->adshash == requested_hash);
    same_xattr=length == stream_dc.utf8.Length && !memcmp(current->colliding_xattr,stream_utf8,length);
    const void* payload=same_xattr ? (const void*)stream_payload : (const void*)other_payload;
    unsigned bytes=same_xattr ? sizeof(stream_payload) : sizeof(other_payload)-1;
    colliding_fcb->adsdata.Length=colliding_fcb->adsdata.MaximumLength=bytes;
    colliding_fcb->adsdata.Buffer=allocate(bytes,"fixture");
    memcpy(colliding_fcb->adsdata.Buffer,payload,bytes);
    colliding_fcb->Header.AllocationSize.QuadPart=bytes;
    colliding_fcb->Header.FileSize.QuadPart=bytes;
    colliding_fcb->Header.ValidDataLength.QuadPart=bytes;
    LIST_ENTRY* after=current->collider_after_target ? &warm_stream_fcb->list_entry : &base_fcb.list_entry;
    InsertHeadList(after,&colliding_fcb->list_entry);
    InsertTailList(&volume.all_fcbs,&colliding_fcb->list_entry_all);
    subvol.fcbs_version++;
    preparing=false;
}

static void check_ads_result(file_ref* result) {
    if (!colliding_fcb || !result) return;
    if (!same_xattr && result->fcb == colliding_fcb)
        fprintf(stderr,"%s: requested user.%.*s, selected cached %.*s (hash %#x); returned %u bytes instead of %zu\n",
                scenario,stream_dc.utf8.Length,stream_utf8,colliding_fcb->adsxattr.Length,
                colliding_fcb->adsxattr.Buffer,colliding_fcb->adshash,
                result->fcb->adsdata.Length,sizeof(stream_payload));
    CHECK(same_xattr ? result->fcb == colliding_fcb : result->fcb != colliding_fcb);
}

static void release_ads_cache(void) {
    if (!colliding_fcb) return;
    CHECK(colliding_fcb->refcount == 1 && !colliding_fcb->dirty && !colliding_fcb->deleted);
    CHECK(colliding_fcb->adsxattr.Length == 5+strlen(current->colliding_xattr));
    CHECK(!strcmp(colliding_fcb->adsxattr.Buffer+5,current->colliding_xattr));
    const void* payload=same_xattr ? (const void*)stream_payload : (const void*)other_payload;
    unsigned bytes=same_xattr ? sizeof(stream_payload) : sizeof(other_payload)-1;
    CHECK(colliding_fcb->adsdata.Length == bytes && colliding_fcb->Header.FileSize.QuadPart == bytes);
    CHECK(!memcmp(colliding_fcb->adsdata.Buffer,payload,bytes));
    free_fcb(colliding_fcb); reap_fcbs(&volume);
}
