/* Host regression for fast I/O and MDL writes that extend a file.
 *
 * The callbacks the driver registers for fast I/O writes, MDL writes,
 * standard information queries, CcFlushCache and write-behind are extracted
 * from the selected source revision, along with write_file and write_file2,
 * without rewriting their statements. host.h holds the kernel mocks. */
#include "host.h"
#include "driver.inc"
#include "dispatch.inc"

/* Write every dirty page in the cached file as the memory manager does. */
static void write_dirty_pages(void) {
    for (int p = 0; p < PAGES; p++) {
        uint8_t page[PAGE];
        IRP irp = {0};
        NTSTATUS status;
        bool write;

        pthread_mutex_lock(&kernel);
        write = dirty[p] && (int64_t)p * PAGE < cache_file_size;
        if (write) {
            dirty[p] = false;
            memcpy(page, &cache[p * PAGE], PAGE);
        }
        pthread_mutex_unlock(&kernel);

        if (!write)
            continue;

        irp.Flags = IRP_PAGING_IO | IRP_NOCACHE | IRP_SYNCHRONOUS_PAGING_IO;
        irp.AssociatedIrp.SystemBuffer = page;
        irp.stack.FileObject = &file_object;
        irp.stack.Parameters.Write.ByteOffset.QuadPart = (int64_t)p * PAGE;
        irp.stack.Parameters.Write.Length = PAGE;

        status = write_file(&volume, &irp, true, false);

        pthread_mutex_lock(&kernel);
        if (!NT_SUCCESS(status))
            dirty[p] = true;
        CHECK(page_write_count < sizeof(page_writes) / sizeof(page_writes[0]));
        page_writes[page_write_count++] = (struct page_write){thread_names[self], (int64_t)p * PAGE, status,
                                                              irp.IoStatus.Information};
        pthread_mutex_unlock(&kernel);
    }
}

static void write_behind(void) {
    CHECK(DISPATCH_AcquireForLazyWrite(&file_object, true));
    write_dirty_pages();
    DISPATCH_ReleaseFromLazyWrite(&file_object);
}

/* Truncate the file as set_end_of_file_information does: the sizes change
 * under the FCB resource, then CcSetFileSizes discards the cached data past
 * the new end. */
static void truncate_file_to(int64_t end) {
    CC_FILE_SIZES sizes;

    ExAcquireResourceSharedLite(&volume.tree_lock, true);
    ExAcquireResourceExclusiveLite(&fcb_resource, true);

    file.inode_item.size = (uint64_t)end;
    file.Header.AllocationSize.QuadPart = (int64_t)sector_align((uint64_t)end, volume.superblock.sectorsize);
    file.Header.FileSize.QuadPart = file.Header.ValidDataLength.QuadPart = end;
    sizes.AllocationSize = file.Header.AllocationSize;
    sizes.FileSize = file.Header.FileSize;
    sizes.ValidDataLength = file.Header.ValidDataLength;

    pthread_mutex_lock(&kernel);
    memset(&disk[end], 0, sizeof(disk) - (size_t)end);
    pthread_mutex_unlock(&kernel);

    ExReleaseResourceLite(&fcb_resource);
    CcSetFileSizes(&second_object, &sizes);
    ExReleaseResourceLite(&volume.tree_lock);
}

/* When the other thread starts. */
enum {
    AFTER_WRITE,        /* once the writer has finished */
    DURING_COPY,        /* while the writer copies its data with the FCB resource held */
    AT_COPY_WRITE,      /* when the writer calls FsRtlCopyWrite */
};

enum { WRITE_BEHIND, CC_FLUSH, CACHED_IRP, TRUNCATE };

struct scenario {
    const char* name;
    int64_t size, allocation;       /* the file before the writes */
    int64_t offset;                 /* the first write, through fast I/O or an MDL */
    ULONG length;
    bool mdl;
    int start;                      /* when the other thread starts */
    int other;                      /* what the other thread does */
    int64_t other_offset;           /* CACHED_IRP: the write it sends; TRUNCATE: the new end of file */
    ULONG other_length;
};

