/* Check the reference owned by the dirty FCB list using production transitions. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define _In_
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
static void InitializeListHead(LIST_ENTRY* h) { h->Flink = h->Blink = h; }
static bool IsListEmpty(const LIST_ENTRY* h) { return h->Flink == h; }
static void InsertTailList(LIST_ENTRY* h, LIST_ENTRY* e) {
    e->Flink = h; e->Blink = h->Blink; h->Blink->Flink = e; h->Blink = e;
}
static void RemoveEntryList(LIST_ENTRY* e) {
    CHECK(e->Flink->Blink == e && e->Blink->Flink == e);
    e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink;
}

typedef void* PIRP;
typedef struct device_extension {
    LIST_ENTRY dirty_fcbs;
    int dirty_fcbs_lock;
    bool need_write;
} device_extension;
typedef struct fcb {
    device_extension* Vcb;
    LIST_ENTRY list_entry_dirty;
    bool dirty;
    int refcount;
} fcb;

static void ExAcquireResourceExclusiveLite(int* lock, bool wait) {
    CHECK(lock && wait && !*lock);
    *lock = 1;
}
static bool ExIsResourceAcquiredExclusiveLite(int* lock) { return *lock != 0; }
static void ExReleaseResourceLite(int* lock) { CHECK(*lock); *lock = 0; }
static int InterlockedIncrement(int* value) { return ++*value; }
static int InterlockedDecrement(int* value) { return --*value; }
static void free_fcb(fcb* file) { InterlockedDecrement(&file->refcount); }
static NTSTATUS flush_status;

#include "flush_ref.inc"

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        puts("dirty_flush_releases_list_reference");
        puts("clean_flush_preserves_owner_reference");
        puts("locked_flush_releases_list_reference");
        puts("failed_flush_releases_list_reference");
        return 0;
    }
    CHECK(argc == 2);
    device_extension volume = {0};
    fcb file = {.Vcb = &volume, .refcount = 1};
    InitializeListHead(&volume.dirty_fcbs);

    mark_fcb_dirty(&file);
    CHECK(file.refcount == 2 && file.dirty && !IsListEmpty(&volume.dirty_fcbs));
    if (!strcmp(argv[1], "failed_flush_releases_list_reference"))
        flush_status = STATUS_IO_DEVICE_ERROR;
    if (!strcmp(argv[1], "locked_flush_releases_list_reference"))
        ExAcquireResourceExclusiveLite(&volume.dirty_fcbs_lock, true);
    CHECK(flush_fcb(&file) == flush_status);
    if (volume.dirty_fcbs_lock)
        ExReleaseResourceLite(&volume.dirty_fcbs_lock);
    CHECK(!file.dirty && IsListEmpty(&volume.dirty_fcbs));
    CHECK(file.refcount == 1);
    if (!strcmp(argv[1], "clean_flush_preserves_owner_reference") ||
        !strcmp(argv[1], "failed_flush_releases_list_reference")) {
        CHECK(flush_fcb(&file) == flush_status);
        CHECK(file.refcount == 1 && IsListEmpty(&volume.dirty_fcbs));
    } else {
        CHECK(!strcmp(argv[1], "dirty_flush_releases_list_reference") ||
              !strcmp(argv[1], "locked_flush_releases_list_reference"));
    }
    free_fcb(&file);
    CHECK(file.refcount == 0);
    return 0;
}
