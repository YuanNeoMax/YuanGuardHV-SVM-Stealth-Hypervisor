/*
 * yghv_inject.c -- five injection primitives, no policy above them (9.592).
 *
 * See common/yghv_inject.h for why these five and not a "DLL injector".
 *
 * THE TWO THINGS THAT GO WRONG IN EVERY HAND-ROLLED INJECTOR
 *
 *   1. The handle is opened with PROCESS_ALL_ACCESS. That fails on a
 *      protected process with STATUS_ACCESS_DENIED and looks like "injection
 *      is blocked", when the real cause is asking for rights the operation
 *      does not need. Each primitive opens with exactly the rights it uses.
 *
 *   2. The payload buffer is allocated with PAGE_EXECUTE_READWRITE and left
 *      that way. A W^X split (RW for the write, then RX for the call) costs
 *      one extra ZwProtectVirtualMemory and removes the single most reliable
 *      signature a scanner has. yghv_inj_write() does that transition itself
 *      when the caller asked for executable memory.
 *
 * IRQL: PASSIVE_LEVEL. Every primitive touches a handle or a user address in
 * another process, and both require it.
 */

#include <ntifs.h>
#include <stdint.h>

#include "common/yghv_inject.h"
#include "common/yghv_proc.h"
#include "common/yghv_nt.h"
#include "common/control_ioctl.h"
#include "common/mem_engine.h"
#include "debug.h"

/*
 * None of these are in the WDK 10.0.19041.0 km headers (measured with grep
 * over every km header: ZwProtectVirtualMemory, ZwQueryInformationThread,
 * MmCopyVirtualMemory, KeInitializeApc, KeInsertQueueApc, RtlCreateUserThread
 * and the three APC routine typedefs are all absent; ntifs.h only has the
 * Zw*VirtualMemory pair).
 */
NTKERNELAPI NTSTATUS ZwProtectVirtualMemory(HANDLE ProcessHandle,
                                            PVOID *BaseAddress,
                                            PSIZE_T RegionSize,
                                            ULONG NewProtect,
                                            PULONG OldProtect);
NTKERNELAPI NTSTATUS RtlCreateUserThread(HANDLE ProcessHandle,
                                         PSECURITY_DESCRIPTOR SecurityDescriptor,
                                         BOOLEAN CreateSuspended,
                                         ULONG StackZeroBits,
                                         PSIZE_T StackReserved,
                                         PSIZE_T StackCommit,
                                         PVOID StartAddress,
                                         PVOID StartParameter,
                                         PHANDLE ThreadHandle,
                                         PCLIENT_ID ClientId);

/* The three APC routine shapes. wdm.h defines KAPC itself but not these. */
typedef enum _YGHV_KAPC_ENV {
    YGHV_KAPC_ORIGINAL = 0,
    YGHV_KAPC_ATTACHED = 1,
    YGHV_KAPC_CURRENT  = 2,
    YGHV_KAPC_INSERT   = 3
} YGHV_KAPC_ENV;

typedef VOID (*YGHV_PKNORMAL_ROUTINE)(PVOID NormalContext, PVOID SystemArgument1,
                                      PVOID SystemArgument2);
typedef VOID (*YGHV_PKKERNEL_ROUTINE)(PKAPC Apc, PVOID *NormalRoutine,
                                      PVOID *NormalContext, PVOID *SystemArgument1,
                                      PVOID *SystemArgument2);
typedef VOID (*YGHV_PKRUNDOWN_ROUTINE)(PKAPC Apc);

NTKERNELAPI VOID KeInitializeApc(PKAPC Apc, PKTHREAD Thread, YGHV_KAPC_ENV Environment,
                                 YGHV_PKKERNEL_ROUTINE KernelRoutine,
                                 YGHV_PKRUNDOWN_ROUTINE RundownRoutine,
                                 YGHV_PKNORMAL_ROUTINE NormalRoutine,
                                 KPROCESSOR_MODE ApcMode, PVOID NormalContext);
