/* Healthy allocated-range queries on the isolated WBTRFS_TEST VM volume. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define STATUS_BUFFER_OVERFLOW_VALUE ((NTSTATUS)0x80000005)
#define STATUS_BUFFER_TOO_SMALL_VALUE ((NTSTATUS)0xC0000023)

typedef struct { LONGLONG offset, length; } expected_range;
static DWORD sector;
static unsigned passed;

static int write_at(HANDLE file, LONGLONG offset, const void *buffer, DWORD length) {
    LARGE_INTEGER position;
    DWORD done = 0;
    position.QuadPart = offset;
    return SetFilePointerEx(file, position, NULL, FILE_BEGIN) &&
           WriteFile(file, buffer, length, &done, NULL) && done == length;
}

static int query(HANDLE file, const char *name, LONGLONG offset, LONGLONG length,
                 ULONG output_bytes, NTSTATUS expected_status,
                 const expected_range *expected, size_t expected_count) {
    FILE_ALLOCATED_RANGE_BUFFER input = {0}, output[4] = {0};
    IO_STATUS_BLOCK iosb = {0};
    NTSTATUS status;
    size_t count;
    input.FileOffset.QuadPart = offset;
    input.Length.QuadPart = length;
    status = NtFsControlFile(file, NULL, NULL, NULL, &iosb,
                             FSCTL_QUERY_ALLOCATED_RANGES,
                             &input, sizeof(input), output, output_bytes);
    count = iosb.Information / sizeof(output[0]);
    printf("%s status=%08lx iosb=%08lx bytes=%llu", name,
           (unsigned long)status, (unsigned long)iosb.Status,
           (unsigned long long)iosb.Information);
    for (size_t i = 0; i < count && i < 4; ++i)
        printf(" range%zu=%lld:%lld", i,
               (long long)output[i].FileOffset.QuadPart,
               (long long)output[i].Length.QuadPart);
    putchar('\n');
    /* The returned NTSTATUS is authoritative; the IOSB status can differ. */
    if (status != expected_status) {
        fprintf(stderr, "Unexpected status for %s\n", name);
        return 0;
    }
    /* A too-small buffer has no promised output or Information value. */
    if (expected_status == STATUS_BUFFER_TOO_SMALL_VALUE) {
        passed++;
        return 1;
    }
    if (iosb.Information != expected_count * sizeof(output[0]) ||
        count != expected_count) {
        fprintf(stderr, "Unexpected output length for %s\n", name);
        return 0;
    }
    for (size_t i = 0; i < expected_count; ++i) {
        if (output[i].FileOffset.QuadPart != expected[i].offset ||
            output[i].Length.QuadPart != expected[i].length) {
            fprintf(stderr, "Unexpected range %zu for %s\n", i, name);
            return 0;
        }
    }
    passed++;
    return 1;
}

