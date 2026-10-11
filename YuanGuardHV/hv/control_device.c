#include <ntddk.h>
#include "control_ioctl.h"
#include "control_device.h"
#include "protect.h"
#include "mem_pte.h"      /* Phase 1: page-table walk (Engine B foundation) */
#include "debug.h"

NTKERNELAPI PEPROCESS IoGetRequestorProcess(PIRP Irp);
NTKERNELAPI HANDLE PsGetProcessId(PEPROCESS Process);
/* Phase 1: the WDK km headers here do not declare this, so declare it
 * explicitly (the project does the same for the others above). */
NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId,
                                                PEPROCESS *Process);
NTKERNELAPI NTSTATUS ZwFlushBuffersFile(HANDLE FileHandle,
                                        PIO_STATUS_BLOCK IoStatusBlock);

static PDEVICE_OBJECT g_yghv_device = NULL;

typedef struct {
    PEPROCESS owner_process;
    uint64_t owner_cr3;
} yghv_ctl_ctx_t;

static void yghv_ioctl_log(const char *dir, ULONG code) {
    static const char hex[] = "0123456789abcdef";
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    char buf[64];
    size_t n = 0;
    int i;
    NTSTATUS st;

    RtlInitUnicodeString(&name, L"\\SystemRoot\\yghv_ioctl.log");
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateFile(&h, FILE_APPEND_DATA, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st))
        return;
    while (dir[n] && n < sizeof(buf) - 3)
        buf[n++] = dir[n];
    buf[n++] = ' ';
    buf[n++] = 'i'; buf[n++] = 'o'; buf[n++] = 'c'; buf[n++] = 't';
    buf[n++] = 'l'; buf[n++] = '='; buf[n++] = '0'; buf[n++] = 'x';
    for (i = 7; i >= 0; i--)
        buf[n++] = hex[(code >> (i * 4)) & 0xF];
    buf[n++] = '\r';
    buf[n++] = '\n';
    ZwWriteFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)n, NULL, NULL);
    ZwFlushBuffersFile(h, &iosb);
    ZwClose(h);
}

