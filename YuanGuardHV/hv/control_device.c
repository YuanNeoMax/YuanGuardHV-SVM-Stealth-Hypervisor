#include <ntddk.h>
#include "control_ioctl.h"
#include "control_device.h"
#include "protect.h"
#include "mem_pte.h"      /* Phase 1: page-table walk (Engine B foundation) */
#include "mem_engine.h"   /* Phase 3: dual-engine typed access */
#include "mem_pa.h"       /* Phase 2: physical frame primitives */
#include "mem_bench.h"    /* Phase 5 (Tier 1.5): Engine A vs B measurement */
#include "mem_selfmap.h"  /* Phase 2b: self-built mapping window */
#include "mem_coop.h"     /* Phase 7: guard cooperation */
#include "mem_island.h"   /* Phase 6: island (direct-map) access */
#include "mem_view.h"     /* Tier 4.1/4.3: split view + aliasing */
#include "mem_watch.h"    /* Tier 4.6: NPF watchpoints */
#include "mem_time.h"     /* Tier 4.2: O(1) snapshot/rollback */
#include "mem_cache.h"    /* Tier 4.7: cache behaviour control */
#include "mem_bp.h"       /* Tier 4.4: instruction-level access control */
#include "mem_frame.h"    /* Tier 4.5: frame ownership takeover */
#include "yghv_fail.h"     /* Tier 7.2/7.7: graceful degradation */
#include "yghv_crypto.h"   /* 9.342: SHA-256 */
#include "yghv_guard.h"    /* Tier 7.1/7.3: self-hiding + integrity */
#include "yghv_sentry.h"   /* Tier 7.4/7.5/7.6: timing/escape/surface */
#include "yghv_auth.h"     /* 9.371 (Tier 9.2): authenticated commands */
#include "debug.h"

/*
 * 9.368 (A4, Tier 5.4): the CPUID view policy lives in svm_simplevm206.cpp —
 * the module that owns the CPUID handler. PASSIVE setters, island-safe getters.
 */
extern int    yghv_s206_cpuid_view_set(UINT32 level);
extern UINT32 yghv_s206_cpuid_view_get(void);
extern void   yghv_s206_cpuid_view_stats(UINT64 *out);

/*
 * 9.372 (C1): the #BP exception intercept, opt-in. Default off, because an
 * exception intercept that was never measured is how the 205k #GP block produced
 * a 0xE2 (9.244). See the arming site in svm_simplevm206.cpp.
 */
extern int    yghv_s206_bp_int3_intercept(UINT32 on);
extern int    yghv_s206_bp_int3_intercept_get(void);
extern int    yghv_s206_bp_int3_apply_all(void);

/*
 * 9.368 (B1/B2, Tier 5.1): the guest's clock, controlled from the same place —
 * what the guest observes is one subject, and 7.6 already owns that subject.
 */
extern int    yghv_s206_tsc_offset_set(UINT64 offset);
extern UINT64 yghv_s206_tsc_offset_get(void);
extern int    yghv_s206_tsc_freeze(UINT32 on, UINT64 cr3, UINT32 max_ms);
extern void   yghv_s206_tsc_stats(UINT64 *out);
extern int    yghv_s206_tsc_armed(void);
extern void   yghv_s206_tsc_probe(UINT64 *out);
/* 9.384: the NPT page inventory, so a bugcheck's faulting address can be checked
 * arithmetically instead of guessed from a symbol. */
extern void     yghv_npt_pa_range(UINT64 *lo, UINT64 *hi);
extern int      yghv_npt_is_our_page(UINT64 pa);
extern UINT64   yghv_npt_alloc_count(void);
/* 9.390: the unload-progress page. Readable AFTER a fatal unload because it is
 * static, non-paged and never hidden. */
