/* Per-invocation planning and reader ownership; nothing survives a flush. */
#pragma once

#define PREFETCH_MAX_RANGES 32768
#define PREFETCH_MAX_JOBS 4096
#define PREFETCH_READERS 4
#define PREFETCH_MIN_JOBS 32
#define PREFETCH_TREE_BUDGET (64 * 1024 * 1024)
#define PREFETCH_PLAN_BUDGET (1024 * 1024)
#define PREFETCH_IO_BUDGET (4 * 1024 * 1024)

typedef struct { uint64_t start, end; } prefetch_range;
typedef struct {
    uint64_t address, generation;
    tree* parent;
    tree_data* item;
} prefetch_job;

typedef struct {
    device_extension* Vcb;
    KEVENT work, done;
    HANDLE handle;
    bool stop;
    prefetch_job job;
    uint8_t* buffer;
    NTSTATUS status;
} prefetch_reader;

typedef struct {
    device_extension* Vcb;
    prefetch_range* ranges;
    prefetch_job* jobs;
    uint32_t range_count, job_count;
    size_t tree_bytes;
    bool successor, full;
    prefetch_reader readers[PREFETCH_READERS];
} prefetch_context;

void prefetch_collect_ranges(prefetch_context* ctx);
void prefetch_plan_trees(prefetch_context* ctx);
void prefetch_read_jobs(prefetch_context* ctx);
size_t prefetch_io_bytes(device_extension* Vcb, uint64_t address);
size_t prefetch_tree_bytes(uint32_t nodesize, uint32_t count, uint8_t level);