static NTSTATUS yghv_control_complete(PIRP irp, NTSTATUS status, ULONG info) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS yghv_control_dispatch_default(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    return yghv_control_complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS yghv_control_dispatch_open(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    return yghv_control_complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS yghv_control_dispatch_create(PDEVICE_OBJECT dev, PIRP irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    PEPROCESS proc = IoGetRequestorProcess(irp);
    yghv_ctl_ctx_t *ctx;
    (void)dev;

    if (!proc) {
        LOG_ERROR("control device: create has no requestor process");
        return yghv_control_complete(irp, STATUS_ACCESS_DENIED, 0);
    }
    ctx = (yghv_ctl_ctx_t *)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*ctx), YGHV_TAG);
    if (!ctx)
        return yghv_control_complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    ctx->owner_process = proc;
    ctx->owner_cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
    ObReferenceObject(proc);
    stack->FileObject->FsContext = ctx;
    return yghv_control_complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS yghv_control_dispatch_cleanup(PDEVICE_OBJECT dev, PIRP irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    yghv_ctl_ctx_t *ctx = (yghv_ctl_ctx_t *)stack->FileObject->FsContext;
    (void)dev;

    if (ctx) {
        stack->FileObject->FsContext = NULL;
        ObDereferenceObject(ctx->owner_process);
        ExFreePoolWithTag(ctx, YGHV_TAG);
    }
    return yghv_control_complete(irp, STATUS_SUCCESS, 0);
}

static BOOLEAN yghv_control_ioctl_authorized(PIRP irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    yghv_ctl_ctx_t *ctx = (yghv_ctl_ctx_t *)stack->FileObject->FsContext;
    PEPROCESS proc = IoGetRequestorProcess(irp);
    uint64_t cr3;

    if (!ctx || !proc || proc != ctx->owner_process)
        return FALSE;
    /* P0 hardening: the controlling client must run with SeDebug enabled
       (elevated debugger-style client), not just any process that opened
       the device. */
    {
        LUID dbg_luid = RtlConvertLongToLuid(SE_DEBUG_PRIVILEGE);
        /* REV-012: PreviousMode=UserMode makes SeSinglePrivilegeCheck RAISE
           STATUS_PRIVILEGE_NOT_HELD on absence (the LOG/return-FALSE lines
           become dead code); KernelMode returns FALSE instead.  The check
           still uses the current thread's token either way. */
        if (!SeSinglePrivilegeCheck(dbg_luid, KernelMode)) {
            LOG_ERROR("control device: caller lacks SeDebugPrivilege");
            return FALSE;
        }
    }
    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
    return ctx->owner_cr3 != 0 && cr3 == ctx->owner_cr3;
}

static NTSTATUS yghv_control_dispatch_ioctl(PDEVICE_OBJECT dev, PIRP irp) {
    IO_STACK_LOCATION *stack;
    ULONG code, in_len, out_len;
    PVOID buf;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;
    (void)dev;

    stack = IoGetCurrentIrpStackLocation(irp);
    if (!yghv_control_ioctl_authorized(irp)) {
        LOG_ERROR("control device: unauthorized caller rejected");
        return yghv_control_complete(irp, STATUS_ACCESS_DENIED, 0);
    }
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buf = irp->AssociatedIrp.SystemBuffer;
    yghv_trace_u64("ioctl code", code);
    /* REV-018: drop the per-request "in" log line and log only on error —
       bounds the forensic artifact and the per-IOCTL synchronous disk I/O
       while keeping error diagnostics. */

    switch (code) {
    case IOCTL_YGHV_SET_TARGET: {
        yghv_ioctl_set_target_t *in = (yghv_ioctl_set_target_t *)buf;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = yghv_protect_set_target(in->pid);
        break;
    }
    case IOCTL_YGHV_ADD_PAGE: {
        yghv_ioctl_va_t *in = (yghv_ioctl_va_t *)buf;
        PEPROCESS req_proc = IoGetRequestorProcess(irp);
        uint64_t caller_cr3 = req_proc ?
            *(volatile uint64_t *)((uint8_t *)req_proc + 0x028) : 0;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = yghv_protect_add_page_for(caller_cr3, in->target_va);
        break;
    }
    case IOCTL_YGHV_REMOVE_PAGE: {
        yghv_ioctl_va_t *in = (yghv_ioctl_va_t *)buf;
        PEPROCESS req_proc = IoGetRequestorProcess(irp);
        uint64_t caller_cr3 = req_proc ?
            *(volatile uint64_t *)((uint8_t *)req_proc + 0x028) : 0;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = yghv_protect_remove_page_for(caller_cr3, in->target_va);
        break;
    }
    case IOCTL_YGHV_START_PROTECT:
        status = yghv_protect_start();
        break;
    case IOCTL_YGHV_STOP_PROTECT:
        status = yghv_protect_stop();
        break;
    case IOCTL_YGHV_GET_STATE: {
        yghv_ioctl_state_t *out = (yghv_ioctl_state_t *)buf;
        ULONG active, pid, page_count;
        if (out_len < sizeof(*out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        yghv_protect_get_state(&active, &pid, &page_count);
        out->active = active;
        out->pid = pid;
        out->page_count = page_count;
        info = sizeof(*out);
        break;
    }
    case IOCTL_YGHV_GET_TARGET: {
        yghv_protect_target_info_t *out = (yghv_protect_target_info_t *)buf;
        if (out_len < sizeof(*out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        yghv_protect_get_target(&out->active, &out->pid, &out->cr3,
            &out->page_count, &out->hook_count);
        info = sizeof(*out);
        break;
    }
    case IOCTL_YGHV_GET_PAGES: {
        yghv_protect_pages_info_t *out = (yghv_protect_pages_info_t *)buf;
        if (in_len < sizeof(*out) || out_len < sizeof(*out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (out->count > YGHV_PROTECT_MAX_PAGES)
            out->count = YGHV_PROTECT_MAX_PAGES;
        yghv_protect_get_pages_info(out);
        info = sizeof(*out);
        break;
    }
    case IOCTL_YGHV_GET_HOOKS: {
        yghv_protect_hooks_info_t *out = (yghv_protect_hooks_info_t *)buf;
        if (in_len < sizeof(*out) || out_len < sizeof(*out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (out->count > YGHV_PROTECT_MAX_HOOKS)
            out->count = YGHV_PROTECT_MAX_HOOKS;
        yghv_protect_get_hooks_info(out);
        info = sizeof(*out);
        break;
    }
    case IOCTL_YGHV_GET_TARGETS: {
        yghv_protect_targets_info_t *out =
            (yghv_protect_targets_info_t *)buf;
        if (in_len < sizeof(*out) || out_len < sizeof(*out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (out->count > YGHV_PROTECT_MAX_TARGETS)
            out->count = YGHV_PROTECT_MAX_TARGETS;
        yghv_protect_get_targets_info(out);
        info = sizeof(*out);
        break;
    }
#if defined(YGHV_BAREMETAL_STEP) && (YGHV_BAREMETAL_STEP == 206)
    case IOCTL_YGHV_GET_LASTHIT: {
        /* 9.258 (206-C3): control-plane read of the last protection hit
         * (exact faulting gpa recorded at ALLOW/DENY verdict time). The
         * getter lives in the vendored 206 TU — default builds have no
         * case here (unknown-code path). */
        UINT64 *out = (UINT64 *)buf;
        if (out_len < sizeof(*out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        { extern UINT64 yghv_s206_last_hit(void); *out = yghv_s206_last_hit(); }
        info = sizeof(*out);
        break;
    }
#endif
    case IOCTL_YGHV_CLEAR:
        status = yghv_protect_clear();
        break;
    case IOCTL_YGHV_SET_CONFIG: {
        yghv_protect_config_t *in = (yghv_protect_config_t *)buf;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = yghv_protect_set_config(in);
        break;
    }
    case IOCTL_YGHV_GET_CONFIG: {
        yghv_protect_config_t *out = (yghv_protect_config_t *)buf;
        if (out_len < sizeof(*out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        yghv_protect_get_config(out);
        info = sizeof(*out);
        break;
    }
    case IOCTL_YGHV_INSTALL_HOOK: {
        yghv_protect_install_hook_info_t *in =
            (yghv_protect_install_hook_info_t *)buf;
        PEPROCESS req_proc = IoGetRequestorProcess(irp);
        uint64_t caller_cr3 = req_proc ?
            *(volatile uint64_t *)((uint8_t *)req_proc + 0x028) : 0;
        uint64_t va;
        BOOLEAN terminated = FALSE;
        ULONG i;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (in->hook_id >= YGHV_PROTECT_MAX_HOOKS) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (!yghv_protect_is_target_cr3(caller_cr3)) {
            status = yghv_protect_set_target(
                (uint32_t)(ULONG_PTR)PsGetProcessId(req_proc));
            if (!NT_SUCCESS(status))
                break;
        }
        for (i = 0; i < 64; i++) {
            if (in->name[i] == 0) {
                terminated = TRUE;
                break;
            }
        }
        if (!terminated) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        va = in->func_va;
        if (!va)
            va = yghv_protect_find_func_pattern(in->name, NULL, 0);
        if (!va) {
            status = STATUS_NOT_FOUND;
            break;
        }
        status = yghv_protect_install_hook((uint8_t)in->hook_id, va);
        break;
    }
    case IOCTL_YGHV_REMOVE_HOOK: {
        yghv_protect_remove_hook_info_t *in =
            (yghv_protect_remove_hook_info_t *)buf;
        PEPROCESS req_proc = IoGetRequestorProcess(irp);
        uint64_t caller_cr3 = req_proc ?
            *(volatile uint64_t *)((uint8_t *)req_proc + 0x028) : 0;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (in->hook_id >= YGHV_PROTECT_MAX_HOOKS) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (!yghv_protect_is_target_cr3(caller_cr3)) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        status = yghv_protect_remove_hook((uint8_t)in->hook_id);
        break;
    }
    case IOCTL_YGHV_ADD_PAGE_FOR_PID: {
        /* 206-C4: cross-process arm — the caller is the external controller;
         * the target slot is resolved by pid, not by the caller's CR3. Both
         * handlers live in protect.c (always compiled) — no 206 gating here,
         * a default-build LNK2019 is the failure mode gating avoids. */
        yghv_ioctl_va_pid_t *in = (yghv_ioctl_va_pid_t *)buf;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = yghv_protect_add_page_for_pid(in->pid, in->target_va);
        break;
    }
    case IOCTL_YGHV_REMOVE_PAGE_FOR_PID: {
        yghv_ioctl_va_pid_t *in = (yghv_ioctl_va_pid_t *)buf;
        if (in_len < sizeof(*in)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = yghv_protect_remove_page_for_pid(in->pid, in->target_va);
        break;
    }
    case IOCTL_YGHV_SET_FAKE_MODE: {
        /* 9.275 (C6): shadow fake-write mode for foreign KERNEL writes
         * (cpl=0, non-target CR3). Default OFF. Dual mode: 4-byte input sets
         * the flag; zero-length input reads diagnostics {mode, attempts, ok,
         * last_reject} (9.277). */
        if (in_len == 0 && out_len >= 40) {
            UINT64 *out = (UINT64 *)buf;
            yghv_protect_fake_diag(out);
            info = 40;
            break;
        }
        if (in_len >= sizeof(yghv_ioctl_fake_mode_t)) {
            yghv_ioctl_fake_mode_t *in = (yghv_ioctl_fake_mode_t *)buf;
            yghv_protect_fake_mode_set(in->enable ? 1 : 0);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_SET_FAKE_TLB: {
        /* 9.297 (A'): runtime TLB-flush strategy for the fake-write path.
         * Zero-length input = read back {mode, times_consulted}. 4-byte input
         * sets the mode (0..3). See protect.c for the strategy table. */
        if (in_len == 0 && out_len >= 16) {
            UINT64 *out = (UINT64 *)buf;
            out[0] = (UINT64)yghv_protect_fake_tlb_get();
            out[1] = (UINT64)yghv_protect_fake_tlb_seen();
            info = 16;
            break;
        }
        if (in_len >= sizeof(ULONG)) {
            ULONG *in = (ULONG *)buf;
            yghv_protect_fake_tlb_set((int)in[0]);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_SET_SYNC: {
        /* 9.299 (C line): sync guard — snapshot + poll, no NPT flip.
         * Zero-length input reads {mode, polls, hits, interval_ms, running, pages}.
         * 8-byte input {ULONG enable, ULONG interval_ms} sets them. */
        if (in_len == 0 && out_len >= 64) {
            UINT64 *out = (UINT64 *)buf;
            yghv_protect_sync_diag(out);
            info = 64;
            break;
        }
        if (in_len >= 8) {
            ULONG *in = (ULONG *)buf;
            yghv_protect_sync_set_interval((int)in[1]);
            /* Master switch; per-page snapshots are taken inside the setter. */
            yghv_protect_sync_mode_set(in[0] ? 1 : 0);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_PTE_PROBE: {
        /* Phase 1 (2026-10-11): exercise the hand-rolled page-table walk.
         * Read: {ULONG pid, ULONG pad, ULONG_PTR va} -> 16 qwords of result.
         * Write: zero-length input -> reset the walk statistics.
         * Nothing is read from the target process — this only translates.
         *
         * METHOD_BUFFERED shares ONE SystemBuffer between input and output,
         * so the reply overwrites the request as it is written. Everything we
         * still need (pid, va) must be read out BEFORE the first store. */
        if (in_len == 0) {
            yghv_pte_stats_reset();
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= sizeof(yghv_ioctl_pte_probe_t) &&
            out_len >= YGHV_PTE_PROBE_OUT_QWORDS * sizeof(UINT64)) {
            yghv_ioctl_pte_probe_t *req = (yghv_ioctl_pte_probe_t *)buf;
            UINT64 *out = (UINT64 *)buf;
            PEPROCESS proc = NULL;
            uint64_t cr3 = 0;
            uint32_t req_pid = req->pid;          /* read BEFORE any store */
            uint64_t req_va  = (uint64_t)req->va; /* ditto */
            yghv_pte_info_t pi;
            yghv_pte_stats_t st;
            NTSTATUS lst;

            lst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)req_pid, &proc);
            RtlZeroMemory(&pi, sizeof(pi));
            if (NT_SUCCESS(lst) && proc) {
                /* KPROCESS.DirectoryTableBase is at offset 0x028 on 19045 —
                 * the same offset the protect path uses (protect.c set_target). */
                cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
                yghv_pte_walk(cr3, req_va, &pi);
            }

            out[0] = pi.pa ? 1 : 0;
            out[1] = cr3;
            out[2] = req_va;
            out[3] = pi.pa;
            out[4] = pi.page_base;
            out[5] = pi.page_size;
            out[6] = (UINT64)pi.kind;
            out[7] = (UINT64)pi.levels;
            out[8] = ((UINT64)pi.present)
                   | ((UINT64)pi.writable  << 1)
                   | ((UINT64)pi.user      << 2)
                   | ((UINT64)pi.accessed  << 3)
                   | ((UINT64)pi.dirty     << 4)
                   | ((UINT64)pi.nx        << 5)
                   | ((UINT64)pi.global    << 6);
            yghv_pte_stats_get(&st);
            out[9]  = st.walks;       out[10] = st.hit_4k;
            out[11] = st.hit_2m;      out[12] = st.hit_1g;
            out[13] = st.not_present; out[14] = st.bad_cr3;
            out[15] = st.table_reads;

            if (NT_SUCCESS(lst) && proc)
                ObDereferenceObject(proc);
            info = YGHV_PTE_PROBE_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    if (!NT_SUCCESS(status)) {
        info = 0;
        yghv_ioctl_log("out", code);   /* only errors are logged (REV-018) */
    }
    return yghv_control_complete(irp, status, info);
}

NTSTATUS yghv_control_device_init(PDRIVER_OBJECT driver) {
    UNICODE_STRING dev_name, link_name;
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS status;
    ULONG i;

    RtlInitUnicodeString(&dev_name, YGHV_DEVICE_NAME_STRING);
    RtlInitUnicodeString(&link_name, YGHV_DOS_DEVICE_NAME_STRING);

    status = IoCreateDevice(driver, 0, &dev_name, FILE_DEVICE_UNKNOWN, 0,
                            FALSE, &dev);
    if (!NT_SUCCESS(status)) {
        LOG_ERROR("control device: IoCreateDevice failed 0x%x", status);
        return status;
    }
    status = IoCreateSymbolicLink(&link_name, &dev_name);
    if (!NT_SUCCESS(status)) {
        LOG_ERROR("control device: IoCreateSymbolicLink failed 0x%x", status);
        IoDeleteDevice(dev);
        return status;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        driver->MajorFunction[i] = yghv_control_dispatch_default;
    driver->MajorFunction[IRP_MJ_CREATE] = yghv_control_dispatch_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = yghv_control_dispatch_open;
    driver->MajorFunction[IRP_MJ_CLEANUP] = yghv_control_dispatch_cleanup;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = yghv_control_dispatch_ioctl;

    dev->Flags |= DO_BUFFERED_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    g_yghv_device = dev;

    LOG_INFO("control device created");
    return STATUS_SUCCESS;
}

void yghv_control_device_cleanup(PDRIVER_OBJECT driver) {
    UNICODE_STRING link_name;
    (void)driver;

    RtlInitUnicodeString(&link_name, YGHV_DOS_DEVICE_NAME_STRING);
    IoDeleteSymbolicLink(&link_name);
    if (g_yghv_device) {
        IoDeleteDevice(g_yghv_device);
        g_yghv_device = NULL;
    }
    LOG_INFO("control device removed");
}