NTKERNELAPI BOOLEAN KeInsertQueueApc(PKAPC Apc, PVOID SystemArgument1,
                                     PVOID SystemArgument2, KPRIORITY Increment);
NTKERNELAPI NTSTATUS PsLookupThreadByThreadId(HANDLE ThreadId, PETHREAD *Thread);

/*
 * Two rights that the km headers do not carry either. They are in winnt.h,
 * which a driver never includes -- the same trap 9.591 hit with
 * PROCESS_TERMINATE.
 */
#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION 0x0008
#endif
#ifndef PROCESS_CREATE_THREAD
#define PROCESS_CREATE_THREAD 0x0002
#endif

#define YGHV_INJ_TAG 'jIvG'

static volatile LONG64 g_inj_allocs;
static volatile LONG64 g_inj_writes;
static volatile LONG64 g_inj_threads;
static volatile LONG64 g_inj_apcs;
static volatile LONG64 g_inj_frees;
static volatile LONG64 g_inj_fails;
static volatile LONG64 g_inj_bytes;
static volatile LONG64 g_inj_apc_live;

/*
 * A queued user APC is non-paged pool owned by this driver, and the kernel
 * routine that frees it is the only code that can free it. If the image
 * unloads while one is still queued, the APC fires into freed address space --
 * so the count is tracked, and cleanup waits for it rather than assuming.
 */
#define YGHV_INJ_APC_WAIT_MS 2000

/* ------------------------------------------------------------------ */
/* Opening the target                                                  */
/* ------------------------------------------------------------------ */

static NTSTATUS yghv_inj_open(ULONG pid, ACCESS_MASK access, HANDLE *out_handle,
                              PEPROCESS *out_proc)
{
    PEPROCESS proc = NULL;
    HANDLE h = NULL;
    NTSTATUS st;

    if (!out_handle || !out_proc) return STATUS_INVALID_PARAMETER;
    *out_handle = NULL;
    *out_proc = NULL;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    /*
     * ObOpenObjectByPointer takes the object type, and PsProcessType is
     * declared as POBJECT_TYPE * under !_NTHALLIB_ (which this tree does not
     * define -- measured, ntddk.h:109), so it is dereferenced once.
     */
    st = ObOpenObjectByPointer(proc, OBJ_KERNEL_HANDLE, NULL, access,
                               *PsProcessType, KernelMode, &h);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(proc);
        return st;
    }

    *out_handle = h;
    *out_proc = proc;
    return STATUS_SUCCESS;
}

static void yghv_inj_close(HANDLE h, PEPROCESS proc)
{
    if (h) ZwClose(h);
    if (proc) ObDereferenceObject(proc);
}

/* ------------------------------------------------------------------ */
/* 1. alloc                                                            */
/* ------------------------------------------------------------------ */

int yghv_inj_alloc(unsigned long pid, uint64_t *va_inout, uint64_t size,
                   unsigned long protect)
{
    HANDLE h = NULL;
    PEPROCESS proc = NULL;
    PVOID base;
    SIZE_T region;
    NTSTATUS st;

    if (!va_inout) return YGHV_INJ_E_SIZE;
    if (size == 0 || size > YGHV_INJ_MAX_ALLOC) return YGHV_INJ_E_SIZE;

    st = yghv_inj_open((ULONG)pid, PROCESS_VM_OPERATION, &h, &proc);
    if (!NT_SUCCESS(st)) {
        /* Distinguish "no such process" from "protected against the open". */
        return (st == STATUS_ACCESS_DENIED) ? YGHV_INJ_E_NO_HANDLE : YGHV_INJ_E_NO_TARGET;
    }

    /*
     * A zero base means "anywhere you like". Rounding the request up to a page
     * is not pedantry: ZwAllocateVirtualMemory commits whole pages, and a
     * SIZE_T the caller computed from a payload length would otherwise leave
     * the tail of the last page readable-but-unowned.
     */
    region = (SIZE_T)((size + 0xFFF) & ~(uint64_t)0xFFF);
    base = (PVOID)(ULONG_PTR)*va_inout;

    st = ZwAllocateVirtualMemory(h, &base, 0, &region,
                                 MEM_COMMIT | MEM_RESERVE,
                                 (ULONG)(protect ? protect : PAGE_READWRITE));
    yghv_inj_close(h, proc);

    if (!NT_SUCCESS(st)) {
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_ALLOC;
    }

    *va_inout = (uint64_t)(ULONG_PTR)base;
    InterlockedIncrement64(&g_inj_allocs);
    return YGHV_INJ_OK;
}

