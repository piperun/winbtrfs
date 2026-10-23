/* Host regression for WinBtrfs issue #820. Kernel APIs are mocked, but the
 * lifecycle functions, worker entrypoints and mount activation are extracted
 * from the selected source revision without rewriting their statements. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _In_
#define __stdcall
#define _Function_class_(x)
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS 0
#define STATUS_WAIT_0 0
#define WaitAny 0
typedef void* PVOID;
#define STATUS_INSUFFICIENT_RESOURCES (-1)
#define TEST_FAILURE (-42)
#define SYNCHRONIZE 0x100000u
#define NonPagedPool 0
#define ALLOC_TAG 0
#define OBJ_KERNEL_HANDLE 0x200u
#define NotificationEvent 0
#define Executive 0
#define KernelMode 0
#define VPB_MOUNTED 1
#define RtlZeroMemory(p, n) memset(p, 0, n)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", scenario, __LINE__, #c); _Exit(1); } } while (0)

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uintptr_t KAFFINITY;
typedef int KIRQL;
typedef int KSPIN_LOCK;
typedef struct { void *Flink, *Blink; } LIST_ENTRY;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool initialized, signaled;
} KEVENT, KTIMER;
typedef struct { unsigned int attributes; } OBJECT_ATTRIBUTES;
typedef struct thread_handle* HANDLE;
typedef struct device_object DEVICE_OBJECT;
typedef struct device_object* PDEVICE_OBJECT;
#include "thread_types.inc"
typedef struct {
    drv_calc_threads calcthreads;
    KEVENT flush_thread_finished;
    KTIMER flush_thread_timer;
    HANDLE flush_thread_handle;
    struct { uint32_t flush_interval; } options;
    bool removing, locked;
} device_extension;
typedef struct {
    PDEVICE_OBJECT DeviceObject;
    unsigned int Flags, ReferenceCount, VolumeLabelLength;
    uint16_t VolumeLabel[32];
} VPB;
struct device_object { device_extension* DeviceExtension; VPB* Vpb; atomic_int refs; };
typedef struct { struct { struct { VPB* Vpb; } MountVolume; } Parameters; } IO_STACK_LOCATION;
struct thread_handle {
    pthread_t thread;
    void (*entry)(void*);
    void* context;
    unsigned int access;
    bool is_flush, at_terminate, terminated, joined, closed;
};

static const char* scenario;
static pthread_mutex_t monitor = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local HANDLE self;
static struct thread_handle handles[8];
static unsigned int attempts, created, joins, closes, frees, flushes;
static unsigned int periodic_arms, trim_calls, trim_drains;
static KEVENT trim_work;
static bool trim_ready;
static unsigned int processors = 4;
static int failure_at = -1;
static bool fail_allocation, allow_start, allow_termination, block_work, allow_work;
static bool work_entered, submitting;
static unsigned int waiting_calc;
static void* pool;
static size_t pool_size;
static device_extension volume;
static VPB vpb;
static struct device_object device;

void calc_thread(void*);
void flush_thread(void*);
void stop_calc_threads(device_extension*);
void stop_flush_thread(device_extension*);

static void InitializeListHead(LIST_ENTRY* list) { list->Flink = list->Blink = list; }
static void KeInitializeSpinLock(KSPIN_LOCK* lock) { *lock = 0; }
static KAFFINITY KeQueryActiveProcessors(void) { return ((KAFFINITY)1 << processors) - 1; }
static void KeSetSystemAffinityThread(KAFFINITY affinity) { CHECK(affinity != 0); }
static void InitializeObjectAttributes(OBJECT_ATTRIBUTES* oa, void* name, unsigned int attributes,
                                       void* root, void* security) {
    CHECK(name == NULL && root == NULL && security == NULL);
    oa->attributes = attributes;
}
static void KeInitializeEvent(KEVENT* event, int type, bool signaled) {
    CHECK(type == NotificationEvent);
    if (event->initialized) {
        CHECK(pthread_mutex_destroy(&event->mutex) == 0);
        CHECK(pthread_cond_destroy(&event->condition) == 0);
    }
    CHECK(pthread_mutex_init(&event->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&event->condition, NULL) == 0);
    event->initialized = true;
    event->signaled = signaled;
}
static void KeInitializeTimer(KTIMER* timer) { KeInitializeEvent(timer, NotificationEvent, false); }
static void KeSetEvent(KEVENT* event, int priority, bool wait) {
    CHECK(event->initialized && priority == 0 && !wait);
    if (!self && !submitting && event == &volume.calcthreads.event) {
        CHECK(volume.flush_thread_handle == NULL || volume.flush_thread_handle->joined);
        for (unsigned int i = 0; i < created; i++) {
            if (!handles[i].is_flush)
                CHECK(((drv_calc_thread*)handles[i].context)->quit);
        }
    }
    pthread_mutex_lock(&event->mutex);
    event->signaled = true;
    pthread_cond_broadcast(&event->condition);
    pthread_mutex_unlock(&event->mutex);
    pthread_mutex_lock(&monitor);pthread_cond_broadcast(&changed);pthread_mutex_unlock(&monitor);
}
static void KeWaitForSingleObject(KEVENT* event, int reason, int mode, bool alertable, void* timeout) {
    CHECK(event->initialized && reason == Executive && mode == KernelMode && !alertable && timeout == NULL);
    if (event == &volume.calcthreads.event) {
        pthread_mutex_lock(&monitor);
        waiting_calc++;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&monitor);
    }
    /* A completion-event wait may release delayed startup, but it must not
     * masquerade as waiting for the actual thread object to terminate. */
    if (event != &volume.calcthreads.event && event != &volume.flush_thread_timer) {
        pthread_mutex_lock(&monitor);
        allow_start = true;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&monitor);
    }
    pthread_mutex_lock(&event->mutex);
    while (!event->signaled)
        pthread_cond_wait(&event->condition, &event->mutex);
    pthread_mutex_unlock(&event->mutex);
}
NTSTATUS KeWaitForMultipleObjects(ULONG count,PVOID* objects,int type,int reason,int mode,bool alert,void* timeout,void* blocks) {
    CHECK(count>=1&&count<=2&&type==WaitAny&&reason==Executive&&mode==KernelMode&&!alert&&!timeout&&!blocks);
    pthread_mutex_lock(&monitor);
    for(;;) {
        for(ULONG i=0;i<count;i++) {
            KEVENT* e=objects[i];CHECK(e->initialized);pthread_mutex_lock(&e->mutex);bool signaled=e->signaled;pthread_mutex_unlock(&e->mutex);
            if(signaled){pthread_mutex_unlock(&monitor);return (NTSTATUS)i;}
        }
        pthread_cond_wait(&changed,&monitor);
    }
}
void trim_initialize(device_extension* v){CHECK(v==&volume);KeInitializeEvent(&trim_work,NotificationEvent,false);trim_ready=true;}
KEVENT* trim_event(device_extension* v){CHECK(v==&volume);return trim_ready?&trim_work:NULL;}
void trim_process(device_extension* v){CHECK(v==&volume);pthread_mutex_lock(&trim_work.mutex);trim_work.signaled=false;pthread_mutex_unlock(&trim_work.mutex);pthread_mutex_lock(&monitor);trim_calls++;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&monitor);}
void trim_drain(device_extension* v){CHECK(v==&volume);if(trim_ready)trim_drains++;}
static bool KeSetTimer(KTIMER* timer, LARGE_INTEGER due, void* dpc) {
    CHECK(timer->initialized && dpc == NULL);
    if (due.QuadPart == 0)
        KeSetEvent(timer, 0, false);
    else {
        /* Initial arming must happen only once the volume is published. */
        CHECK(vpb.Flags & VPB_MOUNTED);
        CHECK(created == processors + 1);
        pthread_mutex_lock(&monitor);periodic_arms++;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&monitor);
        pthread_mutex_lock(&timer->mutex);
        timer->signaled = false;
        pthread_mutex_unlock(&timer->mutex);
    }
    return false;
}
static bool KeCancelTimer(KTIMER* timer) { CHECK(timer->initialized); return true; }
static void ObReferenceObject(PDEVICE_OBJECT object) { atomic_fetch_add(&object->refs, 1); }
static void ObDereferenceObject(PDEVICE_OBJECT object) { CHECK(atomic_fetch_sub(&object->refs, 1) > 0); }
static void* ExAllocatePoolWithTag(int kind, size_t size, int tag) {
    CHECK(kind == NonPagedPool && tag == ALLOC_TAG && pool == NULL);
    if (fail_allocation)
        return NULL;
    pool_size = size;
    pool = malloc(size);
    CHECK(pool != NULL);
    return pool;
}
static void ExFreePool(void* pointer) {
    CHECK(pointer != NULL && pointer == pool);
    for (unsigned int i = 0; i < created; i++)
        CHECK(handles[i].joined && handles[i].closed);
    drv_calc_thread* threads = pointer;
    for (size_t i = 0; i < pool_size / sizeof(*threads); i++) {
        if (threads[i].finished.initialized) {
            CHECK(pthread_mutex_destroy(&threads[i].finished.mutex) == 0);
            CHECK(pthread_cond_destroy(&threads[i].finished.condition) == 0);
        }
    }
    memset(pointer, 0xa5, pool_size);
    free(pointer);
    pool = NULL;
    frees++;
}
static void calc_thread_main(device_extension* vcb, void* job) {
    CHECK(vcb == &volume && job == NULL);
    pthread_mutex_lock(&monitor);
    work_entered = true;
    pthread_cond_broadcast(&changed);
    while (block_work && !allow_work)
        pthread_cond_wait(&changed, &monitor);
    pthread_mutex_unlock(&monitor);
}
static void do_flush(device_extension* vcb) {
    CHECK(vcb == &volume);
    pthread_mutex_lock(&monitor);flushes++;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&monitor);
    if (!strcmp(scenario, "flush-rearm")) {
        /* Model removal and its wakeup arriving after the worker checked
         * removing, while do_flush was running. The next periodic rearm
         * will clear this wakeup. The worker must check before waiting. */
        vcb->removing = true;
        LARGE_INTEGER due = { .QuadPart = 0 };
        KeSetTimer(&vcb->flush_thread_timer, due, NULL);
    }
}
static void* thread_entry(void* context) {
    self = context;
    pthread_mutex_lock(&monitor);
    while (!allow_start)
        pthread_cond_wait(&changed, &monitor);
    pthread_mutex_unlock(&monitor);
    self->entry(self->context);
    CHECK(false); /* A system thread must terminate itself. */
    return NULL;
}
static NTSTATUS PsCreateSystemThread(HANDLE* result, unsigned int access, OBJECT_ATTRIBUTES* oa,
                                     void* process, void* id, void (*entry)(void*), void* context) {
    CHECK(oa->attributes == OBJ_KERNEL_HANDLE && process == NULL && id == NULL);
    if ((int)attempts++ == failure_at)
        return TEST_FAILURE;
    CHECK(created < sizeof(handles) / sizeof(handles[0]));
    HANDLE handle = &handles[created++];
    handle->entry = entry;
    handle->context = context;
    handle->access = access;
    handle->is_flush = entry == flush_thread;
    *result = handle;
    CHECK(pthread_create(&handle->thread, NULL, thread_entry, handle) == 0);
    return STATUS_SUCCESS;
}
static void PsTerminateSystemThread(NTSTATUS status) {
    CHECK(status == STATUS_SUCCESS);
    pthread_mutex_lock(&monitor);
    self->at_terminate = true;
    pthread_cond_broadcast(&changed);
    while (!allow_termination)
        pthread_cond_wait(&changed, &monitor);
    self->terminated = true;
    pthread_mutex_unlock(&monitor);
    pthread_exit(NULL);
}
NTSTATUS ZwWaitForSingleObject(HANDLE handle, bool alertable, void* timeout) {
    CHECK(handle && !handle->closed && !handle->joined && !alertable && timeout == NULL);
    CHECK(handle->access & SYNCHRONIZE);
    pthread_mutex_lock(&monitor);
    allow_start = true;
    pthread_cond_broadcast(&changed);
    if (block_work && !handle->is_flush) {
        while (!work_entered)
            pthread_cond_wait(&changed, &monitor);
        CHECK(!frees && !handle->closed && !handle->joined);
        allow_work = true;
        pthread_cond_broadcast(&changed);
    }
    /* The real worker has already signaled its finished event here. Hold it
     * inside termination until we know cleanup has reached a thread wait. */
    while (!handle->at_terminate)
        pthread_cond_wait(&changed, &monitor);
    CHECK(!handle->closed && !frees);
    allow_termination = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&monitor);
    CHECK(pthread_join(handle->thread, NULL) == 0);
    CHECK(handle->terminated);
    handle->joined = true;
    joins++;
    return STATUS_SUCCESS;
}
static NTSTATUS ZwClose(HANDLE handle) {
    CHECK(handle && handle->joined && !handle->closed);
    handle->closed = true;
    closes++;
    return STATUS_SUCCESS;
}
static void IoAcquireVpbSpinLock(KIRQL* irql) { *irql = 0; }
static void IoReleaseVpbSpinLock(KIRQL irql) { CHECK(irql == 0); }