static const struct scenario scenarios[] = {
    {"write-behind-after-extend", 5000, 8192, 5000, 1000, false, AFTER_WRITE, WRITE_BEHIND, 0, 0},
    {"write-behind-during-overwrite", 5000, 8192, 1000, 500, false, DURING_COPY, WRITE_BEHIND, 0, 0},
    {"write-behind-during-extend", 5000, 8192, 5000, 1000, false, DURING_COPY, WRITE_BEHIND, 0, 0},
    {"write-behind-during-extend-preallocated", 4096, 16384, 4096, 4096, false, DURING_COPY, WRITE_BEHIND, 0, 0},
    {"flush-during-extend", 5000, 8192, 5000, 1000, false, DURING_COPY, CC_FLUSH, 0, 0},
    {"cached-write-during-extend", 5000, 8192, 5000, 1000, false, DURING_COPY, CACHED_IRP, 5500, 100},
    {"truncate-during-overwrite", 5000, 8192, 2500, 500, false, AT_COPY_WRITE, TRUNCATE, 2700, 0},
    {"mdl-overwrite", 5000, 8192, 1000, 500, true, AFTER_WRITE, WRITE_BEHIND, 0, 0},
    {"mdl-extend", 5000, 8192, 5000, 1000, true, AFTER_WRITE, WRITE_BEHIND, 0, 0},
};

static const struct scenario* current;
static int64_t queried_size;
static uint8_t writer_data[PAGE], other_data[PAGE], expected[PAGES * PAGE];

static uint8_t original_byte(int64_t i) { return (uint8_t)('a' + i % 26); }

static void* writer_thread(void* arg) {
    LARGE_INTEGER offset = {.QuadPart = current->offset};
    IO_STATUS_BLOCK iosb = {0};

    UNUSED(arg);
    self = WRITER;

    if (current->mdl) {
        MDL* chain = NULL;
        bool fast = DISPATCH_PrepareMdlWrite(&file_object, &offset, current->length, 0, &chain, &iosb, NULL);

        if (!fast) {
            /* The caller sends an IRP_MN_MDL write when the fast path declines. */
            IRP irp = {0};

            irp.stack.MinorFunction = IRP_MN_MDL;
            irp.stack.FileObject = &file_object;
            irp.stack.Parameters.Write.ByteOffset = offset;
            irp.stack.Parameters.Write.Length = current->length;
            CHECK(write_file(&volume, &irp, true, false) == STATUS_SUCCESS);
            chain = irp.MdlAddress;
            iosb = irp.IoStatus;
        }

        CHECK(iosb.Status == STATUS_SUCCESS && iosb.Information == current->length && chain != NULL);
        memcpy(&cache[chain->offset], writer_data, chain->length);

        /* drv_write completes an IRP_MN_MDL write with CcMdlWriteComplete. */
        if (fast)
            CHECK(DISPATCH_MdlWriteComplete(&file_object, &offset, chain, NULL));
        else
            CcMdlWriteComplete(&file_object, &offset, chain);
    } else if (DISPATCH_FastIoWrite(&file_object, &offset, current->length, true, 0, writer_data, &iosb, NULL)) {
        CHECK(iosb.Status == STATUS_SUCCESS && iosb.Information == current->length);
    } else {
        /* The I/O manager sends an IRP when fast I/O declines the write. */
        IRP irp = {0};

        irp.UserBuffer = writer_data;
        irp.stack.FileObject = &file_object;
        irp.stack.Parameters.Write.ByteOffset = offset;
        irp.stack.Parameters.Write.Length = current->length;
        CHECK(write_file(&volume, &irp, true, false) == STATUS_SUCCESS && irp.IoStatus.Information == current->length);
    }

    CHECK(fs_depth == 0 && IoGetTopLevelIrp() == NULL);

    {
        FILE_STANDARD_INFORMATION info;
        IO_STATUS_BLOCK query = {0};

        CHECK(DISPATCH_FastIoQueryStandardInfo(&file_object, true, &info, &query, NULL));
        CHECK(query.Status == STATUS_SUCCESS && fs_depth == 0);
        queried_size = info.EndOfFile.QuadPart;
    }

    pthread_mutex_lock(&kernel);
    writer_done = true;
    pthread_cond_broadcast(&kernel_changed);
    pthread_mutex_unlock(&kernel);

    return NULL;
}