/* ------------------------------------------------------------------ */
/* 2. write                                                            */
/* ------------------------------------------------------------------ */

int yghv_inj_write(unsigned long pid, uint64_t va, const void *buf,
                   unsigned long len, unsigned long *written)
{
    PEPROCESS proc = NULL;
    SIZE_T done = 0;
    NTSTATUS st;

    if (written) *written = 0;
    if (!buf || len == 0 || len > YGHV_INJ_MAX_WRITE) return YGHV_INJ_E_SIZE;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return YGHV_INJ_E_NO_TARGET;

    /*
     * The memory engine is what the rest of the driver writes through, so an
     * injected payload takes the same path as every other write: engine B
     * (page-table walk) when it resolves, engine A (MmCopyVirtualMemory)
     * otherwise. Using a bare RtlCopyMemory after KeStackAttachProcess here
     * would be a second, differently-buggy copy of the same logic.
     */
    st = yghv_mem_write(proc, va, buf, len, &done, NULL);
    ObDereferenceObject(proc);

    if (written) *written = (unsigned long)done;
    if (!NT_SUCCESS(st) || done != len) {
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_WRITE;
    }

    InterlockedIncrement64(&g_inj_writes);
    InterlockedAdd64(&g_inj_bytes, (LONG64)done);
    return YGHV_INJ_OK;
}

/* ------------------------------------------------------------------ */
/* 3. free                                                             */
/* ------------------------------------------------------------------ */

int yghv_inj_free(unsigned long pid, uint64_t va, uint64_t size)
{
    HANDLE h = NULL;
    PEPROCESS proc = NULL;
    PVOID base;
    SIZE_T region;
    NTSTATUS st;

    if (va == 0) return YGHV_INJ_E_SIZE;

    st = yghv_inj_open((ULONG)pid, PROCESS_VM_OPERATION, &h, &proc);
    if (!NT_SUCCESS(st)) {
        return (st == STATUS_ACCESS_DENIED) ? YGHV_INJ_E_NO_HANDLE : YGHV_INJ_E_NO_TARGET;
    }

    base = (PVOID)(ULONG_PTR)va;
    /* MEM_RELEASE requires size 0 and ignores it; passing the real size would
     * work on some builds and fail with STATUS_INVALID_PARAMETER on others. */
    region = 0;
    (void)size;

    st = ZwFreeVirtualMemory(h, &base, &region, MEM_RELEASE);
    yghv_inj_close(h, proc);

    if (!NT_SUCCESS(st)) {
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_FREE;
    }

    InterlockedIncrement64(&g_inj_frees);
    return YGHV_INJ_OK;
}

/* ------------------------------------------------------------------ */
/* 4. thread                                                           */
/* ------------------------------------------------------------------ */