int wmain(void) {
    const wchar_t *dir = L"R:\\CodexRangePilot";
    const wchar_t *sparse_path = L"R:\\CodexRangePilot\\sparse.bin";
    const wchar_t *plain_path = L"R:\\CodexRangePilot\\plain.bin";
    wchar_t label[64], filesystem[64];
    DWORD sectors_per_cluster, bytes_per_sector, free_clusters, clusters, returned = 0;
    uint8_t block[65536];
    HANDLE sparse = INVALID_HANDLE_VALUE, plain = INVALID_HANDLE_VALUE;
    LARGE_INTEGER eof;
    int result = 1;

    if (!GetVolumeInformationW(L"R:\\", label, 64, NULL, NULL, NULL,
                               filesystem, 64) ||
        wcscmp(label, L"WBTRFS_TEST") != 0 ||
        _wcsicmp(filesystem, L"Btrfs") != 0 ||
        !GetDiskFreeSpaceW(L"R:\\", &sectors_per_cluster, &bytes_per_sector,
                           &free_clusters, &clusters)) {
        fputs("R: is not the isolated Btrfs test volume\n", stderr);
        return 2;
    }
    sector = sectors_per_cluster * bytes_per_sector;
    if (sector < 4096 || sector > sizeof(block) || (sector & 1)) {
        fprintf(stderr, "Unsupported fixture sector size: %lu\n", sector);
        return 2;
    }
    if (!CreateDirectoryW(dir, NULL)) {
        fprintf(stderr, "Task fixture directory exists or cannot be created: %lu\n",
                GetLastError());
        return 2;
    }
    memset(block, 0x5a, sector);
    sparse = CreateFileW(sparse_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    plain = CreateFileW(plain_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    eof.QuadPart = (LONGLONG)sector * 8;
    if (sparse == INVALID_HANDLE_VALUE || plain == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(sparse, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                         &returned, NULL) ||
        !SetFilePointerEx(sparse, eof, NULL, FILE_BEGIN) ||
        !SetEndOfFile(sparse) ||
        !write_at(sparse, (LONGLONG)sector * 2, block, sector) ||
        !write_at(sparse, (LONGLONG)sector * 6, block, sector) ||
        !write_at(plain, 0, block, sector) ||
        !write_at(plain, sector, block, sector) ||
        !write_at(plain, (LONGLONG)sector * 2, block, sector) ||
        !write_at(plain, (LONGLONG)sector * 3, block, sector) ||
        !FlushFileBuffers(sparse) || !FlushFileBuffers(plain)) {
        fprintf(stderr, "Healthy fixture creation failed: %lu\n", GetLastError());
        goto cleanup;
    }
    CloseHandle(sparse); sparse = INVALID_HANDLE_VALUE;
    CloseHandle(plain); plain = INVALID_HANDLE_VALUE;
    sparse = CreateFileW(sparse_path, GENERIC_READ, FILE_SHARE_READ,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    plain = CreateFileW(plain_path, GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (sparse == INVALID_HANDLE_VALUE || plain == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Fixture reopen failed: %lu\n", GetLastError());
        goto cleanup;
    }
    {
        expected_range both[] = {{(LONGLONG)sector * 2, sector},
                                 {(LONGLONG)sector * 6, sector}};
        expected_range first[] = {{(LONGLONG)sector * 2, sector}};
        expected_range half_first[] = {{(LONGLONG)sector * 2 + sector / 2,
                                        sector / 2}};
        expected_range second[] = {{(LONGLONG)sector * 6, sector}};
        expected_range half_second[] = {{(LONGLONG)sector * 6 + sector / 2,
                                         sector / 2}};
        expected_range normal[] = {{sector, (LONGLONG)sector * 2}};
        expected_range normal_tail[] = {{(LONGLONG)sector * 3, sector}};
        if (!query(sparse, "sparse-full", 0, (LONGLONG)sector * 8,
                   sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4, 0, both, 2) ||
            !query(sparse, "sparse-hole", (LONGLONG)sector * 3,
                   (LONGLONG)sector * 2, sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4,
                   0, NULL, 0) ||
            !query(sparse, "sparse-clip-first", (LONGLONG)sector * 2 + sector / 2,
                   sector, sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4, 0,
                   half_first, 1) ||
            !query(sparse, "sparse-later-run", (LONGLONG)sector * 5 + sector / 2,
                   (LONGLONG)sector * 2, sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4,
                   0, second, 1) ||
            !query(sparse, "sparse-eof-clip", (LONGLONG)sector * 6 + sector / 2,
                   (LONGLONG)sector * 2, sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4,
                   0, half_second, 1) ||
            !query(sparse, "sparse-at-eof", (LONGLONG)sector * 8, sector,
                   sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4, 0, NULL, 0) ||
            !query(sparse, "sparse-beyond-eof", (LONGLONG)sector * 9, sector,
                   sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4, 0, NULL, 0) ||
            !query(sparse, "sparse-zero-length", 0, 0,
                   sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4, 0, NULL, 0) ||
            !query(sparse, "sparse-one-slot", 0, (LONGLONG)sector * 8,
                   sizeof(FILE_ALLOCATED_RANGE_BUFFER),
                   STATUS_BUFFER_OVERFLOW_VALUE, first, 1) ||
            !query(sparse, "sparse-zero-slot", 0, (LONGLONG)sector * 8,
                   0, STATUS_BUFFER_TOO_SMALL_VALUE, NULL, 0) ||
            !query(plain, "normal-subrange", sector, (LONGLONG)sector * 2,
                   sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4, 0, normal, 1) ||
            !query(plain, "normal-eof-clip", (LONGLONG)sector * 3,
                   (LONGLONG)sector * 3,
                   sizeof(FILE_ALLOCATED_RANGE_BUFFER) * 4, 0, normal_tail, 1))
            goto cleanup;
    }
    printf("PASS %u healthy range queries\n", passed);
    result = 0;

cleanup:
    if (sparse != INVALID_HANDLE_VALUE) CloseHandle(sparse);
    if (plain != INVALID_HANDLE_VALUE) CloseHandle(plain);
    BOOL sparse_deleted = DeleteFileW(sparse_path);
    BOOL plain_deleted = DeleteFileW(plain_path);
    BOOL directory_removed = RemoveDirectoryW(dir);
    if (!sparse_deleted || !plain_deleted || !directory_removed) {
        fprintf(stderr, "Task fixture cleanup failed: %lu\n", GetLastError());
        result = 1;
    }
    return result;
}
