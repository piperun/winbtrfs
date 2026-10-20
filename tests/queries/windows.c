/* Bounded functional checks for the isolated WB_RAID6 Windows volume. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>
#include "btrfs.h"
#include "btrfsioctl.h"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %u: %s (Win32=%lu)\n", __LINE__, #x, GetLastError()); \
    return 1; \
} } while (0)

static wchar_t root[16], dir[MAX_PATH];
static wchar_t plain[MAX_PATH], mixed[MAX_PATH], sparse[MAX_PATH];
static DWORD sector;
static int mixed_gap;

static uint32_t crc32c(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82f63b78u : 0);
    }
    return ~crc;
}

static int write_at(HANDLE h, LONGLONG offset, const void *data, DWORD length) {
    LARGE_INTEGER position; position.QuadPart = offset;
    DWORD written = 0;
    return SetFilePointerEx(h, position, NULL, FILE_BEGIN) &&
           WriteFile(h, data, length, &written, NULL) && written == length;
}

static int set_inode(HANDLE h, uint64_t flags, uint8_t compression) {
    btrfs_set_inode_info info = {0};
    IO_STATUS_BLOCK iosb = {0};
    info.flags_changed = TRUE;
    info.flags = flags;
    info.compression_type_changed = TRUE;
    info.compression_type = compression;
    NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb,
                                     FSCTL_BTRFS_SET_INODE_INFO,
                                     &info, sizeof(info), NULL, 0);
    if (status) fprintf(stderr, "set inode status=%08lx\n", (unsigned long)status);
    return status == 0;
}

static int checksum_query(HANDLE h, const uint8_t *expected, unsigned sectors,
                          unsigned compressed_sectors) {
    union { uint64_t align; uint8_t bytes[4096]; } output = {0};
    IO_STATUS_BLOCK iosb = {0};
    NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb,
                                     FSCTL_BTRFS_GET_CSUM_INFO,
                                     NULL, 0, output.bytes, sizeof(output.bytes));
    CHECK(status == 0);
    CHECK(iosb.Information >= offsetof(btrfs_csum_info, data));
    CHECK(iosb.Information <= sizeof(output.bytes));
    btrfs_csum_info *info = (void *)output.bytes;
    CHECK(info->csum_type == BTRFS_CSUM_TYPE_CRC32);
    CHECK(info->csum_length == sizeof(uint32_t));
    CHECK(info->num_sectors == sectors);
    CHECK(iosb.Information == offsetof(btrfs_csum_info, data) +
                              sectors * sizeof(uint32_t));
    for (unsigned i = 0; i < sectors; ++i) {
        uint32_t found = 0;
        memcpy(&found, info->data + i * sizeof(found), sizeof(found));
        if (i < compressed_sectors) CHECK(found == 0);
        else CHECK(found == crc32c(expected + i * sector, sector));
    }
    return 0;
}

static int plain_checksums(void) {
    uint8_t *bytes = malloc(sector * 16);
    CHECK(bytes != NULL);
    for (unsigned i = 0; i < sector * 16; ++i)
        bytes[i] = (uint8_t)(i * 73u + (i / sector) * 19u + 19u);
    HANDLE h = CreateFileW(plain, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(write_at(h, 0, bytes, sector * 16));
    CHECK(FlushFileBuffers(h));
    CHECK(CloseHandle(h));
    h = CreateFileW(plain, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(checksum_query(h, bytes, 16, 0) == 0);
    CHECK(CloseHandle(h));
    free(bytes);
    puts("PASS plain checksum query: 16 independently checked sectors");
    return 0;
}

static int mixed_checksums(void) {
    const unsigned compressed = 32;
    uint8_t *bytes = calloc(compressed + 1, sector);
    CHECK(bytes != NULL);
    for (unsigned i = 0; i < sector; ++i)
        bytes[compressed * sector + i] = (uint8_t)(i * 73u + 41u);
    HANDLE h = CreateFileW(mixed, GENERIC_READ | GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
                           0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(set_inode(h, BTRFS_INODE_COMPRESS, BTRFS_COMPRESS_ZLIB));
    CHECK(write_at(h, 0, bytes, compressed * sector));
    CHECK(FlushFileBuffers(h));
    CHECK(CloseHandle(h));
    h = CreateFileW(mixed, GENERIC_READ | GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
                    0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(set_inode(h, BTRFS_INODE_NOCOMPRESS, BTRFS_COMPRESS_ANY));
    CHECK(write_at(h, compressed * sector, bytes + compressed * sector, sector));
    CHECK(FlushFileBuffers(h));
    CHECK(CloseHandle(h));
    h = CreateFileW(mixed, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    btrfs_inode_info inode = {0};
    IO_STATUS_BLOCK iosb = {0};
    NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb,
                                     FSCTL_BTRFS_GET_INODE_INFO,
                                     NULL, 0, &inode, sizeof(inode));
    CHECK(status == 0);
    if (inode.disk_size_zlib == 0) {
        mixed_gap = 1;
        printf("COVERAGE GAP mixed checksum: compressed extent absent (zlib bytes=%llu)\n",
               (unsigned long long)inode.disk_size_zlib);
        CHECK(CloseHandle(h));
        free(bytes);
        return 0;
    }
    STARTING_VCN_INPUT_BUFFER start = {0};
    start.StartingVcn.QuadPart = 0;
    union { uint64_t align; uint8_t bytes[4096]; } mapping = {0};
    DWORD mapped = 0;
    if (!DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTERS, &start, sizeof(start),
                         mapping.bytes, sizeof(mapping.bytes), &mapped, NULL)) {
        mixed_gap = 1;
        puts("COVERAGE GAP mixed checksum: plain retrieval mapping unavailable");
        CHECK(CloseHandle(h));
        free(bytes);
        return 0;
    }
    CHECK(mapped >= offsetof(RETRIEVAL_POINTERS_BUFFER, Extents[0]));
    CHECK(mapped <= sizeof(mapping.bytes));
    RETRIEVAL_POINTERS_BUFFER *runs = (void *)mapping.bytes;
    size_t capacity = (mapped - offsetof(RETRIEVAL_POINTERS_BUFFER, Extents[0])) /
                      sizeof(runs->Extents[0]);
    CHECK(runs->ExtentCount <= capacity);
    BOOL compressed_run = FALSE, plain_run = FALSE;
    LONGLONG prior = runs->StartingVcn.QuadPart;
    for (DWORD i = 0; i < runs->ExtentCount; ++i) {
        LONGLONG next = runs->Extents[i].NextVcn.QuadPart;
        LONGLONG lcn = runs->Extents[i].Lcn.QuadPart;
        CHECK(next > prior);
        if (prior == 0 && next == compressed && lcn == -1) compressed_run = TRUE;
        if (prior == compressed && next >= compressed + 1 && lcn >= 0) plain_run = TRUE;
        prior = next;
    }
    if (!compressed_run || !plain_run) {
        mixed_gap = 1;
        printf("COVERAGE GAP mixed checksum: expected compressed/plain mapping absent (runs=%lu)\n",
               runs->ExtentCount);
        CHECK(CloseHandle(h));
        free(bytes);
        return 0;
    }
    CHECK(checksum_query(h, bytes, compressed + 1, compressed) == 0);
    CHECK(CloseHandle(h));
    free(bytes);
    printf("PASS mixed checksum query: %u compressed placeholders, plain sector CRC32C, zlib bytes=%llu\n",
           compressed, (unsigned long long)inode.disk_size_zlib);
    return 0;
}

static int allocated_ranges(void) {
    uint8_t *bytes = malloc(sector);
    CHECK(bytes != NULL);
    memset(bytes, 0x47, sector);
    HANDLE h = CreateFileW(sparse, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    CHECK(DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL));
    LARGE_INTEGER end; end.QuadPart = sector * 8;
    CHECK(SetFilePointerEx(h, end, NULL, FILE_BEGIN) && SetEndOfFile(h));
    CHECK(write_at(h, sector * 2, bytes, sector));
    CHECK(write_at(h, sector * 6, bytes, sector));
    CHECK(FlushFileBuffers(h));
    CHECK(CloseHandle(h));
    h = CreateFileW(sparse, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    FILE_ALLOCATED_RANGE_BUFFER input = {0}, output[32] = {0};
    input.FileOffset.QuadPart = 0;
    input.Length.QuadPart = sector * 8;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == 2 * sizeof(output[0]));
    CHECK(output[0].FileOffset.QuadPart == sector * 2 && output[0].Length.QuadPart == sector);
    CHECK(output[1].FileOffset.QuadPart == sector * 6 && output[1].Length.QuadPart == sector);

    input.FileOffset.QuadPart = sector * 3; /* Hole only. */
    input.Length.QuadPart = sector * 2;
    returned = 1234;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == 0);

    input.FileOffset.QuadPart = sector;
    input.Length.QuadPart = sector * 3; /* First run, clipped by the request. */
    returned = 0;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == sizeof(output[0]));
    CHECK(output[0].FileOffset.QuadPart == sector * 2 && output[0].Length.QuadPart == sector);

    input.FileOffset.QuadPart = sector * 5;
    input.Length.QuadPart = sector * 3; /* Start after first run. */
    returned = 0;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == sizeof(output[0]));
    CHECK(output[0].FileOffset.QuadPart == sector * 6 && output[0].Length.QuadPart == sector);

    input.FileOffset.QuadPart = sector * 8;
    input.Length.QuadPart = sector; /* Begin at EOF. */
    returned = 0;
    BOOL eof_success = DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                                       &input, sizeof(input), output, sizeof(output), &returned, NULL);
    CHECK(eof_success && returned == 0);
    input.FileOffset.QuadPart = sector * 9;
    returned = 1234;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == 0);
    input.FileOffset.QuadPart = 0;
    input.Length.QuadPart = 0;
    returned = 1234;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == 0);
    input.FileOffset.QuadPart = sector * 6;
    input.Length.QuadPart = sector * 5;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == sizeof(output[0]));
    CHECK(output[0].FileOffset.QuadPart == sector * 6 && output[0].Length.QuadPart == sector);
    CHECK(CloseHandle(h));
    free(bytes);
    puts("PASS allocated ranges: full, hole-only, clipped and later-run queries");
    return 0;
}