static void* other_thread(void* arg) {
    UNUSED(arg);
    self = OTHER;

    pthread_mutex_lock(&kernel);
    if (current->start == DURING_COPY)
        WAIT_UNTIL(writer_copying || writer_done, "the writer to copy its data");
    else if (current->start == AT_COPY_WRITE)
        WAIT_UNTIL(writer_at_copy_write || writer_done, "the writer to call FsRtlCopyWrite");
    else
        WAIT_UNTIL(writer_done, "the writer to finish");
    pthread_mutex_unlock(&kernel);

    if (current->other == WRITE_BEHIND)
        write_behind();
    else if (current->other == CC_FLUSH) {
        CHECK(DISPATCH_AcquireForCcFlush(&file_object, NULL) == STATUS_SUCCESS);
        write_dirty_pages();
        CHECK(DISPATCH_ReleaseForCcFlush(&file_object, NULL) == STATUS_SUCCESS);
    } else if (current->other == TRUNCATE)
        truncate_file_to(current->other_offset);
    else {
        IRP irp = {0};

        irp.UserBuffer = other_data;
        irp.stack.FileObject = &second_object;
        irp.stack.Parameters.Write.ByteOffset.QuadPart = current->other_offset;
        irp.stack.Parameters.Write.Length = current->other_length;
        CHECK(write_file(&volume, &irp, true, false) == STATUS_SUCCESS &&
              irp.IoStatus.Information == current->other_length);
    }

    CHECK(fs_depth == 0 && IoGetTopLevelIrp() == NULL);

    pthread_mutex_lock(&kernel);
    other_done = true;
    pthread_cond_broadcast(&kernel_changed);
    pthread_mutex_unlock(&kernel);

    return NULL;
}

static void setup(void) {
    volume.superblock.sectorsize = 4096;
    volume.superblock.generation = 10;
    volume.options.max_inline = 2048;

    file.Header.Resource = &fcb_resource;
    file.Header.PagingIoResource = &paging_resource;
    file.Header.IsFastIoPossible = FastIoIsPossible;
    file.Header.AllocationSize.QuadPart = current->allocation;
    file.Header.FileSize.QuadPart = file.Header.ValidDataLength.QuadPart = current->size;
    file.Vcb = &volume;
    file.subvol = &subvolume;
    file.inode = 256;
    file.type = BTRFS_FT_REG_FILE;
    file.inode_item.size = (uint64_t)current->size;
    file.inode_item.nbytes = (uint64_t)current->allocation;

    fileref.fcb = &file;
    context.fileref = &fileref;

    file_object.FsContext = &file;
    file_object.FsContext2 = &context;
    file_object.SectionObjectPointer = &section;
    file_object.PrivateCacheMap = &private_cache_map;
    file_object.Flags = FO_SYNCHRONOUS_IO;
    second_object = file_object;

    for (int64_t i = 0; i < current->size; i++)
        cache[i] = disk[i] = expected[i] = original_byte(i);
    cache_file_size = current->size;

    for (unsigned int i = 0; i < sizeof(writer_data); i++) {
        writer_data[i] = (uint8_t)('A' + i % 26);
        other_data[i] = (uint8_t)('0' + i % 10);
    }

    memcpy(&expected[current->offset], writer_data, current->length);
    if (current->other == CACHED_IRP)
        memcpy(&expected[current->other_offset], other_data, current->other_length);
}

