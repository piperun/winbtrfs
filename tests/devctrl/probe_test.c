/* Production volume probe; caller-owned names and arrival copies are modeled. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uint16_t WCHAR;
typedef unsigned KPROCESSOR_MODE;
typedef struct { uint8_t bytes[16]; } GUID;
typedef struct { uint16_t NameLength; WCHAR Name[1]; } MOUNTDEV_NAME;
typedef struct { uint16_t Length, MaximumLength; WCHAR* Buffer; } UNICODE_STRING;
typedef struct { unsigned unused; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct { unsigned unused; } FILE_OBJECT, *PFILE_OBJECT;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_PRIVILEGE_NOT_HELD ((NTSTATUS)0xc0000061)
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define NT_SUCCESS(s) ((s) >= 0)
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
#define FILE_READ_ATTRIBUTES 0x80
#define SE_MANAGE_VOLUME_PRIVILEGE 28
#define IOCTL_DISK_UPDATE_PROPERTIES 1
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "model invariant at %u: %s\n", __LINE__, #c); abort(); } } while (0)

static const GUID GUID_DEVINTERFACE_DISK = {{1}}, volume_guid = {{2}};
static const WCHAR name_text[] = {'p', 'n', 'p'};
static DEVICE_OBJECT device;
static FILE_OBJECT file;
static bool privilege, disk;
static NTSTATUS open_status, name_status, update_status;
static void* allocated_name;
static unsigned allocations, frees, references, removals, disk_arrivals, volume_arrivals, updates;
static WCHAR retained_name[3];

static size_t RtlCompareMemory(const void* a, const void* b, size_t length) {
    size_t i = 0;
    while (i < length && ((const char*)a)[i] == ((const char*)b)[i])
        i++;
    return i;
}
static unsigned RtlConvertLongToLuid(unsigned privilege_id) { return privilege_id; }
static bool SeSinglePrivilegeCheck(unsigned privilege_id, KPROCESSOR_MODE mode) {
    REQUIRE(privilege_id == SE_MANAGE_VOLUME_PRIVILEGE && mode == 1);
    return privilege;
}
static NTSTATUS IoGetDeviceObjectPointer(UNICODE_STRING* name, unsigned access, PFILE_OBJECT* out_file, PDEVICE_OBJECT* out_device) {
    REQUIRE(name->Length == 6 && name->MaximumLength == 6 && access == FILE_READ_ATTRIBUTES);
    if (!NT_SUCCESS(open_status))
        return open_status;
    *out_file = &file;
    *out_device = &device;
    references++;
    return STATUS_SUCCESS;
}
static NTSTATUS get_device_pnp_name(PDEVICE_OBJECT object, UNICODE_STRING* name, const GUID** guid) {
    REQUIRE(object == &device && references == 1);
    if (!NT_SUCCESS(name_status))
        return name_status;
    allocated_name = malloc(sizeof(name_text));
    REQUIRE(allocated_name);
    allocations++;
    memcpy(allocated_name, name_text, sizeof(name_text));
    *name = (UNICODE_STRING){sizeof(name_text), sizeof(name_text), allocated_name};
    *guid = disk ? &GUID_DEVINTERFACE_DISK : &volume_guid;
    return STATUS_SUCCESS;
}
static void ObDereferenceObject(PFILE_OBJECT object) {
    REQUIRE(object == &file && references == 1);
    references--;
}
static NTSTATUS dev_ioctl(PDEVICE_OBJECT object, ULONG code, void* input, ULONG input_length,
                           void* output, ULONG output_length, bool override, void* returned) {
    REQUIRE(object == &device && code == IOCTL_DISK_UPDATE_PROPERTIES && !input && !input_length &&
            !output && !output_length && override && !returned);
    updates++;
    return update_status;
}
static void validate_borrowed_name(UNICODE_STRING* name) {
    REQUIRE(allocated_name && name->Buffer == allocated_name && name->Length == sizeof(name_text));
    REQUIRE(!references && !memcmp(name->Buffer, name_text, sizeof(name_text)));
}
static void volume_removal(UNICODE_STRING* name) {
    validate_borrowed_name(name);
    removals++;
}

static void disk_arrival(UNICODE_STRING* name) {
    validate_borrowed_name(name);
    REQUIRE(removals == 1);
    memcpy(retained_name, name->Buffer, sizeof(retained_name));
    disk_arrivals++;
}

static bool volume_arrival(UNICODE_STRING* name, bool fve_callback) {
    REQUIRE(!fve_callback);
    validate_borrowed_name(name);
    REQUIRE(removals == 1);
    memcpy(retained_name, name->Buffer, sizeof(retained_name));
    volume_arrivals++;
    return true;
}

static void ExFreePool(void* pointer) {
    REQUIRE(pointer == allocated_name && removals == 1 && disk_arrivals + volume_arrivals == 1);
    free(pointer);
    allocated_name = NULL;
    frees++;
}

#include "probe.inc"

int main(void) {
    unsigned failures = 0;
    for (unsigned scenario = 0; scenario < 8; scenario++) {
        struct { uint16_t NameLength; WCHAR Name[8]; } input = {.NameLength = 6, .Name = {'d', 'e', 'v'}};
        ULONG length = scenario == 0 ? 0 : scenario == 1 ? 4 : sizeof(input);
        privilege = scenario != 2;
        disk = scenario != 6;
        open_status = scenario == 3 ? STATUS_NOT_FOUND : STATUS_SUCCESS;
        name_status = scenario == 4 ? STATUS_NOT_FOUND : STATUS_SUCCESS;
        update_status = scenario == 7 ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
        allocations = frees = references = removals = disk_arrivals = volume_arrivals = updates = 0;
        memset(retained_name, 0, sizeof(retained_name));
        NTSTATUS expected = scenario < 2 ? STATUS_INVALID_PARAMETER : scenario == 2 ? STATUS_PRIVILEGE_NOT_HELD :
                            scenario < 5 ? STATUS_NOT_FOUND : STATUS_SUCCESS;
        NTSTATUS status = probe_volume(&input, length, 1);
        bool ok = status == expected && allocations == frees && !references;
        if (scenario >= 5)
            ok = ok && removals == 1 && disk_arrivals == (unsigned)disk && volume_arrivals == (unsigned)!disk &&
                 updates == (unsigned)disk && !memcmp(retained_name, name_text, sizeof(retained_name));
        else
            ok = ok && !allocations && !removals && !disk_arrivals && !volume_arrivals && !updates;
        if (!ok) {
            failures++;
            printf("FAIL scenario=%u status=%08x expected=%08x allocations=%u frees=%u references=%u\n",
                   scenario, (unsigned)status, (unsigned)expected, allocations, frees, references);
        }
        if (allocated_name)
            ExFreePool(allocated_name);
    }
    printf("%u/8 probe scenarios passed\n", 8 - failures);
    return failures != 0;
}