static int normal_subrange(void) {
    HANDLE h = CreateFileW(plain, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    FILE_ALLOCATED_RANGE_BUFFER input = {0}, output[32] = {0};
    input.FileOffset.QuadPart = sector;
    input.Length.QuadPart = sector * 2;
    DWORD returned = 0;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == sizeof(output[0]));
    CHECK(output[0].FileOffset.QuadPart == sector &&
          output[0].Length.QuadPart == sector * 2);
    input.FileOffset.QuadPart = sector * 15;
    input.Length.QuadPart = sector * 4;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == sizeof(output[0]));
    CHECK(output[0].FileOffset.QuadPart == sector * 15 && output[0].Length.QuadPart == sector);
    CHECK(CloseHandle(h));
    puts("PASS allocated ranges: normal file subrange");
    return 0;
}

static int empty_ranges(void) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\empty.bin", dir);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    FILE_ALLOCATED_RANGE_BUFFER input = {0}, output[4] = {0};
    input.Length.QuadPart = sector;
    DWORD returned = 1234;
    CHECK(DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES,
                          &input, sizeof(input), output, sizeof(output), &returned, NULL));
    CHECK(returned == 0);
    CHECK(CloseHandle(h));
    CHECK(DeleteFileW(path));
    puts("PASS allocated ranges: empty file");
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 2 || wcslen(argv[1]) != 2 || argv[1][1] != L':' ||
        argv[1][0] < L'D' || argv[1][0] > L'Z') return 2;
    root[0] = argv[1][0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    wchar_t label[64], fs[64];
    DWORD sectors_per_cluster, bytes_per_sector, free_clusters, clusters;
    CHECK(GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, fs, 64));
    CHECK(wcscmp(label, L"WB_RAID6") == 0 && _wcsicmp(fs, L"Btrfs") == 0);
    CHECK(GetDiskFreeSpaceW(root, &sectors_per_cluster, &bytes_per_sector,
                            &free_clusters, &clusters));
    sector = sectors_per_cluster * bytes_per_sector;
    CHECK(sector == 4096);
    swprintf(dir, MAX_PATH, L"%lsvm-functional-%lu", root, GetCurrentProcessId());
    swprintf(plain, MAX_PATH, L"%ls\\plain.bin", dir);
    swprintf(mixed, MAX_PATH, L"%ls\\mixed.bin", dir);
    swprintf(sparse, MAX_PATH, L"%ls\\sparse.bin", dir);
    CHECK(CreateDirectoryW(dir, NULL));
    printf("Directory: %ls\n", dir);
    int result = 1;
    do {
        if (plain_checksums() || mixed_checksums() ||
            normal_subrange() || allocated_ranges() || empty_ranges()) break;
        result = mixed_gap ? 3 : 0;
    } while (0);
    if (result == 0) {
        CHECK(DeleteFileW(plain) && DeleteFileW(mixed) && DeleteFileW(sparse));
        CHECK(RemoveDirectoryW(dir));
    }
    return result;
}