int yghv_inj_thread(unsigned long pid, uint64_t start_va, uint64_t arg,
                    unsigned long flags, unsigned long *tid_out)
{
    HANDLE h = NULL, th = NULL;
    PEPROCESS proc = NULL;
    CLIENT_ID cid;
    NTSTATUS st;

    if (tid_out) *tid_out = 0;
    if (start_va == 0) return YGHV_INJ_E_SIZE;

    st = yghv_inj_open((ULONG)pid, PROCESS_CREATE_THREAD, &h, &proc);
    if (!NT_SUCCESS(st)) {
        return (st == STATUS_ACCESS_DENIED) ? YGHV_INJ_E_NO_HANDLE : YGHV_INJ_E_NO_TARGET;
    }

    RtlZeroMemory(&cid, sizeof(cid));

    /*
     * CreateSuspended is exposed rather than decided here: an injector that
     * wants to write a second-stage payload needs the thread to exist and not
     * run, and one that is injecting already-written bytes does not. Both are
     * legitimate, so the caller says which.
     */
    st = RtlCreateUserThread(h, NULL, (BOOLEAN)((flags & 0x1u) ? TRUE : FALSE),
                             0, NULL, NULL,
                             (PVOID)(ULONG_PTR)start_va, (PVOID)(ULONG_PTR)arg,
                             &th, &cid);

    if (NT_SUCCESS(st) && th) ZwClose(th);
    yghv_inj_close(h, proc);

    if (!NT_SUCCESS(st)) {
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_THREAD;
    }

    if (tid_out) *tid_out = (unsigned long)(ULONG_PTR)cid.UniqueThread;
    InterlockedIncrement64(&g_inj_threads);
    return YGHV_INJ_OK;
}

/* ------------------------------------------------------------------ */
/* 5. APC                                                              */
/* ------------------------------------------------------------------ */

/*
 * The kernel routine runs at APC_LEVEL in an arbitrary thread context. All it
 * does is free the KAPC -- anything more would have to be designed around a
 * context it does not choose.
 */
static VOID yghv_inj_apc_kernel_routine(PKAPC Apc, PVOID *NormalRoutine,
                                        PVOID *NormalContext, PVOID *SystemArgument1,
                                        PVOID *SystemArgument2)
{
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    InterlockedDecrement64(&g_inj_apc_live);
    if (Apc) ExFreePoolWithTag(Apc, YGHV_INJ_TAG);
}

static VOID yghv_inj_apc_rundown_routine(PKAPC Apc)
{
    InterlockedDecrement64(&g_inj_apc_live);
    if (Apc) ExFreePoolWithTag(Apc, YGHV_INJ_TAG);
}

int yghv_inj_apc(unsigned long pid, unsigned long tid, uint64_t routine_va,
                 uint64_t arg, unsigned long *queued)
{
    PEPROCESS proc = NULL;
    PETHREAD thread = NULL;
    PKAPC apc;
    NTSTATUS st;
    BOOLEAN ok;

    if (queued) *queued = 0;
    if (routine_va == 0 || tid == 0) return YGHV_INJ_E_SIZE;
    if (routine_va > YGHV_USER_VA_MAX) return YGHV_INJ_E_SIZE;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return YGHV_INJ_E_NO_TARGET;

    st = PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)tid, &thread);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(proc);
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_TID;
    }

    /*
     * A thread id is system-wide, so a valid thread is not necessarily THIS
     * process's thread. Queueing onto the wrong one puts a user-mode routine
     * address into a process that cannot execute it, and the APC fires once
     * and dies -- a silent no-op that looks like a delivery bug.
     */
    if (yghv_thread_process(thread) != proc) {
        ObDereferenceObject(thread);
        ObDereferenceObject(proc);
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_TID;
    }

    apc = (PKAPC)ExAllocatePoolWithTag(NonPagedPool, sizeof(KAPC), YGHV_INJ_TAG);
    if (!apc) {
        ObDereferenceObject(thread);
        ObDereferenceObject(proc);
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_APC;
    }

    InterlockedIncrement64(&g_inj_apc_live);

    /*
     * OriginalApcEnvironment, not InsertApcEnvironment: the APC belongs to the
     * thread's own process, and CurrentApcEnvironment would bind it to whoever
     * happens to be current when it is queued.
     *
     * NormalRoutine is a USER address and ApcMode is UserMode, so this is a
     * user APC. It is delivered only when the target thread enters an alertable
     * wait -- that is the mechanism's contract, not a defect to work around,
     * and it is why an APC injection needs a thread doing I/O or waiting.
     */
    KeInitializeApc(apc, (PKTHREAD)thread, YGHV_KAPC_ORIGINAL,
                    yghv_inj_apc_kernel_routine, yghv_inj_apc_rundown_routine,
                    (YGHV_PKNORMAL_ROUTINE)(ULONG_PTR)routine_va,
                    UserMode, (PVOID)(ULONG_PTR)arg);

    ok = KeInsertQueueApc(apc, NULL, NULL, 0);

    ObDereferenceObject(thread);
    ObDereferenceObject(proc);

    if (!ok) {
        /* Not inserted means the kernel routine will never run, so the pool
         * is ours to free. */
        InterlockedDecrement64(&g_inj_apc_live);
        ExFreePoolWithTag(apc, YGHV_INJ_TAG);
        InterlockedIncrement64(&g_inj_fails);
        return YGHV_INJ_E_APC;
    }

    InterlockedIncrement64(&g_inj_apcs);
    if (queued) *queued = 1;
    return YGHV_INJ_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void yghv_inj_cleanup(void)
{
    ULONG waited = 0;
    LARGE_INTEGER interval;

    interval.QuadPart = -10000;   /* 1 ms, relative */

    /*
     * Waiting here is the whole point. An outstanding user APC holds non-paged
     * pool and will call back into this image; unloading first turns a queued
     * injection into a jump into freed memory. The wait is bounded because
     * hanging the unload is the other way to be wrong -- and when it expires
     * the count is left visible in the stats rather than the fact being
     * swallowed.
     */
    while (InterlockedCompareExchange64(&g_inj_apc_live, 0, 0) > 0 &&
           waited < YGHV_INJ_APC_WAIT_MS) {
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        waited++;
    }

    if (InterlockedCompareExchange64(&g_inj_apc_live, 0, 0) > 0) {
        LOG_ERROR("inject: %ld APC(s) still queued after %u ms; unloading anyway",
                  (long)InterlockedCompareExchange64(&g_inj_apc_live, 0, 0), waited);
    }
}

