/* Production add_device with modeled kernel, device and tree services. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btrfs.h"

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uint16_t USHORT, WCHAR;
typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef int KPROCESSOR_MODE;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { uint8_t bytes[16]; } GUID;
typedef struct { int64_t value; } LUID;
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct { int held; } ERESOURCE;
typedef struct _DEVICE_OBJECT { struct _DEVICE_OBJECT* AttachedDevice; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct { PDEVICE_OBJECT DeviceObject; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct { struct { struct { ULONG InputBufferLength; } FileSystemControl; } Parameters; } IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct { IO_STACK_LOCATION* stack; struct { void* SystemBuffer; } AssociatedIrp; KPROCESSOR_MODE RequestorMode; } IRP, *PIRP;
typedef struct { ULONG DeviceType, DeviceNumber, PartitionNumber; } STORAGE_DEVICE_NUMBER;
typedef struct { LARGE_INTEGER Length; } GET_LENGTH_INFORMATION;
typedef struct { ULONG PartitionStyle, PartitionCount; } DRIVE_LAYOUT_INFORMATION_EX;
typedef struct { struct btrfs_key key; } tree_data;
typedef struct { tree_data* item; } traverse_ptr;
typedef struct { int unused; } root;
typedef struct {
    LIST_ENTRY list_entry, space; PDEVICE_OBJECT devobj; PFILE_OBJECT fileobj;
    bool seeding, trim, readonly; struct btrfs_dev_item devitem;
} device;
typedef struct { LIST_ENTRY list_entry; } space;
typedef struct { ERESOURCE child_lock; LIST_ENTRY children; uint64_t num_children, children_loaded; } pdo_device_extension;
typedef struct { pdo_device_extension* pdode; } volume_device_extension;
typedef struct {
    LIST_ENTRY list_entry; uint8_t uuid[BTRFS_UUID_SIZE]; uint64_t devid, generation, size;
    PDEVICE_OBJECT devobj; PFILE_OBJECT fileobj; void* notification_entry;
    bool boot_volume, seeding, had_drive_letter; UNICODE_STRING pnp_name; ULONG disk_num, part_num;
} volume_child;
typedef struct {
    bool direct_mount, readonly, need_write; volume_device_extension* vde; ERESOURCE tree_lock; LIST_ENTRY devices;
    struct btrfs_super_block superblock; uint64_t devices_loaded; root *chunk_root, *dev_root; void* root_file;
    struct { bool no_trim; } options;
} device_extension;

#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225)
#define STATUS_PRIVILEGE_NOT_HELD ((NTSTATUS)0xc0000061)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#define STATUS_MEDIA_WRITE_PROTECTED ((NTSTATUS)0xc00000a2)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xc00000e5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define NT_SUCCESS(s) ((s) >= 0)
#define IOCTL_DISK_IS_WRITABLE 1
#define IOCTL_DISK_GET_DRIVE_LAYOUT_EX 2
#define IOCTL_STORAGE_GET_DEVICE_NUMBER 3
#define IOCTL_DISK_GET_LENGTH_INFO 4
#define FILE_DEVICE_DISK 7
#define SE_MANAGE_VOLUME_PRIVILEGE 28
#define NonPagedPool 0
#define PagedPool 1
#define ALLOC_TAG 0
#define EventCategoryTargetDeviceChange 2
#define FILE_READ_ATTRIBUTES 0x80
#define FSRTL_VOLUME_CHANGE_SIZE 13
#define MOUNTMGR_DEVICE_NAME u"\\Device\\MountPointManager"
#define __stdcall
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p, n) memset(p, 0, n)
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define keycmp(key1, key2)\
    ((key1.objectid < key2.objectid) ? -1 :\
    ((key1.objectid > key2.objectid) ? 1 :\
    ((key1.type < key2.type) ? -1 :\
    ((key1.type > key2.type) ? 1 :\
    ((key1.offset < key2.offset) ? -1 :\
    ((key1.offset > key2.offset) ? 1 :\
    0))))))
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %u: %s\n", __LINE__, #c); exit(1); } } while (0)

static const GUID GUID_DEVINTERFACE_DISK = { { 0x07, 0x63, 0xf5, 0x53 } };
static const GUID GUID_DEVINTERFACE_VOLUME = { { 0x0d, 0x30, 0xf2, 0x53 } };
static void* file_object_type;
static void** IoFileObjectType = &file_object_type;
static void* drvobj;
static DEVICE_OBJECT target_device, mountmgr_device;
static FILE_OBJECT target_file = { &target_device }, mountmgr_file;
static unsigned allocation_attempts, fail_allocation, pool_live, volume_notifications, held_locks;
static unsigned notifications_registered;
static ERESOURCE* active_tree_lock;
enum failure_point {
    NO_FAILURE, WRITABLE, RAID_MEMBER, WRONG_DEVICE_TYPE, LENGTH_IOCTL, TOO_SMALL,
    PREFLUSH, INSERT_DEV_ITEM, FIND_STATS, DELETE_STATS, INSERT_STATS, WRITE_ZERO_BUFFER, POSTFLUSH
};
static enum failure_point fail_operation;
static int file_references;

static void* ExAllocatePoolWithTag(int pool, size_t size, unsigned tag) {
    (void)pool; (void)tag;
    if (++allocation_attempts == fail_allocation) return NULL;
    void* p = calloc(1, size); CHECK(p); pool_live++; return p;
}
static void ExFreePool(void* p) { CHECK(p && pool_live); pool_live--; free(p); }
static void InitializeListHead(LIST_ENTRY* head) { head->Flink = head->Blink = head; }
static bool IsListEmpty(const LIST_ENTRY* head) { return head->Flink == head; }
static LIST_ENTRY* RemoveHeadList(LIST_ENTRY* head) {
    CHECK(!IsListEmpty(head));
    LIST_ENTRY* entry = head->Flink;
    entry->Flink->Blink = head;
    head->Flink = entry->Flink;
    return entry;
}
static void InsertTailList(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Blink = head->Blink; entry->Flink = head; head->Blink->Flink = entry; head->Blink = entry;
}
static size_t RtlCompareMemory(const void* a, const void* b, size_t n) {
    size_t i = 0; while (i < n && ((const uint8_t*)a)[i] == ((const uint8_t*)b)[i]) i++; return i;
}
static void RtlInitUnicodeString(PUNICODE_STRING s, const WCHAR* text) {
    USHORT n = 0; while (text[n]) n++;
    s->Buffer = (WCHAR*)text; s->Length = s->MaximumLength = n * sizeof(WCHAR);
}
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return irp->stack; }
static LUID RtlConvertLongToLuid(long value) { LUID luid = { value }; return luid; }
static bool SeSinglePrivilegeCheck(LUID privilege, KPROCESSOR_MODE mode) { (void)mode; return privilege.value == SE_MANAGE_VOLUME_PRIVILEGE; }
static NTSTATUS ObReferenceObjectByHandle(HANDLE h, ULONG access, void* type, KPROCESSOR_MODE mode, void** object, void* info) {
    (void)access; (void)mode; (void)info; CHECK(type == file_object_type && h == (HANDLE)&target_file);
    *object = &target_file; file_references++; return STATUS_SUCCESS;
}
static void ObReferenceObject(void* object) { if (object == &target_file) file_references++; }
static void ObDereferenceObject(void* object) { if (object == &target_file) file_references--; }
/* The PnP name is caller-owned pool, like the driver's. The driver's lookup tries three interface
 * classes and reports STATUS_NOT_FOUND when none succeeds, whatever the reason. */
