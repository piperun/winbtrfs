#pragma once
#include "host.h"
typedef unsigned POOL_TYPE;
typedef unsigned KIRQL;
typedef struct {bool held;} KSPIN_LOCK;
typedef int32_t LONG;
#include "balance-types.inc"
typedef uint32_t ULONG;typedef void* PVOID;
typedef struct {unsigned held;} ERESOURCE;
typedef struct {bool signaled;} KEVENT;
typedef struct {unsigned refs,id,StackSize;} DEVICE_OBJECT,*PDEVICE_OBJECT,FILE_OBJECT,*PFILE_OBJECT;
typedef struct {NTSTATUS Status;uintptr_t Information;} IO_STATUS_BLOCK;
typedef struct {int64_t StartingOffset;uint64_t LengthInBytes;} DEVICE_DATA_SET_RANGE;
typedef struct {ULONG Size,Action,Flags,ParameterBlockOffset,ParameterBlockLength,DataSetRangesOffset,DataSetRangesLength;} DEVICE_MANAGE_DATA_SET_ATTRIBUTES;
typedef struct IRP IRP,*PIRP;
typedef NTSTATUS (*COMPLETION)(PDEVICE_OBJECT,PIRP,PVOID);
typedef struct {unsigned MajorFunction;PFILE_OBJECT FileObject;struct {struct {ULONG IoControlCode,InputBufferLength,OutputBufferLength;} DeviceIoControl;} Parameters;} IO_STACK_LOCATION,*PIO_STACK_LOCATION;
struct IRP {IO_STACK_LOCATION stack;struct{void* SystemBuffer;}AssociatedIrp;unsigned Flags;void* UserBuffer;IO_STATUS_BLOCK* UserIosb;COMPLETION completion;void* context;bool submitted,completed;};
typedef struct {LIST_ENTRY list_entry,space,trim_list;PDEVICE_OBJECT devobj;PFILE_OBJECT fileobj;bool trim,readonly;ULONG num_trim_entries;struct{uint64_t devid,total_bytes;}devitem;} device;
struct btrfs_chunk {uint64_t type,stripe_len,length;uint16_t num_stripes,sub_stripes;struct {uint64_t offset,devid;} stripe[4];};
struct chunk {LIST_ENTRY list_entry,space,space_size,deleting,pending_trims,list_entry_balance;unsigned held;uint64_t offset,used;struct btrfs_chunk* chunk_item;device** devices;bool space_changed,reloc;};
struct trim_state;
typedef struct {LIST_ENTRY devices,chunks;ERESOURCE tree_lock,chunk_lock;bool trim,removing;struct{bool no_trim,no_barrier;}options;struct trim_state* trim_state;struct {btrfs_balance_opts opts[3];}balance;}device_extension;
#define NonPagedPool 0
#define NotificationEvent 0
#define Executive 0
#define KernelMode 0
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xc0000016)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define STATUS_CANCELLED ((NTSTATUS)0xc0000120)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3)
#define IRP_BUFFERED_IO 16
#define IRP_MJ_DEVICE_CONTROL 14
#define IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES 0x2d9404
#define DeviceDsmAction_Trim 1
#define DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED 0x80000000
#define BTRFS_BLOCK_GROUP_DATA 1
#define BTRFS_BLOCK_GROUP_METADATA 4
#define BTRFS_BLOCK_GROUP_DUP 32
#define BTRFS_BLOCK_GROUP_RAID0 8
#define BTRFS_BLOCK_GROUP_RAID1 16
#define BTRFS_BLOCK_GROUP_RAID10 64
#define BTRFS_BLOCK_GROUP_RAID5 128
#define BTRFS_BLOCK_GROUP_RAID6 256
#define BTRFS_BLOCK_GROUP_RAID1C3 512
#define BTRFS_BLOCK_GROUP_RAID1C4 1024
#define __stdcall
#define _Function_class_(x)
#define UNUSED(x) ((void)(x))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define sector_align(n,a) (((n)+(a)-1)&~((a)-1))
#define IoSizeOfIrp(n) (sizeof(IRP)+(n)*sizeof(IO_STACK_LOCATION))
#define min(a,b) ((a)<(b)?(a):(b))
#define IoGetNextIrpStackLocation(i) (&(i)->stack)
static device_extension* active;
static void free_spaces(LIST_ENTRY* list);
static void trim_initialize(device_extension*);
static void trim_destroy(device_extension*);
static void trim_process(device_extension*);
static void trim_drain(device_extension*);
static void trim_commit(device_extension*);
static bool trim_chunk_pending(const chunk*);
static NTSTATUS trim_merge_pending(chunk*,LIST_ENTRY*,LIST_ENTRY*);

static void get_raid0_offset(uint64_t a,uint64_t b,uint16_t c,uint64_t* d,uint16_t* e){*d=(a/(b*c))*b+a%b;*e=(uint16_t)((a/b)%c);}

static void poll_during_signal(void);
static void balance_allocation_during_preparation(void);

#define TRACE(...) ((void)0)

void trim_pause_locked(device_extension*);
void trim_wait_idle(device_extension*);
void trim_resume(device_extension*);
