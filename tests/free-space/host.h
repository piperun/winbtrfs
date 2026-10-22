/* Native list and pool models for the production space-list functions. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct chunk chunk;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define NT_SUCCESS(s) ((s) >= 0)
#define PagedPool 1
#define ALLOC_TAG 0x53664254
#define ERR(...) ((void)0)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p) - offsetof(t,m)))
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

/* Links of freed entries and of the unused links in list heads. */
#define POISON ((LIST_ENTRY*)(uintptr_t)UINT64_C(0xdddddddddddddddd))

#include "types.inc"

static void InitializeListHead(LIST_ENTRY* h) { h->Flink = h->Blink = h; }
static bool IsListEmpty(const LIST_ENTRY* h) { return h->Flink == h; }

/* The integrity checks of the Windows list primitives, plus a check for freed entries. */
static void check_entry(const LIST_ENTRY* e) {
    if (e->Flink == POISON || e->Blink == POISON) {
        fprintf(stderr, "list operation on freed or unlinked entry %p\n", (const void*)e);
        exit(1);
    }
    if (e->Flink->Blink != e || e->Blink->Flink != e) {
        fprintf(stderr, "corrupt list links at %p\n", (const void*)e);
        exit(1);
    }
}

static void InsertHeadList(LIST_ENTRY* h, LIST_ENTRY* e) {
    check_entry(h);
    e->Flink = h->Flink; e->Blink = h;
    h->Flink->Blink = e; h->Flink = e;
}
static void InsertTailList(LIST_ENTRY* h, LIST_ENTRY* e) {
    check_entry(h);
    e->Blink = h->Blink; e->Flink = h;
    h->Blink->Flink = e; h->Blink = e;
}
static void RemoveEntryList(LIST_ENTRY* e) {
    check_entry(e);
    e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink;
}
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* h) {
    check_entry(h);
    LIST_ENTRY* e = h->Flink; CHECK(e != h); RemoveEntryList(e); return e;
}

static void* ExAllocatePoolWithTag(unsigned pool, size_t size, unsigned tag);
static void ExFreePool(void* p);
static NTSTATUS add_rollback_space(LIST_ENTRY* rollback, bool add, LIST_ENTRY* list,
    LIST_ENTRY* list_size, uint64_t address, uint64_t length, chunk* c);