static NTSTATUS get_device_pnp_name(PDEVICE_OBJECT device, PUNICODE_STRING name, const GUID** guid) {
    static const WCHAR text[] = u"\\??\\Volume{1234}";
    CHECK(device == &target_device);
    name->Buffer = ExAllocatePoolWithTag(PagedPool, sizeof(text), ALLOC_TAG);
    if (!name->Buffer) return STATUS_NOT_FOUND;
    memcpy(name->Buffer, text, sizeof(text)); name->Length = name->MaximumLength = sizeof(text) - sizeof(WCHAR);
    *guid = &GUID_DEVINTERFACE_VOLUME; return STATUS_SUCCESS;
}
static NTSTATUS dev_ioctl(PDEVICE_OBJECT device, ULONG code, void* in, ULONG inlen, void* out, ULONG outlen, bool override, void* iosb) {
    (void)in; (void)inlen; (void)outlen; (void)override; (void)iosb; CHECK(device == &target_device);
    if ((code == IOCTL_DISK_IS_WRITABLE && fail_operation == WRITABLE) ||
        (code == IOCTL_DISK_GET_LENGTH_INFO && fail_operation == LENGTH_IOCTL))
        return STATUS_IO_DEVICE_ERROR;
    if (code == IOCTL_STORAGE_GET_DEVICE_NUMBER) {
        STORAGE_DEVICE_NUMBER* sdn = out;
        sdn->DeviceType = fail_operation == WRONG_DEVICE_TYPE ? 0 : FILE_DEVICE_DISK;
        sdn->DeviceNumber = 2; sdn->PartitionNumber = 1;
    } else if (code == IOCTL_DISK_GET_LENGTH_INFO)
        ((GET_LENGTH_INFORMATION*)out)->Length.QuadPart = fail_operation == TOO_SMALL ? 0x80000 : 0x200000;
    else CHECK(code == IOCTL_DISK_IS_WRITABLE);
    return STATUS_SUCCESS;
}
static NTSTATUS is_device_part_of_mounted_btrfs_raid(PDEVICE_OBJECT device, PFILE_OBJECT file) {
    CHECK(device == &target_device && file == &target_file);
    return fail_operation == RAID_MEMBER ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
static void volume_removal(PUNICODE_STRING name) { (void)name; }
static void ExAcquireResourceExclusiveLite(ERESOURCE* r, bool wait) { (void)wait; r->held++; held_locks++; }
static void ExReleaseResourceLite(ERESOURCE* r) { CHECK(r->held); r->held--; held_locks--; }
static NTSTATUS do_write(device_extension* v, PIRP irp) {
    (void)v; (void)irp;
    return fail_operation == PREFLUSH || fail_operation == POSTFLUSH ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
static void free_trees(device_extension* v) { (void)v; }
static void init_device(device_extension* v, device* dev, bool get_nums) { (void)v; (void)get_nums; dev->trim = dev->readonly = false; }
static NTSTATUS add_space_entry(LIST_ENTRY* list, LIST_ENTRY* list_size, uint64_t offset, uint64_t size) {
    (void)list_size; (void)offset; (void)size;
    space* entry = ExAllocatePoolWithTag(PagedPool, sizeof(space), ALLOC_TAG);
    if (!entry) return STATUS_INSUFFICIENT_RESOURCES;
    InsertTailList(list, &entry->list_entry);
    return STATUS_SUCCESS;
}
static void get_uuid(uint8_t* uuid) { memset(uuid, 0x5a, BTRFS_UUID_SIZE); }
/* Tree insertion takes ownership of the item data, which stays valid while the tree holds it. */
static void* inserted_items[4];
static unsigned inserted_count;
static NTSTATUS insert_tree_item(device_extension* v, root* r, uint64_t objid, uint8_t type, uint64_t offset, void* data,
                                 uint16_t size, traverse_ptr* ptp, PIRP irp) {
    (void)v; (void)r; (void)objid; (void)type; (void)offset; (void)size; (void)ptp; (void)irp;
    if ((inserted_count == 0 && fail_operation == INSERT_DEV_ITEM) ||
        (inserted_count == 1 && fail_operation == INSERT_STATS))
        return STATUS_IO_DEVICE_ERROR;
    CHECK(inserted_count < 4); inserted_items[inserted_count++] = data; return STATUS_SUCCESS;
}
static tree_data found_item;
static NTSTATUS find_item(device_extension* v, root* r, traverse_ptr* tp, struct btrfs_key* key, bool ignore, PIRP irp) {
    (void)v; (void)r; (void)key; (void)ignore; (void)irp;
    if (fail_operation == FIND_STATS) return STATUS_IO_DEVICE_ERROR;
    tp->item = &found_item; return STATUS_SUCCESS;
}
static NTSTATUS delete_tree_item(device_extension* v, traverse_ptr* tp) {
    (void)v; (void)tp;
    return fail_operation == DELETE_STATS ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
static void trim_whole_device(device* dev) { (void)dev; CHECK(false); }
static NTSTATUS write_data_phys(PDEVICE_OBJECT device, PFILE_OBJECT file, uint64_t address, void* data, uint32_t length) {
    (void)data; CHECK(device == &target_device && file == &target_file && address == 0 && length == 0x100000);
    return fail_operation == WRITE_ZERO_BUFFER ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
static NTSTATUS __stdcall pnp_removal(void* notification, void* context) { (void)notification; (void)context; return STATUS_SUCCESS; }
static NTSTATUS IoRegisterPlugPlayNotification(int category, ULONG flags, void* data, void* driver,
                                               NTSTATUS (*callback)(void*, void*), void* context,
                                               void** entry) {
    (void)category; (void)flags; (void)data; (void)driver; (void)callback; (void)context;
    *entry = &notifications_registered; notifications_registered++; return STATUS_SUCCESS;
}
static void IoUnregisterPlugPlayNotification(void* entry) {
    CHECK(entry == &notifications_registered && notifications_registered);
    CHECK(active_tree_lock && !active_tree_lock->held);
    notifications_registered--;
}
typedef NTSTATUS (*tIoUnregisterPlugPlayNotificationEx)(void*);
tIoUnregisterPlugPlayNotificationEx fIoUnregisterPlugPlayNotificationEx;
static NTSTATUS unregister_notification_ex(void* entry) {
    IoUnregisterPlugPlayNotification(entry);
    return STATUS_SUCCESS;
}
static NTSTATUS IoGetDeviceObjectPointer(PUNICODE_STRING name, ULONG access, PFILE_OBJECT* file, PDEVICE_OBJECT* device) {
    (void)name; (void)access; *file = &mountmgr_file; *device = &mountmgr_device; return STATUS_SUCCESS;
}
static NTSTATUS remove_drive_letter(PDEVICE_OBJECT mountmgr, PUNICODE_STRING name) { (void)mountmgr; (void)name; return STATUS_NOT_FOUND; }
static void FsRtlNotifyVolumeEvent(void* file, ULONG event) { (void)file; CHECK(event == FSRTL_VOLUME_CHANGE_SIZE); volume_notifications++; }
#include "add_device.inc"

/* Pool allocations add_device makes on success, in order. */
static const char* const allocations[] = {
    "pnp-name", "device", "space-entry", "dev-item", "stats", "zero-buffer", "volume-child", "child-name"
};

static void exercise(unsigned fail, enum failure_point operation, bool use_ex_unregister) {
    pdo_device_extension pdode = { 0 }; volume_device_extension vde = { &pdode };
    device_extension volume = { .vde = &vde };
    root chunk_root, dev_root; volume.chunk_root = &chunk_root; volume.dev_root = &dev_root;
    volume.superblock.num_devices = 1; volume.superblock.sectorsize = 4096;
    InitializeListHead(&volume.devices); InitializeListHead(&pdode.children);
    active_tree_lock = &volume.tree_lock;
    found_item.key.objectid = operation == DELETE_STATS ? 0 : 1;
    found_item.key.type = BTRFS_PERSISTENT_ITEM_KEY;
    found_item.key.offset = 1;
    HANDLE handle = (HANDLE)&target_file;
    IO_STACK_LOCATION stack = { .Parameters.FileSystemControl.InputBufferLength = sizeof(HANDLE) };
    IRP irp = { .stack = &stack, .AssociatedIrp.SystemBuffer = &handle };
    fail_allocation = fail;
    fail_operation = operation;
    fIoUnregisterPlugPlayNotificationEx = use_ex_unregister ? unregister_notification_ex : NULL;
    volume.need_write = operation == PREFLUSH;
    NTSTATUS status = add_device(&volume, &irp, 1);
    for (unsigned i = 0; i < inserted_count; i++) ExFreePool(inserted_items[i]);
    CHECK(held_locks == 0);
    if (!fail && (operation == NO_FAILURE || operation == POSTFLUSH)) {
        CHECK(status == (operation == POSTFLUSH ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS));
        CHECK(volume_notifications == (operation == POSTFLUSH ? 0u : 1u));
        CHECK(volume.superblock.num_devices == 2);
        CHECK(pdode.num_children == 1 && volume.devices.Flink != &volume.devices && file_references == 1);
        volume_child* child = CONTAINING_RECORD(RemoveHeadList(&pdode.children), volume_child, list_entry);
        device* dev = CONTAINING_RECORD(RemoveHeadList(&volume.devices), device, list_entry);
        IoUnregisterPlugPlayNotification(child->notification_entry);
        ExFreePool(child->pnp_name.Buffer);
        ExFreePool(child);
        while (!IsListEmpty(&dev->space))
            ExFreePool(CONTAINING_RECORD(RemoveHeadList(&dev->space), space, list_entry));
        ExFreePool(dev);
        ObDereferenceObject(&target_file);
        CHECK(pool_live == 0 && notifications_registered == 0 && file_references == 0);
        return;
    }
    NTSTATUS wanted = fail ? (fail == 1 ? STATUS_NOT_FOUND : STATUS_INSUFFICIENT_RESOURCES) :
        operation == WRONG_DEVICE_TYPE ? STATUS_INVALID_PARAMETER :
        operation == TOO_SMALL ? STATUS_INTERNAL_ERROR : STATUS_IO_DEVICE_ERROR;
    CHECK(status == wanted);
    CHECK(volume_notifications == 0 && volume.superblock.num_devices == 1 && file_references == 0);
    CHECK(volume.devices.Flink == &volume.devices && pdode.num_children == 0);
    CHECK(pool_live == 0 && notifications_registered == 0);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    for (unsigned fail = 0; fail <= sizeof(allocations) / sizeof(allocations[0]); fail++) {
        char name[64];
        snprintf(name, sizeof(name), fail ? "%s-allocation-fails" : "success", fail ? allocations[fail - 1] : "");
        if (strcmp(argv[1], "--list") == 0) puts(name);
        else if (strcmp(argv[1], name) == 0) { exercise(fail, NO_FAILURE, false); return 0; }
    }
    static const struct { const char* name; enum failure_point point; } failures[] = {
        { "writable-ioctl-fails", WRITABLE }, { "raid-check-fails", RAID_MEMBER },
        { "wrong-device-type", WRONG_DEVICE_TYPE }, { "length-ioctl-fails", LENGTH_IOCTL },
        { "device-too-small", TOO_SMALL }, { "preflush-fails", PREFLUSH },
        { "dev-item-insert-fails", INSERT_DEV_ITEM }, { "stats-lookup-fails", FIND_STATS },
        { "old-stats-delete-fails", DELETE_STATS }, { "stats-insert-fails", INSERT_STATS },
        { "zero-buffer-write-fails", WRITE_ZERO_BUFFER },
        { "post-link-flush-fails", POSTFLUSH }
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        if (strcmp(argv[1], "--list") == 0) puts(failures[i].name);
        else if (strcmp(argv[1], failures[i].name) == 0) {
            exercise(0, failures[i].point, false);
            return 0;
        }
    }
    if (strcmp(argv[1], "--list") == 0) puts("child-name-allocation-fails-ex-unregister");
    else if (strcmp(argv[1], "child-name-allocation-fails-ex-unregister") == 0) {
        exercise(8, NO_FAILURE, true);
        return 0;
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
