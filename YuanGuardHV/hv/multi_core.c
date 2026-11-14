#include <ntddk.h>
#include "svm_defs.h"
#include "svm_vcpu.h"
#include "debug.h"

extern npt_mgr_t g_npt;
extern uint64_t g_guest_hb_va;
extern uint64_t g_vmmcall_auth_cookie;

typedef struct {
    uint32_t core;
    NTSTATUS status;
} yghv_resident_ctx_t;

static HANDLE g_resident_threads[SVM_MAX_CORES];
static yghv_resident_ctx_t *g_resident_ctx[SVM_MAX_CORES];
static KEVENT g_ready_events[SVM_MAX_CORES];
static ULONG g_resident_thread_count;

/* 9.404: read-only accessor for the pre-teardown snapshot. The counter itself is
 * zeroed by svm_core_stop_all_residents, so a post-crash read cannot tell "never
 * started" from "started then cleared" - the snapshot is what can. */
ULONG_PTR svm_core_resident_threads_for_trace(void) {
    return (ULONG_PTR)g_resident_thread_count;
}

static VOID yghv_resident_thread(PVOID context) {
    yghv_resident_ctx_t *ctx = (yghv_resident_ctx_t *)context;
    uint32_t core = ctx->core;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    if (!svm_resident_try_activate(g_vcpus[core])) {
        KeSetEvent(&g_ready_events[core], IO_NO_INCREMENT, FALSE);
        InterlockedExchange((volatile LONG *)&g_vcpus[core]->resident_state,
                            SVM_RESIDENT_STOPPED);
        KeRevertToUserAffinityThread();
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }
    KeSetEvent(&g_ready_events[core], IO_NO_INCREMENT, FALSE);
    ctx->status = (svm_core_enter_resident_current(core) == 0)
                      ? STATUS_SUCCESS
                      : STATUS_UNSUCCESSFUL;
    KeRevertToUserAffinityThread();

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS svm_core_start_residents_range(ULONG online, ULONG first,
                                               const char *kind) {
    ULONG i;

    g_resident_thread_count = 0;
    for (i = first; i < online && i < SVM_MAX_CORES; i++) {
        yghv_resident_ctx_t *ctx;
        HANDLE thread;
        NTSTATUS status;

        if (!g_vcpus[i]) continue;

        ctx = (yghv_resident_ctx_t *)ExAllocatePoolWithTag(
            NonPagedPool, sizeof(*ctx), YGHV_TAG);
        if (!ctx) {
            LOG_ERROR("alloc resident ctx core %u failed", i);
            svm_core_stop_all_residents();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(ctx, sizeof(*ctx));
        ctx->core = i;
        KeInitializeEvent(&g_ready_events[i], NotificationEvent, FALSE);

        status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                      yghv_resident_thread, ctx);
        if (!NT_SUCCESS(status)) {
            /* 9.608: no API name in the literal -- a dead LOG_ERROR string is
             * still emitted into .rdata and gives string searches the name. */
            LOG_ERROR("core %u system thread failed 0x%x", i, status);
            ExFreePool(ctx);
            svm_core_stop_all_residents();
            return status;
        }

        g_resident_threads[i] = thread;
        g_resident_ctx[i] = ctx;
        g_resident_thread_count++;
        LOG_INFO("resident thread created for core %u", i);
    }

    LOG_INFO("started %u %s resident threads", g_resident_thread_count, kind);
    return STATUS_SUCCESS;
}

NTSTATUS svm_core_start_remote_residents(ULONG online) {
    return svm_core_start_residents_range(online, 1, "remote");
}

NTSTATUS svm_core_start_persistent_residents(ULONG online) {
    return svm_core_start_residents_range(online, 0, "persistent");
}

void svm_core_wait_remote_ready(ULONG online) {
    ULONG i;
    for (i = 0; i < online && i < SVM_MAX_CORES; i++) {
        if (!g_resident_threads[i]) continue;
        KeWaitForSingleObject(&g_ready_events[i], Executive, KernelMode, FALSE, NULL);
    }
}

void svm_core_wait_all_stopped(ULONG online) {
    ULONG i;

    for (i = 0; i < online && i < SVM_MAX_CORES; i++) {
        PETHREAD thread_obj = NULL;
        NTSTATUS status;

        if (!g_resident_threads[i]) continue;

        status = ObReferenceObjectByHandle(
            g_resident_threads[i], THREAD_ALL_ACCESS, *PsThreadType,
            KernelMode, (PVOID *)&thread_obj, NULL);
        if (NT_SUCCESS(status) && thread_obj) {
            KeWaitForSingleObject(thread_obj, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(thread_obj);
        } else {
            LOG_ERROR("[api] core %u failed 0x%x", i, status);
        }

        if (g_resident_ctx[i] && !NT_SUCCESS(g_resident_ctx[i]->status))
            LOG_ERROR("resident thread core %u status 0x%x", i, g_resident_ctx[i]->status);

        ZwClose(g_resident_threads[i]);
        g_resident_threads[i] = NULL;
        if (g_resident_ctx[i]) {
            ExFreePool(g_resident_ctx[i]);
            g_resident_ctx[i] = NULL;
        }
    }
    g_resident_thread_count = 0;
    LOG_INFO("all resident threads joined");
}
