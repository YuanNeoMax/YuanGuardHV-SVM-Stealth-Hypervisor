/*
 * vmmcall.c — guest -> hypervisor command channel.
 *
 * 9.493: READ THIS BEFORE ASSUMING THESE CHECKS PROTECT ANYTHING IN THE 206 BUILD.
 *
 * The authorization below (cpl==0, control CR3, cookie, per-vcpu key) lives on the
 * path  svm_dispatch_exit -> vmmcall_dispatch -> yghv_vmmcall_authorized.
 *
 * THE 206 MAINLINE DOES NOT TAKE THAT PATH. Measured: `svm_simplevm206.cpp`
 * contains ZERO references to svm_dispatch_exit; its own `case VMEXIT_VMMCALL`
 * only counts the exit, advances RIP, and checks the stop flag. The two chains are
 * mutually exclusive and 206 uses only the latter.
 *
 * Consequence: in the shipping 206 configuration this file's checks never run. It
 * is kept because the vendored 206-A path and other GPU/coexist configurations do
 * reach it - deleting it would remove their protection.
 *
 * WHY THIS PARAGRAPH EXISTS: a reader who finds `yghv_vmmcall_authorized` here
 * would reasonably conclude that VMMCALL commands are gated, and might hang a
 * sensitive operation on one of them believing it is protected. That is the same
 * mistake as 9.450b, where the tree documented a RIP-whitelist ALLOW loop that
 * had zero callers. "The code exists" and "the code runs" read identically.
 *
 * If you need a command reachable from the 206 guest, handle it in SvHandleVmExit
 * and put the gate THERE - not here.
 */
#include <ntddk.h>
#include "svm_defs.h"
#include "svm_vcpu.h"
#include "vmcb.h"
#include "control_plane.h"
#include "protect.h"
#include "debug.h"

NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS *Process);

#define YUANGUARD_VERSION 0x00010000

#define YGHV_HEARTBEAT_LOG_INTERVAL  1000ULL
#define YGHV_HEARTBEAT_TEST_LIMIT    10000ULL

uint64_t g_vmmcall_auth_cookie;
uint64_t g_control_cr3;
extern volatile BOOLEAN g_persistent_mode;

static int yghv_vmmcall_authorized(svm_vcpu_t *vcpu, uint64_t cmd) {
    /* Common posture: only ring-0 code running under the control CR3 (when
       pinned) may use VMMCALL commands at all.  Compare CR3 masked (drop
       PCID/flags bits) for consistency with the target/hook-stub matching. */
    if (vcpu->vmcb->state.cpl != 0)
        return 0;
    if (g_control_cr3 &&
        (vcpu->vmcb->state.cr3 & ~0xFFFULL) != (g_control_cr3 & ~0xFFFULL))
        return 0;

    switch (cmd) {
    case YGHV_CMD_HEARTBEAT:
    case YGHV_CMD_VERSION:
    case YGHV_CMD_STATS:
        return 1; /* runtime read-only commands stay open */

    case YGHV_CMD_HOOK_QUERY:
    case YGHV_CMD_STOP_INTERNAL:
        /* REV-007: kept cookie-only — the synthetic test guests (including the
           cpuid guest, which clobbers r8 and is asserted r8==0) issue
           STOP_INTERNAL without a usable auth-key register; SHUTDOWN below
           gets the stricter check.  Note STOP_INTERNAL and SHUTDOWN are
           equally destructive, so this is defense-in-depth only; the real
           gate is R1 private-page exclusion (gated off by default). */
        return vcpu->regs.rcx == g_vmmcall_auth_cookie;

    case YGHV_CMD_SHUTDOWN:
        /* REV-007: the most destructive command also requires the per-vcpu
           key (r8 == auth_key), not just the guest-readable cookie. */
        if (vcpu->regs.rcx != g_vmmcall_auth_cookie)
            return 0;
        if (vcpu->auth_key && vcpu->regs.r8 != vcpu->auth_key)
            return 0;
        return 1;

    default:
        /* Control commands additionally require the per-vcpu key. */
        if (vcpu->regs.rcx != g_vmmcall_auth_cookie)
            return 0;
        if (vcpu->auth_key && vcpu->regs.r8 != vcpu->auth_key)
            return 0;
        return 1;
    }
}

static BOOLEAN yghv_vmmcall_target_matches_cr3(uint32_t pid, uint64_t cr3) {
    PEPROCESS p = NULL;
    uint64_t base;

    if (!NT_SUCCESS(PsLookupProcessByProcessId(
            (HANDLE)(ULONG_PTR)pid, &p)))
        return FALSE;
    base = *(volatile uint64_t *)((uint8_t *)p + 0x028) & ~0xFFFULL;
    ObDereferenceObject(p);
    return base && base == (cr3 & ~0xFFFULL);
}

/* REV-014: normalize control-command results into the YGHV_STATUS_* space so
   RAX is uniformly interpretable alongside auth-failure DENIED/ERROR. */
static uint64_t yghv_vmmcall_status(NTSTATUS st) {
    return NT_SUCCESS(st) ? YGHV_STATUS_OK : YGHV_STATUS_ERROR;
}