static void report_page_writes(void) {
    for (unsigned int i = 0; i < page_write_count; i++)
        fprintf(stderr, "  %s: paging write at %lld returned %#x after writing %zu of %d bytes\n",
                page_writes[i].thread, (long long)page_writes[i].offset, (unsigned int)page_writes[i].status,
                (size_t)page_writes[i].written, PAGE);
}

static int verify(void) {
    ERESOURCE* resources[] = {&volume.tree_lock, &fcb_resource, &paging_resource};
    int64_t written = max(current->size, current->offset + current->length);
    int64_t size = written, first = -1, last = -1;

    if (current->other == CACHED_IRP)
        size = max(size, current->other_offset + current->other_length);

    /* The truncation can come before or after the write, and before or after
     * the size query. */
    if (current->other == TRUNCATE) {
        size = file.Header.FileSize.QuadPart == current->other_offset ? current->other_offset :
               max(current->other_offset, current->offset + current->length);
    }

    for (int t = 0; t < THREADS; t++)
        CHECK(waiting_for[t] == NULL);

    for (unsigned int i = 0; i < sizeof(resources) / sizeof(resources[0]); i++)
        CHECK(resources[i]->exclusive_owner == NOBODY && resources[i]->shared_owners == 0 && resources[i]->queued == 0);

    if (file.Header.FileSize.QuadPart != size || file.inode_item.size != (uint64_t)size) {
        fprintf(stderr, "FAIL %s: file size %lld and inode size %llu, expected %lld\n", scenario,
                (long long)file.Header.FileSize.QuadPart, (unsigned long long)file.inode_item.size, (long long)size);
        report_page_writes();
        return 1;
    }

    if (current->other != TRUNCATE && queried_size != written) {
        fprintf(stderr, "FAIL %s: the size query after the write reported %lld, expected %lld\n",
                scenario, (long long)queried_size, (long long)written);
        return 1;
    }

    if (memcmp(cache, expected, (size_t)size)) {
        for (int64_t i = 0; i < size; i++)
            if (cache[i] != expected[i])
                last = i, first = first < 0 ? i : first;

        fprintf(stderr, "FAIL %s: cached bytes %lld-%lld differ from the data written\n", scenario,
                (long long)first, (long long)last);
        return 1;
    }

    for (int p = 0; p < PAGES; p++)
        CHECK(!dirty[p]);

    if (memcmp(disk, expected, (size_t)size)) {
        for (int64_t i = 0; i < size; i++)
            if (disk[i] != expected[i])
                last = i, first = first < 0 ? i : first;

        fprintf(stderr, "FAIL %s: bytes %lld-%lld never reached the disk and no page is dirty\n", scenario,
                (long long)first, (long long)last);
        report_page_writes();
        return 1;
    }

    printf("PASS %s\n", scenario);
    return 0;
}

int main(int argc, char** argv) {
    pthread_t writer, other;

    CHECK(argc == 2);
    scenario = argv[1];

    for (unsigned int i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        if (!strcmp(scenarios[i].name, scenario))
            current = &scenarios[i];

    if (!current) {
        fprintf(stderr, "unknown scenario %s\n", scenario);
        return 2;
    }

    self = MAIN;
    setup();
    hold_copy = preempt_after_release = current->start == DURING_COPY;
    hold_copy_write = current->start == AT_COPY_WRITE;

    CHECK(pthread_create(&writer, NULL, writer_thread, NULL) == 0);
    CHECK(pthread_create(&other, NULL, other_thread, NULL) == 0);
    CHECK(pthread_join(writer, NULL) == 0);
    CHECK(pthread_join(other, NULL) == 0);

    /* Write-behind eventually writes whatever is still dirty. */
    write_behind();

    return verify();
}