extern UINT64   yghv_ut_page_pa(void);
extern UINT32   yghv_ut_last_step(void);
extern UINT64   yghv_ut_last_value(void);

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
    case IOCTL_YGHV_MEM_READ:
    case IOCTL_YGHV_MEM_WRITE: {
        /* Phase 3 (2026-10-11): typed cross-process access via either engine.
         *
         * METHOD_BUFFERED shares ONE SystemBuffer, so the header is saved to
         * a local BEFORE any store — writing the reply would otherwise
         * clobber the request fields we still need (the bug hit in Phase 1).
         *
         * MEM_READ reply: 8-qword header, then the payload.
         * MEM_WRITE reply: 8-qword header only. */
        if (in_len >= sizeof(yghv_ioctl_mem_req_t) &&
            out_len >= YGHV_MEM_HDR_BYTES) {
            yghv_ioctl_mem_req_t *req = (yghv_ioctl_mem_req_t *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG  req_pid    = req->pid;
            ULONG  req_type   = req->type;
            ULONG  req_count  = req->count;
            ULONG  req_engine = req->engine;
            ULONG_PTR req_va  = req->va;
            PEPROCESS proc = NULL;
            NTSTATUS lst;
            SIZE_T want, done = 0;
            yghv_engine_t used = YGHV_ENGINE_AUTO;
            BOOLEAN is_read = (code == IOCTL_YGHV_MEM_READ);

            /* Payload size: fixed types derive it; strings/bytes take count. */
            if (req_type == YGHV_T_STR_ASCII || req_type == YGHV_T_STR_UTF8 ||
                req_type == YGHV_T_BYTES) {
                want = req_count;
            } else if (req_type == YGHV_T_STR_UTF16) {
                want = (SIZE_T)req_count * 2;
            } else {
                want = (SIZE_T)yghv_mem_type_size(req_type) * req_count;
            }
            if (want == 0 || want > YGHV_MEM_MAX_PAYLOAD) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            lst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)req_pid, &proc);
            if (!NT_SUCCESS(lst) || !proc) {
                out[0] = 0; out[1] = 0; out[2] = 0;
                out[3] = req_type; out[4] = req_count; out[5] = YGHV_MEM_HDR_BYTES;
                out[6] = 0; out[7] = 0;
                info = YGHV_MEM_HDR_BYTES;
                status = STATUS_SUCCESS;
                break;
            }

            /* Engine 0 means "use the current selection" — do NOT overwrite
             * it with AUTO, or a mem-engine <sel> set earlier in the session
             * would be undone by the very next read/write. Only an explicit
             * 1/2/3 in the request overrides for this call. */
            /* 9.326 FIX: the condition used to be `req_engine <= SELFMAP`,
             * which is true for 0 as well — so every read and write carrying
             * the default (0) called set(AUTO) and silently undid an earlier
             * `mem-engine pte`. That is why bp_add saw a non-PTE engine right
             * after the user selected one: a single baseline read reset it.
             * AUTO must mean "keep what is selected", exactly as the comment
             * above says. */
            if (req_engine != YGHV_ENGINE_AUTO &&
                req_engine <= YGHV_ENGINE_PTE_SELFMAP)
                yghv_mem_engine_set((yghv_engine_t)req_engine);

            if (is_read) {
                /* Read the payload into a stack staging area first: the reply
                 * header is written into the same buffer. */
                UCHAR stage[YGHV_MEM_MAX_PAYLOAD];
                if (want > sizeof(stage)) {
                    ObDereferenceObject(proc);
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                lst = yghv_mem_read(proc, (uint64_t)req_va, stage, want, &done, &used);
                out[0] = NT_SUCCESS(lst) ? 1 : 0;
                out[1] = (UINT64)used;
                out[2] = (UINT64)done;
                out[3] = req_type;
                out[4] = req_count;
                out[5] = YGHV_MEM_HDR_BYTES;
                out[6] = NT_SUCCESS(lst) ? 0 : (UINT64)lst;
                out[7] = 0;
                if (NT_SUCCESS(lst) && done) {
                    if (out_len >= YGHV_MEM_HDR_BYTES + done)
                        RtlCopyMemory((UCHAR *)buf + YGHV_MEM_HDR_BYTES, stage, done);
                    else
                        done = 0;
                }
                info = (ULONG)(YGHV_MEM_HDR_BYTES + done);
            } else {
                UCHAR stage[YGHV_MEM_MAX_PAYLOAD];
                if (want > sizeof(stage) || in_len < YGHV_MEM_HDR_BYTES + want) {
                    ObDereferenceObject(proc);
                    status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }
                RtlCopyMemory(stage, (UCHAR *)buf + YGHV_MEM_HDR_BYTES, want);
                lst = yghv_mem_write(proc, (uint64_t)req_va, stage, want, &done, &used);
                out[0] = NT_SUCCESS(lst) ? 1 : 0;
                out[1] = (UINT64)used;
                out[2] = (UINT64)done;
                out[3] = req_type;
                out[4] = req_count;
                out[5] = YGHV_MEM_HDR_BYTES;
                out[6] = NT_SUCCESS(lst) ? 0 : (UINT64)lst;
                out[7] = 0;
                info = YGHV_MEM_HDR_BYTES;
            }

            ObDereferenceObject(proc);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_MEM_ENGINE: {
        /* Phase 3: engine selection + diagnostics.
         * Zero-length input = reset stats; 4-byte input = set selection. */
        if (in_len == 0 && out_len >= YGHV_MEM_ENGINE_OUT_QWORDS * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_mem_stats_t ms;
            yghv_pa_stats_t ps;

            yghv_mem_stats_get(&ms);
            yghv_pa_stats_get(&ps);
            out[0]  = (UINT64)yghv_mem_engine_get();
            out[1]  = ms.api_reads;       out[2]  = ms.api_writes;
            out[3]  = ms.api_fail;
            out[4]  = ms.pte_reads;       out[5]  = ms.pte_writes;
            out[6]  = ms.pte_fail;
            out[7]  = ms.fallbacks;
            out[8]  = ms.auto_choices_api; out[9] = ms.auto_choices_pte;
            out[10] = ps.reads;           out[11] = ps.reads_failed;
            out[12] = ps.writes;          out[13] = ps.writes_failed;
            out[14] = ps.bytes_read;      out[15] = ps.map_failures;
            /* Engine B write-failure breakdown (added after a wrong mapping
             * API made every B write fall back silently). */
            out[16] = ms.pte_wfail_irql;
            out[17] = ms.pte_wfail_walk;
            out[18] = ms.pte_wfail_pa;
            info = YGHV_MEM_ENGINE_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= sizeof(ULONG) && in_len < sizeof(yghv_ioctl_mem_req_t)) {
            ULONG sel = *(ULONG *)buf;
            if (sel > YGHV_ENGINE_PTE_SELFMAP)
                sel = YGHV_ENGINE_AUTO;
            yghv_mem_engine_set((yghv_engine_t)sel);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len == 0) {
            yghv_mem_stats_reset();
            yghv_pa_stats_reset();
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_MEM_BENCH: {
        /*
         * 9.449 (Tier 1.5): Engine A vs Engine B on the same range.
         *
         * PASSIVE_LEVEL only: yghv_mem_bench allocates a page buffer and calls
         * MmCopyVirtualMemory, neither of which is legal at DISPATCH.
         */
        if (in_len >= sizeof(yghv_ioctl_bench_req_t) &&
            out_len >= YGHV_BENCH_OUT_QWORDS * sizeof(UINT64)) {
            yghv_ioctl_bench_req_t *req = (yghv_ioctl_bench_req_t *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG req_pid = req->pid;
            ULONG req_pat = req->pat_len;
            ULONG req_budget = req->page_budget;
            ULONG_PTR req_start = req->range_start;
            ULONG_PTR req_end = req->range_end;
            UCHAR pat[64], msk[64];
            PEPROCESS proc = NULL;
            NTSTATUS lst;
            yghv_bench_result_t bres;

            /* Copy the pattern out of the shared buffer BEFORE writing any
             * reply: METHOD_BUFFERED gives input and output the same memory, so
             * a reply store would overwrite the request (the note at the top of
             * this file's MEM_READ handler exists because this was learned once
             * already). */
            RtlCopyMemory(pat, req->pattern, 64);
            RtlCopyMemory(msk, req->mask, 64);

            if (req_pat == 0 || req_pat > 64) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            if (req_start >= req_end) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            lst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)req_pid, &proc);
            if (!NT_SUCCESS(lst) || proc == NULL) {
                status = STATUS_INVALID_CID;
                break;
            }

            if (!yghv_mem_bench(proc, (uint64_t)req_start, (uint64_t)req_end,
                                pat, msk, req_pat, (uint64_t)req_budget, &bres)) {
                ObDereferenceObject(proc);
                status = STATUS_UNSUCCESSFUL;
                break;
            }

            ObDereferenceObject(proc);

            out[0] = bres.a_cycles;
            out[1] = bres.a_reads;
            out[2] = bres.a_bytes;
            out[3] = bres.a_hits;
            out[4] = bres.b_cycles;
            out[5] = bres.b_reads;
            out[6] = bres.b_bytes;
            out[7] = bres.b_hits;
            out[8] = bres.pages_visited;
            out[9] = bres.speedup_x100;
            /*
             * 9.449: the CR3 the walk actually used.
             *
             * The first two runs reported `pages visited = 0` alongside a 26,594x
             * "speedup" - an artefact, because B had done no work and the timing
             * could not tell the difference between "fast" and "idle". Exposing the
             * CR3 lets a caller see WHY the walk found nothing: 0 means we could not
             * read the target's DTB at all.
             */
            out[10] = bres.walk_cr3;
            info = YGHV_BENCH_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    case IOCTL_YGHV_MEM_SCAN: {
        /* Phase 4 (2026-10-11): page-table driven signature scan. */
        if (in_len >= sizeof(yghv_ioctl_scan_req_t) &&
            out_len >= YGHV_SCAN_OUT_HDR_QWORDS * sizeof(UINT64)) {
            yghv_ioctl_scan_req_t *req = (yghv_ioctl_scan_req_t *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG req_pid = req->pid;
            ULONG req_pat = req->pat_len;
            ULONG req_max = req->max_results;
            ULONG req_flags = req->flags;
            ULONG_PTR req_start = req->range_start;
            ULONG_PTR req_end   = req->range_end;
            ULONG64 req_align   = req->align;
            UCHAR pat[64], msk[64];
            PEPROCESS proc = NULL;
            NTSTATUS lst;
            uint32_t max_va, found;
            uint64_t *hits = NULL;
            uint64_t pr = 0, psk = 0, bs = 0;

            if (req_pat == 0 || req_pat > 64) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            RtlCopyMemory(pat, req->pattern, 64);
            RtlCopyMemory(msk, req->mask, 64);

            /* How many VAs fit after the header? */
            max_va = (ULONG)((out_len - YGHV_SCAN_OUT_HDR_QWORDS * sizeof(UINT64)) /
                             sizeof(UINT64));
            if (req_max && req_max < max_va)
                max_va = req_max;
            if (max_va == 0) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            if (!req_start)
                req_start = (ULONG_PTR)YGHV_USER_VA_MIN;
            if (!req_end)
                req_end = (ULONG_PTR)YGHV_USER_VA_MAX;

            lst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)req_pid, &proc);
            if (!NT_SUCCESS(lst) || !proc) {
                out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0; out[4] = 0;
                info = YGHV_SCAN_OUT_HDR_QWORDS * sizeof(UINT64);
                status = STATUS_SUCCESS;
                break;
            }

            hits = (uint64_t *)ExAllocatePoolWithTag(
                NonPagedPoolNx, (SIZE_T)max_va * sizeof(uint64_t), 'TSHS');
            if (hits) {
                found = yghv_mem_scan(proc, (uint64_t)req_start, (uint64_t)req_end,
                                      pat, msk, req_pat, (uint64_t)req_align,
                                      req_flags, hits, max_va, &pr, &psk, &bs);
                out[0] = 1;
                out[1] = (UINT64)found;
                out[2] = pr;
                out[3] = psk;
                out[4] = bs;
                if (found) {
                    RtlCopyMemory(out + YGHV_SCAN_OUT_HDR_QWORDS, hits,
                                  (SIZE_T)found * sizeof(uint64_t));
                }
                info = (ULONG)(YGHV_SCAN_OUT_HDR_QWORDS * sizeof(UINT64) +
                               (SIZE_T)found * sizeof(uint64_t));
                ExFreePoolWithTag(hits, 'TSHS');
            } else {
                out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0; out[4] = 0;
                info = YGHV_SCAN_OUT_HDR_QWORDS * sizeof(UINT64);
            }
            ObDereferenceObject(proc);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_SELFMAP: {
        /* Phase 2b (2026-10-11): self-built physical mapping window.
         *   in_len == 0            -> read diagnostics (11 qwords)
         *   in_len >= 4 {ULONG op} -> 0 stop, 1 init, 2 reset stats
         *
         * init/stop are PASSIVE_LEVEL operations (reserve VA, lock MDL,
         * map) so they belong here rather than in the island. */
        if (in_len == 0 && out_len >= YGHV_SELFMAP_OUT_QWORDS * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_selfmap_stats_t ss;
            yghv_selfmap_stats_get(&ss);
            out[0]  = (UINT64)(yghv_selfmap_ready() ? 1 : 0);
            out[1]  = yghv_selfmap_window_va();
            out[2]  = ss.inits;
            out[3]  = ss.retargets;
            out[4]  = ss.reads;
            out[5]  = ss.writes;
            out[6]  = ss.failures;
            out[7]  = ss.tlb_flushes;
            out[8]  = (UINT64)yghv_mem_engine_get();
            out[9]  = 0;
            out[10] = 0;
            info = YGHV_SELFMAP_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= sizeof(ULONG)) {
            ULONG op = *(ULONG *)buf;
            if (op == 1) {
                status = yghv_selfmap_init() ? STATUS_SUCCESS
                                             : STATUS_UNSUCCESSFUL;
                break;
            }
            if (op == 0) {
                yghv_selfmap_stop();
                status = STATUS_SUCCESS;
                break;
            }
            if (op == 2) {
                yghv_selfmap_stats_reset();
                status = STATUS_SUCCESS;
                break;
            }
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_CACHE: {
        /* Tier 4.7: cache behaviour control.
         * Zero-length input = diagnostics.
         * 32-byte input {ULONG op, pad, ULONG_PTR a, b, c}:
         *   op 0 = set(gpa, mode)    op 1 = set_alt(gpa, mode)
         *   op 2 = reset(gpa) */
        if (in_len == 0 && out_len >= 6 * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_cache_stats_t cs;
            yghv_cache_stats_get(&cs);
            out[0] = 1;
            out[1] = 0;
            out[2] = cs.tracked;
            out[3] = cs.sets;
            out[4] = cs.resets;
            out[5] = cs.failures;
            info = 6 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = (ULONG)in[0];
            ULONG_PTR a = (ULONG_PTR)in[1];
            ULONG_PTR b = (ULONG_PTR)in[2];
            int ok = 0;

            switch (op) {
            case 0: ok = yghv_cache_set((uint64_t)a, (uint32_t)b); break;
            case 1: ok = yghv_cache_set_alt((uint64_t)a, (uint32_t)b); break;
            case 2: ok = yghv_cache_reset((uint64_t)a); break;
            case 3: {
                /* 9.329: read the NPT entry back and report the real bits.
                 * "The call returned success" is not evidence that PWT/PCD
                 * landed in the table — that was the only proof Tier 4.7 had. */
                int pwt = 0, pcd = 0;
                uint64_t raw = 0;
                ok = yghv_cache_verify((uint64_t)a, &pwt, &pcd, &raw);
                out[0] = (UINT64)(ok ? 1 : 0);
                out[1] = (UINT64)pwt;
                out[2] = (UINT64)pcd;
                out[3] = raw;
                info = 32;
                status = STATUS_SUCCESS;
                break;
            }
            default: ok = 0; break;
            }
            if (op != 3) {
                out[0] = (UINT64)(ok ? 1 : 0);
                info = 8;
            }
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_SENTRY: {
        /* Tier 7.4/7.5/7.6: what is being attempted against us, how slow our
         * exits are, and what we currently expose. */
        UINT64 *out = (UINT64 *)buf;
        ULONG op = 0;
        if (in_len >= 32)
            op = (ULONG)((UINT64 *)buf)[0];

        if (op == 1) {
            yghv_sentry_surface_entry_t se[YGHV_SENTRY_MAX_SURFACE];
            uint32_t n, i;
            n = yghv_sentry_surface_list(se, YGHV_SENTRY_MAX_SURFACE);
            out[0] = n;
            for (i = 0; i < n && (1 + (i + 1) * 3) <= (out_len / sizeof(UINT64)); i++) {
                UINT64 *e = &out[1 + i * 3];
                RtlCopyMemory(&e[0], se[i].name, 16);
                e[2] = (se[i].is_ioctl ? 1ULL : 0ULL) | ((UINT64)se[i].count << 8);
            }
            info = (ULONG)(8 + (SIZE_T)n * 3 * sizeof(UINT64));
            if (info > out_len)
                info = out_len;
            status = STATUS_SUCCESS;
            break;
        }
        if (op == 2) {
            yghv_sentry_reset();
            out[0] = 1;
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }
        /*
         * 9.368 (A4, Tier 5.4): the CPUID view lives here because it is the same
         * subject 7.6 already owns — "what does the guest get to observe". It is
         * a policy over the attack surface, not a new mechanism.
         *
         *   op 3  read  -> out[0]=level out[1]=how many times it was set
         *                        out[2]=how many CPUID answers it actually ALTERED
         *   op 4  write <- in[1]=level (0 honest / 1 no-vendor-bit / 2 off)
         *
         * `altered` is the one field that cannot be faked by returning a constant:
         * a policy that never fires leaves it at 0 while the level reads back as
         * whatever was set.
         */
        if (op == 3 && out_len >= 3 * sizeof(UINT64)) {
            UINT64 st[3];
            yghv_s206_cpuid_view_stats(st);
            out[0] = st[0];
            out[1] = st[1];
            out[2] = st[2];
            info = 3 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (op == 4 && in_len >= 32) {
            UINT64 lvl = ((UINT64 *)buf)[1];
            out[0] = (UINT64)(INT64)yghv_s206_cpuid_view_set((UINT32)lvl);
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }
        /*
         * 9.368 (B1/B2, Tier 5.1): the guest's clock.
         *
         *   op 5  read -> out[0]=offset out[1]=mode(0 normal / 1 frozen)
         *                 out[2]=frozen_at out[3]=rdtsc exits handled
         *                 out[4]=answers changed out[5]=intercept bit armed
         *                 out[6]=real rdtsc now
         *   op 6  set offset <- in[1]
         *   op 7  freeze    <- in[1] (1 freeze / 0 thaw)
         *
         * `intercept bit armed` is the field that separates the two mechanisms:
         * an OFFSET needs no intercept (hardware applies it), a FREEZE cannot work
         * without one. So (mode=1, armed=0) is a policy that will not take effect,
         * and this readout says so instead of leaving it to be discovered.
         */
        if (op == 5 && out_len >= 17 * sizeof(UINT64)) {
            UINT64 st[17];
            yghv_s206_tsc_stats(st);
            out[0]  = st[0];  out[1]  = st[1];  out[2]  = st[2];
            out[3]  = st[3];  out[4]  = st[4];  out[5]  = st[5];
            out[6]  = st[6];  out[7]  = st[7];  out[8]  = st[8];
            out[9]  = st[9];  out[10] = st[10]; out[11] = st[11];
            out[12] = st[12]; out[13] = st[13]; out[14] = st[14];
            out[15] = st[15]; out[16] = st[16];
            info = 17 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (op == 6 && in_len >= 32) {
            UINT64 v = ((UINT64 *)buf)[1];
            out[0] = (UINT64)(INT64)yghv_s206_tsc_offset_set(v);
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }
        /* 9.372 (C1): the #BP intercept flag, in the same IOCTL as the rest of
         * what the guest can observe.
         *   op 9  read  -> out[0]=intercept on/off
         *   op 10 write <- in[1]=1 on / 0 off
         */
        if (op == 9 && out_len >= sizeof(UINT64)) {
            out[0] = (UINT64)yghv_s206_bp_int3_intercept_get();
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }
        if (op == 10 && in_len >= 32) {
            UINT64 onv = ((UINT64 *)buf)[1];
            int ok = yghv_s206_bp_int3_intercept((UINT32)(onv ? 1 : 0));
            int walked = 0;
            /* 9.372: setting the flag is not enough - the VMCB bit is per-core
             * state and prepare runs once at load. Walk the cores and write it. */
            if (ok)
                walked = yghv_s206_bp_int3_apply_all();
            out[0] = (UINT64)((ok && walked) ? 1 : 0);
            out[1] = (UINT64)walked;
            info = 16;
            status = STATUS_SUCCESS;
            break;
        }
        if (op == 8 && out_len >= 3 * sizeof(UINT64)) {
            /* 9.370c: walk every core and VALIDATE the VPD pointer, arming nothing.
             * The previous attempt at this walk faulted at +0x36 because the
             * handshake returned a physical address that the caller dereferenced. */
            UINT64 pr[3];
            yghv_s206_tsc_probe(pr);
            out[0] = pr[0]; out[1] = pr[1]; out[2] = pr[2];
            info = 3 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (op == 7 && in_len >= 32) {
            /* 9.369: freeze now takes a SCOPE and a DEADLINE.
             *   in[1] = on (1/0)  in[2] = cr3 to freeze (0 = refused)
             *   in[3] = max_ms     (clamped to [1,10000])
             * A global freeze is refused: it stopped the OS and hung the machine
             * with no dump, and refusing the shape costs less than remembering. */
            UINT64 on    = ((UINT64 *)buf)[1];
            UINT64 cr3   = ((UINT64 *)buf)[2];
            UINT64 maxms = ((UINT64 *)buf)[3];
            out[0] = (UINT64)(INT64)yghv_s206_tsc_freeze((UINT32)(on ? 1 : 0),
                                                         cr3, (UINT32)maxms);
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }
        if (out_len >= YGHV_SENTRY_OUT_QWORDS * sizeof(UINT64)) {
            yghv_sentry_escape_stats_t es;
            yghv_sentry_timing_t tt;
            int i;
            yghv_sentry_escape_stats_get(&es);
            yghv_sentry_timing_get(&tt);
            for (i = 0; i < 6; i++)
                out[i] = es.escapes[i];
            out[6] = es.escapes_total;
            out[7] = es.last_kind;
            out[8] = es.last_detail;
            out[9] = es.last_a;
            out[10] = es.last_b;
            out[11] = tt.exits;
            out[12] = tt.worst_cycles;
            out[13] = tt.total_cycles;
            info = YGHV_SENTRY_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_PA: {
        /* 9.338: physical frame read/write. PASSIVE (pa_write uses an MDL). */
        if (in_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = (ULONG)in[0];
            uint64_t pa = (uint64_t)in[1];

            /*
             * 9.384, op 5: THE NPT PAGE RANGE.
             *
             * Fifteen restarts, three of them landing on two ADJACENT pages that
             * looked like NPT tables. "Looks like" is not a measurement, so this
             * reports the physical addresses the driver actually allocated for NPT
             * - recorded at the single allocation site - and lets a faulting address
             * be checked by arithmetic instead of guessed from a symbol table.
             *
             * out[0] = count, out[1] = lowest PA, out[2] = highest PA
             */
            if (op == 5) {
                uint64_t lo = 0, hi = 0;
                yghv_npt_pa_range(&lo, &hi);
                out[0] = yghv_npt_alloc_count();
                out[1] = lo;
                out[2] = hi;
                info = 24;
                status = STATUS_SUCCESS;
                break;
            }
            /*
             * 9.390, op 7: the unload-progress page address and its last stamp.
             *
             * out[0] = page PA, out[1] = last step, out[2] = step's value.
             *
             * This exists so the record can be confirmed working on a LIVE machine:
             * if this reads 0 here, no dump will show anything either, and finding
             * that out by crashing would be a wasted restart.
             */
            if (op == 7) {
                out[0] = yghv_ut_page_pa();
                out[1] = (UINT64)yghv_ut_last_step();
                out[2] = yghv_ut_last_value();
                info = 24;
                status = STATUS_SUCCESS;
                break;
            }
            /* op 6: is this address one of ours? in[1] = the faulting address. */
            if (op == 6) {
                out[0] = (UINT64)(yghv_npt_is_our_page(pa) ? 1 : 0);
                info = 8;
                status = STATUS_SUCCESS;
                break;
            }
            if (op == 0) {
                uint32_t len = (uint32_t)in[2];
                uint32_t off = (uint32_t)in[3];
                uint32_t want;
                if (len == 0 || len > 256)
                    len = 256;
                want = len;
                if ((SIZE_T)(8 + want) > out_len)
                    want = (uint32_t)(out_len - 8);
                if (yghv_pa_read(pa + off, (void *)(out + 1), want)) {
                    out[0] = (UINT64)want;
                    info = (ULONG)(8 + want);
                    status = STATUS_SUCCESS;
                } else {
                    out[0] = 0;
                    info = 8;
                    status = STATUS_UNSUCCESSFUL;
                }
                break;
            }
            if (op == 1) {
                uint64_t v = (uint64_t)in[2];
                out[0] = (UINT64)(yghv_pa_write(pa, &v, sizeof(v)) ? 1 : 0);
                info = 8;
                status = STATUS_SUCCESS;
                break;
            }
            if (op == 2) {
                uint32_t len = (uint32_t)in[2];
                if (len > 0 && len <= 4096 && in_len >= (ULONG)(24 + len)) {
                    out[0] = (UINT64)(yghv_pa_write(pa, (uint8_t *)&in[3], len) ? 1 : 0);
                } else {
                    out[0] = 0;
                }
                info = 8;
                status = STATUS_SUCCESS;
                break;
            }
            out[0] = 0;
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_EXITMASK: {
        /*
         * 9.448: exit-coverage control and measurement.
         *
         * The 206 handler now covers 35 exit codes. Which of them are ARMED
         * decides which can ever fire, and until this IOCTL there was no way to
         * change that set or to read back what fired. See the definition in
         * control_ioctl.h for the wire layout.
         */
        extern void  yghv_s206_set_extra_intercepts(UINT32 m1, UINT32 m2);
        extern void  yghv_s206_get_extra_intercepts(UINT32 *m1, UINT32 *m2);
        extern LONG  yghv_s206_apply_extra_intercepts(void);
        extern LONG  yghv_s206_exit_codes_observed(void);
        extern LONG64 yghv_s206_exits_count(void);
        extern UINT32 yghv_s206_preset_quiet_m1(void);
        extern UINT32 yghv_s206_preset_quiet_m2(void);
        extern UINT32 yghv_s206_preset_full_m1(void);
        extern UINT32 yghv_s206_preset_full_m2(void);
        extern UINT32 yghv_s206_preset_events_m1(void);
        extern UINT32 yghv_s206_preset_events_m2(void);
        extern UINT32 yghv_s206_preset_host_m1(void);
        extern UINT32 yghv_s206_preset_host_m2(void);

        ULONG op = 0;
        ULONG m1 = 0, m2 = 0;
        LONG applied = -1;   /* -1 = no set/clear happened this call */

        if (in_len >= 16) {
            UINT64 *in = (UINT64 *)buf;
            op = (ULONG)in[0];
            m1 = (ULONG)in[1];
            m2 = (ULONG)in[2];
        }

        switch (op) {
        case 1:   /* set explicit mask */
            yghv_s206_set_extra_intercepts(m1, m2);
            break;
        case 2:   /* clear back to the default set */
            yghv_s206_set_extra_intercepts(0, 0);
            break;
        case 3:   /* quiet preset: HLT + PAUSE */
            yghv_s206_set_extra_intercepts(yghv_s206_preset_quiet_m1(),
                                           yghv_s206_preset_quiet_m2());
            break;
        case 4:   /* full preset: every bit a GUEST can execute */
            yghv_s206_set_extra_intercepts(yghv_s206_preset_full_m1(),
                                           yghv_s206_preset_full_m2());
            break;
        case 5:   /* events preset: hardware event re-delivery. EXPERIMENTAL. */
            yghv_s206_set_extra_intercepts(yghv_s206_preset_events_m1(),
                                           yghv_s206_preset_events_m2());
            break;
        case 6:   /* host-insn preset: traps our own VMSAVE/STGI. Self-inflicted. */
            yghv_s206_set_extra_intercepts(yghv_s206_preset_host_m1(),
                                           yghv_s206_preset_host_m2());
            break;
        case 0:
        default:
            break;   /* read back only */
        }

        if (op >= 1 && op <= 6) {
            applied = yghv_s206_apply_extra_intercepts();
        }

        if (out_len >= YGHV_EXITMASK_OUT_QWORDS * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            UINT32 rm1 = 0, rm2 = 0;
            yghv_s206_get_extra_intercepts(&rm1, &rm2);
            out[0] = (UINT64)rm1;
            out[1] = (UINT64)rm2;
            out[2] = (UINT64)(INT64)yghv_s206_exit_codes_observed();
            out[3] = (UINT64)yghv_s206_exits_count();
            /*
             * 9.448b: HOW MANY LIVE VMCBs THE CHANGE REACHED.
             *
             * -1 means this call was a read-back and changed nothing. 0 means a
             * change was requested but no core is virtualized, so it is only in
             * the statics for the next prepare. A positive number is the count of
             * VMCB control areas rewritten. Reporting it is the difference between
             * "the mask took effect" and "the mask was accepted" - the first
             * version of this command reported the latter and called it the former.
             */
            out[4] = (UINT64)(INT64)applied;
            info = 5 * sizeof(UINT64);
            status = STATUS_SUCCESS;
        } else {
            /* The set/clear half-succeeded; the read-back did not fit. Reporting
             * STATUS_BUFFER_TOO_SMALL keeps a caller from believing it read a
             * mask it never received. */
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    case IOCTL_YGHV_GUARD: {
        /* Tier 7.1/7.3: which pages are hidden from the guest, and whether
         * any of them changed since they were armed. */
        if (in_len == 0 && out_len >= 8 * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_guard_stats_t gs;
            yghv_guard_stats_get(&gs);
            out[0] = gs.guarded;
            out[1] = gs.hidden;
            out[2] = gs.verified;
            out[3] = gs.changed;
            out[4] = gs.probes;
            out[5] = gs.hide_fail;
            out[6] = gs.hash_fail;
            /* 9.343: the reserved slot carries the SHA-256 self-test outcome.
             * A hash that is silently wrong makes every "unchanged" answer a
             * lie, so a reader can check it here instead of assuming it. */
            out[7] = (UINT64)yghv_crypto_selftest_cached();
            info = 8 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = (ULONG)in[0];
            ULONG_PTR a = (ULONG_PTR)in[1];
            ULONG_PTR b = (ULONG_PTR)in[2];
            ULONG_PTR c = (ULONG_PTR)in[3];

            switch (op) {
            case 0: {
                char tag[YGHV_GUARD_TAG_LEN];
                int i;
                for (i = 0; i < YGHV_GUARD_TAG_LEN - 1; i++) {
                    char ch = (char)((in_len >= 64 && (ULONG)(4 + i/8) < (in_len/8))
                                     ? ((char *)&in[4 + i/8])[i % 8] : 0);
                    tag[i] = ch;
                    if (!ch) break;
                }
                tag[YGHV_GUARD_TAG_LEN - 1] = 0;
                out[0] = (UINT64)(INT64)yghv_guard_add((uint64_t)a, tag,
                                                       (int)b, (int)c);
                info = 8;
                break;
            }
            case 1:
                out[0] = (UINT64)(yghv_guard_remove((uint64_t)a) ? 1 : 0);
                info = 8;
                break;
            case 2:
                out[0] = (UINT64)yghv_guard_verify_all();
                info = 8;
                break;
            case 3: {
                yghv_guard_info_t gi[8];
                uint32_t n, i;
                n = yghv_guard_list(gi, 8);
                out[0] = (UINT64)n;
                /* 9.342: 10 qwords per entry — gpa, checks, changes, hidden,
                 * armed, hash_off|hash_len, digest[4], tag[2]. */
                for (i = 0; i < n && (1 + (i + 1) * 10) <= (out_len / sizeof(UINT64)); i++) {
                    UINT64 *e = &out[1 + i * 10];
                    e[0] = gi[i].gpa;
                    e[1] = gi[i].checks;
                    e[2] = gi[i].changes;
                    e[3] = gi[i].hidden;
                    e[4] = ((UINT64)gi[i].hash_off << 32) | (UINT64)gi[i].hash_len;
                    RtlCopyMemory(&e[5], gi[i].digest, 32);
                    RtlCopyMemory(&e[9], gi[i].tag, 8);
                }
                info = (ULONG)(8 + (SIZE_T)n * 10 * sizeof(UINT64));
                break;
            }
            case 4:
                out[0] = (UINT64)yghv_guard_arm_core();
                info = 8;
                break;
            case 5: {
                /* 9.343 #3: pages in [a, a+b) with NO guard entry. A change to
                 * one of those is invisible, so "we watch everything we care
                 * about" becomes a measured statement. */
                uint32_t n;
                n = yghv_guard_uncovered((uint64_t)a, (uint32_t)b,
                                         (uint64_t *)&out[1],
                                         (out_len > 8)
                                             ? (uint32_t)((out_len - 8) / sizeof(UINT64))
                                             : 0);
                out[0] = (UINT64)n;
                info = (ULONG)(8 + (SIZE_T)((n + 1) > ((out_len - 8) / sizeof(UINT64))
                                            ? ((out_len - 8) / sizeof(UINT64))
                                            : (n + 1)) * sizeof(UINT64));
                if (info > out_len)
                    info = out_len;
                break;
            }
            case 6: {
                /* 9.343 #1: hide the Tier-4 tables for real, now that our own
                 * access is served by the RIP-whitelist ALLOW loop. */
                uint32_t hid = 0;
                hid = yghv_guard_hide_core();
                out[0] = (UINT64)hid;
                info = 8;
                break;
            }
            case 7: {
                /*
                 * 9.366: ASK THE REFUSAL WHY, FOR ONE PAGE.
                 *
                 * `hide_fail=3` + `hash_fail=1` says "a page was skipped as guard
                 * state, and the SPAN test matched" — but not WHICH page, and not
                 * whether the answer is even right. Measured cost of not saying:
                 * the frame table's page was refused while the other two tables
                 * hid, and the state counters alone could not distinguish "the
                 * span genuinely covers it" from "the span comparison is wrong".
                 *
                 * This reports the three predicates a hide decision is built from,
                 * for any page, without performing a hide. Read-only; PASSIVE.
                 *   out[0] = state_why bitmask (0 = not guard state)
                 *   out[1] = target_page (1 = it is one of the three tables)
                 *   out[2] = guard slot for this page (-1 = none)
                 *   out[3] = hidden flag for that slot
                 *   out[4] = hide_mode for that slot
                 */
                extern uint32_t yghv_guard_state_why(uint64_t gpa);
                extern int yghv_guard_target_page_probe(uint64_t gpa);
                extern int yghv_guard_find_probe(uint64_t gpa);
                extern int yghv_guard_hidden_probe(uint64_t gpa);
                extern int yghv_guard_hide_mode(uint64_t gpa);
                out[0] = (UINT64)yghv_guard_state_why((uint64_t)a);
                out[1] = (UINT64)yghv_guard_target_page_probe((uint64_t)a);
                out[2] = (UINT64)(INT64)yghv_guard_find_probe((uint64_t)a);
                out[3] = (UINT64)(INT64)yghv_guard_hidden_probe((uint64_t)a);
                out[4] = (UINT64)(INT64)yghv_guard_hide_mode((uint64_t)a);
                info = 5 * sizeof(UINT64);
                break;
            }
            default:
                out[0] = 0;
                info = 8;
                break;
            }
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_FAIL: {
        /* Tier 7.2/7.7: how many abnormal conditions we survived vs. died on. */
        if (out_len >= 8 * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_fail_stats_t fs;
            yghv_fail_stats_get(&fs);
            out[0] = fs.total;
            out[1] = fs.degraded;
            out[2] = fs.fatal;
            out[3] = fs.last_reason;
            out[4] = fs.last_a;
            out[5] = fs.last_b;
            out[6] = fs.last_c;
            out[7] = 0;
            info = 8 * sizeof(UINT64);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    case IOCTL_YGHV_FRAME: {
        /* Tier 4.5: frame ownership takeover (retained copy survives release). */
        if (in_len == 0 && out_len >= 8 * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_frame_stats_t fs;
            yghv_frame_stats_get(&fs);
            out[0] = 1;
            out[1] = 0;
            out[2] = fs.active;
            out[3] = fs.reserved;
            out[4] = fs.released;
            out[5] = fs.bytes;
            out[6] = fs.failures;
            out[7] = 0;
            info = 8 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = (ULONG)in[0];
            ULONG_PTR a = (ULONG_PTR)in[1];
            ULONG_PTR b = (ULONG_PTR)in[2];
            int rc = 0;

            switch (op) {
            case 0:
                rc = yghv_frame_reserve((uint64_t)a, (uint32_t)b);
                out[0] = (UINT64)(INT64)rc;   /* slot, or -1 */
                info = 8;
                break;
            case 1:
                rc = yghv_frame_release((int)a);
                out[0] = (UINT64)(rc ? 1 : 0);
                info = 8;
                break;
            case 2: {
                /* read(slot, off) — the payload goes into out[1..], with the
                 * byte count in out[0]. Works after the owner is gone. */
                uint32_t want = (uint32_t)((out_len - 8) / 1);
                uint32_t got;
                if (want > 4096)
                    want = 4096;
                got = yghv_frame_read((int)a, (void *)(out + 1), want,
                                      (uint32_t)b);
                out[0] = (UINT64)got;
                info = (ULONG)(8 + got);
                break;
            }
            case 3: {
                yghv_frame_info_t fi[8];
                uint32_t n, i;
                n = yghv_frame_list(fi, 8);
                out[0] = (UINT64)n;
                for (i = 0; i < n && (i + 1) * 4 + 1 <= (out_len / sizeof(UINT64)); i++) {
                    out[1 + i*4 + 0] = fi[i].gpa;
                    out[1 + i*4 + 1] = fi[i].seq;
                    out[1 + i*4 + 2] = fi[i].len;
                    out[1 + i*4 + 3] = fi[i].captured;
                }
                info = (ULONG)(8 + (SIZE_T)n * 4 * sizeof(UINT64));
                break;
            }
            default:
                out[0] = 0;
                info = 8;
                break;
            }
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_BP: {

        /* Tier 4.4: instruction-level access control.
         * Zero-length input = diagnostics.
         * 32-byte input {ULONG op, pad, ULONG_PTR a(gpa), b(rip), c(kinds)}:
         *   op 0 = add   op 1 = remove   op 2 = drain(a = max qwords) */
        if (in_len == 0 && out_len >= 16 * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_bp_stats_t bs;
            yghv_bp_stats_get(&bs);
            out[0] = 1;
            out[1] = 0;
            out[2] = bs.count;
            out[3] = bs.hits;
            out[4] = bs.recorded;
            out[5] = bs.refusals;
            out[6] = bs.npf;      /* 9.327: mode breakdown */
            out[7] = bs.int3;
            out[8] = bs.dr;
            out[9] = bs.missed;
            /* 9.327: what the driver last received in bp_add_ex — compared
             * against the PowerShell-side print to catch offset mistakes. */
            out[10] = g_dbg_args[0];
            out[11] = g_dbg_args[1];
            out[12] = g_dbg_args[2];
            out[13] = g_dbg_args[3];
            /* 9.373: leaf flips to/from the shadow frame. This is the observable
             * that replaces "the byte changed" for INT3 mode: with a shadow the
             * guest page keeps 0xCC for the breakpoint's whole life. */
            out[14] = bs.shadow_switches;
            out[15] = bs.fail_shadow;
            info = 16 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = (ULONG)in[0];
            ULONG_PTR a = (ULONG_PTR)in[1];
            ULONG_PTR b = (ULONG_PTR)in[2];
            ULONG_PTR c = (ULONG_PTR)in[3];
            int ok = 0;

            switch (op) {
            case 0:
                /* 9.327: pass the stage code through (a negative value names
                 * the guard that refused). PowerShell prints it.
                 * `mode` 0 means NPF, the previously-implied default; a
                 * 48-byte request additionally carries (va, mode). */
                {
                    uint32_t mode = (in_len >= 48) ? (uint32_t)in[5] : 0;
                    if (mode == 0)
                        mode = YGHV_BP_MODE_NPF;
                    ok = yghv_bp_add_ex((uint64_t)a,
                                        (in_len >= 48) ? (uint64_t)in[4] : 0,
                                        (uint64_t)b, (uint32_t)c, mode);
                }
                out[0] = (UINT64)(INT64)ok;
                info = 8;
                break;
            case 1:
                ok = yghv_bp_remove((uint64_t)a, (uint64_t)b);
                out[0] = (UINT64)(ok ? 1 : 0);
                info = 8;
                break;
            case 3:
                /* 9.327: clear every breakpoint of one mode (a = mode, 0 = all).
                 * Returns how many were removed. */
                {
                    int ncleared = yghv_bp_clear_mode((uint32_t)a);
                    out[0] = (UINT64)(ncleared > 0 ? (UINT64)ncleared : 0);
                }
                info = 8;
                break;
            case 2: {
                uint32_t maxq, got;
                uint64_t *tmp;
                if (out_len < 8) {
                    ok = 0;
                    break;
                }
                maxq = (uint32_t)((out_len - 8) / sizeof(UINT64));
                if (a && a < maxq)
                    maxq = (uint32_t)a;
                if (maxq == 0)
                    maxq = 1;
                tmp = (uint64_t *)ExAllocatePoolWithTag(
                    NonPagedPoolNx, (SIZE_T)maxq * sizeof(uint64_t), 'BPDR');
                if (tmp) {
                    got = yghv_bp_drain(tmp, maxq);
                    out[0] = (UINT64)got;
                    if (got)
                        RtlCopyMemory(out + 1, tmp, (SIZE_T)got * sizeof(UINT64));
                    info = (ULONG)(8 + (SIZE_T)got * sizeof(UINT64));
                    ExFreePoolWithTag(tmp, 'BPDR');
                } else {
                    out[0] = 0;
                    info = 8;
                }
                ok = 1;
                break;
            }
            default:
                out[0] = 0;
                info = 8;
                break;
            }
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_WATCH: {

        /* Tier 4.6: NPF watchpoints. op 2 drains the event ring into the
         * output buffer (header: [0]=count, [1]=dropped). */
        if (in_len == 0 && out_len >= 6 * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_watch_stats_t ws;
            yghv_watch_stats_get(&ws);
            out[0] = ws.pages;
            out[1] = ws.total_hits;
            out[2] = ws.recorded;
            out[3] = ws.dropped;
            out[4] = ws.seq;
            out[5] = 0;
            info = 6 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = (ULONG)in[0];
            ULONG_PTR a = (ULONG_PTR)in[1];
            ULONG_PTR b = (ULONG_PTR)in[2];
            int ok = 0;

            switch (op) {
            case 0: ok = yghv_watch_add((uint64_t)a, (uint32_t)b); break;
            case 1: ok = yghv_watch_remove((uint64_t)a); break;
            case 2: {
                uint32_t maxev, got;
                yghv_watch_event_t *ev;
                if (out_len <= YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64)) {
                    ok = 0;
                    break;
                }
                maxev = (uint32_t)((out_len - YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64)) /
                                   (4 * sizeof(UINT64)));
                if (a && a < maxev)
                    maxev = (uint32_t)a;
                ev = (yghv_watch_event_t *)ExAllocatePoolWithTag(
                    NonPagedPoolNx, (SIZE_T)maxev * sizeof(*ev), 'WCHG');
                if (ev) {
                    got = yghv_watch_drain(ev, maxev);
                    out[0] = (UINT64)got;
                    out[1] = 0;
                    if (got) {
                        UINT64 *dst = out + YGHV_WATCH_OUT_HDR_QWORDS;
                        uint32_t i;
                        for (i = 0; i < got; i++) {
                            dst[i*4+0] = ev[i].cr3;
                            dst[i*4+1] = ev[i].rip;
                            dst[i*4+2] = ev[i].gpa;
                            dst[i*4+3] = (UINT64)ev[i].kind |
                                         ((UINT64)ev[i].cpl << 8);
                        }
                        info = (ULONG)(YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64) +
                                       (SIZE_T)got * 4 * sizeof(UINT64));
                    } else {
                        info = YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64);
                    }
                    ExFreePoolWithTag(ev, 'WCHG');
                    status = STATUS_SUCCESS;
                    break;
                }
                ok = 0;
                break;
            }
            case 3: yghv_watch_stats_reset(); ok = 1; break;
            case 4: {
                /*
                 * 9.368 (A3, Tier 5.6): op 4 = NON-DESTRUCTIVE trace snapshot.
                 *
                 * op 2 drains: it advances the ring tail, so the events are gone
                 * afterwards. That is a queue read. A trace needs to be re-read:
                 * look at a different slice of the same window, save it twice,
                 * size a buffer from the real depth first.
                 *
                 * Same output layout as op 2 (so a client can parse either with
                 * one function), different semantics:
                 *   op 2  consumes; yghv_watch_dropped stops climbing while you drain
                 *   op 4  copies;   yghv_watch_dropped keeps climbing if you only
                 *                   ever snapshot, because the ring fills up
                 *
                 * out[0] = events copied, out[1] = depth BEFORE the copy (so a
                 * truncation is visible: copied < depth means the output buffer
                 * was too small, not that the ring was nearly empty).
                 */
                uint32_t maxev, got, depth;
                yghv_watch_event_t *ev;

                if (out_len <= YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64)) {
                    ok = 0;
                    break;
                }
                maxev = (uint32_t)((out_len - YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64)) /
                                   (4 * sizeof(UINT64)));
                if (a && a < maxev)
                    maxev = (uint32_t)a;
                depth = yghv_watch_depth();
                ev = (yghv_watch_event_t *)ExAllocatePoolWithTag(
                    NonPagedPoolNx, (SIZE_T)maxev * sizeof(*ev), 'WCHG');
                if (!ev) {
                    ok = 0;
                    break;
                }
                got = yghv_watch_snapshot(ev, maxev);
                out[0] = (UINT64)got;
                out[1] = (UINT64)depth;
                if (got) {
                    UINT64 *dst = out + YGHV_WATCH_OUT_HDR_QWORDS;
                    uint32_t i;
                    for (i = 0; i < got; i++) {
                        dst[i*4+0] = ev[i].cr3;
                        dst[i*4+1] = ev[i].rip;
                        dst[i*4+2] = ev[i].gpa;
                        dst[i*4+3] = (UINT64)ev[i].kind |
                                     ((UINT64)ev[i].cpl << 8);
                    }
                    info = (ULONG)(YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64) +
                                   (SIZE_T)got * 4 * sizeof(UINT64));
                } else {
                    info = YGHV_WATCH_OUT_HDR_QWORDS * sizeof(UINT64);
                }
                ExFreePoolWithTag(ev, 'WCHG');
                status = STATUS_SUCCESS;
                break;
            }
            default: ok = 0; break;
            }
            if (op != 2 && op != 4) {
                out[0] = (UINT64)(ok ? 1 : 0);
                info = 8;
            }
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_TIME: {
        /* Tier 4.2: O(1) snapshot / rollback.
         * Zero-length input = diagnostics (same convention as the others). */
        if (in_len == 0 && out_len >= YGHV_TIME_OUT_QWORDS * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_time_stats_t ts;
            yghv_time_stats_get(&ts);
            out[0] = 1;
            out[1] = 0;
            out[2] = ts.pages;
            out[3] = ts.snapshots;
            out[4] = ts.switches;
            out[5] = ts.failures;
            info = YGHV_TIME_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = (ULONG)in[0];
            ULONG_PTR a = (ULONG_PTR)in[1];
            ULONG_PTR b = (ULONG_PTR)in[2];
            int ok = 0;
            int cur = -1;

            switch (op) {
            case 0: ok = yghv_time_track((uint64_t)a); break;
            case 1: ok = yghv_time_untrack((uint64_t)a); break;
            case 2: cur = yghv_time_snapshot((uint64_t)a); ok = (cur >= 0); break;
            case 3: ok = yghv_time_goto((uint64_t)a, (uint32_t)b); break;
            case 4: cur = yghv_time_current((uint64_t)a); ok = (cur >= 0); break;
            default: ok = 0; break;
            }
            {
                yghv_time_stats_t ts;
                yghv_time_stats_get(&ts);
                out[0] = (UINT64)(ok ? 1 : 0);
                out[1] = (UINT64)cur;
                out[2] = ts.pages;
                out[3] = ts.snapshots;
                out[4] = ts.switches;
                out[5] = ts.failures;
            }
            info = YGHV_TIME_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_VIEW: {

        /* Tier 4.1/4.3: split-view + cross-process aliasing.
         * Every op is PASSIVE (they call npt remap, which needs it). */
        if (in_len == 0 && out_len >= YGHV_VIEW_OUT_QWORDS * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_view_stats_t vs;
            yghv_view_stats_get(&vs);
            out[0] = (UINT64)(yghv_view_ready() ? 1 : 0);
            out[1] = yghv_view_main_pa();
            out[2] = yghv_view_alt_pa();
            out[3] = vs.pages;
            out[4] = vs.observers;
            out[5] = vs.switches_to_alt;
            out[6] = vs.switches_to_main;
            out[7] = vs.failures;
            info = YGHV_VIEW_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= sizeof(yghv_ioctl_view_t)) {
            yghv_ioctl_view_t *req = (yghv_ioctl_view_t *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG op = req->op;
            ULONG_PTR a = req->a, b = req->b, c = req->c;
            int ok = 0;

            switch (op) {
            case 0: ok = yghv_view_init(); break;
            case 1: yghv_view_stop(); ok = 1; break;
            case 2: ok = yghv_view_add((uint64_t)a, (uint64_t)b, (uint64_t)c); break;
            case 3: ok = yghv_view_remove((uint64_t)a); break;
            case 4: ok = yghv_view_add_observer((uint64_t)a); break;
            case 5: ok = yghv_view_remove_observer((uint64_t)a); break;
            case 6: out[0] = yghv_view_pick_ncr3((uint64_t)a); ok = 1; break;
            default: ok = 0; break;
            }
            if (op != 6)
                out[0] = (UINT64)(ok ? 1 : 0);
            info = 8 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_NPT_REMAP: {
        /* Tier 4 prerequisite: rewrite one NPT entry to a different frame.
         *
         * Safe by construction: an NPT entry is hardware metadata owned by
         * this driver (the tables come from npt_alloc_table), not a guest
         * PTE — so this does NOT touch the PFN reference counts that 9.307
         * corrupted. The open question is only whether the CPU observes the
         * change, which is the TLB problem 9.297/9.298 measured for
         * permission flips. */
        if (in_len >= sizeof(yghv_ioctl_npt_remap_t) &&
            out_len >= 3 * sizeof(UINT64)) {
            yghv_ioctl_npt_remap_t *req = (yghv_ioctl_npt_remap_t *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG_PTR gpa = req->gpa;
            ULONG_PTR spa = req->spa;
            ULONG mode = req->mode;
            uint64_t prev = 0;
            int ok;

            ok = yghv_protect_npt_remap((uint64_t)gpa, (uint64_t)spa,
                                        (mode == 1) ? 1 : 0, &prev);
            out[0] = (UINT64)(ok ? 1 : 0);
            out[1] = prev;
            out[2] = (UINT64)spa;
            info = 3 * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_ISLAND: {
        /* Phase 6 (2026-10-11): island-context access via the direct map.
         *   in_len == 0                        -> 8 qwords of diagnostics
         *   in_len >= 32 {va_a,pa_a,va_b,pa_b} -> discover + validate base
         *   in_len >= 16 {pa,len}              -> island read probe */
        if (in_len == 0 && out_len >= YGHV_ISLAND_OUT_QWORDS * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_island_stats_t is;
            yghv_island_stats_get(&is);
            out[0] = (UINT64)(yghv_island_ready() ? 1 : 0);
            out[1] = is.base;
            out[2] = is.reads;
            out[3] = is.writes;
            out[4] = is.refusals;
            out[5] = is.probes;
            out[6] = 0;
            out[7] = 0;
            info = YGHV_ISLAND_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= 4 * sizeof(UINT64)) {
            UINT64 *in = (UINT64 *)buf;
            int ok = yghv_island_init(in[0], in[1], in[2], in[3]);
            info = 0;
            status = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
            break;
        }
        if (in_len >= 2 * sizeof(UINT64) && out_len >= 32) {
            UINT64 *in = (UINT64 *)buf;
            UINT64 pa = in[0];
            UINT64 len = in[1];
            UINT64 *out = (UINT64 *)buf;
            UCHAR tmp[32];
            SIZE_T n = (SIZE_T)(len > sizeof(tmp) ? sizeof(tmp) : len);
            int ok;
            if (n == 0)
                n = 8;
            ok = yghv_island_read(pa, tmp, n);
            out[0] = (UINT64)(ok ? 1 : 0);
            out[1] = (UINT64)n;
            if (ok)
                RtlCopyMemory((UCHAR *)buf + 16, tmp, n);
            info = (ULONG)(16 + (ok ? n : 0));
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_MEM_SCAN_REV: {
        /* Phase 4b (2026-10-11): reverse scan — every VA mapping a frame. */
        if (in_len >= sizeof(yghv_ioctl_scan_rev_t) &&
            out_len >= YGHV_SCAN_REV_OUT_HDR_QWORDS * sizeof(UINT64)) {
            yghv_ioctl_scan_rev_t *req = (yghv_ioctl_scan_rev_t *)buf;
            UINT64 *out = (UINT64 *)buf;
            ULONG  req_pid = req->pid;
            ULONG  req_max = req->max_results;
            ULONG_PTR req_pa = req->target_pa;
            ULONG_PTR req_s = req->range_start;
            ULONG_PTR req_e = req->range_end;
            PEPROCESS proc = NULL;
            NTSTATUS lst;
            uint32_t max_va, found;
            uint64_t *hits;
            uint64_t visited = 0;

            max_va = (ULONG)((out_len - YGHV_SCAN_REV_OUT_HDR_QWORDS * sizeof(UINT64)) /
                             sizeof(UINT64));
            if (req_max && req_max < max_va)
                max_va = req_max;
            if (max_va == 0) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            lst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)req_pid, &proc);
            if (!NT_SUCCESS(lst) || !proc) {
                out[0] = 0; out[1] = 0; out[2] = 0;
                info = YGHV_SCAN_REV_OUT_HDR_QWORDS * sizeof(UINT64);
                status = STATUS_SUCCESS;
                break;
            }

            hits = (uint64_t *)ExAllocatePoolWithTag(
                NonPagedPoolNx, (SIZE_T)max_va * sizeof(uint64_t), 'TSRV');
            if (hits) {
                found = yghv_mem_scan_reverse(proc, (uint64_t)req_pa,
                                              (uint64_t)req_s, (uint64_t)req_e,
                                              hits, max_va, &visited);
                out[0] = 1;
                out[1] = (UINT64)found;
                out[2] = visited;
                if (found)
                    RtlCopyMemory(out + YGHV_SCAN_REV_OUT_HDR_QWORDS, hits,
                                  (SIZE_T)found * sizeof(uint64_t));
                info = (ULONG)(YGHV_SCAN_REV_OUT_HDR_QWORDS * sizeof(UINT64) +
                               (SIZE_T)found * sizeof(uint64_t));
                ExFreePoolWithTag(hits, 'TSRV');
            } else {
                out[0] = 0; out[1] = 0; out[2] = 0;
                info = YGHV_SCAN_REV_OUT_HDR_QWORDS * sizeof(UINT64);
            }
            ObDereferenceObject(proc);
            status = STATUS_SUCCESS;
            break;
        }
        status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_YGHV_MEM_COOP: {
        /* Phase 7 (2026-10-11): guard cooperation diagnostics.
         *   in_len == 0            -> read 5 qwords
         *   in_len >= 4 {ULONG op} -> 2 = reset stats */
        if (in_len == 0 && out_len >= YGHV_COOP_OUT_QWORDS * sizeof(UINT64)) {
            UINT64 *out = (UINT64 *)buf;
            yghv_coop_stats_t cs;
            yghv_coop_stats_get(&cs);
            out[0] = cs.notifies;
            out[1] = cs.guarded_hits;
            out[2] = cs.unguarded;
            out[3] = cs.rollbacks_avoided;
            out[4] = 0;
            info = YGHV_COOP_OUT_QWORDS * sizeof(UINT64);
            status = STATUS_SUCCESS;
            break;
        }
        if (in_len >= sizeof(ULONG)) {
            ULONG op = *(ULONG *)buf;
            if (op == 2) {
                yghv_coop_stats_reset();
                status = STATUS_SUCCESS;
                break;
            }
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    case IOCTL_YGHV_AUTH: {
        /*
         * 9.371 (Tier 9.2): the control channel's own front door.
         *
         * NOTE ON ORDERING, because it is a security decision and not a
         * convenience: this handler is reached only after
         * yghv_control_ioctl_authorized() has already checked owner process,
         * owner CR3 and SeDebugPrivilege. So authentication is a SECOND gate, not
         * a replacement - an ACL and a key fail differently (a handle can be
         * duplicated; a key cannot be guessed), and the feature is worth having
         * only because both are in the path.
         */
        UINT64 *out = (UINT64 *)buf;
        ULONG op = 0;

        if (in_len >= 32)
            op = (ULONG)((UINT64 *)buf)[0];

        if (op == 0) {
            if (out_len >= YGHV_AUTH_OUT_QWORDS * sizeof(UINT64)) {
                yghv_auth_stats_t as;
                yghv_auth_stats_get(&as);
                out[0] = as.mode;
                out[1] = as.key_len;
                out[2] = as.accepted;
                out[3] = as.rejected;
                out[4] = as.would_reject;
                out[5] = as.bad_seq;
                out[6] = as.no_tag;
                out[7] = as.last_seq;
                out[8] = as.selftest;
                out[9] = 0;
                info = YGHV_AUTH_OUT_QWORDS * sizeof(UINT64);
                status = STATUS_SUCCESS;
                break;
            }
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (op == 1 && in_len >= 32) {
            UINT64 klen = ((UINT64 *)buf)[1];
            const uint8_t *kbytes = (const uint8_t *)&((UINT64 *)buf)[4];
            int ok = 0;
            if (klen <= YGHV_AUTH_KEY_MAX && in_len >= 32 + (ULONG)klen)
                ok = yghv_auth_set_key(klen ? kbytes : NULL, (uint32_t)klen);
            out[0] = (UINT64)(ok ? 1 : 0);
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }

        if (op == 2 && in_len >= 32) {
            UINT64 m = ((UINT64 *)buf)[1];
            out[0] = (UINT64)(yghv_auth_set_mode((uint32_t)m) ? 1 : 0);
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }

        if (op == 3) {
            yghv_auth_reset_stats();
            out[0] = 1;
            info = 8;
            status = STATUS_SUCCESS;
            break;
        }

        if (op == 4 && in_len >= 32) {
            /*
             * Sign a command on the caller's behalf.
             *
             * WHY THE DRIVER SIGNS AT ALL: the signed layout is (seq, code, len,
             * args) in a fixed byte order. If the client re-derived that layout,
             * a mismatch would surface as "bad tag" - the same error a wrong key
             * produces - and the real cause would be invisible. Signing here
             * means there is one layout, in one file, and the client cannot drift
             * from it.
             *
             * `code` comes from the request, not from the IOCTL being executed:
             * the client is asking "what tag WOULD authenticate command N", which
             * is normally a different command than this one.
             */
            UINT64 seq = ((UINT64 *)buf)[1];
            UINT64 alen_raw = ((UINT64 *)buf)[2];
            UINT64 code_arg = ((UINT64 *)buf)[3];
            const uint8_t *args = (const uint8_t *)&((UINT64 *)buf)[4];
            uint8_t tag[YGHV_AUTH_TAG];
            uint32_t alen;
            (void)tag;
            if (alen_raw > 0xFFFF) {
                out[0] = 0;
                info = 8;
                status = STATUS_SUCCESS;
                break;
            }
            alen = (uint32_t)alen_raw;
            if (alen && in_len < 32 + alen) {
                out[0] = 0;
                info = 8;
                status = STATUS_SUCCESS;
                break;
            }
            yghv_auth_sign(seq, (uint32_t)code_arg, alen ? args : NULL, alen, tag);
            out[0] = 1;
            RtlCopyMemory(&out[1], tag, YGHV_AUTH_TAG);
            info = (ULONG)(8 + YGHV_AUTH_TAG);
            status = STATUS_SUCCESS;
            break;
        }

        status = STATUS_INVALID_PARAMETER;
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    /*
     * 9.366 (Tier 7.6, wiring): the IOCTL half of the attack-surface inventory.
     *
     * Recorded HERE rather than from a list, because a list drifts: the switch
     * above is the only thing that decides which codes this device serves, so a
     * name added to an inventory by hand would eventually describe a device that
     * no longer exists. A code that reaches the end of the switch with success
     * has just PROVED it is accepted, and that proof is the entry.
     *
     * Function number, not the raw CTL code: `0x5947`-elsewhere bits are the
     * device type and access, identical for all of them, so carrying them would
     * make every name unreadably long for no information. (code >> 2) & 0xFFF is
     * CTL_CODE's Function field — 0x800..0x828 in this driver.
     *
     * Cost: one interlocked increment on a name lookup, on an IOCTL the caller
     * already waited a synchronous trip for. The `hits` column it produces is
     * what makes the list describe use as well as reach.
     */
    if (NT_SUCCESS(status))
        (void)yghv_sentry_surface_add_ioctl((code >> 2) & 0xFFFu);

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

#ifdef YGHV_NO_DEVICE
    /*
     * 9.375 (Tier 9.1): BUILD WITHOUT THE DEVICE, ON PURPOSE.
     *
     * Tier 9.1 claims the hypervisor can be commanded with no object at all - no
     * \.\YuanGuardHV, no symbol link, no IOCTL. That claim cannot be tested on a
     * build that still creates the device: every doorbell answer could have come
     * from the IOCTL path, and the test would pass while testing nothing.
     *
     * So the device is compiled OUT under YGHV_NO_DEVICE. In that configuration
     * the ONLY way to reach the driver is:
     *   - the CPUID channel (Tier 9.1), and
     *   - the loader, which installs it.
     *
     * The default build keeps the device: this tree has 42 IOCTL codes and a
     * 13/13 regression suite that all go through it, so removing it by default
     * would trade a working control plane for a claim. The point of the switch is
     * that the claim becomes MEASURABLE, not that it becomes the default.
     */
    UNREFERENCED_PARAMETER(dev_name);
    UNREFERENCED_PARAMETER(link_name);
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(i);
    LOG_INFO("control device: NOT CREATED (YGHV_NO_DEVICE build) - channel only");
    return STATUS_SUCCESS;
#else
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
#endif /* YGHV_NO_DEVICE */
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
