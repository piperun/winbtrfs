/* Copyright (c) Mark Harmstone 2019
 *
 * This file is part of WinBtrfs.
 *
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

/* Exercise boot-option parsing and pool/registry cleanup with host API mocks. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef wchar_t WCHAR;
typedef void* HANDLE;
typedef struct { unsigned short Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING;
typedef struct { int unused; } OBJECT_ATTRIBUTES;
typedef struct { ULONG TitleIndex, Type, DataOffset, DataLength, NameLength; WCHAR Name[1]; } KEY_VALUE_FULL_INFORMATION;
#define try if (1)
#define except(x) if (0 && (x))
#define EXCEPTION_EXECUTE_HANDLER 1
#define NT_SUCCESS(s) ((s) >= 0)
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define OBJ_CASE_INSENSITIVE 0
#define OBJ_KERNEL_HANDLE 0
#define KEY_QUERY_VALUE 0
#define PagedPool 0
#define ALLOC_TAG 0
#define KeyValueFullInformation 0
static uint64_t boot_subvol;
static unsigned allocations, frees, opens, closes;
static size_t allocated_bytes;
static void* pool;
static const WCHAR* options;
static bool query_fails, open_fails, allocation_fails;
static void InitializeObjectAttributes(OBJECT_ATTRIBUTES* a, const UNICODE_STRING* b, int c, void* d, void* e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
static NTSTATUS ZwOpenKey(HANDLE* handle, int access, OBJECT_ATTRIBUTES* oa) { (void)access; (void)oa; if (open_fails) return -1; opens++; *handle = (void*)1; return 0; }
static void NtClose(HANDLE handle) { if (handle != (void*)1) abort(); closes++; }
static void* ExAllocatePoolWithTag(int kind, size_t size, int tag) { (void)kind; (void)tag; if (allocation_fails) return NULL; allocations++; allocated_bytes = size; return pool = calloc(1, size); }
static void ExFreePool(void* ptr) { if (ptr != pool) abort(); frees++; free(ptr); pool = NULL; }
static NTSTATUS ZwQueryValueKey(HANDLE handle, UNICODE_STRING* name, int type, KEY_VALUE_FULL_INFORMATION* value, ULONG capacity, ULONG* needed) {
    (void)handle; (void)name; (void)type;
    if (query_fails) return -1;
    size_t count = 0;
    while (options[count]) count++;
    value->DataOffset = sizeof(*value);
    value->DataLength = (ULONG)((count + 1) * sizeof(WCHAR));
    *needed = value->DataOffset + value->DataLength;
    if (*needed + sizeof(WCHAR) > capacity) abort();
    memcpy((char*)value + value->DataOffset, options, value->DataLength);
    return 0;
}
static WCHAR* host_wcsstr(const WCHAR* input, const WCHAR* token) {
    for (; *input; input++) {
        size_t n = 0;
        while (token[n] && input[n] == token[n]) n++;
        if (!token[n]) return (WCHAR*)input;
    }
    return NULL;
}
#define wcsstr host_wcsstr
#include "boot_options.inc"
int main(void) {
    const WCHAR* cases[] = { L"FASTDETECT SUBVOL=1a", L"FASTDETECT", L"FASTDETECT", L"", L"" };
    const char* names[] = { "success-with-subvol", "success-without-subvol", "query-failure", "open-failure", "allocation-failure" };
    unsigned failed = 0;
    for (unsigned i = 0; i < 5; i++) {
        allocations = frees = opens = closes = 0;
        allocated_bytes = 0;
        boot_subvol = 0;
        options = cases[i];
        query_fails = i == 2;
        open_fails = i == 3;
        allocation_fails = i == 4;
        check_boot_options();
        bool passed = allocations == frees && opens == closes && (i != 0 || boot_subvol == 0x1a);
        printf("%s %s: alloc=%u free=%u bytes=%zu open=%u close=%u subvol=%llu\n", passed ? "PASS" : "FAIL", names[i], allocations, frees, allocated_bytes, opens, closes, (unsigned long long)boot_subvol);
        failed += !passed;
        /* Reclaim leaked memory in the harness after measuring the failure. */
        if (pool) ExFreePool(pool);
    }
    printf("%u/5 boot scenarios passed\n", 5-failed);
    return failed != 0;
}