void yghv_inj_stats(uint64_t *out)
{
    if (!out) return;
    out[0] = (uint64_t)InterlockedCompareExchange64(&g_inj_allocs, 0, 0);
    out[1] = (uint64_t)InterlockedCompareExchange64(&g_inj_writes, 0, 0);
    out[2] = (uint64_t)InterlockedCompareExchange64(&g_inj_threads, 0, 0);
    out[3] = (uint64_t)InterlockedCompareExchange64(&g_inj_apcs, 0, 0);
    out[4] = (uint64_t)InterlockedCompareExchange64(&g_inj_frees, 0, 0);
    out[5] = (uint64_t)InterlockedCompareExchange64(&g_inj_fails, 0, 0);
    out[6] = (uint64_t)InterlockedCompareExchange64(&g_inj_bytes, 0, 0);
    out[7] = (uint64_t)InterlockedCompareExchange64(&g_inj_apc_live, 0, 0);
}


/* ------------------------------------------------------------------ */
/* IOCTL dispatch                                                      */
/* ------------------------------------------------------------------ */

/*
 * The request header and the reply header share ONE METHOD_BUFFERED buffer, and
 * the reply header is 64 bytes while the request header is 56. So a WRITE op
 * that read its payload straight out of the buffer would have its first eight
 * payload bytes overwritten by the reply before the copy finished. The payload
 * is therefore copied to pool first, before anything writes to out.
 */
