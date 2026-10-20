/* Native models for the driver query contracts; production handlers are included separately. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
typedef int NTSTATUS;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef int KPROCESSOR_MODE;
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS 0
#define STATUS_INSUFFICIENT_RESOURCES -1
#define STATUS_INTERNAL_ERROR -2
#define STATUS_INVALID_PARAMETER -3
#define STATUS_BUFFER_TOO_SMALL -4
#define STATUS_ACCESS_DENIED -5
#define STATUS_INVALID_DEVICE_REQUEST -6
#define STATUS_FILE_IS_A_DIRECTORY -7
#define STATUS_BUFFER_OVERFLOW -8
#define UserMode 1
#define FILE_READ_DATA 1
#define FILE_WRITE_DATA 2
#define BTRFS_FT_DIR 2
#define BTRFS_INODE_NODATASUM 1
#define BTRFS_FILE_EXTENT_INLINE 0
#define BTRFS_COMPRESS_NONE 0
#define BTRFS_CSUM_TYPE_CRC32 0
#define BTRFS_CSUM_TYPE_XXHASH 1
#define BTRFS_CSUM_TYPE_SHA256 2
#define BTRFS_CSUM_TYPE_BLAKE2 3
#define MAX_HASH_SIZE 32
#define PagedPool 0
#define ALLOC_TAG 0
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { unsigned sectorsize, csum_type; } superblock;
typedef struct { superblock superblock; unsigned sector_shift, csum_size; } device_extension;
typedef struct { unsigned type, compression; uint64_t num_bytes, ram_bytes, disk_bytenr; } extent_data;
typedef struct { LIST_ENTRY list_entry; bool ignore; uint64_t offset; extent_data extent_data; void *csum; } extent;
typedef struct { struct {void *Resource;} Header; bool ads; unsigned type, atts; struct {uint64_t flags,size;} inode_item; LIST_ENTRY extents; } fcb;
typedef struct { unsigned access; } ccb;
typedef struct { void *FsContext, *FsContext2; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct { uint8_t csum_type, csum_length; uint64_t num_sectors; uint8_t data[1]; } btrfs_csum_info;
static bool fail_pool;
static unsigned allocations, frees;
static void *ExAllocatePoolWithTag(int pool, size_t bytes, int tag) {
    (void)pool; (void)tag;
    if (fail_pool) return NULL;
    void *p = malloc(bytes);
    if (p) allocations++;
    return p;
}
static void ExFreePool(void *p) { if (p) frees++; free(p); }
static void get_sector_csum(device_extension *v, void *p, void *out) {(void)v;(void)p; memset(out,0x6b,32);}
static unsigned acquired, released;
static void ExAcquireResourceSharedLite(void *p,bool wait) {(void)p;(void)wait; acquired++;}
static void ExReleaseResourceLite(void *p) {(void)p;released++;}

#define FILE_ATTRIBUTE_SPARSE_FILE 0x200
#define BTRFS_FILE_EXTENT_REG 1
#define BTRFS_FILE_EXTENT_PREALLOC 2
#define min(a,b) ((a)<(b)?(a):(b))
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { LARGE_INTEGER FileOffset, Length; } FILE_ALLOCATED_RANGE_BUFFER;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr,"line %u: %s\n",__LINE__,#condition); return 1; } } while (0)
static void link_extents(fcb *node, extent *items, unsigned count) {
    node->extents.Flink=count ? &items[0].list_entry : &node->extents;
    for (unsigned i=0;i<count;i++) items[i].list_entry.Flink=i+1<count ? &items[i+1].list_entry : &node->extents;
}
