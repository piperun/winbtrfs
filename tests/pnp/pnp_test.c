/* Exact child usage handler; kernel IRP ownership and completion are modeled. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef void* PVOID;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { NTSTATUS Status; uintptr_t Information; } IO_STATUS_BLOCK;
typedef struct { bool signaled; } KEVENT;
typedef struct { unsigned Type; bool InPath; } USAGE_NOTIFICATION;
typedef struct {
    unsigned MajorFunction, MinorFunction;
    struct { USAGE_NOTIFICATION UsageNotification; } Parameters;
    void* FileObject;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct {
    unsigned StackSize, completion_mode;
    NTSTATUS final_status;
    NTSTATUS rollback_status;
    int usage_count;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct IRP IRP, *PIRP;
typedef NTSTATUS (*COMPLETION)(PDEVICE_OBJECT, PIRP, PVOID);
struct IRP {
    IO_STACK_LOCATION stack;
    IO_STATUS_BLOCK IoStatus, *UserIosb;
    KEVENT* UserEvent;
    COMPLETION completion;
    void* context;
    DEVICE_OBJECT* target;
    bool completed;
};
typedef struct { LIST_ENTRY list_entry; PDEVICE_OBJECT devobj; void* fileobj; } volume_child;
typedef struct { unsigned child_lock; LIST_ENTRY children; } pdo_device_extension;

#define __stdcall
#define _Function_class_(x)
#define UNUSED(x) ((void)(x))
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define STATUS_SUCCESS 0
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xc0000016)
#define NT_SUCCESS(s) ((s) >= 0)
#define IRP_MJ_PNP 0x1b
#define IRP_MN_DEVICE_USAGE_NOTIFICATION 0x16
#define NotificationEvent 0
#define Executive 0
#define KernelMode 0
#define CONTAINING_RECORD(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "model invariant at %u: %s\n", __LINE__, #c); abort(); } } while (0)

enum { COMPLETE_SYNCHRONOUSLY, COMPLETE_ON_WAIT, COMPLETE_BEFORE_PENDING_RETURN };
static PIRP allocated_irps[4];
static PIRP pending_irp;
static unsigned allocations, frees, allocation_attempts, fail_allocation;
static unsigned calls, waits, locks, cases, failures;
static unsigned ownership_failures, status_failures;
static unsigned bad_initial_status, initial_status_failures;
static USAGE_NOTIFICATION expected_usage;
static struct { PDEVICE_OBJECT device; bool in_path; } notifications[8];

static void init_list(LIST_ENTRY* head) {
    head->Flink = head->Blink = head;
}

static void append(LIST_ENTRY* head, LIST_ENTRY* item) {
    item->Blink = head->Blink;
    item->Flink = head;
    head->Blink->Flink = item;
    head->Blink = item;
}

static void ExAcquireResourceSharedLite(unsigned* lock, bool wait) {
    REQUIRE(wait && *lock == 0);
    (*lock)++;
    locks++;
}

static void ExReleaseResourceLite(unsigned* lock) {
    REQUIRE(*lock == 1);
    (*lock)--;
    locks--;
}
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return &irp->stack; }
static PIO_STACK_LOCATION IoGetNextIrpStackLocation(PIRP irp) { return &irp->stack; }
static PIRP IoAllocateIrp(unsigned stack_size, bool charge_quota) {
    REQUIRE(stack_size == 3 && !charge_quota);
    if (++allocation_attempts == fail_allocation)
        return NULL;
    for (unsigned i = 0; i < 4; i++) if (!allocated_irps[i]) {
        PIRP irp = calloc(1, sizeof(*irp));
        REQUIRE(irp);
        irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
        irp->IoStatus.Information = UINTPTR_MAX;
        allocated_irps[i] = irp;
        allocations++;
        return irp;
    }
    abort();
}
static void IoFreeIrp(PIRP irp) {
    REQUIRE(irp->completed && pending_irp != irp);
    for (unsigned i = 0; i < 4; i++) if (allocated_irps[i] == irp) {
        allocated_irps[i] = NULL;
        frees++;
        free(irp);
        return;
    }
    abort();
}
static void KeInitializeEvent(KEVENT* event, unsigned type, bool state) {
    REQUIRE(type == NotificationEvent && !state);
    event->signaled = state;
}
static void KeSetEvent(KEVENT* event, unsigned increment, bool wait) {
    REQUIRE(increment == 0 && !wait);
    event->signaled = true;
}
static void IoSetCompletionRoutine(PIRP irp, COMPLETION completion, void* context,
                                   bool success, bool error, bool cancel) {
    REQUIRE(success && error && cancel);
    irp->completion = completion;
    irp->context = context;
}
static void complete_irp(PIRP irp) {
    REQUIRE(!irp->completed);
    bool in_path = irp->stack.Parameters.UsageNotification.InPath;
    irp->IoStatus.Status = in_path == expected_usage.InPath ? irp->target->final_status : irp->target->rollback_status;
    if (NT_SUCCESS(irp->IoStatus.Status))
        irp->target->usage_count += in_path ? 1 : -1;
    irp->completed = true;
    REQUIRE(irp->completion(irp->target, irp, irp->context) == STATUS_MORE_PROCESSING_REQUIRED);
}
static NTSTATUS IoCallDriver(PDEVICE_OBJECT device, PIRP irp) {
    bad_initial_status += irp->IoStatus.Status != STATUS_SUCCESS || irp->IoStatus.Information != 0;
    REQUIRE(irp->stack.MajorFunction == IRP_MJ_PNP && irp->stack.MinorFunction == IRP_MN_DEVICE_USAGE_NOTIFICATION);
    REQUIRE(irp->stack.Parameters.UsageNotification.Type == expected_usage.Type);
    REQUIRE(irp->stack.FileObject == device && irp->UserIosb && irp->UserEvent);
    REQUIRE(calls < 8);
    notifications[calls].device = device;
    notifications[calls].in_path = irp->stack.Parameters.UsageNotification.InPath;
    NTSTATUS final_status = notifications[calls].in_path == expected_usage.InPath ? device->final_status : device->rollback_status;
    calls++;
    irp->target = device;
    if (device->completion_mode == COMPLETE_ON_WAIT) {
        REQUIRE(!pending_irp);
        pending_irp = irp;
        return STATUS_PENDING;
    }
    complete_irp(irp);
    return device->completion_mode == COMPLETE_BEFORE_PENDING_RETURN ? STATUS_PENDING : final_status;
}
static void KeWaitForSingleObject(KEVENT* event, unsigned reason, unsigned mode, bool alertable, void* timeout) {
    REQUIRE(reason == Executive && mode == KernelMode && !alertable && !timeout);
    waits++;
    if (!event->signaled) {
        REQUIRE(pending_irp && pending_irp->UserEvent == event);
        PIRP irp = pending_irp;
        pending_irp = NULL;
        complete_irp(irp);
    }
    REQUIRE(event->signaled);
}

#include "usage.inc"

static void usage_case(unsigned layout, unsigned mode, unsigned failed_child,
                       unsigned allocation_failure, bool in_path) {
    pdo_device_extension pdode = {0};
    init_list(&pdode.children);
    DEVICE_OBJECT devices[2];
    volume_child children[4] = {0};
    unsigned child_count = layout == 0 ? 0 : layout == 1 ? 1 : 2;
    for (unsigned i = 0; i < child_count; i++) {
        devices[i] = (DEVICE_OBJECT){.StackSize=3, .completion_mode=mode,
            .final_status=failed_child == i + 1 ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS,
            .usage_count=in_path?0:1};
        if (layout == 3) append(&pdode.children, &children[i * 2].list_entry);
        unsigned slot = layout == 3 ? i * 2 + 1 : i;
        children[slot].devobj = &devices[i];
        children[slot].fileobj = &devices[i];
        append(&pdode.children, &children[slot].list_entry);
    }
    expected_usage = (USAGE_NOTIFICATION){2, in_path};
    IRP original = {.stack.Parameters.UsageNotification = expected_usage};
    allocations = frees = allocation_attempts = calls = waits = locks = bad_initial_status = 0;
    fail_allocation = allocation_failure;
    NTSTATUS expected_status = STATUS_SUCCESS;
    unsigned expected_calls = 0;
    for (unsigned i = 0; i < child_count; i++) {
        if (allocation_failure == i + 1) {
            expected_status = STATUS_INSUFFICIENT_RESOURCES;
            expected_calls += i;
            break;
        }
        expected_calls++;
        if (failed_child == i + 1) {
            expected_status = STATUS_IO_DEVICE_ERROR;
            expected_calls += i;
            break;
        }
    }
    NTSTATUS status = pdo_device_usage_notification(&pdode, &original);
    ownership_failures += allocations != frees;
    status_failures += status != expected_status;
    initial_status_failures += bad_initial_status;
    bool ok = status == expected_status && allocations == frees && calls == expected_calls &&
              waits == (mode == COMPLETE_SYNCHRONOUSLY ? 0 : expected_calls) && !locks && !pending_irp && !bad_initial_status;
    for (unsigned i = 0; i < child_count; i++)
        ok = ok && devices[i].usage_count == (expected_status == STATUS_SUCCESS ? (in_path?1:0) : (in_path?0:1));
    cases++;
    if (!ok) {
        failures++;
        printf("FAIL layout=%u mode=%u failed-child=%u allocation=%u in-path=%u status=%08x expected=%08x alloc=%u free=%u calls=%u expected-calls=%u\n",
               layout, mode, failed_child, allocation_failure, in_path, (unsigned)status,
               (unsigned)expected_status, allocations, frees, calls, expected_calls);
    }
    /* Reclaim modeled baseline leaks after observing the ownership failure. */
    for (unsigned i = 0; i < 4; i++) if (allocated_irps[i]) IoFreeIrp(allocated_irps[i]);
}

