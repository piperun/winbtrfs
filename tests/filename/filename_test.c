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

/* Exercise actual filename-query functions with Windows-sized character types. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int32_t NTSTATUS;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef wchar_t WCHAR;
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct { ULONG FileNameLength; WCHAR FileName[1]; } FILE_NAME_INFORMATION;
typedef struct file_ref file_ref;
typedef struct { file_ref* root_fileref; } device_extension;
typedef struct { device_extension* Vcb; bool ads; } fcb;
typedef struct { UNICODE_STRING name; } dir_child;
struct file_ref { fcb* fcb; file_ref* parent; dir_child* dc; };
#define STATUS_SUCCESS 0
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define NT_SUCCESS(s) ((s) >= 0)
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define RtlCopyMemory memcpy
#define RtlMoveMemory memmove
#include "filename.inc"

int main(void) {
    _Static_assert(sizeof(WCHAR) == 2, "Windows WCHAR size");
    device_extension vcb = {0};
    fcb root_fcb = {&vcb, false}, file_fcb = {&vcb, false}, ads_fcb = {&vcb, true};
    dir_child file_dc = {{8, 8, L"file"}}, ads_dc = {{12, 12, L"stream"}};
    file_ref root = {&root_fcb, NULL, NULL};
    file_ref file = {&file_fcb, &root, &file_dc};
    file_ref ads = {&ads_fcb, &file, &ads_dc};
    vcb.root_fileref = &root;
    const WCHAR* expected[] = {L"\\file:stream:$DATA", L"\\file", L"\\"};
    file_ref* refs[] = {&ads, &file, &root};
    unsigned required[] = {36, 10, 2};
    unsigned failed = 0, total = 0;
    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned capacity = 2; capacity <= required[kind] + 2; capacity += 2) {
            union { uint64_t align; unsigned char bytes[128]; } buffer;
            memset(buffer.bytes, 0xa5, sizeof(buffer.bytes));
            FILE_NAME_INFORMATION* info = (void*)buffer.bytes;
            LONG remaining = (LONG)(offsetof(FILE_NAME_INFORMATION, FileName) + capacity);
            NTSTATUS status = fill_in_file_name_information(info, refs[kind]->fcb, refs[kind], &remaining);
            bool overflow = capacity < required[kind];
            unsigned copied = min(capacity, required[kind]);
            bool passed = status == (overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS)
                && info->FileNameLength == required[kind]
                && remaining == (overflow ? -1 : (LONG)(capacity - required[kind]))
                && memcmp(info->FileName, expected[kind], copied) == 0;
            for (unsigned j = offsetof(FILE_NAME_INFORMATION, FileName) + capacity; j < sizeof(buffer.bytes); j++)
                passed = passed && buffer.bytes[j] == 0xa5;
            total++;
            if (!passed) {
                failed++;
                printf("FAIL kind=%u capacity=%u status=%x required=%u actual=%u remaining=%d\n",
                       kind, capacity, (unsigned)status, required[kind], info->FileNameLength, remaining);
            }
        }
    }
    printf("%u/%u filename scenarios passed\n", total-failed, total);
    return failed != 0;
}