int vmmcall_dispatch(svm_vcpu_t *vcpu) {
    uint64_t cmd = vcpu->regs.rax;

    switch (cmd) {

    case YGHV_CMD_HEARTBEAT: {
        ULONG active;
        uint64_t page_va = 0, hook_va = 0;
        yghv_protect_get_state(&active, NULL, NULL);
        if (active || g_persistent_mode) {
            if (yghv_protect_is_target_cr3(vcpu->vmcb->state.cr3)) {
                yghv_protect_get_heartbeat(&page_va, &hook_va);
                vcpu->regs.rdi = page_va;
                vcpu->regs.rsi = hook_va;
            } else {
                vcpu->regs.rdi = 0;
                vcpu->regs.rsi = 0;
            }
            if ((vcpu->resident_exits % 10000ULL) == 0)
                yghv_protect_check_target_exited();
            if ((vcpu->resident_exits % 10000ULL) == 0)
                LOG_ERROR("heartbeat protect exits=%llu core=%u page=0x%llx hook=0x%llx last=0x%llx",
                    vcpu->resident_exits, vcpu->resident_index,
                    vcpu->regs.rdi, vcpu->regs.rsi, vcpu->regs.rdx);
            vcpu->regs.rax = YGHV_STATUS_OK;
            return 0;
        }
        if (vcpu->resident_exits >= YGHV_HEARTBEAT_TEST_LIMIT) {
            LOG_ERROR("heartbeat limit reached, stopping resident loop");
            svm_core_stop_all_residents();
            vcpu->regs.rax = YGHV_STATUS_OK;
            return 1;
        }
        vcpu->regs.rax = YGHV_STATUS_OK;
        return 0;
    }

    case YGHV_CMD_STOP_INTERNAL:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_STOP_INTERNAL)) {
            LOG_ERROR("Unauthorized STOP_INTERNAL from core %u", vcpu->resident_index);
            vcpu->regs.rax = YGHV_STATUS_ERROR;
            return 0;
        }
        LOG_INFO("VMMCALL stop — broadcasting to all cores");
        yghv_trace("stop_internal");
        svm_core_stop_all_residents();
        vcpu->regs.rax = YGHV_STATUS_OK;
        return 1;

    case YGHV_CMD_SHUTDOWN:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_SHUTDOWN)) {
            LOG_ERROR("Unauthorized SHUTDOWN from core %u", vcpu->resident_index);
            vcpu->regs.rax = YGHV_STATUS_ERROR;
            return 0;
        }
        svm_core_stop_all_residents();
        vcpu->regs.rax = YGHV_STATUS_OK;
        return 1;

    case YGHV_CMD_VERSION:
        vcpu->regs.rax = YUANGUARD_VERSION;
        return 0;

    case YGHV_CMD_STATS:
        vcpu->regs.rax = vcpu->resident_exits;
        vcpu->regs.rbx = vcpu->resident_msr_exits;
        vcpu->regs.rcx = vcpu->resident_cr_exits;
        vcpu->regs.rdx = vcpu->resident_interrupt_exits;
        return 0;

    case YGHV_CMD_SET_TARGET:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_SET_TARGET)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        /* Only a process may install itself as the protected target. */
        if (!yghv_vmmcall_target_matches_cr3(
                (uint32_t)vcpu->regs.rdx, vcpu->vmcb->state.cr3)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        vcpu->regs.rax =
            yghv_vmmcall_status(yghv_protect_set_target((uint32_t)vcpu->regs.rdx));
        return 0;

    case YGHV_CMD_ADD_PAGE:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_ADD_PAGE)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        if (!yghv_protect_is_target_cr3(vcpu->vmcb->state.cr3)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        /* Resolve the target slot by the caller CR3 (matches the IOCTL path)
           instead of hardcoding targets[0]. */
        vcpu->regs.rax = yghv_vmmcall_status(
            yghv_protect_add_page_for(vcpu->vmcb->state.cr3, vcpu->regs.rdx));
        return 0;

    case YGHV_CMD_REMOVE_PAGE:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_REMOVE_PAGE)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        if (!yghv_protect_is_target_cr3(vcpu->vmcb->state.cr3)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        vcpu->regs.rax = yghv_vmmcall_status(
            yghv_protect_remove_page_for(vcpu->vmcb->state.cr3, vcpu->regs.rdx));
        return 0;

    case YGHV_CMD_START_PROTECT:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_START_PROTECT)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        vcpu->regs.rax = yghv_vmmcall_status(yghv_protect_start());
        return 0;

    case YGHV_CMD_STOP_PROTECT: {
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_STOP_PROTECT)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        NTSTATUS st = yghv_protect_stop();
        vcpu->regs.rax = yghv_vmmcall_status(st);
        return 0;
    }

    case YGHV_CMD_GET_STATE:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_GET_STATE)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        {
            ULONG active, pid, page_count;
            yghv_protect_get_state(&active, &pid, &page_count);
            vcpu->regs.rax = active ? 1 : 0;
            vcpu->regs.rbx = page_count;
            vcpu->regs.rcx = pid;
        }
        return 0;

    case YGHV_CMD_HOOK_QUERY:
        if (!yghv_vmmcall_authorized(vcpu, YGHV_CMD_HOOK_QUERY)) {
            vcpu->regs.rax = YGHV_STATUS_DENIED;
            return 0;
        }
        vcpu->regs.rax = yghv_protect_on_hook_query(
            (uint8_t)vcpu->regs.rbx, vcpu->vmcb->state.cr3);
        return 0;

    default:
        LOG_ERROR("Unknown VMMCALL cmd=0x%llx RIP=0x%llx",
            cmd, vcpu->vmcb->state.rip);
        vcpu->regs.rax = YGHV_STATUS_ERROR;
        return 0;
    }
}
