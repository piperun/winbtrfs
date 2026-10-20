/* Production FS usage handler with preserved stack and completion contracts. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t NTSTATUS, LONG;
typedef void* PVOID;
typedef struct {
    unsigned Type;
    bool InPath;
} USAGE_NOTIFICATION;
typedef struct {
    struct { USAGE_NOTIFICATION UsageNotification; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct DEVICE_OBJECT {
    void* DeviceExtension;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct { PDEVICE_OBJECT RealDevice; } VPB;
typedef struct {
    LONG page_file_count;
    VPB* Vpb;
} device_extension;
typedef struct IRP IRP, *PIRP;
typedef NTSTATUS (*COMPLETION)(PDEVICE_OBJECT, PIRP, PVOID);
struct IRP {
    IO_STACK_LOCATION stack, next;
    struct {
        NTSTATUS Status;
        uintptr_t Information;
    } IoStatus;
    bool PendingReturned, marked_pending, copied, skipped, completed;
    COMPLETION completion;
    void* context;
};
#define __stdcall
#define _Function_class_(x)
#define STATUS_SUCCESS 0
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define NT_SUCCESS(s) ((s) >= 0)
#define DeviceUsageTypePaging 1
#define DeviceUsageTypeDumpFile 2
#define DeviceUsageTypeHibernation 3
#define UNUSED(x) ((void)(x))
#define REQUIRE(c) do { \
    if (!(c)) { \
        fprintf(stderr, "model invariant at %u: %s\n", __LINE__, #c); \
        abort(); \
    } \
} while (0)

enum { COMPLETE_SYNCHRONOUSLY, COMPLETE_LATER, COMPLETE_BEFORE_PENDING_RETURN };
static DEVICE_OBJECT lower;
static unsigned completion_mode, forwarded, adjustments;
static NTSTATUS lower_status;
static PDEVICE_OBJECT original_device;
static USAGE_NOTIFICATION expected_usage;
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) {
    return &irp->stack;
}

static void IoAdjustPagingPathCount(LONG* count, bool in_path) {
    *count += in_path ? 1 : -1;
    adjustments++;
}

static void __attribute__((unused)) IoCopyCurrentIrpStackLocationToNext(PIRP irp) {
    irp->next = irp->stack;
    irp->copied = true;
}

static void __attribute__((unused)) IoSkipCurrentIrpStackLocation(PIRP irp) {
    irp->next = irp->stack;
    irp->skipped = true;
}

static void __attribute__((unused)) IoMarkIrpPending(PIRP irp) {
    irp->marked_pending = true;
}

static void __attribute__((unused)) IoSetCompletionRoutine(PIRP irp, COMPLETION completion, void* context,
                                                          bool success, bool error, bool cancel) {
    REQUIRE(irp->copied && success && error && cancel);
    irp->completion = completion;
    irp->context = context;
}

static void complete_irp(PIRP irp) {
    REQUIRE(!irp->completed);
    irp->IoStatus.Status = lower_status;
    irp->PendingReturned = completion_mode != COMPLETE_SYNCHRONOUSLY;

    /* Lower stack locations are cleared before the upper completion runs. */
    memset(&irp->next, 0, sizeof(irp->next));
    if (irp->completion)
        REQUIRE(irp->completion(original_device, irp, irp->context) == STATUS_SUCCESS);

    irp->completed = true;
}

static NTSTATUS IoCallDriver(PDEVICE_OBJECT target, PIRP irp) {
    REQUIRE(target == &lower && (irp->copied || irp->skipped));
    REQUIRE(irp->next.Parameters.UsageNotification.Type == expected_usage.Type &&
            irp->next.Parameters.UsageNotification.InPath == expected_usage.InPath);
    forwarded++;
    if (completion_mode != COMPLETE_LATER)
        complete_irp(irp);

    return completion_mode == COMPLETE_SYNCHRONOUSLY ? lower_status : STATUS_PENDING;
}

#include "count.inc"

int main(void) {
    unsigned cases = 0, failures = 0;
    for (unsigned type = 0; type < 5; type++)
        for (unsigned mode = 0; mode < 3; mode++)
            for (unsigned failed = 0; failed < 2; failed++)
                for (unsigned in_path = 0; in_path < 2; in_path++)
                    for (unsigned existing = 0; existing < 2; existing++) {
                        LONG initial = existing ? 3 : (in_path ? 0 : 1);
                        VPB vpb = {.RealDevice = &lower};
                        device_extension volume = {.page_file_count = initial, .Vpb = &vpb};
                        DEVICE_OBJECT object = {.DeviceExtension = &volume};
                        original_device = &object;
                        expected_usage = (USAGE_NOTIFICATION){type, in_path};
                        IRP irp = {.stack.Parameters.UsageNotification = expected_usage};
                        completion_mode = mode;
                        lower_status = failed ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
                        forwarded = adjustments = 0;

                        bool tracked = type >= 1 && type <= 3;
                        LONG change = tracked ? (in_path ? 1 : -1) : 0;
                        NTSTATUS status = pnp_device_usage_notification(&object, &irp);
                        LONG pending_count = volume.page_file_count;
                        bool ok = status == (mode == COMPLETE_SYNCHRONOUSLY ? lower_status : STATUS_PENDING) &&
                                  forwarded == 1;
                        if (mode == COMPLETE_LATER) {
                            ok = ok && pending_count == initial + change;
                            complete_irp(&irp);
                        }

                        ok = ok && volume.page_file_count == initial + (failed ? 0 : change) &&
                             irp.IoStatus.Status == lower_status && irp.IoStatus.Information == 0 && irp.completed &&
                             irp.marked_pending == (tracked && mode != COMPLETE_SYNCHRONOUSLY) &&
                             adjustments == (tracked ? (failed ? 2u : 1u) : 0u);
                        cases++;
                        if (!ok) {
                            failures++;
                            printf("FAIL type=%u mode=%u failed=%u in-path=%u pending-count=%d final-count=%d marked=%u adjustments=%u\n",
                                   type, mode, failed, in_path, pending_count, volume.page_file_count,
                                   irp.marked_pending, adjustments);
                        }
                    }
    printf("%u/%u usage count scenarios passed\n", cases - failures, cases);
    return failures != 0;
}