static void rollback_case(unsigned mode, bool in_path) {
    pdo_device_extension pdode = {0};
    init_list(&pdode.children);
    DEVICE_OBJECT devices[3];
    volume_child children[3] = {0};
    for (unsigned i = 0; i < 3; i++) {
        devices[i] = (DEVICE_OBJECT){.StackSize=3, .completion_mode=mode,
            .final_status=i == 2 ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS,
            .rollback_status=i == 1 ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS,
            .usage_count=in_path?0:1};
        children[i].devobj = &devices[i];
        children[i].fileobj = &devices[i];
        append(&pdode.children, &children[i].list_entry);
    }
    expected_usage = (USAGE_NOTIFICATION){2, in_path};
    IRP original = {.stack.Parameters.UsageNotification = expected_usage};
    allocations = frees = allocation_attempts = calls = waits = locks = fail_allocation = bad_initial_status = 0;
    NTSTATUS status = pdo_device_usage_notification(&pdode, &original);
    initial_status_failures += bad_initial_status;
    bool ok = status == STATUS_IO_DEVICE_ERROR && allocations == frees && calls == 5 && !locks && !pending_irp && !bad_initial_status &&
        devices[0].usage_count == (in_path?0:1) && devices[1].usage_count == (in_path?1:0) && devices[2].usage_count == (in_path?0:1);
    const unsigned order[] = {0,1,2,1,0};
    for (unsigned i = 0; i < 5; i++)
        ok = ok && notifications[i].device == &devices[order[i]] && notifications[i].in_path == (i < 3 ? in_path : !in_path);
    cases++;
    if (!ok) {
        failures++;
        printf("FAIL compensating rejection mode=%u in-path=%u calls=%u status=%08x\n",
               mode, in_path, calls, (unsigned)status);
    }
    for (unsigned i = 0; i < 4; i++) if (allocated_irps[i]) IoFreeIrp(allocated_irps[i]);
}

int main(void) {
    for (unsigned layout = 0; layout < 4; layout++)
        for (unsigned mode = 0; mode < 3; mode++)
            for (unsigned failed_child = 0; failed_child < 3; failed_child++)
                for (unsigned allocation_failure = 0; allocation_failure < 3; allocation_failure++)
                    for (unsigned in_path = 0; in_path < 2; in_path++)
                        usage_case(layout, mode, failed_child, allocation_failure, in_path);
    for (unsigned mode = 0; mode < 3; mode++)
        for (unsigned in_path = 0; in_path < 2; in_path++)
            rollback_case(mode, in_path);
    printf("%u/%u PnP usage scenarios passed\n", cases - failures, cases);
    printf("IRP ownership failures=%u final-status failures=%u\n", ownership_failures, status_failures);
    printf("Uninitialized forwarded request status=%u\n", initial_status_failures);
    return failures != 0;
}