static NTSTATUS yghv_inject_write_payload(const yghv_ioctl_inject_req_t *req,
                                          const UCHAR *payload, ULONG payload_len,
                                          uint64_t *out)
{
    void *staged;
    int rc;
    unsigned long done = 0;

    if (req->arg0 == 0 || req->arg0 > YGHV_INJ_MAX_WRITE) {
        out[1] = (UINT64)YGHV_INJ_E_SIZE;
        return STATUS_INVALID_PARAMETER;
    }
    if (payload_len < (ULONG)req->arg0) {
        out[1] = (UINT64)YGHV_INJ_E_SIZE;
        return STATUS_BUFFER_TOO_SMALL;
    }

    staged = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)req->arg0, YGHV_INJ_TAG);
    if (!staged) {
        out[1] = (UINT64)YGHV_INJ_E_WRITE;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(staged, payload, (SIZE_T)req->arg0);
    rc = yghv_inj_write(req->pid, (uint64_t)req->va, staged,
                        (unsigned long)req->arg0, &done);
    ExFreePoolWithTag(staged, YGHV_INJ_TAG);

    out[1] = (UINT64)rc;
    out[2] = (UINT64)done;
    out[3] = (UINT64)done;
    return rc == YGHV_INJ_OK ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS yghv_inject_dispatch(const void *in, const void *payload, ULONG payload_len,
                              void *outv, ULONG out_len, ULONG *info)
{
    yghv_ioctl_inject_req_t req;
    uint64_t *out = (uint64_t *)outv;
    NTSTATUS st = STATUS_SUCCESS;
    unsigned long a = 0, b = 0;

    if (!in || !out || !info) return STATUS_INVALID_PARAMETER;
    if (out_len < YGHV_INJECT_HDR_QWORDS * 8) return STATUS_BUFFER_TOO_SMALL;

    /* Copy the request out BEFORE clearing the reply: in and out are the same
     * buffer, so a memset first would zero the op code and every call would
     * silently become op 0. */
    RtlCopyMemory(&req, in, sizeof(req));
    RtlZeroMemory(out, YGHV_INJECT_HDR_QWORDS * 8);
    *info = YGHV_INJECT_HDR_QWORDS * 8;

    switch (req.op) {
    case YGHV_INJECT_OP_ALLOC:
    {
        uint64_t va = (uint64_t)req.va;
        int rc = yghv_inj_alloc(req.pid, &va, (uint64_t)req.arg0, req.flags);
        out[0] = (rc == YGHV_INJ_OK) ? 1 : 0;
        out[1] = (UINT64)rc;
        out[2] = va;
        if (rc != YGHV_INJ_OK) st = STATUS_UNSUCCESSFUL;
        break;
    }
    case YGHV_INJECT_OP_WRITE:
        st = yghv_inject_write_payload(&req, (const UCHAR *)payload, payload_len, out);
        out[0] = (out[1] == 0) ? 1 : 0;
        break;

    case YGHV_INJECT_OP_FREE:
    {
        int rc = yghv_inj_free(req.pid, (uint64_t)req.va, (uint64_t)req.arg0);
        out[0] = (rc == YGHV_INJ_OK) ? 1 : 0;
        out[1] = (UINT64)rc;
        if (rc != YGHV_INJ_OK) st = STATUS_UNSUCCESSFUL;
        break;
    }
    case YGHV_INJECT_OP_THREAD:
    {
        int rc = yghv_inj_thread(req.pid, (uint64_t)req.arg0, (uint64_t)req.arg1,
                                 req.flags, &a);
        out[0] = (rc == YGHV_INJ_OK) ? 1 : 0;
        out[1] = (UINT64)rc;
        out[2] = (UINT64)a;
        if (rc != YGHV_INJ_OK) st = STATUS_UNSUCCESSFUL;
        break;
    }
    case YGHV_INJECT_OP_APC:
    {
        /* flags carries the thread id: the struct has no free ULONG left that a
         * caller would not confuse with a size, and a tid is a ULONG anyway. */
        int rc = yghv_inj_apc(req.pid, req.flags, (uint64_t)req.arg0,
                              (uint64_t)req.arg1, &b);
        out[0] = (rc == YGHV_INJ_OK) ? 1 : 0;
        out[1] = (UINT64)rc;
        out[2] = (UINT64)req.flags;
        out[3] = (UINT64)b;
        if (rc != YGHV_INJ_OK) st = STATUS_UNSUCCESSFUL;
        break;
    }
    case YGHV_INJECT_OP_STATS:
        yghv_inj_stats(out);
        out[0] = 1;
        break;

    default:
        out[0] = 0;
        out[1] = (UINT64)YGHV_INJ_E_SIZE;
        st = STATUS_INVALID_PARAMETER;
        break;
    }

    return st;
}

