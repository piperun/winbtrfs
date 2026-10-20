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

/* Bounded filename-query checks on disposable WB_RAID5/WB_RAID6 test volumes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
typedef struct { ULONG FileNameLength; WCHAR FileName[1]; } NAME_INFORMATION;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %u: %s (error %lu)\n", \
    __LINE__, #c, GetLastError()); failed = 1; goto cleanup; } } while (0)

int wmain(int argc, wchar_t** argv) {
    int failed = 0;
    BOOL directory_created = FALSE, base_created = FALSE;
    HANDLE file = INVALID_HANDLE_VALUE;
    wchar_t directory[160] = {0}, base[200] = {0}, stream[220] = {0};
    if (argc != 2 || wcslen(argv[1]) != 2 || argv[1][1] != L':' ||
        argv[1][0] < L'D' || argv[1][0] > L'Z') return 2;

    wchar_t root[] = L"D:\\", label[64], fs[64];
    root[0] = argv[1][0];
    CHECK(GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, fs, 64));
    CHECK((!wcscmp(label, L"WB_RAID5") || !wcscmp(label, L"WB_RAID6")) &&
          !_wcsicmp(fs, L"Btrfs"));
    CHECK(swprintf(directory, 160, L"%lsfilename-%lu-%llu", root, GetCurrentProcessId(),
                   (unsigned long long)GetTickCount64()) > 0);
    /* Exclusive directory and file creation preserve any existing fixture. */
    CHECK(CreateDirectoryW(directory, NULL));
    directory_created = TRUE;
    CHECK(swprintf(base, 200, L"%ls\\file", directory) > 0);
    file = CreateFileW(base, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    base_created = TRUE;
    CHECK(CloseHandle(file));
    file = INVALID_HANDLE_VALUE;
    CHECK(swprintf(stream, 220, L"%ls:stream", base) > 0);
    file = CreateFileW(stream, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);

    wchar_t expected[240];
    CHECK(swprintf(expected, 240, L"%ls:$DATA", stream + 2) > 0);
    ULONG path_length = (ULONG)(wcslen(stream + 2) * sizeof(WCHAR));
    ULONG required = (ULONG)(wcslen(expected) * sizeof(WCHAR));
    unsigned passed = 0;
    /* One truncated path, six partial suffixes, and two complete names. */
    for (ULONG capacity = path_length - sizeof(WCHAR); capacity <= required + sizeof(WCHAR);
         capacity += sizeof(WCHAR)) {
        union { uint64_t align; unsigned char bytes[1024]; } storage;
        memset(storage.bytes, 0xa5, sizeof(storage.bytes));
        NAME_INFORMATION* info = (void*)storage.bytes;
        ULONG output_length = (ULONG)offsetof(NAME_INFORMATION, FileName) + capacity;
        IO_STATUS_BLOCK iosb;
        NTSTATUS status = NtQueryInformationFile(file, &iosb, info, output_length,
                                                 (FILE_INFORMATION_CLASS)9);
        BOOL overflow = capacity < required;
        CHECK(status == (overflow ? STATUS_BUFFER_OVERFLOW : 0));
        CHECK(info->FileNameLength == required);
        ULONG copied = capacity < required ? capacity : required;
        CHECK(!memcmp(info->FileName, expected, copied));
        passed++;
    }
    printf("PASS %u named-stream filename capacities, including truncated :$DATA\n", passed);

cleanup:
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) failed = 1;
    /* Deleting this newly created base file also deletes its named stream. */
    if (base_created && !DeleteFileW(base)) {
        fwprintf(stderr, L"Cleanup failed for %ls (error %lu)\n", base, GetLastError());
        failed = 1;
    }
    if (directory_created && !RemoveDirectoryW(directory)) {
        fwprintf(stderr, L"Cleanup failed for %ls (error %lu)\n", directory, GetLastError());
        failed = 1;
    }
    return failed;
}