#include "lifecycle.inc"

static NTSTATUS activate_mount(void) {
    PDEVICE_OBJECT NewDeviceObject = &device;
    IO_STACK_LOCATION stack = { .Parameters.MountVolume.Vpb = &vpb };
    IO_STACK_LOCATION* IrpSp = &stack;
    device_extension* Vcb = &volume;
    OBJECT_ATTRIBUTES oa;
    KIRQL irql;
    NTSTATUS Status;
    LARGE_INTEGER due_time;
    (void)due_time; /* Used by the corrected mount sequence. */
#include "mount_activation.inc"
    return STATUS_SUCCESS;
exit:
    return Status;
}

static void check_clean(unsigned int expected_threads, bool allocated) {
    CHECK(created == expected_threads);
    CHECK(joins == created && closes == created);
    CHECK(pool == NULL && frees == (unsigned int)allocated);
    CHECK(volume.calcthreads.num_threads == 0);
    CHECK(volume.calcthreads.threads == NULL);
    CHECK(atomic_load(&device.refs) == 0);
}
static void check_private(void) {
    CHECK(vpb.Flags == 0x80 && vpb.DeviceObject == NULL);
    CHECK(vpb.ReferenceCount == 2 && vpb.VolumeLabelLength == 6 && vpb.VolumeLabel[0] == 'O');
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    scenario = argv[1];
    device.DeviceExtension = &volume;
    device.Vpb = &vpb;
    volume.options.flush_interval = 30;
    vpb.Flags = 0x80;
    vpb.ReferenceCount = 2;
    vpb.VolumeLabelLength = 6;
    vpb.VolumeLabel[0] = 'O';
    block_work = strcmp(scenario, "busy") == 0;
    if (!strcmp(scenario, "single"))
        processors = 1;
    if (!strcmp(scenario,"trim-deadline") || !strcmp(scenario,"trim-locked")) {
        CHECK(activate_mount()==STATUS_SUCCESS);volume.locked=!strcmp(scenario,"trim-locked");
        pthread_mutex_lock(&monitor);allow_start=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&monitor);
        KeSetEvent(&trim_work,0,false);
        pthread_mutex_lock(&monitor);while(trim_calls<1)pthread_cond_wait(&changed,&monitor);CHECK(periodic_arms==1&&flushes==0);pthread_mutex_unlock(&monitor);
        for(unsigned i=0;i<3;i++){
            LARGE_INTEGER now={0};KeSetTimer(&volume.flush_thread_timer,now,NULL);
            pthread_mutex_lock(&monitor);while(trim_calls<i+2)pthread_cond_wait(&changed,&monitor);
            CHECK(periodic_arms==i+2&&flushes==(volume.locked?0:i+1));pthread_mutex_unlock(&monitor);
        }
        KeSetEvent(&trim_work,0,false);
        pthread_mutex_lock(&monitor);while(trim_calls<5)pthread_cond_wait(&changed,&monitor);CHECK(periodic_arms==4);pthread_mutex_unlock(&monitor);
        volume.removing=true;stop_volume_threads(&volume);CHECK(trim_drains==1);check_clean(processors+1,true);
    } else if (!strcmp(scenario, "allocation")) {
        fail_allocation = true;
        CHECK(create_calc_threads(&device) == STATUS_INSUFFICIENT_RESOURCES);
        check_clean(0, false);
    } else if (!strcmp(scenario, "empty")) {
        stop_calc_threads(&volume);
        stop_flush_thread(&volume);
        check_clean(0, false);
    } else if (!strncmp(scenario, "calc-failure-", 13)) {
        failure_at = atoi(scenario + 13);
        CHECK(create_calc_threads(&device) == TEST_FAILURE);
        check_clean((unsigned int)failure_at, true);
        stop_calc_threads(&volume);
    } else if (!strcmp(scenario, "mount-allocation")) {
        fail_allocation = true;
        CHECK(activate_mount() == STATUS_INSUFFICIENT_RESOURCES);
        check_private();
        check_clean(0, false);
    } else if (!strncmp(scenario, "mount-failure-", 14) || !strcmp(scenario, "flush-failure")) {
        failure_at = !strcmp(scenario, "flush-failure") ? (int)processors : atoi(scenario + 14);
        CHECK(activate_mount() == TEST_FAILURE);
        check_private();
        check_clean((unsigned int)failure_at, true);
        CHECK(volume.flush_thread_handle == NULL);
        stop_volume_threads(&volume);
    } else if (!strcmp(scenario, "mount-success") || !strcmp(scenario, "flush-rearm")) {
        CHECK(activate_mount() == STATUS_SUCCESS);
        CHECK(vpb.Flags & VPB_MOUNTED);
        CHECK(vpb.DeviceObject == &device && vpb.ReferenceCount == 3);
        if (!strcmp(scenario, "mount-success"))
            volume.removing = true;
        stop_volume_threads(&volume);
        CHECK(handles[0].is_flush == false);
        CHECK(volume.flush_thread_handle == NULL);
        CHECK(flushes == (unsigned int)!strcmp(scenario, "flush-rearm"));
        check_clean(processors + 1, true);
        stop_volume_threads(&volume);
    } else {
        CHECK(!strcmp(scenario, "success") || !strcmp(scenario, "single") ||
              !strcmp(scenario, "busy") || !strcmp(scenario, "termination") || !strcmp(scenario, "restart"));
        allow_start = strcmp(scenario, "termination") != 0;
        CHECK(create_calc_threads(&device) == STATUS_SUCCESS);
        CHECK(volume.calcthreads.num_threads == processors);
        if (allow_start) {
            pthread_mutex_lock(&monitor);
            while (waiting_calc < processors)
                pthread_cond_wait(&changed, &monitor);
            pthread_mutex_unlock(&monitor);
        }
        if (block_work) {
            submitting = true;
            KeSetEvent(&volume.calcthreads.event, 0, false);
            submitting = false;
            pthread_mutex_lock(&monitor);
            while (!work_entered)
                pthread_cond_wait(&changed, &monitor);
            pthread_mutex_unlock(&monitor);
        }
        stop_calc_threads(&volume);
        check_clean(processors, true);
        stop_calc_threads(&volume);
        if (!strcmp(scenario, "restart")) {
            attempts = created = joins = closes = frees = 0;
            memset(handles, 0, sizeof(handles));
            allow_start = allow_termination = false;
            CHECK(create_calc_threads(&device) == STATUS_SUCCESS);
            stop_calc_threads(&volume);
            check_clean(processors, true);
        }
    }
    printf("PASS %s\n", scenario);
    return 0;
}
