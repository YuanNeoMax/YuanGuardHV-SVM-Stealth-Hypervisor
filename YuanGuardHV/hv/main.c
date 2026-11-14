#include <ntddk.h>
#include <ntstrsafe.h>
#include "svm_defs.h"
#include "svm_vcpu.h"
#include "npt.h"
#include "control_plane.h"
#include "control_device.h"
#include "protect.h"
#include "debug.h"
#include "yghv_unload_trace.h"   /* 9.390: unload progress record */
#include "common/yghv_nt.h"     /* 9.589: native offsets + accessors */
#include "common/yghv_sig.h"    /* 9.589: signature / accessor scan */
#include "common/yghv_proc.h"   /* 9.591: process / thread control */
#include "common/yghv_inject.h"
#include "common/yghv_win.h"   /* 9.594: window capture suppression */ /* 9.592: injection primitives */

#define YGHV_R1_SKIP_NPT_TEST 0
#define YGHV_R1_NPT_UNIT_TEST 1
#define YGHV_RESIDENT_WORKLOAD_TEST 1
#ifndef YGHV_BAREMETAL_NO_RESIDENT
#define YGHV_BAREMETAL_NO_RESIDENT 0
#endif
#ifndef YGHV_BAREMETAL_STEP
#define YGHV_BAREMETAL_STEP 0
#endif
#ifndef YGHV_R1_EXCLUDE_PRIVATE
#define YGHV_R1_EXCLUDE_PRIVATE 0
#endif
#ifndef YGHV_REAL_HOOK_TEST
#define YGHV_REAL_HOOK_TEST 0
#endif
#ifndef YGHV_HOOK_RENDEZVOUS_TEST
#define YGHV_HOOK_RENDEZVOUS_TEST 0
#endif
#ifndef YGHV_LOADER_STEALTH
#define YGHV_LOADER_STEALTH 0
#endif
#ifndef YGHV_UNLOAD_GUARD
#define YGHV_UNLOAD_GUARD 0
#endif

NTKERNELAPI NTSTATUS ZwFlushBuffersFile(HANDLE FileHandle,
                                        PIO_STATUS_BLOCK IoStatusBlock);
NTSTATUS yghv_loader_stealth(PDRIVER_OBJECT DriverObject,
                             PUNICODE_STRING RegistryPath);
/* 9.177: clone the host page table into a fresh independent tree (defined later
   in this TU) and return its PML4 PA for use as a dedicated guest CR3. */
static uint64_t yghv_clone_host_cr3(void);
static npt_entry_t *yghv_guest_pt_alloc(void);

npt_mgr_t g_npt;
uint64_t g_npt_test_pa;
volatile int g_npt_test_active;
volatile BOOLEAN g_persistent_mode = FALSE;
void *g_guest_code_page = NULL;
void *g_resident_workload_page = NULL;
HANDLE g_hook_rendezvous_thread = NULL;
volatile BOOLEAN g_watchdog_stop = FALSE;
HANDLE g_watchdog_thread = NULL;
uint64_t g_guest_hb_va = 0;
uint64_t g_guest_npt_va = 0;
HANDLE g_trace_file = NULL;
volatile LONG g_os_guest_test_active = 0;
volatile LONG g_os_guest_counter = 0;
volatile ULONG64 g_os_guest_cpuid_acc = 0;
volatile BOOLEAN g_os_resident_mode = FALSE;
volatile BOOLEAN g_os_guest_intr_intercept = FALSE;
volatile BOOLEAN g_os_guest_host_isr = FALSE;
volatile BOOLEAN g_os_guest_inject_intr = FALSE;
volatile BOOLEAN g_os_guest_avic_irr_inject = FALSE;
volatile BOOLEAN g_os_guest_avic_eoi_only = FALSE;
volatile BOOLEAN g_os_guest_avic_timer_emu = FALSE;
void *g_avic_eoi_apic_va = NULL;
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} yghv_idtr_desc_t;
yghv_idtr_desc_t g_avic_eoi_idtr;
yghv_idtr_desc_t g_avic_old_idtr;
volatile BOOLEAN g_os_guest_delay_quiet = FALSE;
/* 9.159: OS-as-guest ASID/TLB hygiene switch.  Default ON.  When set, every
   OS-as-guest vcpu gets a per-core unique guest ASID (host keeps ASID 0) and
   requests FLUSH_BY_ASID on each VMRUN, testing whether the guest/host TLB
   aliasing behind the 9.152 shared-CR3 freeze can be avoided for OS-as-guest
   (which cannot use a dedicated minimal guest CR3). */
volatile BOOLEAN g_os_guest_tlb_hygiene = TRUE;
/* 9.172: FLUSH_ALL variant of TLB hygiene.  When TRUE (and tlb_hygiene TRUE),
   every OS-as-guest VMRUN requests a FULL TLB flush (tlb_control=2) instead of
   FLUSH_BY_ASID (1).  Rationale: the 9.152 shared-CR3 freeze was judged to be
   guest TLB (guest ASID) coexisting with host TLB (ASID 0) for the same
   VA->PA; FLUSH_BY_ASID only clears guest entries, leaving host ASID 0 entries
   behind.  FLUSH_ALL clears the whole TLB (host included) before the guest
   runs, so guest TLB never coexists with host entries — the last untested
   TLB-level variable for the OS-as-guest resident freeze. */
volatile BOOLEAN g_os_guest_tlb_flush_all = TRUE;
/* 9.177: dedicated guest CR3 root for OS-as-guest.  When TRUE, each OS-as-guest
   vcpu gets a new PML4 (copy of the host PML4's 512 entries) so the guest runs
   under its own CR3 root value, avoiding the 9.152 shared-CR3 TLB aliasing
   freeze.  Lower page tables are shared with the host on purpose (the guest IS
   Windows; guest PTE updates must stay visible to the host). */
volatile BOOLEAN g_os_guest_clone_cr3 = TRUE;
/* 9.180 MSV: minimal shadow validation mode.  When TRUE (YGHV_BAREMETAL_STEP=200),
   the OS-as-guest blocking delay thread runs on a DEEP-copied fully independent
   page table (yghv_clone_host_cr3_deep), CR3 writes fail-close (stop the guest
   cleanly instead of letting it fall back to shared host tables), and helper
   System threads force same-process scheduler switching on the guest core —
   the combination "independent CR3 + real context switch" never tested before. */
volatile BOOLEAN g_msv_test = FALSE;
volatile BOOLEAN g_msv_helper_stop = FALSE;
volatile BOOLEAN g_msv_cr3_seen = FALSE;
volatile ULONG64 g_msv_cr3_writes = 0;
static uint64_t yghv_clone_host_cr3(void) {
    npt_entry_t *src_pml4 = NULL, *dst_pml4 = NULL;
    uint64_t src_pa, dst_pml4_pa = 0;

    __asm__ volatile("mov %%cr3, %0" : "=r"(src_pa));  /* host CR3 = PA of PML4 */

    src_pml4 = (npt_entry_t *)MmGetVirtualForPhysical(
        (PHYSICAL_ADDRESS){ .QuadPart = src_pa });
    if (!src_pml4)
        return 0;
    dst_pml4 = yghv_guest_pt_alloc();
    if (!dst_pml4)
        return 0;
    dst_pml4_pa = MmGetPhysicalAddress(dst_pml4).QuadPart;

    /* Copy all 512 PML4 entries verbatim: each entry already holds the PA of
       the next-level table plus attributes, so sharing the lower tables is
       correct for OS-as-guest (guest and host are the same Windows).  Only the
       CR3 root differs, which is what gives the guest its own TLB tag. */
    RtlCopyMemory(dst_pml4, src_pml4, 512 * sizeof(npt_entry_t));
    yghv_trace_u64("os guest cloned cr3", dst_pml4_pa);
    return dst_pml4_pa;
}

/* 9.180 MSV: page-table page allocator for the deep clone.  Uses -1
   HighestAcceptableAddress so the tree may live above 4 GB (per 9.140). */
extern volatile BOOLEAN g_b0_cloning;   /* D4: defined later; set during deep clone */
static npt_entry_t *yghv_msv_pt_alloc(void) {
    npt_entry_t *t = (npt_entry_t *)MmAllocateContiguousMemory(
        HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = -1LL });
    if (t)
        RtlZeroMemory(t, HV_PAGE_SIZE);
    return t;
}

static npt_entry_t *yghv_msv_pa_to_va(uint64_t pa) {
    return (npt_entry_t *)MmGetVirtualForPhysical(
        (PHYSICAL_ADDRESS){ .QuadPart = pa });
}

/* 9.180 MSV: deep-copy the WHOLE host page table tree (PML4/PDPT/PD/PT pages
   all private; leaf mappings still point at the same physical pages, since the
   guest IS the same Windows).  This gives OS-as-guest a guest CR3 that never
   aliases the host, and — unlike the shallow 9.177/9.178 clone — no lower-level
   page-table page is shared, so the guest never walks a host-owned page table.
   Invalid-PA protection (skip any present entry whose next-level page cannot be
   resolved) fixes the 9.177 deep-copy 0x7E crash.  Returns new PML4 PA, or 0. */
static uint64_t yghv_clone_host_cr3_deep(void) {
    npt_entry_t *src_pml4, *dst_pml4;
    uint64_t src_pa, dst_pml4_pa = 0;
    ULONG i;

    /* D4: pause the 10ms diag's file I/O while we walk the LIVE host page
       table, so its allocations don't churn the tables we are copying (the
       D1c 0x50 clone-vs-I/O race). */
    g_b0_cloning = TRUE;

    __asm__ volatile("mov %%cr3, %0" : "=r"(src_pa));  /* host CR3 = PML4 PA */
    src_pml4 = yghv_msv_pa_to_va(src_pa);
    if (!src_pml4) {
        g_b0_cloning = FALSE;
        return 0;
    }
    dst_pml4 = yghv_msv_pt_alloc();
    if (!dst_pml4) {
        g_b0_cloning = FALSE;
        return 0;
    }
    dst_pml4_pa = MmGetPhysicalAddress(dst_pml4).QuadPart;

    for (i = 0; i < 512; i++) {
        uint64_t se = src_pml4[i].all;
        npt_entry_t *src_pdpt, *dst_pdpt;
        ULONG j;

        if (!(se & 1))                     /* not present */
            continue;
        src_pdpt = yghv_msv_pa_to_va(se & 0x000FFFFFFFFFF000ULL);
        if (!src_pdpt)                     /* invalid PA: skip */
            continue;
        dst_pdpt = yghv_msv_pt_alloc();
        if (!dst_pdpt)
            goto fail;
        dst_pml4[i].all = MmGetPhysicalAddress(dst_pdpt).QuadPart | 0x3;

        for (j = 0; j < 512; j++) {
            uint64_t se2 = src_pdpt[j].all;
            npt_entry_t *src_pd, *dst_pd;
            ULONG k;

            if (!(se2 & 1))
                continue;
            if (se2 & (1ULL << 7)) {       /* 1GB large page: copy verbatim */
                dst_pdpt[j].all = se2;
                continue;
            }
            src_pd = yghv_msv_pa_to_va(se2 & 0x000FFFFFFFFFF000ULL);
            if (!src_pd)
                continue;
            dst_pd = yghv_msv_pt_alloc();
            if (!dst_pd)
                goto fail;
            dst_pdpt[j].all = MmGetPhysicalAddress(dst_pd).QuadPart | 0x3;

            for (k = 0; k < 512; k++) {
                uint64_t se3 = src_pd[k].all;
                npt_entry_t *src_pt, *dst_pt;

                if (!(se3 & 1))
                    continue;
                if (se3 & (1ULL << 7)) {   /* 2MB large page: copy verbatim */
                    dst_pd[k].all = se3;
                    continue;
                }
                src_pt = yghv_msv_pa_to_va(se3 & 0x000FFFFFFFFFF000ULL);
                if (!src_pt)
                    continue;
                dst_pt = yghv_msv_pt_alloc();
                if (!dst_pt)
                    goto fail;
                dst_pd[k].all = MmGetPhysicalAddress(dst_pt).QuadPart | 0x3;
                RtlCopyMemory(dst_pt, src_pt, 512 * sizeof(npt_entry_t));
            }
        }
    }
    yghv_trace_u64("msv deep cr3 cloned", dst_pml4_pa);
    g_b0_cloning = FALSE;
    return dst_pml4_pa;

fail:
    g_b0_cloning = FALSE;
    yghv_trace("msv deep cr3 clone failed");
    return 0;
}
/* 9.162: clean-unload for OS-as-guest resident (fixes 0xCE).  When
   g_os_guest_stop_requested is set, svm_dispatch_exit returns 1 so the
   trampoline's jnz host_done terminates the guest thread on its next VMEXIT
   (spin guests VMEXIT on every CPUID).  The resident thread handles are owned
   by DriverUnload so sc stop can join them before tearing down. */
volatile BOOLEAN g_os_guest_stop_requested = FALSE;
HANDLE g_os_guest_resident_thread = NULL;   /* spin/block OS-as-guest thread */
HANDLE g_os_guest_alive_thread = NULL;      /* resident alive logger */
HANDLE g_os_guest_watchdog_thread = NULL;   /* resident watchdog (0xE2 guard) */
volatile BOOLEAN g_v96_apic_tpr_stress = FALSE;
void *g_v96_apic_tpr_va = NULL;
volatile BOOLEAN g_v97_hlt_intercept = FALSE;
volatile BOOLEAN g_v98_apic_shadow = FALSE;
void *g_v98_apic_shadow_va = NULL;
uint64_t g_v98_apic_shadow_pa = 0;
void *g_v98_real_apic_va = NULL;
volatile ULONG g_v98_last_tpr = 0;
volatile ULONG g_v98_last_icrl = 0;
volatile ULONG g_v98_last_icrh = 0;
volatile ULONG g_v98_last_lvtt = 0;
volatile ULONG g_v98_last_tmict = 0;
volatile ULONG g_v98_last_tdcr = 0;
volatile ULONG64 g_v98_apic_mmio_count = 0;   /* B-1full: APIC MMIO write-trap hits */
volatile ULONG64 g_npf_count = 0;              /* D1: total NPF exits */
volatile uint64_t g_last_npf_gpa = 0;          /* D1: last NPF faulting GPA */
volatile uint64_t g_last_npf_err = 0;          /* D1: last NPF exitinfo1 error code */
volatile uint64_t g_last_npf_rip = 0;          /* D1: last NPF guest RIP */
volatile ULONG64 g_intr_exit_count = 0;        /* D1b: INTR/NMI VMEXITs (= injection attempts) */
volatile BOOLEAN g_b0_inject = FALSE;          /* D3-base control: off = no injector */
volatile BOOLEAN g_b0_cloning = FALSE;         /* D4: set during deep clone so the 10ms
                                                  diag pauses its file I/O (fixes the
                                                  D1c 0x50 clone-vs-I/O page-table race) */
static volatile LONG g_v99_allcore_ready = 0;
static volatile LONG g_v99_allcore_go = 0;
static volatile LONG g_v99_allcore_abort = 0;
static volatile LONG g_v99_allcore_online = 0;

/* 9.227 step203: SimpleSvm-equivalent all-core OS-as-guest config (see
 * docs/YGHV_SIMPLEVM_LEVERAGE_20260918.md and yghv_os_guest_simplevm_thread).
 * Gate: YGHV_BAREMETAL_STEP=203.  Thread handles live in g_s203_threads so
 * DriverUnload can join every core's trampoline thread before teardown. */
static HANDLE g_s203_threads[SVM_MAX_CORES];
static HANDLE g_s203_observer = NULL;
static volatile LONG g_s203_online = 0;
static volatile LONG g_s203_ready = 0;
static volatile LONG g_s203_go = 0;
static volatile LONG g_s203_abort = 0;
/* ~10ms CPUID-poll throttle at 3GHz TSC (wrap-safe unsigned subtraction). */
#define YGHV_S203_POLL_TSC 30000000ULL
/* 9.229 self-diagnosis: while TRUE, svm_dispatch_exit logs the first exits
 * write-through and fail-closes any non-CPUID/VMRUN/VMMCALL exit (only the
 * guest core exits; the host stays up and progress.log keeps the cause). */
volatile BOOLEAN g_s203_lean = FALSE;
volatile LONG g_s203_exit_log = 0;
/* 9.231 step204: when TRUE, the guest continuation does NOT keep a yghv poll
 * thread on the core — it PsTerminateSystemThreads in GUEST mode immediately,
 * returning the core to the normal guest OS (the SimpleSvm-like steady state:
 * a persistent per-core host loop only services synchronous CPUID/VMRUN
 * VMEXITs; no host/guest split-brain, no busy poll thread mutating scheduler
 * state on a core that Windows still thinks is a normal processor). */
volatile BOOLEAN g_s204_ret_immediately = FALSE;
/* 9.229c2: set (pure memory) by the dispatch island when an unexpected exit
 * occurs; the observer thread logs it from normal host context. */
volatile uint64_t g_s203_unexpected_exit = 0;
volatile BOOLEAN g_os_resident_log_active = FALSE;
volatile ULONG64 g_os_resident_exits = 0;
volatile BOOLEAN g_v100_monitor_active = FALSE;
volatile BOOLEAN g_v100_guest_entered = FALSE;
volatile BOOLEAN g_v101_gp_intercept = FALSE;
volatile BOOLEAN g_v101_gp_seen = FALSE;
volatile uint64_t g_v101_gp_exitcode = 0;
volatile uint64_t g_v101_gp_err = 0;
volatile uint64_t g_v101_gp_rip = 0;
volatile uint64_t g_v101_gp_rsp = 0;
volatile uint64_t g_v101_gp_cr3 = 0;
volatile uint64_t g_v101_gp_gs_base = 0;
volatile BOOLEAN g_v102_catchall = FALSE;
volatile BOOLEAN g_r1_diag = FALSE;
volatile ULONG g_r1_diag_count = 0;
volatile ULONG g_r1_entry_seq = 0;
static KEVENT g_os_guest_done_events[SVM_MAX_CORES];
static KEVENT g_os_resident_stop_event;

extern const uint8_t svm_trampoline_test_guest[];
extern const uint8_t svm_trampoline_test_guest_resume[];
extern const uint8_t svm_trampoline_test_guest_end[];
extern const uint8_t svm_os_seamless_cont[];
extern const uint8_t svm_trampoline_test_npt_guest[];
extern const uint8_t svm_trampoline_test_npt_guest_resume[];
extern const uint8_t svm_trampoline_test_npt_guest_end[];
extern const uint8_t svm_trampoline_test_prot_write[];
extern const uint8_t svm_trampoline_test_prot_write_resume[];
extern const uint8_t svm_trampoline_test_prot_write_end[];
extern const uint8_t svm_trampoline_test_hook_guest[];
extern const uint8_t svm_trampoline_test_hook_guest_end[];
extern const uint8_t svm_trampoline_test_min_guest[];
extern const uint8_t svm_trampoline_test_min_guest_end[];
extern const uint8_t svm_trampoline_test_cpuid_guest[];
extern const uint8_t svm_trampoline_test_cpuid_guest_end[];
extern const uint8_t svm_trampoline_test_resident_guest[];
extern const uint8_t svm_trampoline_test_resident_guest_end[];
extern const uint8_t yghv_avic_eoi_isr[];

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
static void yghv_join_system_thread(HANDLE h);   /* defined near DriverUnload */

static void yghv_trace_init(void) {
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st;

    RtlInitUnicodeString(&name, L"\\SystemRoot\\yghv_progress.log");
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateFile(&g_trace_file, GENERIC_WRITE, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st))
        LOG_ERROR("yghv_trace_init failed 0x%x", st);
}

void yghv_trace(const char *msg) {
    IO_STATUS_BLOCK iosb;
    size_t msg_len = 0;
    char buf[256];

    /*
     * 9.490 CORRECTED: NO RUNTIME IRQL GATE HERE, AND THE CHECKER CAUGHT WHY.
     *
     * The first attempt guarded with KeGetCurrentIrql() > PASSIVE_LEVEL - which
     * does not link on this toolchain (mem_engine.c 9.262: it lowers to
     * `__readcr8`, not exported by the WDM km libs). The second attempt used the
     * tree's usual substitute KeAreAllApcsDisabled() - and `irql_contract_check`
     * rejected it with the right reason:
     *
     *     "yghv_trace is reachable from a VMEXIT root but gates on
     *      KeAreAllApcsDisabled(); an NPF/VMEXIT handler runs at DISPATCH with
     *      GIF=0, which IS all-APCs-disabled"
     *
     * So that gate would not have prevented a high-IRQL write - it would have
     * silently disabled logging EVERYWHERE, because the legitimate callers are
     * dispatch paths. Losing all trace output to avoid a bugcheck is not a trade,
     * it is deleting the evidence.
     *
     * THE REAL CONTRACT: this function performs blocking I/O and MUST only be
     * called at PASSIVE_LEVEL. That is a property of its CALLERS, and the honest
     * way to enforce it is to keep the call sites auditable - which the
     * irql_contract_check island set already does by refusing *_bare-style gates.
     * A guard that cannot distinguish "dispatch at PASSIVE with APCs off" from
     * "DISPATCH_LEVEL" cannot enforce this, so no guard is added.
     *
     * Every caller today writes from DriverEntry, DriverUnload, or a PASSIVE
     * thread (verified by audit: no DPC/timer callback calls it).
     */
    if (!g_trace_file) return;
    if (!NT_SUCCESS(RtlStringCchLengthA(msg, sizeof(buf) - 2, &msg_len))) return;
    RtlCopyMemory(buf, msg, msg_len);
    buf[msg_len] = '\r';
    buf[msg_len + 1] = '\n';
    ZwWriteFile(g_trace_file, NULL, NULL, NULL, &iosb, buf, (ULONG)(msg_len + 2), NULL, NULL);
    /* Write-through: a hard freeze must leave the last milestone on disk. */
    ZwFlushBuffersFile(g_trace_file, &iosb);
}

void yghv_trace_u64(const char *label, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[64];
    size_t n = 0;
    int i;
    size_t max_label = sizeof(buf) - 20;   /* '=' + "0x" + 16 hex + NUL */
    while (label[n] && n < max_label) {
        buf[n] = label[n];
        n++;
    }
    buf[n++] = '=';
    buf[n++] = '0';
    buf[n++] = 'x';
    for (i = 15; i >= 0; i--)
        buf[n++] = hex[(v >> (i * 4)) & 0xF];
    buf[n] = 0;
    yghv_trace(buf);
}

static void yghv_trace_close(void) {
    if (g_trace_file) {
        ZwClose(g_trace_file);
        g_trace_file = NULL;
    }
}

static NTSTATUS yghv_npt_map_ram(npt_mgr_t *m) {
    PPHYSICAL_MEMORY_RANGE ranges;
    ULONG i;

    ranges = MmGetPhysicalMemoryRanges();
    if (!ranges) return STATUS_UNSUCCESSFUL;

    for (i = 0; ranges[i].NumberOfBytes.QuadPart != 0; i++) {
        uint64_t base = ranges[i].BaseAddress.QuadPart;
        uint64_t end = base + ranges[i].NumberOfBytes.QuadPart;
        NTSTATUS st = npt_identity_map_range(m, base, end);
        if (st) {
            ExFreePool(ranges);
            return st;
        }
    }
    ExFreePool(ranges);
    return STATUS_SUCCESS;
}

#if YGHV_R1_EXCLUDE_PRIVATE
static void yghv_exclude_hv_private(PDRIVER_OBJECT d) {
    ULONG i;


    /* R1 bare-metal: remove hypervisor-owned pages from the guest NPT.
       The synthetic guest executes from the driver image, so the driver
       stays mapped; everything hypervisor-private is made guest-invisible. */
    for (i = 0; i < SVM_MAX_CORES; i++) {
        svm_vcpu_t *v = g_vcpus[i];
        PHYSICAL_ADDRESS pa;
        if (!v) continue;
        if (v->vmcb) npt_exclude_pa(&g_npt, v->vmcb_pa);
        if (v->host_vmcb) npt_exclude_pa(&g_npt, v->host_vmcb_pa);
        if (v->hsave) npt_exclude_pa(&g_npt, v->hsave_pa);
        if (v->host_stack) {
            pa = MmGetPhysicalAddress(v->host_stack);
            npt_exclude_range(&g_npt, pa.QuadPart,
                              SVM_HOST_STACK_PAGES * HV_PAGE_SIZE);
        }
        if (v->msrpm)
            npt_exclude_range(&g_npt, v->msrpm_pa,
                              SVM_MSRPM_PAGES * HV_PAGE_SIZE);
        if (v->iopm)
            npt_exclude_range(&g_npt, v->iopm_pa,
                              SVM_IOPM_PAGES * HV_PAGE_SIZE);
    }
    npt_exclude_self(&g_npt);


static NTSTATUS yghv_r1_exclude_check(void) {
    svm_vcpu_t *v = svm_core_get_vcpu(0);
    uint64_t pa;

    if (!v || !v->vmcb)
        return STATUS_INVALID_PARAMETER;

    pa = npt_read_entry(&g_npt, v->vmcb_pa);
    if (pa & NPT_PERM_PRESENT) {
        LOG_ERROR("r1 exclude: vmcb still mapped entry=0x%llx", pa);
        return STATUS_UNSUCCESSFUL;
    }
    pa = npt_read_entry(&g_npt, g_npt.pml4_pa);
    if (pa & NPT_PERM_PRESENT) {
        LOG_ERROR("r1 exclude: pml4 still mapped entry=0x%llx", pa);
        return STATUS_UNSUCCESSFUL;
    }

    pa = MmGetPhysicalAddress((PVOID)(uintptr_t)svm_trampoline_test_guest).QuadPart;
    pa = npt_read_entry(&g_npt, pa);
    if (!(pa & NPT_PERM_PRESENT)) {
        LOG_ERROR("r1 exclude: guest code page missing entry=0x%llx", pa);
        return STATUS_UNSUCCESSFUL;
    }

    LOG_ERROR("r1 exclude check: PASS (vmcb/pml4 excluded, guest code mapped)");
    return STATUS_SUCCESS;
}
#endif

static NTSTATUS yghv_r1_npt_unit_test(void) {
    void *buf = NULL;
    uint64_t test_pa, test_pa2;
    uint64_t entry, trans;
    NTSTATUS st;

    yghv_trace("r1 start");
    buf = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE, YGHV_TAG);
    if (!buf) {
        LOG_ERROR("r1 unit: alloc failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(buf, HV_PAGE_SIZE);
    test_pa = MmGetPhysicalAddress(buf).QuadPart;
    test_pa2 = test_pa + HV_LARGE_PAGE_SIZE;
    LOG_ERROR("r1 unit: buf_pa=0x%llx test_pa=0x%llx test_pa2=0x%llx",
        test_pa, test_pa, test_pa2);
    yghv_trace("r1 alloc ok");

    entry = npt_read_entry(&g_npt, test_pa);
    trans = npt_translate(&g_npt, test_pa);
    yghv_trace_u64("r1 test_pa", test_pa);
    yghv_trace_u64("r1 trans", trans);
    LOG_ERROR("r1 unit: large entry=0x%llx trans=0x%llx", entry, trans);
    if (trans != test_pa) {
        LOG_ERROR("r1 unit: large identity FAILED");
        st = STATUS_UNSUCCESSFUL;
        goto done;
    }

    st = npt_split_2mb_to_4kb(&g_npt, test_pa);
    LOG_ERROR("r1 unit: split rc=0x%x", st);
    if (st) goto done;
    entry = npt_read_entry(&g_npt, test_pa);
    trans = npt_translate(&g_npt, test_pa);
    LOG_ERROR("r1 unit: split entry=0x%llx trans=0x%llx", entry, trans);
    if (!(entry & NPT_PERM_PRESENT) || trans != test_pa) {
        LOG_ERROR("r1 unit: split identity FAILED");
        st = STATUS_UNSUCCESSFUL;
        goto done;
    }
    yghv_trace("r1 split ok");

    st = npt_set_page_perm(&g_npt, test_pa,
        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    entry = npt_read_entry(&g_npt, test_pa);
    trans = npt_translate(&g_npt, test_pa);
    LOG_ERROR("r1 unit: perm rw entry=0x%llx trans=0x%llx rc=0x%x", entry, trans, st);
    if (st || !(entry & NPT_PERM_WRITABLE) || trans != test_pa) {
        LOG_ERROR("r1 unit: perm rw FAILED");
        st = STATUS_UNSUCCESSFUL;
        goto done;
    }

    st = npt_set_page_perm(&g_npt, test_pa,
        NPT_PERM_PRESENT | NPT_PERM_NX);
    entry = npt_read_entry(&g_npt, test_pa);
    trans = npt_translate(&g_npt, test_pa);
    LOG_ERROR("r1 unit: perm nx entry=0x%llx trans=0x%llx rc=0x%x", entry, trans, st);
    if (st || !(entry & NPT_PERM_NX) || trans != test_pa) {
        LOG_ERROR("r1 unit: perm nx FAILED");
        st = STATUS_UNSUCCESSFUL;
        goto done;
    }

    st = npt_set_page_perm(&g_npt, test_pa,
        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    entry = npt_read_entry(&g_npt, test_pa);
    LOG_ERROR("r1 unit: perm restore entry=0x%llx rc=0x%x", entry, st);
    if (st || (entry & NPT_PERM_NX) || !(entry & NPT_PERM_WRITABLE)) {
        LOG_ERROR("r1 unit: perm restore FAILED");
        st = STATUS_UNSUCCESSFUL;
        goto done;
    }

    st = npt_set_page_perm_range(&g_npt, test_pa2, HV_PAGE_SIZE * 2,
        NPT_PERM_PRESENT | NPT_PERM_NX);
    entry = npt_read_entry(&g_npt, test_pa2);
    trans = npt_translate(&g_npt, test_pa2);
    LOG_ERROR("r1 unit: range nx entry=0x%llx trans=0x%llx rc=0x%x", entry, trans, st);
    if (st || !(entry & NPT_PERM_NX) || trans != test_pa2) {
        LOG_ERROR("r1 unit: range nx FAILED");
        st = STATUS_UNSUCCESSFUL;
        goto done;
    }
    yghv_trace("r1 range nx ok");

    st = npt_set_page_perm_range(&g_npt, test_pa2, HV_PAGE_SIZE * 2,
        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    entry = npt_read_entry(&g_npt, test_pa2);
    LOG_ERROR("r1 unit: range restore entry=0x%llx rc=0x%x", entry, st);
    if (st || (entry & NPT_PERM_NX) || !(entry & NPT_PERM_WRITABLE)) {
        LOG_ERROR("r1 unit: range restore FAILED");
        st = STATUS_UNSUCCESSFUL;
        goto done;
    }

    LOG_ERROR("r1 unit: PASS");
    yghv_trace("r1 unit pass");
    st = STATUS_SUCCESS;

done:
    if (buf) ExFreePoolWithTag(buf, YGHV_TAG);
    return st;
}

static NTSTATUS yghv_protect_test(void) {
    void *buf;
    uint64_t buf_pa, buf_va;
    uint64_t entry_after;
    svm_vcpu_t *v;
    NTSTATUS st;
    int ok = 1;

    /* Phase B: policy matrix (synthetic) */
    g_protect.targets[0].cr3 = 0x1000ULL;   /* fake target CR3 */
    if (!yghv_protect_is_target_cr3(0x1000ULL)) ok = 0;
    if (yghv_protect_is_target_cr3(0x2000ULL)) ok = 0;
    LOG_ERROR("protect test: policy %s", ok ? "PASS" : "FAIL");
    if (!ok) return STATUS_UNSUCCESSFUL;

    /* Phase C: real write trap, target = System (current process) */
    st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect test: set_target FAILED 0x%x", st);
        g_protect.targets[0].cr3 = 0;   /* don't leave the synthetic policy CR3 behind */
        return st;
    }
    buf = MmAllocateContiguousMemory(HV_PAGE_SIZE,
        (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!buf) {
        yghv_protect_cleanup();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(buf, HV_PAGE_SIZE);
    buf_va = (uint64_t)buf;
    buf_pa = MmGetPhysicalAddress(buf).QuadPart;
    st = yghv_protect_add_page(buf_va);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect test: setup FAILED 0x%x", st);
        MmFreeContiguousMemory(buf);
        return st;
    }
    st = yghv_protect_start();
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect test: setup FAILED 0x%x", st);
        yghv_protect_stop();
        yghv_protect_remove_page(buf_va);
        MmFreeContiguousMemory(buf);
        return st;
    }
    entry_after = npt_read_entry(&g_npt, buf_pa);
    if (entry_after & NPT_PERM_WRITABLE) {
        LOG_ERROR("protect test: page still writable after arm");
        ok = 0;
    }

    v = svm_core_get_vcpu(0);
    v->regs.rdi = buf_va;
    v->regs.rcx = g_vmmcall_auth_cookie;
    v->vmcb->state.rip = (uint64_t)svm_trampoline_test_prot_write;
    v->vmcb->state.rax = 0;
    /* Resident exit from the NPT test clears intercepts + NPT; restore them
       so the write is trapped and STOP_INTERNAL VMMCALL can stop the loop. */
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (!NT_SUCCESS(svm_core_set_npt(0, g_npt.pml4_pa))) {
        LOG_ERROR("protect test: npt restore FAILED");
        yghv_protect_stop();
        yghv_protect_remove_page(buf_va);
        MmFreeContiguousMemory(buf);
        return STATUS_UNSUCCESSFUL;
    }
    svm_core_enter_resident_current(0);
    entry_after = npt_read_entry(&g_npt, buf_pa);
    if (entry_after & NPT_PERM_WRITABLE) {
        LOG_ERROR("protect test: page not re-armed after write");
        ok = 0;
    }
    LOG_ERROR("protect test: real write %s", ok ? "PASS" : "FAIL");

    /* Second write to the same page: TLB flush + re-arm must keep trapping. */
    v->regs.rdi = buf_va;
    v->regs.rcx = g_vmmcall_auth_cookie;
    v->vmcb->state.rip = (uint64_t)svm_trampoline_test_prot_write;
    v->vmcb->state.rax = 0;
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (!NT_SUCCESS(svm_core_set_npt(0, g_npt.pml4_pa))) {
        LOG_ERROR("protect test: npt restore #2 FAILED");
        yghv_protect_stop();
        yghv_protect_remove_page(buf_va);
        MmFreeContiguousMemory(buf);
        return STATUS_UNSUCCESSFUL;
    }
    svm_core_enter_resident_current(0);
    entry_after = npt_read_entry(&g_npt, buf_pa);
    if (entry_after & NPT_PERM_WRITABLE) {
        LOG_ERROR("protect test: page not re-armed after second write");
        ok = 0;
    }
    LOG_ERROR("protect test: real write #2 %s", ok ? "PASS" : "FAIL");
    st = yghv_protect_stop();
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect test: stop FAILED 0x%x", st);
        yghv_protect_remove_page(buf_va);
        MmFreeContiguousMemory(buf);
        return st;
    }
    st = yghv_protect_remove_page(buf_va);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect test: remove_page FAILED 0x%x", st);
        MmFreeContiguousMemory(buf);
        return st;
    }
    MmFreeContiguousMemory(buf);
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

__attribute__((naked, noinline)) static void yghv_hook_test_dummy(void) {
    __asm__ volatile(
        "nop; nop; nop; nop; nop; nop; nop; nop;"
        "nop; nop; nop; nop; nop; nop; nop; nop;"
        "ret");
}

__attribute__((naked, noinline)) static void yghv_hook_test_dummy2(void) {
    __asm__ volatile(
        "nop; nop; nop; nop; nop; nop; nop; nop;"
        "nop; nop; nop; nop; nop; nop; nop; nop;"
        "ret");
}

static NTSTATUS yghv_hook_test(void) {
    uint64_t term;
    uint64_t open;
    uint64_t dummy;
    uint64_t dummy2;
    uint64_t dummy_page_va;
    uint64_t dummy2_page_va;
    uint64_t gpa;
    uint64_t gpa2;
    uint64_t entry_before, entry_after;
    int ok = 1;

    yghv_trace("hook test start");
    term = yghv_protect_find_func_pattern(L"ZwTerminateProcess", NULL, 0);
    LOG_ERROR("protect hook test: [api]=0x%llx", term);
    open = yghv_protect_find_func_pattern(L"NtOpenProcess", NULL, 0);
    LOG_ERROR("protect hook test: NtOpenProcess=0x%llx", open);
    dummy = (uint64_t)yghv_hook_test_dummy;
    dummy_page_va = dummy & ~(HV_PAGE_SIZE - 1);
    gpa = MmGetPhysicalAddress((PVOID)dummy_page_va).QuadPart;
    if (yghv_protect_install_hook(0, (uint64_t)yghv_hook_test_dummy)) {
        LOG_ERROR("protect hook test: install FAILED");
        return STATUS_UNSUCCESSFUL;
    }
    entry_before = npt_read_entry(&g_npt, gpa);
    if (entry_before & NPT_PERM_WRITABLE) ok = 0;

    if (yghv_protect_on_hook_query(0, g_protect.targets[0].cr3) != YGHV_STATUS_OK) ok = 0;
    if (yghv_protect_on_hook_query(0, g_protect.targets[0].cr3 + 0x1000) != YGHV_STATUS_DENIED) ok = 0;

    if (!NT_SUCCESS(yghv_protect_remove_hook(0))) {
        LOG_ERROR("protect hook test: remove FAILED");
        ok = 0;
    }
    entry_after = npt_read_entry(&g_npt, gpa);
    if (!(entry_after & NPT_PERM_WRITABLE)) ok = 0;
    if (memcmp((void *)dummy, &g_protect_hooks[0].original,
               g_protect_hooks[0].patch_len) != 0) ok = 0;

    dummy2 = (uint64_t)yghv_hook_test_dummy2;
    dummy2_page_va = dummy2 & ~(HV_PAGE_SIZE - 1);
    gpa2 = MmGetPhysicalAddress((PVOID)dummy2_page_va).QuadPart;
    if (yghv_protect_install_hook(1, (uint64_t)yghv_hook_test_dummy2)) {
        LOG_ERROR("protect hook test: install1 FAILED");
        ok = 0;
    } else {
        entry_before = npt_read_entry(&g_npt, gpa2);
        if (entry_before & NPT_PERM_WRITABLE) ok = 0;

        if (yghv_protect_on_hook_query(1, g_protect.targets[0].cr3) != YGHV_STATUS_OK) ok = 0;
        if (yghv_protect_on_hook_query(1, g_protect.targets[0].cr3 + 0x1000) != YGHV_STATUS_DENIED) ok = 0;

        if (!NT_SUCCESS(yghv_protect_remove_hook(1))) {
            LOG_ERROR("protect hook test: remove1 FAILED");
            ok = 0;
        }
        entry_after = npt_read_entry(&g_npt, gpa2);
        if (!(entry_after & NPT_PERM_WRITABLE)) ok = 0;
        if (memcmp((void *)dummy2, &g_protect_hooks[1].original,
                   g_protect_hooks[1].patch_len) != 0) ok = 0;
    }

    LOG_ERROR("protect hook test: %s", ok ? "PASS" : "FAIL");
    yghv_trace(ok ? "hook test pass" : "hook test fail");
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS yghv_hook_resident_test(void) {
    uint64_t dummy = (uint64_t)yghv_hook_test_dummy;
    uint64_t real_cr3;
    svm_vcpu_t *v;
    NTSTATUS st;
    int ok = 1;

    yghv_trace("hook resident start");
    st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("hook resident test: set_target FAILED 0x%x", st);
        return st;
    }
    if (yghv_protect_install_hook(0, dummy)) {
        LOG_ERROR("hook resident test: install FAILED");
        return STATUS_UNSUCCESSFUL;
    }
    real_cr3 = g_protect.targets[0].cr3;

    v = svm_core_get_vcpu(0);
    v->regs.rsi = dummy;
    v->regs.rcx = g_vmmcall_auth_cookie;
    v->vmcb->state.cr3 = real_cr3;
    v->vmcb->state.rip = (uint64_t)svm_trampoline_test_hook_guest;
    v->vmcb->state.rax = 0;
    v->regs.rax = 0;
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (!NT_SUCCESS(svm_core_set_npt(0, g_npt.pml4_pa))) {
        LOG_ERROR("hook resident test: npt restore FAILED");
        yghv_protect_remove_hook(0);
        return STATUS_UNSUCCESSFUL;
    }
    svm_core_enter_resident_current(0);
    LOG_ERROR("hook resident test: allow rdx=0x%llx", v->regs.rdx);
    yghv_trace_u64("hook resident allow", v->regs.rdx);
    if (v->regs.rdx == 0xC0000022ULL)
        ok = 0;

    /* Deny: keep guest CR3 valid, shift g_protect.targets[0].cr3 out of match. */
    g_protect.targets[0].cr3 = real_cr3 + 0x1000;
    v->regs.rsi = dummy;
    v->regs.rcx = g_vmmcall_auth_cookie;
    v->vmcb->state.cr3 = real_cr3;
    v->vmcb->state.rip = (uint64_t)svm_trampoline_test_hook_guest;
    v->vmcb->state.rax = 0;
    v->regs.rax = 0;
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (!NT_SUCCESS(svm_core_set_npt(0, g_npt.pml4_pa))) {
        LOG_ERROR("hook resident test: npt restore #2 FAILED");
        g_protect.targets[0].cr3 = real_cr3;
        yghv_protect_remove_hook(0);
        return STATUS_UNSUCCESSFUL;
    }
#ifdef YGHV_R1_EXCLUDE_PRIVATE
    v->vmcb->control.exception_intercepts = 0xFFFFFFFFULL;
    v->vmcb->control.general1_intercepts |= INTR_GEN1(SVM_INTERCEPT_HLT);
    g_v102_catchall = TRUE;
#endif
    svm_core_enter_resident_current(0);
#ifdef YGHV_R1_EXCLUDE_PRIVATE
    g_v102_catchall = FALSE;
#endif
    LOG_ERROR("hook resident test: deny rdx=0x%llx", v->regs.rdx);
    yghv_trace_u64("hook resident deny", v->regs.rdx);
    if (v->regs.rdx != 0xC0000022ULL)
        ok = 0;
    g_protect.targets[0].cr3 = real_cr3;

    st = yghv_protect_remove_hook(0);
    yghv_trace_u64("hook resident remove", (uint64_t)st);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("hook resident test: remove FAILED 0x%x", st);
        ok = 0;
    }
    LOG_ERROR("hook resident test: %s", ok ? "PASS" : "FAIL");
    yghv_trace(ok ? "hook resident pass" : "hook resident fail");
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS yghv_hook_boundary_test(void) {
    void *buf;
    uint64_t va;
    int ok = 1;
    int i;

    yghv_trace("hook boundary start");
    buf = MmAllocateContiguousMemory(
        HV_PAGE_SIZE * 2, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!buf)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(buf, HV_PAGE_SIZE * 2);
    va = ((uint64_t)buf + HV_PAGE_SIZE - 1) & ~(uint64_t)(HV_PAGE_SIZE - 1);

    /* valid target: 16 NOPs then ret -> 12-byte patch */
    RtlFillMemory((void *)va, 16, 0x90);
    *(uint8_t *)(va + 16) = 0xC3;
    if (yghv_protect_validate_hook_target(va) != YGHV_PROTECT_PATCH_MIN)
        ok = 0;

    /* variable boundary: 11 NOPs + ret -> resume at offset 12 */
    RtlZeroMemory((void *)va, 32);
    RtlFillMemory((void *)va, 11, 0x90);
    *(uint8_t *)(va + 11) = 0xC3;
    if (yghv_protect_validate_hook_target(va) != YGHV_PROTECT_PATCH_MIN)
        ok = 0;

    /* patch region crossing the 4KB page boundary must be rejected */
    if (yghv_protect_validate_hook_target(va + HV_PAGE_SIZE - 8) != -1)
        ok = 0;

    /* relative branch inside the copied region (11 NOPs + E9 rel32) rejected */
    RtlZeroMemory((void *)va, 32);
    RtlFillMemory((void *)va, 11, 0x90);
    *(uint8_t *)(va + 11) = 0xE9;
    *(uint8_t *)(va + 15) = 0x01;
    if (yghv_protect_validate_hook_target(va) != -1)
        ok = 0;

    /* RIP-relative first instruction rejected */
    RtlZeroMemory((void *)va, 32);
    ((uint8_t *)va)[0] = 0x48; ((uint8_t *)va)[1] = 0x8D;
    ((uint8_t *)va)[2] = 0x05; ((uint8_t *)va)[3] = 0x01;
    ((uint8_t *)va)[4] = 0x00; ((uint8_t *)va)[5] = 0x00;
    ((uint8_t *)va)[6] = 0x00;
    if (yghv_protect_validate_hook_target(va) != -1)
        ok = 0;

    /* REV-037 decoder regression cases (all must decode to boundary 12): */

    /* B8 imm32 (no REX, mov eax,imm32, 5 bytes) + mov [rsp+8],rbx (5) +
       ret (1) + nop -> boundaries 5,10,11,12 */
    RtlZeroMemory((void *)va, 32);
    ((uint8_t *)va)[0] = 0xB8;
    ((uint8_t *)va)[1] = 0x34; ((uint8_t *)va)[2] = 0x12;
    ((uint8_t *)va)[3] = 0x00; ((uint8_t *)va)[4] = 0x00;
    ((uint8_t *)va)[5] = 0x48; ((uint8_t *)va)[6] = 0x89;
    ((uint8_t *)va)[7] = 0x5C; ((uint8_t *)va)[8] = 0x24;
    ((uint8_t *)va)[9] = 0x08;
    ((uint8_t *)va)[10] = 0xC3;
    ((uint8_t *)va)[11] = 0x90;
    if (yghv_protect_validate_hook_target(va) != 12)
        ok = 0;

    /* 48 B8 imm64 (movabs rax,imm64, 10 bytes) + ret + nop -> 12 */
    RtlZeroMemory((void *)va, 32);
    ((uint8_t *)va)[0] = 0x48; ((uint8_t *)va)[1] = 0xB8;
    for (i = 2; i < 10; i++)
        ((uint8_t *)va)[i] = 0xAA;
    ((uint8_t *)va)[10] = 0xC3;
    ((uint8_t *)va)[11] = 0x90;
    if (yghv_protect_validate_hook_target(va) != 12)
        ok = 0;

    /* 0F 38 F0 C0 (pshufb xmm0,xmm0, 4 bytes) + 8 NOPs -> 12 */
    RtlZeroMemory((void *)va, 32);
    ((uint8_t *)va)[0] = 0x0F; ((uint8_t *)va)[1] = 0x38;
    ((uint8_t *)va)[2] = 0xF0; ((uint8_t *)va)[3] = 0xC0;
    RtlFillMemory((void *)(va + 4), 8, 0x90);
    if (yghv_protect_validate_hook_target(va) != 12)
        ok = 0;

    /* F6 /0 (test byte [rdi],imm8, 3 bytes) + 9 NOPs -> 12 */
    RtlZeroMemory((void *)va, 32);
    ((uint8_t *)va)[0] = 0xF6; ((uint8_t *)va)[1] = 0x07;
    ((uint8_t *)va)[2] = 0x01;
    RtlFillMemory((void *)(va + 3), 9, 0x90);
    if (yghv_protect_validate_hook_target(va) != 12)
        ok = 0;

    /* F7 /2 (not qword ptr [rdi], no immediate, 3 bytes) + 9 NOPs -> 12 */
    RtlZeroMemory((void *)va, 32);
    ((uint8_t *)va)[0] = 0x48; ((uint8_t *)va)[1] = 0xF7;
    ((uint8_t *)va)[2] = 0x17;
    RtlFillMemory((void *)(va + 3), 9, 0x90);
    if (yghv_protect_validate_hook_target(va) != 12)
        ok = 0;

    LOG_ERROR("hook boundary test: %s", ok ? "PASS" : "FAIL");
    yghv_trace(ok ? "hook boundary pass" : "hook boundary fail");
    MmFreeContiguousMemory(buf);
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

#if YGHV_REAL_HOOK_TEST
static NTSTATUS yghv_real_hook_test(void) {
    uint64_t term, open;
    uint64_t real_cr3;
    NTSTATUS st, deny_st;
    int ok = 1;

    yghv_trace("real hook start");
    term = yghv_protect_find_func_pattern(L"ZwTerminateProcess", NULL, 0);
    open = yghv_protect_find_func_pattern(L"ZwOpenProcess", NULL, 0);
    LOG_ERROR("real hook test: [api]=0x%llx [api]=0x%llx",
        term, open);
    if (!term || !open) {
        LOG_ERROR("real hook test: exported functions not found");
        return STATUS_NOT_FOUND;
    }

    st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("real hook test: set_target FAILED 0x%x", st);
        return st;
    }
    real_cr3 = g_protect.targets[0].cr3;

    /* install/remove validation on the handle-open function first. */
    if (yghv_protect_install_hook(1, open)) {
        LOG_ERROR("real hook test: install [api] FAILED");
        ok = 0;
    } else {
        st = yghv_protect_remove_hook(1);
        yghv_trace("real hook open ok");
        if (!NT_SUCCESS(st)) {
            LOG_ERROR("real hook test: remove [api] FAILED 0x%x", st);
            ok = 0;
        }
    }

    if (yghv_protect_install_hook(0, term)) {
        LOG_ERROR("real hook test: install [api] FAILED");
        ok = 0;
    } else {
        NTSTATUS (*fn)(HANDLE) = (NTSTATUS (*)(HANDLE))term;

        /* Allow: current process CR3 matches the protected target. */
        st = fn((HANDLE)(ULONG_PTR)0xDEADBEEFULL);
        LOG_ERROR("real hook test: allow st=0x%x", st);
        yghv_trace_u64("real hook allow", (uint64_t)st);
        if (st == 0xC0000022L)
            ok = 0;

        /* Deny: shift g_protect.targets[0].cr3 out of match, guest CR3 stays real. */
        g_protect.targets[0].cr3 = real_cr3 + 0x1000;
        deny_st = fn((HANDLE)(ULONG_PTR)0xDEADBEEFULL);
        LOG_ERROR("real hook test: deny st=0x%x", deny_st);
        yghv_trace_u64("real hook deny", (uint64_t)deny_st);
        if (deny_st != 0xC0000022L)
            ok = 0;
        g_protect.targets[0].cr3 = real_cr3;

        st = yghv_protect_remove_hook(0);
        if (!NT_SUCCESS(st)) {
            LOG_ERROR("real hook test: remove [api] FAILED 0x%x",
                st);
            ok = 0;
        }
    }

    LOG_ERROR("real hook test: %s", ok ? "PASS" : "FAIL");
    yghv_trace(ok ? "real hook pass" : "real hook fail");
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
#endif

static NTSTATUS yghv_cpuid_stealth_test(void) {
    svm_vcpu_t *v;
    int ok = 1;

    v = svm_core_get_vcpu(0);
    v->regs.rcx = g_vmmcall_auth_cookie;
    v->regs.rax = 0;
    v->vmcb->state.rip = (uint64_t)svm_trampoline_test_cpuid_guest;
    v->vmcb->state.rax = 0;
    v->vmcb->control.general1_intercepts = INTERCEPT_CPUID;
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (!NT_SUCCESS(svm_core_set_npt(0, g_npt.pml4_pa))) {
        LOG_ERROR("cpuid stealth test: npt restore FAILED");
        return STATUS_UNSUCCESSFUL;
    }
    svm_core_enter_resident_current(0);
    LOG_ERROR("cpuid stealth test: leaf1_ecx=0x%llx hyper=0x%llx/0x%llx/0x%llx/0x%llx svm_ecx=0x%llx svm_leaf_eax=0x%llx",
        v->regs.r14,
        v->regs.r8, v->regs.r9, v->regs.r10, v->regs.r11,
        v->regs.r12, v->regs.r13);
    if (v->regs.r14 & (1ULL << 31))
        ok = 0;
    if (v->regs.r8 != 0 || v->regs.r9 != 0 || v->regs.r10 != 0 ||
        v->regs.r11 != 0)
        ok = 0;
    if (v->regs.r12 & (1ULL << 2))
        ok = 0;
    if (v->regs.r13 != 0)
        ok = 0;
    LOG_ERROR("cpuid stealth test: %s", ok ? "PASS" : "FAIL");
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static __declspec(noinline) void yghv_os_guest_main(void) {
    uint32_t a, b, c, d;

    for (;;) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                 : "a"(1) : "memory");
        InterlockedIncrement(&g_os_guest_counter);
        g_os_guest_cpuid_acc += a;
        (void)__rdtsc();
    }
}

/* 9.159: OS-as-guest ASID/TLB hygiene helper.  Called right after
   svm_core_set_npt() in every yghv_os_guest_*_thread so the guest runs under
   a per-core unique ASID with a by-ASID TLB flush requested on every VMRUN.
   The goal is to decouple the guest TLB (ASID != 0) from the host TLB (ASID 0)
   that both translate the same host kernel addresses — the aliasing that the
   9.152 shared-CR3 freeze implicated.  gated by g_os_guest_tlb_hygiene. */
static void yghv_os_guest_tlb_hygiene_apply(svm_vcpu_t *v, uint32_t core) {
    if (!v || !g_os_guest_tlb_hygiene)
        return;
    v->vmcb->control.guest_asid = core + 1;   /* per-core unique; host keeps 0 */
    v->vmcb->control.tlb_control = g_os_guest_tlb_flush_all
                                       ? SVM_TLB_CONTROL_FLUSH_ALL  /* 2 */
                                       : SVM_TLB_CONTROL_FLUSH;     /* 1 */
}

/* 9.171: give OS-as-guest a DEDICATED TSS so the guest Windows context switch
   (KiSwapThread writes TSS.RSP0) never pollutes the HOST TSS.  With the guest
   TR sharing the host TSS, the guest's RSP0 write lands in the host TSS, so the
   very next host interrupt/exception uses a corrupted RSP0 -> whole-machine
   freeze.  This is exactly what real hypervisors do (per-vCPU TSS).  The guest
   TR base is repointed at a per-vcpu copy of the host TSS; selector/attrib/
   limit are kept (the VMCB provides the base directly, so the GDT descriptor
   base is irrelevant to VMRUN).  Guest RSP0 writes then go to the guest TSS
   only. */
static void yghv_os_guest_tss_isolate_apply(svm_vcpu_t *v) {
    ULONG copy_len;

    if (!v || !v->guest_tss)
        return;
    /* copy the host TSS contents (RSP0, IST, I/O map base, etc.) so the guest
       starts with a valid task state; cap at one page.  tr_limit is bytes. */
    copy_len = (ULONG)v->vmcb->state.tr_limit + 1;
    if (copy_len > HV_PAGE_SIZE)
        copy_len = HV_PAGE_SIZE;
    RtlCopyMemory(v->guest_tss, (void *)v->vmcb->state.tr_base, copy_len);
    v->vmcb->state.tr_base = (uint64_t)v->guest_tss;
    yghv_trace_u64("os guest tss isolated", v->guest_tss_pa);
}

/* 9.175: enable guest CR3 write interception for OS-as-guest.  Windows process
   switch writes CR3 constantly; without interception that write executes naked
   in guest mode.  With the CR3 write intercept (cr_write_intercepts bit 16+3)
   the write VMEXITs and svm_handle_cr emulates it (updates VMCB state.cr3; RIP
   advanced via next_rip), so the host owns the CR3 change and TLB refresh.
   Only set for OS-as-guest vcpus — synthetic residents keep existing behavior. */
static void yghv_os_guest_cr3_intercept_apply(svm_vcpu_t *v) {
    if (!v)
        return;
    /* cr_write_intercepts is a u16 field (VMCB+0x02): bit N = CRn write.  CR3
       write is bit 3 (SVM_CR_INTERCEPT_WRITE_SHIFT(3)=19 is the 32-bit merged
       offset; the split u16 field uses bit 3 directly). */
    v->vmcb->control.cr_write_intercepts |= (uint16_t)(1u << 3);  /* CR3 write */
    yghv_trace("os guest cr3 intercept enabled");
}

/* 9.177: give OS-as-guest a dedicated full-mirror CR3 (clone of the host page
   table, new page-table pages, same VA->PA).  9.152's shared-CR3 freeze was
   avoided by a different CR3 value; OS-as-guest has never tried a dedicated
   CR3.  KNOWN LIMITATION: the clone is a static snapshot — Windows-as-guest
   PTE updates touch the clone, not the host tables, so mapping changes diverge
   (guest may fault on newly-mapped pages).  This first step is a mechanism
   test: does a different CR3 value change the freeze into something
   diagnosable (e.g. triple-fault / NPF) rather than a hard lock?  gated by
   g_os_guest_clone_cr3. */
static void yghv_os_guest_clone_cr3_apply(svm_vcpu_t *v) {
    uint64_t cloned;

    if (!v || !g_os_guest_clone_cr3)
        return;
    cloned = g_msv_test ? yghv_clone_host_cr3_deep() : yghv_clone_host_cr3();
    if (!cloned) {
        LOG_ERROR("os guest clone cr3 failed, keeping host cr3");
        return;
    }
    v->vmcb->state.cr3 = cloned;
    yghv_trace_u64("os guest cr3 cloned to", cloned);
}

__declspec(noinline) __declspec(noreturn)
void yghv_os_guest_host_done(svm_vcpu_t *vcpu) {
    uint32_t core = vcpu ? vcpu->resident_index : 0;
    yghv_trace_u64("os guest host done", core);
    if (g_v101_gp_seen) {
        yghv_trace(g_v102_catchall ? "v102 fault captured" : "v101 gp captured");
        yghv_trace_u64("v101 gp exitcode", g_v101_gp_exitcode);
        yghv_trace_u64("v101 gp err", g_v101_gp_err);
        yghv_trace_u64("v101 gp rip", g_v101_gp_rip);
        yghv_trace_u64("v101 gp rsp", g_v101_gp_rsp);
        yghv_trace_u64("v101 gp cr3", g_v101_gp_cr3);
        yghv_trace_u64("v101 gp gsbase", g_v101_gp_gs_base);
        yghv_trace_u64("v101 gp cs", vcpu ? vcpu->vmcb->state.cs_selector : 0);
        yghv_trace_u64("v101 gp ss", vcpu ? vcpu->vmcb->state.ss_selector : 0);
    }
    if (core < SVM_MAX_CORES)
        KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID yghv_os_guest_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    svm_vcpu_t *v;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    v = svm_core_get_vcpu(core);
    if (!v || g_vcpu_count <= core) {
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }

    svm_prepare_vcpu(v, (uint64_t)yghv_os_guest_main);
    /* svm_prepare_vcpu's final VMSAVE overwrites RIP/RSP with the current
       context; restore the guest entry point like the synthetic tests do. */
    v->vmcb->state.rip = (uint64_t)yghv_os_guest_main;
    v->vmcb->state.rsp = 0;
    v->vmcb->control.general1_intercepts =
        INTERCEPT_CPUID | INTERCEPT_RDTSC |
        INTR_GEN1(SVM_INTERCEPT_SHUTDOWN) |
        INTR_GEN1(SVM_INTERCEPT_MSR_PROT);
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    v->vmcb->control.exception_intercepts = 0;
    v->vmcb->control.tlb_control = 0;
    v->vmcb->control.vmcb_clean_bits = 0;
    v->resident_index = core;
    svm_core_set_npt(core, g_npt.pml4_pa);
    yghv_os_guest_tlb_hygiene_apply(v, core);
    yghv_os_guest_tss_isolate_apply(v);
    yghv_os_guest_cr3_intercept_apply(v);
    yghv_os_guest_clone_cr3_apply(v);
    yghv_trace_u64("os guest thread enter", core);
    svm_trampoline_os_enter(v, 0);
    /* Trampoline exits via yghv_os_guest_host_done; this is a fallback. */
    if (core < SVM_MAX_CORES)
        KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID yghv_os_guest_seamless_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    svm_vcpu_t *v;
    volatile ULONG i;
    uint32_t a, b, c, d;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    v = svm_core_get_vcpu(core);
    if (!v || g_vcpu_count <= core) {
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }
    svm_prepare_vcpu(v, (uint64_t)svm_os_seamless_cont);
    v->vmcb->state.rip = (uint64_t)svm_os_seamless_cont;
    v->vmcb->state.rsp = 0;
    v->vmcb->control.general1_intercepts =
        INTERCEPT_CPUID | INTERCEPT_RDTSC |
        INTR_GEN1(SVM_INTERCEPT_SHUTDOWN) |
        INTR_GEN1(SVM_INTERCEPT_MSR_PROT);
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    v->vmcb->control.exception_intercepts = 0;
    v->vmcb->control.tlb_control = 0;
    v->vmcb->control.vmcb_clean_bits = 0;
    v->resident_index = core;
    svm_core_set_npt(core, g_npt.pml4_pa);
    yghv_os_guest_tlb_hygiene_apply(v, core);
    yghv_os_guest_tss_isolate_apply(v);
    yghv_os_guest_cr3_intercept_apply(v);
    yghv_os_guest_clone_cr3_apply(v);
    yghv_trace_u64("os seamless enter", core);
    svm_trampoline_os_enter(v, 0);
    /* Seamless continuation: this caller now runs in guest mode. */
    for (i = 0; i < 5000; i++) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                 : "a"(1) : "memory");
        InterlockedIncrement(&g_os_guest_counter);
        (void)__rdtsc();
    }
    for (;;) {
        __asm__ volatile("pause");
    }
}

/* 9.226 裸机等价物: step17 线程在 YGHV_APIC_SHADOW=1 时使用 APIC 影子（B-1full，
   guest 写 APIC 触发 NPF 由 host 转发），而非 passthrough——裸机无 VMware 虚拟
   APIC，guest 直接碰物理 APIC = B-0 硬冻结（errata 1363）。前置声明供
   yghv_os_guest_resident_thread 调用。 */
static void yghv_v98_apic_shadow_apply(svm_vcpu_t *v);

static VOID yghv_os_guest_resident_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    svm_vcpu_t *v;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    v = svm_core_get_vcpu(core);
    if (!v || g_vcpu_count <= core) {
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }
    svm_prepare_vcpu(v, (uint64_t)svm_os_seamless_cont);
    v->vmcb->state.rip = (uint64_t)svm_os_seamless_cont;
    v->vmcb->state.rsp = 0;
    v->vmcb->control.general1_intercepts =
        INTERCEPT_CPUID | INTERCEPT_RDTSC |
        INTR_GEN1(SVM_INTERCEPT_SHUTDOWN) |
        INTR_GEN1(SVM_INTERCEPT_MSR_PROT);
    if (g_os_guest_intr_intercept) {
        v->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_INTR) | INTR_GEN1(SVM_INTERCEPT_NMI);
    }
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (g_v97_hlt_intercept) {
        v->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_HLT);
        v->vmcb->control.general2_intercepts |=
            INTR_GEN2(SVM_INTERCEPT_MWAIT) |
            INTR_GEN2(SVM_INTERCEPT_MWAIT_COND);
    }
    v->vmcb->control.exception_intercepts = 0;
    /* 9.222 变体 J: YGHV_CATCHALL=1 时拦截 guest 全异常 + HLT——把单核下
       guest 态真实 Windows 的 triple fault 变成可诊断 VMEXIT（定位崩在哪个
       异常/指令），而非 VM 立即复位。 */
#ifdef YGHV_CATCHALL
    v->vmcb->control.exception_intercepts = 0xFFFFFFFFULL;
    v->vmcb->control.general1_intercepts |=
        INTR_GEN1(SVM_INTERCEPT_HLT);
    yghv_trace("ctlJ: catchall intercepts armed");
#elif defined(YGHV_V101_4VEC)
    /* 9.224 变体 K: 只拦截 4 向量（#DF=8/#NP=11/#SS=12/#GP=13），不拦 HLT——
       VMware 可能对 L1 的 0xFFFFFFFF 拦截组合 VMRUN 不兼容（变体 J 崩得比
       H/I 更早），用最小向量集判别 VMware 是否转发 L1 异常拦截。 */
    v->vmcb->control.exception_intercepts =
        (1ULL << 8) | (1ULL << 11) | (1ULL << 12) | (1ULL << 13);
    yghv_trace("ctlK: 4-vec intercepts armed (DF/NP/SS/GP)");
#endif
    v->vmcb->control.tlb_control = 0;
    v->vmcb->control.vmcb_clean_bits = 0;
    v->resident_index = core;
    svm_core_set_npt(core, g_npt.pml4_pa);
    /* 9.213 变体 D: YGHV_APIC_PASSTHROUGH=1 时把 xAPIC 页 0xFEE00000 identity
       映射进 NPT（全权限，不清 W，不拦截）——guest 直接访问 VMware 虚拟 APIC
       （step20 的 PASS 方式），同时跑真实 Windows 调度器（阻塞常驻）。区分
       "APIC 页未映射"（step17 冻结的 NPF 根因）vs "拦截 APIC"（变体 B/C' 冻结）。 */
#ifdef YGHV_APIC_PASSTHROUGH
    if (npt_identity_map_range(&g_npt, 0xFEE00000ULL,
                               0xFEE00000ULL + HV_PAGE_SIZE) != STATUS_SUCCESS)
        LOG_ERROR("ctlD: identity-map APIC 2MB failed");
    yghv_trace("ctlD apic passthrough mapped");
#endif
#ifdef YGHV_APIC_SHADOW
    /* 9.226 裸机等价物: APIC 影子（B-1full）——guest 写 APIC 触发 NPF 由 host
       EOI/ICR/timer 转发，避免 guest 直接碰物理 APIC（裸机 B-0 硬冻结）。 */
    yghv_v98_apic_shadow_apply(v);
#endif
    yghv_os_guest_tlb_hygiene_apply(v, core);
    yghv_os_guest_tss_isolate_apply(v);
    /* 9.214 变体 E (根治): YGHV_NO_CR3_INTERCEPT=1 时跳过 CR3 拦截 + 克隆 CR3，
       guest 直接用宿主 CR3——VM 里 VMware L0 处理 CR3 写/TLB，core0/core1 共享
       同一套宿主页表，跨核线程分派时栈/页表一致（0x139 FAST_FAIL_INCORRECT_STACK
       根因 = 克隆静态快照 + 两核页表不一致）。 */
#ifndef YGHV_NO_CR3_INTERCEPT
    yghv_os_guest_cr3_intercept_apply(v);
    yghv_os_guest_clone_cr3_apply(v);
#else
    yghv_trace("ctlE: host cr3 passthrough (no intercept/clone)");
#endif
    if (g_v96_apic_tpr_stress && !g_v96_apic_tpr_va) {
        PHYSICAL_ADDRESS apic_pa;
        apic_pa.QuadPart = 0xFEE00000ULL;
        g_v96_apic_tpr_va = MmMapIoSpace(apic_pa, HV_PAGE_SIZE, MmNonCached);
        if (!g_v96_apic_tpr_va)
            LOG_ERROR("v96: map APIC TPR failed");
        else
            yghv_trace("v96 tpr stress armed");
    }
    g_os_resident_mode = TRUE;
    yghv_trace_u64("os resident enter", core);
    if (core < SVM_MAX_CORES)
        KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
    svm_trampoline_os_enter(v, 1);
    /* Guest continuation: block forever so the scheduler keeps this core
       running the rest of Windows in guest mode. */
    KeWaitForSingleObject(&g_os_resident_stop_event, Executive,
                          KernelMode, FALSE, NULL);
    for (;;) {
        __asm__ volatile("pause");
    }
}

static VOID yghv_resident_alive_thread(PVOID ctx) {
    LARGE_INTEGER delay;
    ULONG seconds = 0;
    (void)ctx;

    KeSetSystemAffinityThread((KAFFINITY)1);
    delay.QuadPart = -5LL * 10000000LL;
    for (;;) {
        if (g_os_guest_stop_requested)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        seconds += 5;
        yghv_trace_u64("resident alive", seconds);
    }
}

/* D1c: 10ms host-side diagnostic for the B experiments.  The 0x101 crashes
   <100ms after guest entry (16 clock ticks), so even 100ms polling misses it.
   At 10ms we capture a sample within ~10ms of entry: whether the guest VMEXITs
   (last exit/RIP), hits NPFs, and how many INTR exits (= injection attempts).
   Runs on host core 0; file writes safe there (not in the VMEXIT path). */
static VOID yghv_b0_diag_thread(PVOID ctx) {
    LARGE_INTEGER delay;
    svm_vcpu_t *v;
    (void)ctx;

    KeSetSystemAffinityThread((KAFFINITY)1);
    delay.QuadPart = -10LL * 10000LL;   /* 10 ms */
    for (;;) {
        if (g_os_guest_stop_requested)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (g_b0_cloning)
            continue;   /* D4: pause file I/O during the deep clone (0x50 race) */
        yghv_trace_u64("b0 diag npf", g_npf_count);
        yghv_trace_u64("b0 diag npf gpa", g_last_npf_gpa);
        yghv_trace_u64("b0 diag intr", g_intr_exit_count);
        yghv_trace_u64("b0 diag apic mmio", g_v98_apic_mmio_count);
        yghv_trace_u64("b0 diag exits", g_os_resident_exits);
        v = g_vcpu_count > 1 ? svm_core_get_vcpu(1) : NULL;
        if (v) {
            yghv_trace_u64("b0 diag last exit", v->last_exitcode);
            yghv_trace_u64("b0 diag last rip", v->last_rip);
        }
    }
}

static VOID yghv_resident_watchdog_thread(PVOID ctx) {
    LARGE_INTEGER delay;
    ULONG64 last = 0;
    ULONG stall = 0;
    (void)ctx;

    KeSetSystemAffinityThread((KAFFINITY)1);
    delay.QuadPart = -1LL * 10000000LL;
    for (;;) {
        if (g_os_guest_stop_requested)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (!g_os_resident_mode)
            continue;
        if (g_os_resident_exits == last) {
            if (++stall >= 3) {
                /*
                 * 9.334 (Tier 7.2/7.7): degrade before dying.
                 *
                 * Three polls with no VMEXIT progress used to be an immediate
                 * bugcheck. It is a real stall, but the machine is often still
                 * serviceable — the guest is running, we simply stopped seeing
                 * exits, which usually means an intercept was dropped or the
                 * watchdog is racing a paused core. Report it, and only die if
                 * the framework says nothing safe is left.
                 */
                extern int yghv_fail_degrade(int reason, uint64_t a,
                                             uint64_t b, uint64_t c);
                if (!yghv_fail_degrade(2 /* WATCHDOG */,
                                       (uint64_t)g_os_resident_exits,
                                       (uint64_t)stall, 0)) {
                    KeBugCheckEx(0xE2, 0x59475644,
                                 (ULONG_PTR)g_os_resident_exits,
                                 (ULONG_PTR)stall, 0);
                }
                stall = 0;
            }
        } else {
            last = g_os_resident_exits;
            stall = 0;
        }
    }
}

/* D3: controlled interrupt injection.  Every 100ms, re-inject the most recent
   intercepted interrupt (exitintinfo, when valid) into the guest via VMCB
   event_injection, while host ISR keeps servicing the real clock.  Tests
   whether the guest can handle a SINGLE injected interrupt in guest mode
   without being flooded — with the machine kept alive by host ISR so polling
   can observe it. */
static VOID yghv_b0_injector_thread(PVOID ctx) {
    LARGE_INTEGER delay;
    svm_vcpu_t *v;
    (void)ctx;

    KeSetSystemAffinityThread((KAFFINITY)1);
    delay.QuadPart = -100LL * 10000LL;   /* 100 ms */
    for (;;) {
        if (g_os_guest_stop_requested)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        v = g_vcpu_count > 1 ? svm_core_get_vcpu(1) : NULL;
        if (v && g_v98_apic_shadow &&
            (v->vmcb->control.exitintinfo & (1ULL << 31))) {
            v->vmcb->control.event_injection = v->vmcb->control.exitintinfo;
        }
    }
}

static void yghv_v100_dump(svm_vcpu_t *vcpu) {
    uint64_t seq;
    uint64_t start;
    uint64_t n;
    ULONG i;

    if (!vcpu)
        return;
    seq = vcpu->v100_seq;
    if (seq == 0)
        return;
    yghv_trace("v100 freeze site");
    yghv_trace_u64("v100 seq", seq);
    yghv_trace_u64("v100 exits", vcpu->resident_exits);
    yghv_trace_u64("v100 intr exits", vcpu->resident_interrupt_exits);
    yghv_trace_u64("v100 msr exits", vcpu->resident_msr_exits);
    yghv_trace_u64("v100 cr exits", vcpu->resident_cr_exits);
    yghv_trace_u64("v100 last exitcode",
        vcpu->vmcb->control.exitcode);
    yghv_trace_u64("v100 last rip", vcpu->vmcb->state.rip);
    yghv_trace_u64("v100 last cr3", vcpu->vmcb->state.cr3);
    yghv_trace_u64("v100 last rsp", vcpu->vmcb->state.rsp);
    yghv_trace_u64("v100 last rflags", vcpu->vmcb->state.rflags);
    yghv_trace_u64("v100 last cpl", vcpu->vmcb->state.cpl);

    start = (seq >= YGHV_V100_RING_ENTRIES)
                ? seq - (YGHV_V100_RING_ENTRIES - 1)
                : 0;
    n = seq - start + 1;
    for (i = 0; i < n; i++) {
        uint64_t s = start + i;
        svm_v100_ring_entry_t *e = &vcpu->v100_ring[s % YGHV_V100_RING_ENTRIES];
        if (e->seq != s)
            continue;
        yghv_trace_u64("v100 seq", s);
        yghv_trace_u64("v100 exit", e->exitcode);
        yghv_trace_u64("v100 i1", e->exitinfo1);
        yghv_trace_u64("v100 i2", e->exitinfo2);
        yghv_trace_u64("v100 rip", e->rip);
        yghv_trace_u64("v100 cr3", e->cr3);
        yghv_trace_u64("v100 rsp", e->rsp);
        yghv_trace_u64("v100 rf", e->rflags);
        yghv_trace_u64("v100 cpl", e->cpl);
    }
}

static VOID yghv_v100_monitor_thread(PVOID ctx) {
    LARGE_INTEGER delay;
    ULONG64 last_exits = 0;
    uint64_t last_seq = 0;
    ULONG stall = 0;
    BOOLEAN dumped = FALSE;
    (void)ctx;

    KeSetSystemAffinityThread((KAFFINITY)1);
    delay.QuadPart = -250LL * 10000LL;
    for (;;) {
        svm_vcpu_t *v;

        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (!g_v100_monitor_active)
            continue;
        if (!g_v100_guest_entered)
            continue;
        v = svm_core_get_vcpu(1);
        if (!v) {
            if (++stall >= 4) {
                yghv_trace("v100 vcpu1 unavailable");
                break;
            }
            continue;
        }
        yghv_trace_u64("v100 pulse exits", v->resident_exits);
        yghv_trace_u64("v100 pulse seq", v->v100_seq);
        yghv_trace_u64("v100 pulse last exit", v->vmcb->control.exitcode);
        yghv_trace_u64("v100 pulse rip", v->vmcb->state.rip);
        yghv_trace_u64("v100 pulse cr3", v->vmcb->state.cr3);
        yghv_trace_u64("v100 pulse rsp", v->vmcb->state.rsp);
        if (v->resident_exits == last_exits && v->v100_seq == last_seq) {
            if (++stall >= 4 && !dumped) {
                yghv_trace_u64("v100 stall exits", v->resident_exits);
                yghv_trace_u64("v100 stall ticks",
                               (uint64_t)__rdtsc());
                yghv_v100_dump(v);
                dumped = TRUE;
            }
        } else {
            last_exits = v->resident_exits;
            last_seq = v->v100_seq;
            stall = 0;
        }
    }
}

/* 9.185 B-1min: xAPIC MMIO shadow.  NPT-remaps guest 0xFEE00000 to a private
   shadow RAM page so the guest's APIC register access (incl. the ISR EOI write)
   never touches the PHYSICAL APIC in guest mode (the errata 1363 path).  The
   real APIC is mapped host-side; yghv_v98_apic_scan_forward (on VMEXIT)
   forwards TPR/ICR/timer changes.  EOI is intentionally NOT forwarded here
   (B-1min tests whether avoiding physical APIC access alone prevents the
   freeze). */
static void yghv_v98_apic_shadow_apply(svm_vcpu_t *v) {
    PHYSICAL_ADDRESS apic_pa;

    if (!g_v98_apic_shadow || !v || g_v98_apic_shadow_va)
        return;
    apic_pa.QuadPart = 0xFEE00000ULL;
    g_v98_apic_shadow_va = MmAllocateContiguousMemory(
        HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!g_v98_apic_shadow_va) {
        LOG_ERROR("v98: shadow page alloc failed");
        return;
    }
    RtlZeroMemory(g_v98_apic_shadow_va, HV_PAGE_SIZE);
    g_v98_apic_shadow_pa = MmGetPhysicalAddress(g_v98_apic_shadow_va).QuadPart;
    g_v98_real_apic_va = MmMapIoSpace(apic_pa, HV_PAGE_SIZE, MmNonCached);
    if (!g_v98_real_apic_va) {
        LOG_ERROR("v98: map real APIC failed");
        return;
    }
    RtlCopyMemory(g_v98_apic_shadow_va, g_v98_real_apic_va, HV_PAGE_SIZE);
    g_v98_last_tpr = READ_REGISTER_ULONG(
        (PULONG)((ULONG_PTR)g_v98_real_apic_va + APIC_OFFSET_TPR));
    g_v98_last_icrl = READ_REGISTER_ULONG(
        (PULONG)((ULONG_PTR)g_v98_real_apic_va + APIC_OFFSET_ICRL));
    g_v98_last_icrh = READ_REGISTER_ULONG(
        (PULONG)((ULONG_PTR)g_v98_real_apic_va + APIC_OFFSET_ICRH));
    g_v98_last_lvtt = READ_REGISTER_ULONG(
        (PULONG)((ULONG_PTR)g_v98_real_apic_va + APIC_OFFSET_LVTT));
    g_v98_last_tmict = READ_REGISTER_ULONG(
        (PULONG)((ULONG_PTR)g_v98_real_apic_va + APIC_OFFSET_TMICT));
    g_v98_last_tdcr = READ_REGISTER_ULONG(
        (PULONG)((ULONG_PTR)g_v98_real_apic_va + APIC_OFFSET_TDCR));
    /* 9.206 v98fix: the xAPIC page (0xFEE00000) is MMIO, not RAM, so
       yghv_npt_map_ram (MmGetPhysicalMemoryRanges) leaves its 2MB PD entry
       unmapped on VMs whose RAM layout does not cover it (bare metal 16GB
       happened to).  Map the 2MB page identity first so npt_map_page's
       npt_split_2mb_to_4kb finds a present PD entry; then remap the single
       4K APIC page to the shadow. */
    if (npt_identity_map_range(&g_npt, 0xFEE00000ULL,
                               0xFEE00000ULL + HV_PAGE_SIZE) != STATUS_SUCCESS)
        LOG_ERROR("v98: identity-map APIC 2MB failed");
    /* 9.212 变体 C (对照): YGHV_APIC_IDENTITY=1 构建时 APIC 影子不重定向到 shadow
       RAM 页，而是把 0xFEE00000 映射到自身（identity）并清 W 位——guest 写 APIC
       仍触发 NPF 由 handler 转发，但 NPT 目标不变。区分"重定向到 shadow"vs
       "拦截 APIC 写"哪个是 VM 里的冻结点。 */
#ifndef YGHV_APIC_IDENTITY
    if (npt_map_page(&g_npt, 0xFEE00000ULL, g_v98_apic_shadow_pa,
                     (NPT_4K_PAGE_FLAGS & ~NPT_PERM_WRITABLE) |
                     (1ULL << 4) | (1ULL << 63)) != STATUS_SUCCESS)
        LOG_ERROR("v98: npt_map_page failed");
#else
    if (npt_map_page(&g_npt, 0xFEE00000ULL, 0xFEE00000ULL,
                     (NPT_4K_PAGE_FLAGS & ~NPT_PERM_WRITABLE) |
                     (1ULL << 4) | (1ULL << 63)) != STATUS_SUCCESS)
        LOG_ERROR("v98 ctlC: npt_map_page(identity) failed");
#endif
    v->vmcb->control.tlb_control = SVM_TLB_CONTROL_FLUSH;
    yghv_trace_u64("v98 apic shadow armed", g_v98_apic_shadow_pa);
}

static VOID yghv_os_guest_resident_spin_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    svm_vcpu_t *v;
    uint32_t a, b, c, d;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    v = svm_core_get_vcpu(core);
    if (!v || g_vcpu_count <= core) {
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }
    svm_prepare_vcpu(v, (uint64_t)svm_os_seamless_cont);
    v->vmcb->state.rip = (uint64_t)svm_os_seamless_cont;
    v->vmcb->state.rsp = 0;
    v->vmcb->control.general1_intercepts =
        INTERCEPT_CPUID | INTERCEPT_RDTSC |
        INTR_GEN1(SVM_INTERCEPT_SHUTDOWN) |
        INTR_GEN1(SVM_INTERCEPT_MSR_PROT);
    if (g_os_guest_intr_intercept) {
        v->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_INTR) | INTR_GEN1(SVM_INTERCEPT_NMI);
    }
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (g_v97_hlt_intercept) {
        v->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_HLT);
        v->vmcb->control.general2_intercepts |=
            INTR_GEN2(SVM_INTERCEPT_MWAIT) |
            INTR_GEN2(SVM_INTERCEPT_MWAIT_COND);
    }
    v->vmcb->control.exception_intercepts = 0;
    v->vmcb->control.tlb_control = 0;
    v->vmcb->control.vmcb_clean_bits = 0;
    v->resident_index = core;
    svm_core_set_npt(core, g_npt.pml4_pa);
    yghv_os_guest_tlb_hygiene_apply(v, core);
    yghv_os_guest_tss_isolate_apply(v);
    yghv_os_guest_cr3_intercept_apply(v);
    yghv_os_guest_clone_cr3_apply(v);
    yghv_v98_apic_shadow_apply(v);
    g_os_resident_mode = TRUE;
    yghv_trace_u64("os resident spin enter", core);
    if (core < SVM_MAX_CORES)
        KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
    svm_trampoline_os_enter(v, 1);
    /* Guest continuation: spin without blocking so the scheduler never
       switches this core to another thread while in guest mode. */
    for (;;) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                 : "a"(1) : "memory");
        (void)__rdtsc();
        if (g_v96_apic_tpr_stress && g_v96_apic_tpr_va) {
            *(volatile ULONG *)((ULONG_PTR)g_v96_apic_tpr_va + APIC_OFFSET_TPR) = 0;
        }
    }
}

static VOID yghv_os_guest_allcore_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    svm_vcpu_t *v;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    v = svm_core_get_vcpu(core);
    if (!v || g_vcpu_count <= core) {
        InterlockedExchange(&g_v99_allcore_abort, 1);
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }
    svm_prepare_vcpu(v, (uint64_t)svm_os_seamless_cont);
    v->vmcb->state.rip = (uint64_t)svm_os_seamless_cont;
    v->vmcb->state.rsp = 0;
    v->vmcb->control.general1_intercepts =
        INTERCEPT_CPUID | INTERCEPT_RDTSC |
        INTR_GEN1(SVM_INTERCEPT_SHUTDOWN) |
        INTR_GEN1(SVM_INTERCEPT_MSR_PROT);
#ifdef YGHV_HOST_ISR
    v->vmcb->control.general1_intercepts |=
        INTR_GEN1(SVM_INTERCEPT_INTR) | INTR_GEN1(SVM_INTERCEPT_NMI);
#endif
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    v->vmcb->control.exception_intercepts = 0;
    v->vmcb->control.tlb_control = 0;
    v->vmcb->control.vmcb_clean_bits = 0;
    v->resident_index = core;
    svm_core_set_npt(core, g_npt.pml4_pa);
    /* 9.216/9.217 变体 G: step99 全核形态复用变体 E/F 的"最小干预"门控——
       APIC 直通（不清 W 不拦截）+ 宿主 CR3 直通（不拦截不克隆）。全核（core0+
       core1）都进 guest 态后，跨核 IPI 在两个 guest 态 vCPU 之间走 VMware 模拟
       路径，消除"一核 guest + 一核 host"的跨核死锁（9.216 实锤）。 */
#ifdef YGHV_APIC_PASSTHROUGH
    if (npt_identity_map_range(&g_npt, 0xFEE00000ULL,
                               0xFEE00000ULL + HV_PAGE_SIZE) != STATUS_SUCCESS)
        LOG_ERROR("ctlG: identity-map APIC 2MB failed");
    yghv_trace("ctlG apic passthrough mapped");
#endif
    yghv_os_guest_tlb_hygiene_apply(v, core);
    yghv_os_guest_tss_isolate_apply(v);
#ifndef YGHV_NO_CR3_INTERCEPT
    yghv_os_guest_cr3_intercept_apply(v);
    yghv_os_guest_clone_cr3_apply(v);
#else
    yghv_trace("ctlG: host cr3 passthrough (no intercept/clone)");
#endif
    g_os_resident_mode = TRUE;

    InterlockedIncrement(&g_v99_allcore_ready);
    while (!g_v99_allcore_go && !g_v99_allcore_abort) {
        __asm__ volatile("pause");
    }
    if (g_v99_allcore_abort) {
        yghv_trace_u64("allcore abort", core);
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
    }

    yghv_trace_u64("allcore enter", core);
    if (core < SVM_MAX_CORES)
        KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
    svm_trampoline_os_enter(v, 1);
    /* Guest continuation: block so the scheduler switches threads on this core. */
    KeWaitForSingleObject(&g_os_resident_stop_event, Executive,
                          KernelMode, FALSE, NULL);
    for (;;) {
        __asm__ volatile("pause");
    }
}

/* 9.229c2: observer thread. The VMEXIT window must stay pure memory (c2 lesson:
 * a file write in the window blocked on a file lock, the context switch then
 * #GP'd in KiAbProcessContextSwitch -> 0x139). The observer runs in NORMAL
 * host context and flushes the guest core's latest exit/counter/RIP to the
 * progress log every ~500ms: a silent lock leaves the last flushed state on
 * disk within 0.5s of death, which dates the death relative to guest polling. */
static VOID yghv_s203_observer_thread(PVOID ctx) {
    uint32_t watch_core = (uint32_t)(uintptr_t)ctx;
    LARGE_INTEGER delay;
    ULONG ticks = 0;
    uint64_t reported_unexpected = 0;

    KeSetSystemAffinityThread((KAFFINITY)1);
    delay.QuadPart = -5LL * 10000LL;   /* 5ms sample */
    for (;;) {
        svm_vcpu_t *v;
        if (g_os_guest_stop_requested)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (g_s203_unexpected_exit &&
            g_s203_unexpected_exit != reported_unexpected) {
            reported_unexpected = g_s203_unexpected_exit;
            yghv_trace_u64("s203 UNEXPECTED EXIT", g_s203_unexpected_exit);
        }
        if (++ticks < 100)
            continue;                  /* flush once per ~500ms */
        ticks = 0;
        v = svm_core_get_vcpu(watch_core);
        if (!v)
            continue;
        yghv_trace_u64("s203 obs exits", v->resident_exits);
        yghv_trace_u64("s203 obs last", v->last_exitcode);
        yghv_trace_u64("s203 obs rip", v->last_rip);
    }
}

/* 9.227 step203 — SimpleSvm 等价构型（全减法实验，YGHV_BAREMETAL_STEP=203）。
 *
 * 背景：此前全部裸机常驻系（v95/99、step17、变体 E/F/M）都保留了
 * "INTR/NMI 拦截 + 宿主 ISR"或其他高频退出（RDTSC/MSR_PROT/CR3 拦截+克隆）。
 * 而在 AMD 硬件上被实机验证可用的 OS-as-guest 实现（上游 SimpleSvm 驱动态、
 * HelloAmdHv UEFI 态）只拦 CPUID/VMRUN(/EFER 写)，中断经 virtual wire 由
 * guest 原生服务，CR3/TSS/ASID 一概不动。该构型从未在本机裸机测试过，
 * step203 补上这一格（C1；C0 对照 = 直接实机运行原版 SimpleSvm，
 * 见 docs/YGHV_SIMPLEVM_LEVERAGE_20260918.md）。
 *
 * 相对 step99 线程的纯减法清单：
 * - 拦截集 = CPUID(general1) + VMRUN|VMMCALL(general2，VMMCALL 属 fail-safe，
 *   guest 态 Windows 不会执行)；去掉 RDTSC/SHUTDOWN/MSR_PROT/INTR/NMI，
 *   异常/CR/DR 全 0。MSR_PROT 位为 0 时 MSRPM 内容（VM_CR/GS_BASE 位）惰性，
 *   所有 MSR 直通。
 * - 不做 tlb hygiene / TSS 隔离 / CR3 拦截 / CR3 克隆 / APIC 影子。
 * - svm_trampoline_os_enter(v,1) 保留 IF → 物理中断由 guest（真实 Windows）
 *   的 IDT 直接服务，VMM 不介入（与 B/D 系"宿主 ISR 代服务"路线分道）。
 * - guest 延续体只做 ~10ms 节流的 CPUID(1) 心跳轮询：空闲时每核每 10ms 一次
 *   VMEXIT，处理路径（svm_dispatch_exit CPUID 分支）为纯内存 + __cpuidex，
 *   无任何 NT API（SimpleSvm issue #1 的 VMEXIT 岛纪律）。停止契约沿用
 *   9.162/9.163：DriverUnload 置 g_os_guest_stop_requested → 下一次轮询
 *   退出返回 1 → 本核 host_done 反虚拟化并线程退出；per-core EFER.SVME/
 *   VM_HSAVE 由 svm_core_cleanup 的 IPI（svm_core_ipi_cleanup）恢复。
 * - 全核同时进入（v99 barrier 机制），排除"一核 guest + 一核 host"混态。
 */
static VOID yghv_os_guest_simplevm_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    svm_vcpu_t *v;
    uint64_t t0;
    uint32_t a, b, c, d;
    static volatile LONG s203_dumped = 0;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    v = svm_core_get_vcpu(core);
    if (!v || g_vcpu_count <= core) {
        InterlockedExchange(&g_s203_abort, 1);
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }
    svm_prepare_vcpu(v, (uint64_t)svm_os_seamless_cont);
    v->vmcb->state.rip = (uint64_t)svm_os_seamless_cont;
    v->vmcb->state.rsp = 0;
    /* 纯减法拦截集（对比 step99 线程的 CPUID|RDTSC|SHUTDOWN|MSR_PROT 组合）。 */
    v->vmcb->control.general1_intercepts = INTERCEPT_CPUID;
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    v->vmcb->control.exception_intercepts = 0;
    v->vmcb->control.cr_read_intercepts = 0;
    v->vmcb->control.cr_write_intercepts = 0;
    v->vmcb->control.dr_read_intercepts = 0;
    v->vmcb->control.dr_write_intercepts = 0;
    v->vmcb->control.tlb_control = 0;
    v->vmcb->control.vmcb_clean_bits = 0;
    v->resident_index = core;
    svm_core_set_npt(core, g_npt.pml4_pa);
    g_os_resident_mode = TRUE;

    InterlockedIncrement(&g_s203_ready);
    while (!g_s203_go && !g_s203_abort) {
        __asm__ volatile("pause");
    }
    if (g_s203_abort) {
        yghv_trace_u64("s203 abort", core);
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
    }

    yghv_trace_u64("s203 enter", core);
    if (core < SVM_MAX_CORES)
        KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
    /* 9.229: write the exact VMCB state going into VMRUN so a frozen boot can
     * be diffed offline against SimpleSvm's VMCB (the only known-good AMD
     * OS-as-guest on this silicon). Only the first core logs the full dump to
     * keep the write-through log small. */
    if (!InterlockedExchange(&s203_dumped, 1)) {
        volatile uint8_t *ctl = (volatile uint8_t *)v->vmcb;
        yghv_trace_u64("s203 VMCB g1", *(volatile uint32_t *)(ctl + 0x0C));
        yghv_trace_u64("s203 VMCB g2", *(volatile uint32_t *)(ctl + 0x10));
        yghv_trace_u64("s203 VMCB exc", *(volatile uint32_t *)(ctl + 0x08));
        yghv_trace_u64("s203 VMCB asid", *(volatile uint32_t *)(ctl + 0x58));
        yghv_trace_u64("s203 VMCB tlb", *(volatile uint8_t *)(ctl + 0x5C));
        yghv_trace_u64("s203 VMCB ncr3", *(volatile uint64_t *)(ctl + 0xB0));
        yghv_trace_u64("s203 VMCB np_en", *(volatile uint64_t *)(ctl + 0x90));
        yghv_trace_u64("s203 VMCB cr3", v->vmcb->state.cr3);
        yghv_trace_u64("s203 VMCB efer", v->vmcb->state.efer);
        yghv_trace_u64("s203 VMCB rflags", v->vmcb->state.rflags);
    }
    svm_trampoline_os_enter(v, 1);   /* IF=1: guest services interrupts natively */

    /* ---- from here on this code executes in GUEST mode on this core ---- */
    if (g_s204_ret_immediately) {
        /* 9.231 step204: hand this core straight back to the guest OS. The
         * per-core host loop inside svm_trampoline_os_enter persists and
         * services only synchronous CPUID/VMRUN VMEXITs; nothing of ours runs
         * continuously on the core and no core stays in host mode — the exact
         * shape C0's SimpleSvm has and that survives on this silicon. The guest
         * thread terminates normally IN guest context (a plain Windows thread
         * exit), so the host_stack (separately allocated in svm_prepare_vcpu)
         * is unaffected. Reboot clears this experiment (no fast stop channel). */
        yghv_trace_u64("s204 ret-to-guest", core);
        PsTerminateSystemThread(STATUS_SUCCESS);
    }
#ifdef YGHV_203Q
    /* 203-q single-variable experiment (hypothesis: the busy `pause` spin-loop
     * this yghv thread runs IN guest is what, once the guest also takes a real
     * clock interrupt/DPC on this core, leaves the IRQL/DPC handoff unclean ->
     * 0xB8 ATTEMPTED_SWITCH_FROM_DPC). Replace busy-wait with a NORMAL sleep so
     * this core yields like any other guest thread. Keep one CPUID/iteration as
     * the stop-sensing VMEXIT (dispatch returns 1 on stop -> host_done). */
    for (;;) {
        LARGE_INTEGER nap;
        if (g_os_guest_stop_requested)
            break;
        nap.QuadPart = -5LL * 10000LL;   /* 5 ms, like a normal guest worker */
        KeDelayExecutionThread(KernelMode, FALSE, &nap);
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1) : "memory");
    }
#else
    for (;;) {
        t0 = __rdtsc();
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1) : "memory");
        while ((__rdtsc() - t0) < YGHV_S203_POLL_TSC)
            __asm__ volatile("pause");
    }
#endif
}

static VOID yghv_os_guest_resident_delay_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    svm_vcpu_t *v;
    uint32_t a, b, c, d;
    LARGE_INTEGER delay;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    v = svm_core_get_vcpu(core);
    if (!v || g_vcpu_count <= core) {
        if (core < SVM_MAX_CORES)
            KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }
    svm_prepare_vcpu(v, (uint64_t)svm_os_seamless_cont);
    v->vmcb->state.rip = (uint64_t)svm_os_seamless_cont;
    v->vmcb->state.rsp = 0;
    v->vmcb->control.general1_intercepts =
        INTERCEPT_CPUID | INTERCEPT_RDTSC |
        INTR_GEN1(SVM_INTERCEPT_SHUTDOWN) |
        INTR_GEN1(SVM_INTERCEPT_MSR_PROT);
    if (g_os_guest_intr_intercept) {
        v->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_INTR) | INTR_GEN1(SVM_INTERCEPT_NMI);
    }
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    if (g_v97_hlt_intercept) {
        v->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_HLT);
        v->vmcb->control.general2_intercepts |=
            INTR_GEN2(SVM_INTERCEPT_MWAIT) |
            INTR_GEN2(SVM_INTERCEPT_MWAIT_COND);
    }
    v->vmcb->control.exception_intercepts = 0;
    if (g_v101_gp_intercept) {
        /* Turn the context-switch fault into a VMEXIT so we can capture the
           site before Windows exception dispatch destroys the stack. */
        v->vmcb->control.exception_intercepts =
            (1ULL << 8) | (1ULL << 11) | (1ULL << 12) | (1ULL << 13);
    }
    if (g_v102_catchall) {
        /* v102: catch every guest fault vector plus HLT. If the machine
           still stops without any of these, it is a platform-level halt. */
        v->vmcb->control.exception_intercepts = 0xFFFFFFFFULL;
        v->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_HLT);
    }
    v->vmcb->control.tlb_control = 0;
    v->vmcb->control.vmcb_clean_bits = 0;
    v->resident_index = core;
    svm_core_set_npt(core, g_npt.pml4_pa);
    yghv_os_guest_tlb_hygiene_apply(v, core);
    yghv_os_guest_tss_isolate_apply(v);
    yghv_os_guest_cr3_intercept_apply(v);
    yghv_os_guest_clone_cr3_apply(v);
    yghv_v98_apic_shadow_apply(v);
    g_os_resident_mode = TRUE;
    yghv_trace_u64("os resident delay enter", core);
    g_v100_guest_entered = TRUE;
    if (core < SVM_MAX_CORES)
        KeSetEvent(&g_os_guest_done_events[core], IO_NO_INCREMENT, FALSE);
    svm_trampoline_os_enter(v, 1);
    /* Guest continuation: block once so the scheduler performs at least one
       real context switch inside guest mode, then return to the proven spin
       shape with the host-ISR path. */
    delay.QuadPart = -1LL * 10000000LL;
    if (!g_os_guest_delay_quiet)
        yghv_trace("os resident delay guest block");
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    if (!g_os_guest_delay_quiet)
        yghv_trace("os resident delay guest wake");
    for (;;) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                 : "a"(1) : "memory");
        (void)__rdtsc();
    }
}

/* 9.180 MSV: same-process scheduler-switch helpers.  Pinned to the guest core,
   each loops on a 1 ms delay.  When the OS-as-guest delay thread blocks, the
   scheduler (running inside guest mode) switches among these System-process
   threads — same CR3, no CR3 write — exercising KiSwapThread on the independent
   deep-copied CR3, the exact combination never tested before. */
static VOID yghv_msv_helper_thread(PVOID ctx) {
    uint32_t core = (uint32_t)(uintptr_t)ctx;
    LARGE_INTEGER t;

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << core));
    t.QuadPart = -1LL * 10000LL;           /* 1 ms */
    while (!g_msv_helper_stop) {
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void *yghv_avic_alloc_page(uint64_t *pa_out) {
    void *page;

    page = MmAllocateContiguousMemory(HV_PAGE_SIZE,
        (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!page)
        return NULL;
    RtlZeroMemory(page, HV_PAGE_SIZE);
    *pa_out = MmGetPhysicalAddress(page).QuadPart;
    return page;
}

static void yghv_avic_cleanup(svm_vcpu_t *vcpu) {
    if (!vcpu)
        return;
    if (vcpu->avic_host_apic_va) {
        MmUnmapIoSpace(vcpu->avic_host_apic_va, HV_PAGE_SIZE);
        vcpu->avic_host_apic_va = NULL;
    }
    if (vcpu->avic_physical_id_table) {
        MmFreeContiguousMemory(vcpu->avic_physical_id_table);
        vcpu->avic_physical_id_table = NULL;
        vcpu->avic_physical_id_pa = 0;
    }
    if (vcpu->avic_logical_id_table) {
        MmFreeContiguousMemory(vcpu->avic_logical_id_table);
        vcpu->avic_logical_id_table = NULL;
        vcpu->avic_logical_id_pa = 0;
    }
    if (vcpu->avic_backing_page) {
        MmFreeContiguousMemory(vcpu->avic_backing_page);
        vcpu->avic_backing_page = NULL;
        vcpu->avic_backing_pa = 0;
    }
}

/*
 * 9.388b: a NON-STATIC alias for yghv_avic_cleanup, so svm_core.c can call it from
 * the unload teardown. The function above is static and that file is a separate
 * translation unit, so the release path had no way to reach it - which is exactly
 * why the AVIC pages were leaked on every unload.
 */
void yghv_avic_cleanup_release(svm_vcpu_t *v) {
    yghv_avic_cleanup(v);
}

static uint32_t yghv_avic_ffs_u32(uint32_t value) {
    uint32_t i;
    for (i = 0; i < 32; i++) {
        if (value & (1U << i))
            return i;
    }
    return 0;
}

static NTSTATUS yghv_avic_prepare(svm_vcpu_t *vcpu, uint32_t core) {
    int cpu_info[4];
    KAFFINITY old_affinity;
    uint32_t guest_apic_id, host_apic_id;
    uint32_t ldr, dfr, logical_id, table_index;
    uint64_t entry;
    PHYSICAL_ADDRESS apic_pa;
    NTSTATUS st = STATUS_SUCCESS;

    if (!vcpu)
        return STATUS_INVALID_PARAMETER;

    __cpuidex(cpu_info, CPUID_AMD_NPT, 0);
    if (!(cpu_info[3] & CPUID_NPT_FEATURE_AVIC)) {
        LOG_ERROR("step25: AVIC unsupported by CPU");
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    vcpu->avic_backing_page = yghv_avic_alloc_page(&vcpu->avic_backing_pa);
    if (!vcpu->avic_backing_page) { st = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    vcpu->avic_logical_id_table = yghv_avic_alloc_page(&vcpu->avic_logical_id_pa);
    if (!vcpu->avic_logical_id_table) { st = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    vcpu->avic_physical_id_table = yghv_avic_alloc_page(&vcpu->avic_physical_id_pa);
    if (!vcpu->avic_physical_id_table) { st = STATUS_INSUFFICIENT_RESOURCES; goto fail; }

    old_affinity = KeSetSystemAffinityThreadEx((KAFFINITY)(1ULL << core));
    __cpuidex(cpu_info, 1, 0);
    guest_apic_id = ((uint32_t)cpu_info[1] >> 24) & 0xFF;
    KeSetSystemAffinityThread(old_affinity);
    if (guest_apic_id == 0xFF) {
        LOG_ERROR("step25: guest APIC ID 0xFF reserved");
        st = STATUS_INVALID_PARAMETER;
        goto fail;
    }
    host_apic_id = guest_apic_id;
    vcpu->avic_apic_id = host_apic_id;

    apic_pa.QuadPart = APIC_DEFAULT_PHYS_BASE;
    vcpu->avic_host_apic_va = MmMapIoSpace(apic_pa, HV_PAGE_SIZE, MmNonCached);
    if (!vcpu->avic_host_apic_va) {
        LOG_ERROR("step25: map host APIC failed");
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    RtlCopyMemory(vcpu->avic_backing_page, vcpu->avic_host_apic_va, HV_PAGE_SIZE);

    ldr = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)vcpu->avic_host_apic_va + APIC_OFFSET_LDR));
    dfr = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)vcpu->avic_host_apic_va + APIC_OFFSET_DFR));

    entry = (vcpu->avic_backing_pa & AVIC_PHYSICAL_ID_ENTRY_BACKING_PAGE_MASK) |
            AVIC_PHYSICAL_ID_ENTRY_VALID |
            AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING |
            (host_apic_id & AVIC_PHYSICAL_ID_ENTRY_HOST_ID_MASK);
    ((uint64_t *)vcpu->avic_physical_id_table)[guest_apic_id] = entry;

    logical_id = (ldr >> 24) & 0xFF;
    if (logical_id) {
        if (dfr == 0xFFFFFFFF) {
            table_index = yghv_avic_ffs_u32(logical_id) * 4;
        } else {
            uint32_t cluster = (logical_id >> 4) & 0xF;
            uint32_t apic_ix = yghv_avic_ffs_u32(logical_id & 0xF);
            table_index = cluster < 15 ? cluster * 16 + apic_ix * 4 : 0;
        }
        if (table_index + 4 <= HV_PAGE_SIZE) {
            *(volatile uint32_t *)((ULONG_PTR)vcpu->avic_logical_id_table + table_index) =
                AVIC_LOGICAL_ID_ENTRY_VALID |
                (guest_apic_id & AVIC_LOGICAL_ID_ENTRY_GUEST_ID_MASK);
        }
    }

    vcpu->vmcb->control.avic_apic_bar = APIC_DEFAULT_PHYS_BASE;
    vcpu->vmcb->control.avic_backing_page = vcpu->avic_backing_pa;
    vcpu->vmcb->control.avic_logical_id = vcpu->avic_logical_id_pa;
    vcpu->vmcb->control.avic_physical_id =
        (vcpu->avic_physical_id_pa & AVIC_PHYSICAL_ID_ENTRY_BACKING_PAGE_MASK) |
        (0xFE & AVIC_PHYSICAL_MAX_INDEX_MASK);
    vcpu->vmcb->control.vintr |= SVM_INT_CTL_AVIC_ENABLE;
    vcpu->vmcb->control.vintr &= ~SVM_INT_CTL_X2APIC_MODE;
    vcpu->vmcb->control.vmcb_clean_bits = 0;

    yghv_trace_u64("avic prepare backing", vcpu->avic_backing_pa);
    yghv_trace_u64("avic prepare apicid", guest_apic_id);
    yghv_trace_u64("avic ctl vintr", vcpu->vmcb->control.vintr);
    yghv_trace_u64("avic bar", vcpu->vmcb->control.avic_apic_bar);
    yghv_trace_u64("avic backing", vcpu->vmcb->control.avic_backing_page);
    yghv_trace_u64("avic phys", vcpu->vmcb->control.avic_physical_id);
    yghv_trace_u64("avic log", vcpu->vmcb->control.avic_logical_id);
    return STATUS_SUCCESS;

fail:
    yghv_avic_cleanup(vcpu);
    return st;
}

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} yghv_idt_entry_t;

_Static_assert(sizeof(yghv_idt_entry_t) == 16, "IDT entry size");

static void yghv_avic_read_idtr(uint64_t *base, uint16_t *limit) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    __asm__ volatile("sidt %0" : "=m"(idtr));
    *limit = idtr.limit;
    *base = idtr.base;
}

static void yghv_avic_restore_idt(svm_vcpu_t *vcpu) {
    if (!vcpu || !vcpu->avic_host_idt)
        return;
    ExFreePoolWithTag(vcpu->avic_host_idt, YGHV_TAG);
    vcpu->avic_host_idt = NULL;
    g_avic_eoi_apic_va = NULL;
}

static NTSTATUS yghv_avic_install_eoi_idt(svm_vcpu_t *vcpu) {
    yghv_idt_entry_t *idt;
    uint64_t handler;
    uint16_t cs;
    int i;

    if (!vcpu || !vcpu->avic_host_apic_va)
        return STATUS_INVALID_PARAMETER;
    if (vcpu->avic_host_idt)
        return STATUS_SUCCESS;

    idt = (yghv_idt_entry_t *)ExAllocatePoolWithTag(NonPagedPool, 0x1000, YGHV_TAG);
    if (!idt)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(idt, 0x1000);

    yghv_avic_read_idtr(&vcpu->avic_old_idt_base, &vcpu->avic_old_idt_limit);
    g_avic_old_idtr.limit = vcpu->avic_old_idt_limit;
    g_avic_old_idtr.base = vcpu->avic_old_idt_base;

    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    handler = (uint64_t)yghv_avic_eoi_isr;
    for (i = 0; i < 256; i++) {
        idt[i].offset_low  = (uint16_t)(handler & 0xFFFF);
        idt[i].selector    = cs;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0x8E;
        idt[i].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
        idt[i].offset_high = (uint32_t)(handler >> 32);
        idt[i].reserved    = 0;
    }

    vcpu->avic_host_idt = idt;
    g_avic_eoi_apic_va = vcpu->avic_host_apic_va;
    g_avic_eoi_idtr.limit = 0xFFF;
    g_avic_eoi_idtr.base = (uint64_t)idt;
    return STATUS_SUCCESS;
}

static NTSTATUS yghv_baremetal_step_test(int step) {
    svm_vcpu_t *v = svm_core_get_vcpu(0);
    void *npf_page = NULL;
    void *prot_page = NULL;
    uint64_t npf_pa = 0;
    NTSTATUS st;

    if (!v)
        return STATUS_NOT_FOUND;
    /* 9.227/9.232: bound raised to 205 to admit step203/204 (SimpleSvm-equivalent)
     * and step205 (faithful SimpleSvm port). */
    if (step > 205)
        return STATUS_NOT_IMPLEMENTED;
    yghv_trace_u64("bm step", (uint64_t)step);
    yghv_trace("bm start");
    g_os_guest_host_isr = FALSE;
    g_os_guest_inject_intr = FALSE;
    g_os_guest_avic_irr_inject = FALSE;
    g_os_guest_avic_eoi_only = FALSE;
    g_os_guest_avic_timer_emu = FALSE;
    g_os_guest_delay_quiet = FALSE;
    g_v96_apic_tpr_stress = FALSE;
    g_v97_hlt_intercept = FALSE;
    g_v98_apic_shadow = FALSE;
    g_v101_gp_intercept = FALSE;
    g_v101_gp_seen = FALSE;
    g_v102_catchall = FALSE;
    g_os_resident_log_active = FALSE;

    /* v96: step20 PASS baseline + persistent guest physical-APIC TPR writes. */
    if (step == 96) {
        g_v96_apic_tpr_stress = TRUE;
        step = 20;
    }
    /* v97: step23 blocking baseline + intercept guest HLT/MWAIT so idle
       instructions are emulated by the VMM instead of halting in guest mode. */
    if (step == 97) {
        g_v97_hlt_intercept = TRUE;
        step = 23;
    }
    /* v98: step23 blocking baseline + NPT-remapped shadow xAPIC page. */
    if (step == 98) {
        g_v98_apic_shadow = TRUE;
        step = 23;
    }
    /* v101: step100 blocking baseline + intercept #DF/#NP/#SS/#GP so the
       guest context-switch fault becomes a VMEXIT we can capture. */
    if (step == 101) {
        g_v101_gp_intercept = TRUE;
        step = 100;
    }
    /* v102: catch all guest faults + HLT to prove or rule out an
       interceptable exception as the machine-stop cause. */
    if (step == 102) {
        g_v102_catchall = TRUE;
        step = 100;
    }

    /* 9.180 MSV: minimal shadow validation.  step23 shape (blocking delay
       resident on core 1) but the guest runs on a DEEP-copied fully independent
       page table (yghv_clone_host_cr3_deep via g_msv_test), CR3 writes
       fail-close (svm_handle_cr stops the guest on the first process-switch
       CR3 write), and 3 System-process helper threads force same-process
       scheduler switching on the guest core.  Judge H1 (independent address
       space fixes the freeze -> full shadow paging) vs H2 (errata-class
       context-switch deadlock -> stop the line on this machine). */
    if (step == 200) {
        HANDLE thread = NULL, alive = NULL, watchdog = NULL;
        HANDLE helpers[3] = { NULL, NULL, NULL };
        NTSTATUS st200;
        ULONG h;

        yghv_trace("bm msv start");
        g_msv_test = TRUE;
        g_msv_cr3_seen = FALSE;
        g_msv_cr3_writes = 0;
        g_msv_helper_stop = FALSE;
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_delay_quiet = TRUE;   /* no guest-mode file I/O (0x139 lesson) */
        g_v97_hlt_intercept = TRUE;      /* MSV-2: intercept HLT/MWAIT so guest
                                            idle never actually halts in guest
                                            mode (0x101 fix direction) */
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;

        for (h = 0; h < 3; h++) {
            st200 = PsCreateSystemThread(&helpers[h], THREAD_ALL_ACCESS, NULL,
                                         NULL, NULL, yghv_msv_helper_thread,
                                         (PVOID)(uintptr_t)1);
            if (!NT_SUCCESS(st200)) {
                LOG_ERROR("bm msv: helper %u create failed 0x%x", h, st200);
                break;
            }
        }
        st200 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                     NULL, yghv_os_guest_resident_delay_thread,
                                     (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st200)) {
            LOG_ERROR("bm msv: thread create failed 0x%x", st200);
            goto msv_fail;
        }
        st200 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                     NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st200)) {
            LOG_ERROR("bm msv: alive create failed 0x%x", st200);
            goto msv_fail;
        }
        st200 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                     NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st200)) {
            LOG_ERROR("bm msv: watchdog create failed 0x%x", st200);
            goto msv_fail;
        }
        {
            LARGE_INTEGER msv_timeout;
            NTSTATUS wait_st;

            /* Bounded wait: the guest only stops itself on an MSV CR3-write
               fail-close or a fault.  If H1 holds (independent address space
               fixes the freeze) the guest runs indefinitely doing same-process
               switches, so we must time out and stop it cleanly, otherwise
               DriverEntry would hang in START_PENDING forever. */
            msv_timeout.QuadPart = -60LL * 10000000LL;   /* 60 s */
            wait_st = KeWaitForSingleObject(&g_os_guest_done_events[1],
                                            Executive, KernelMode, FALSE,
                                            &msv_timeout);
            if (wait_st == STATUS_TIMEOUT) {
                yghv_trace("msv survived 60s timeout - H1 supported");
                g_os_guest_stop_requested = TRUE;
                msv_timeout.QuadPart = -10LL * 10000000LL;   /* 10 s grace */
                KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                                      KernelMode, FALSE, &msv_timeout);
            } else {
                yghv_trace("msv stopped by guest event");
            }
        }
        /* Resident guest stopped (host_done) — MSV CR3-write fail-close, a
           fault, or the 60s timeout.  Stop helpers and exit. */
        g_msv_helper_stop = TRUE;
        if (g_msv_cr3_seen)
            yghv_trace_u64("msv stopped on cr3 write", g_msv_cr3_writes);
        else
            yghv_trace("msv stopped without cr3 write");
        if (thread) ZwClose(thread);
        if (alive) ZwClose(alive);
        if (watchdog) ZwClose(watchdog);
        for (h = 0; h < 3; h++)
            if (helpers[h]) ZwClose(helpers[h]);
        g_msv_test = FALSE;
        g_os_resident_mode = FALSE;
        g_os_guest_intr_intercept = FALSE;
        g_os_guest_host_isr = FALSE;
        g_os_guest_delay_quiet = FALSE;
        g_v97_hlt_intercept = FALSE;
        g_os_resident_log_active = FALSE;
        yghv_trace("bm msv done");
        return STATUS_SUCCESS;

    msv_fail:
        g_msv_test = FALSE;
        g_msv_helper_stop = TRUE;
        g_os_resident_mode = FALSE;
        g_os_guest_intr_intercept = FALSE;
        g_os_guest_host_isr = FALSE;
        g_os_guest_delay_quiet = FALSE;
        g_v97_hlt_intercept = FALSE;
        g_os_resident_log_active = FALSE;
        if (thread) ZwClose(thread);
        if (alive) ZwClose(alive);
        if (watchdog) ZwClose(watchdog);
        for (h = 0; h < 3; h++)
            if (helpers[h]) ZwClose(helpers[h]);
        return st200;
    }

    /* 9.184 B-0: Option B first sub-step — guest-mode virtual interrupt
       reception test.  step21 shape (spin guest + INTR/NMI intercept + re-inject
       the intercepted event into the guest via VMCB event injection) combined
       with the MSV deep-copied independent CR3.  This isolates the single
       riskiest assumption of Option B (full APIC virtualization): can the guest
       RECEIVE and handle an interrupt (the clock ISR) in guest mode at all?  If
       it survives 60 s (resident alive keeps advancing, no 0x101) -> guest-mode
       interrupt reception works -> build the full APIC virtualization.  If it
       0x101/freezes -> guest-mode interrupt handling is the wall (errata 1363),
       and no APIC virtualization can fix it (the guest ISR must run in guest
       mode regardless) -> Option B low-value on this CPU, stop. */
    if (step == 202) {
        HANDLE thread = NULL, alive = NULL, watchdog = NULL, diag = NULL;
        HANDLE injector = NULL;
        NTSTATUS st202;

        yghv_trace("bm b0 spin inject start");
        /* 9.210 变体 B (对照): YGHV_NO_MSV_DEEP=1 构建时用浅拷贝 CR3（g_msv_test=FALSE，
           保留 APIC 影子）——变体 A 已证深拷贝是 VM 冻结源，本变体确认"浅拷贝+APIC
           影子"是否可存活。 */
#ifndef YGHV_NO_MSV_DEEP
        /* D4: use the DEEP clone (g_msv_test=TRUE -> yghv_clone_host_cr3_deep) for
           the STABLE base — 9.152 proved deep independent CR3 + spin + host ISR is
           stable (12 cores, 5 min).  The deep clone's slowness/race (D1c 0x50) is
           mitigated by pausing the 10ms diag's file I/O during the clone
           (g_b0_cloning).  Controlled injection is gated by g_b0_inject. */
        g_msv_test = TRUE;
#endif
        /* 9.209 ctl A: YGHV_NO_APIC_SHADOW disables APIC shadow for VM freeze isolation. */
#ifndef YGHV_NO_APIC_SHADOW
        g_v98_apic_shadow = TRUE;   /* B-1min: shadow xAPIC MMIO so guest ISR
                                       EOI/TPR never touch the physical APIC */
#endif
        g_v98_apic_mmio_count = 0;
        g_npf_count = 0;
        g_last_npf_gpa = 0;
        g_last_npf_err = 0;
        g_last_npf_rip = 0;
        g_intr_exit_count = 0;
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        /* D3: host ISR services the real clock (machine stays alive, polling
           works); a controlled injector thread re-injects ONE intercepted
           interrupt into the guest every 100ms to test guest-mode ISR handling
           without flooding. */
        g_os_guest_inject_intr = FALSE;
        g_os_guest_host_isr = TRUE;
        /* g_os_resident_mode deliberately NOT set here: the spin thread sets it
           AFTER the deep clone, so the watchdog skips the (multi-second) page
           table deep-copy setup phase instead of 0xE2-ing on a 3s stall (9.186
           invalid test lesson). */
        g_os_resident_log_active = TRUE;
        st202 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                     NULL, yghv_os_guest_resident_spin_thread,
                                     (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st202)) {
            LOG_ERROR("bm b0: thread create failed 0x%x", st202);
            goto b0_fail;
        }
        st202 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                     NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st202)) {
            LOG_ERROR("bm b0: alive create failed 0x%x", st202);
            goto b0_fail;
        }
        st202 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                     NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st202)) {
            LOG_ERROR("bm b0: watchdog create failed 0x%x", st202);
            goto b0_fail;
        }
        st202 = PsCreateSystemThread(&diag, THREAD_ALL_ACCESS, NULL, NULL,
                                     NULL, yghv_b0_diag_thread, NULL);
        if (!NT_SUCCESS(st202)) {
            LOG_ERROR("bm b0: diag create failed 0x%x", st202);
            goto b0_fail;
        }
        if (g_b0_inject) {   /* D3-base control: injector off by default */
            st202 = PsCreateSystemThread(&injector, THREAD_ALL_ACCESS, NULL, NULL,
                                         NULL, yghv_b0_injector_thread, NULL);
            if (!NT_SUCCESS(st202)) {
                LOG_ERROR("bm b0: injector create failed 0x%x", st202);
                goto b0_fail;
            }
        }
        {
            LARGE_INTEGER b0_timeout;
            NTSTATUS wait_st;

            /* Returns on guest entry (the resident thread signals the event
               before the trampoline); survival is measured by the alive thread
               logging "resident alive" every 5 s on core 0. */
            b0_timeout.QuadPart = -60LL * 10000000LL;   /* 60 s cap */
            wait_st = KeWaitForSingleObject(&g_os_guest_done_events[1],
                                            Executive, KernelMode, FALSE,
                                            &b0_timeout);
            if (wait_st == STATUS_TIMEOUT) {
                yghv_trace("b0 waited 60s");
                g_os_guest_stop_requested = TRUE;
                b0_timeout.QuadPart = -10LL * 10000000LL;
                KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                                      KernelMode, FALSE, &b0_timeout);
            } else {
                yghv_trace("b0 guest entered");
            }
        }
        if (thread) ZwClose(thread);
        if (alive) ZwClose(alive);
        if (watchdog) ZwClose(watchdog);
        if (diag) ZwClose(diag);
        if (injector) ZwClose(injector);
        g_msv_test = FALSE;
        g_v98_apic_shadow = FALSE;
        g_os_resident_mode = FALSE;
        g_os_guest_intr_intercept = FALSE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_host_isr = FALSE;
        g_os_resident_log_active = FALSE;
        yghv_trace_u64("b0 apic mmio count", g_v98_apic_mmio_count);
        yghv_trace_u64("b0 npf count", g_npf_count);
        yghv_trace("bm b0 done");
        return STATUS_SUCCESS;

    b0_fail:
        g_msv_test = FALSE;
        g_v98_apic_shadow = FALSE;
        g_os_resident_mode = FALSE;
        g_os_guest_intr_intercept = FALSE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_host_isr = FALSE;
        g_os_resident_log_active = FALSE;
        if (thread) ZwClose(thread);
        if (alive) ZwClose(alive);
        if (watchdog) ZwClose(watchdog);
        if (diag) ZwClose(diag);
        if (injector) ZwClose(injector);
        return st202;
    }

    if (step == 6) {
        ULONG i;
        for (i = 0; i < g_vcpu_count; i++) {
            svm_vcpu_t *cv = g_vcpus[i];
            if (!cv) continue;
            cv->regs.rcx = g_vmmcall_auth_cookie;
            cv->vmcb->state.rip = (uint64_t)svm_trampoline_test_guest;
            cv->vmcb->state.rax = 0;
            cv->vmcb->control.general2_intercepts =
                INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
        }
        st = svm_core_start_remote_residents(g_vcpu_count);
        if (st)
            return st;
        svm_core_wait_remote_ready(g_vcpu_count);
        yghv_trace("bm multi vmrun");
        svm_core_enter_resident_current(0);
        svm_core_wait_all_stopped(g_vcpu_count);
        yghv_trace("bm multi done");
        return STATUS_SUCCESS;
    }

    if (step == 8) {
        ULONG i;
        yghv_trace("bm persistent start");
        g_resident_workload_page = ExAllocatePoolWithTag(
            NonPagedPool, HV_PAGE_SIZE, YGHV_TAG);
        if (!g_resident_workload_page)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(g_resident_workload_page, HV_PAGE_SIZE);
        st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
        if (!st)
            st = yghv_protect_add_page((uint64_t)g_resident_workload_page);
        if (!st)
            st = yghv_protect_install_hook(0, (uint64_t)yghv_hook_test_dummy);
        if (!st)
            st = yghv_protect_start();
        if (st) {
            LOG_ERROR("bm step 8: setup failed 0x%x", st);
            yghv_protect_cleanup();
            if (g_resident_workload_page) ExFreePoolWithTag(g_resident_workload_page, YGHV_TAG);
            g_resident_workload_page = NULL;
            return st;
        }
        for (i = 0; i < g_vcpu_count; i++) {
            svm_vcpu_t *cv = g_vcpus[i];
            if (!cv) continue;
            cv->regs.rcx = g_vmmcall_auth_cookie;
            cv->vmcb->state.cr3 = g_protect.targets[0].cr3;
            cv->vmcb->control.general1_intercepts = INTERCEPT_CPUID;
            cv->vmcb->control.general2_intercepts =
                INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
            if (i == 0) {
                cv->vmcb->state.rip = (uint64_t)svm_trampoline_test_resident_guest;
                cv->regs.rdi = (uint64_t)g_resident_workload_page;
                cv->regs.rsi = (uint64_t)yghv_hook_test_dummy;
            } else {
                cv->vmcb->state.rip = (uint64_t)svm_trampoline_test_guest;
                cv->regs.rdi = 0;
                cv->regs.rsi = 0;
            }
            cv->vmcb->state.rax = 0;
        }
        st = svm_core_start_persistent_residents(g_vcpu_count);
        if (st) {
            LOG_ERROR("bm step 8: persistent start failed 0x%x", st);
            yghv_protect_cleanup();
            if (g_resident_workload_page) ExFreePoolWithTag(g_resident_workload_page, YGHV_TAG);
            g_resident_workload_page = NULL;
            return st;
        }
        svm_core_wait_remote_ready(g_vcpu_count);
        yghv_trace("bm persistent running");
        return STATUS_SUCCESS;
    }

    if (step == 9) {
        ULONG i;
        yghv_trace("bm persistent hb start");
        st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
        if (!st)
            st = yghv_protect_start();  /* 0 pages: keepalive heartbeat only */
        if (st) {
            LOG_ERROR("bm step 9: setup failed 0x%x", st);
            yghv_protect_cleanup();
            return st;
        }
        for (i = 0; i < g_vcpu_count; i++) {
            svm_vcpu_t *cv = g_vcpus[i];
            if (!cv) continue;
            cv->regs.rcx = g_vmmcall_auth_cookie;
            cv->vmcb->state.cr3 = g_control_cr3;
            cv->vmcb->control.general1_intercepts = INTERCEPT_CPUID;
            cv->vmcb->control.general2_intercepts =
                INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
            cv->vmcb->state.rip = (uint64_t)svm_trampoline_test_guest;
            cv->regs.rdi = 0;
            cv->regs.rsi = 0;
            cv->vmcb->state.rax = 0;
        }
        st = svm_core_start_persistent_residents(g_vcpu_count);
        if (st) {
            LOG_ERROR("bm step 9: persistent start failed 0x%x", st);
            yghv_protect_cleanup();
            return st;
        }
        svm_core_wait_remote_ready(g_vcpu_count);
        yghv_trace("bm persistent hb running");
        return STATUS_SUCCESS;
    }

    if (step == 10) {
        ULONG i;
        ULONG bm_cores = 2;
        yghv_trace("bm persistent hb 2core start");
        st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
        if (!st)
            st = yghv_protect_start();  /* 0 pages: keepalive heartbeat only */
        if (st) {
            LOG_ERROR("bm step 10: setup failed 0x%x", st);
            yghv_protect_cleanup();
            return st;
        }
        for (i = 0; i < bm_cores && i < g_vcpu_count; i++) {
            svm_vcpu_t *cv = g_vcpus[i];
            if (!cv) continue;
            cv->regs.rcx = g_vmmcall_auth_cookie;
            cv->vmcb->state.cr3 = g_control_cr3;
            cv->vmcb->control.general1_intercepts = INTERCEPT_CPUID;
            cv->vmcb->control.general2_intercepts =
                INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
            cv->vmcb->state.rip = (uint64_t)svm_trampoline_test_guest;
            cv->regs.rdi = 0;
            cv->regs.rsi = 0;
            cv->vmcb->state.rax = 0;
        }
        st = svm_core_start_persistent_residents(bm_cores);
        if (st) {
            LOG_ERROR("bm step 10: persistent start failed 0x%x", st);
            yghv_protect_cleanup();
            return st;
        }
        svm_core_wait_remote_ready(bm_cores);
        yghv_trace("bm persistent hb 2core running");
        return STATUS_SUCCESS;
    }

    if (step == 11) {
        ULONG i;
        ULONG bm_cores = 2;
        yghv_trace("bm bounded hb 2core intr start");
        for (i = 0; i < bm_cores && i < g_vcpu_count; i++) {
            svm_vcpu_t *cv = g_vcpus[i];
            if (!cv) continue;
            cv->regs.rcx = g_vmmcall_auth_cookie;
            cv->vmcb->state.cr3 = g_control_cr3;
            cv->vmcb->control.general1_intercepts =
                INTERCEPT_CPUID | INTR_GEN1(SVM_INTERCEPT_INTR) |
                INTR_GEN1(SVM_INTERCEPT_NMI) |
                INTR_GEN1(SVM_INTERCEPT_SHUTDOWN);
            cv->vmcb->control.general2_intercepts =
                INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
            cv->vmcb->state.rip = (uint64_t)svm_trampoline_test_guest;
            cv->regs.rdi = 0;
            cv->regs.rsi = 0;
            cv->vmcb->state.rax = 0;
        }
        st = svm_core_start_remote_residents(bm_cores);
        if (st)
            return st;
        svm_core_wait_remote_ready(bm_cores);
        yghv_trace("bm bounded hb 2core intr vmrun");
        svm_core_enter_resident_current(0);
        svm_core_wait_all_stopped(bm_cores);
        for (i = 0; i < bm_cores && i < g_vcpu_count; i++) {
            svm_vcpu_t *cv = g_vcpus[i];
            if (!cv) continue;
            const char *lbl = (i == 0) ? "bm s11 c0" : "bm s11 c1";
            yghv_trace_u64(lbl, cv->resident_interrupt_exits);
            yghv_trace_u64("bm s11 ext", cv->resident_exits);
        }
        yghv_trace("bm bounded hb 2core intr done");
        return STATUS_SUCCESS;
    }

    if (step == 12) {
        HANDLE thread;
        NTSTATUS st12;
        LARGE_INTEGER timeout;

        yghv_trace("bm os guest start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        g_os_guest_test_active = 1;
        g_os_guest_counter = 0;
        st12 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st12)) {
            LOG_ERROR("bm step 12: thread create failed 0x%x", st12);
            return st12;
        }
        timeout.QuadPart = -60LL * 10000000LL;
        st12 = KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                                     KernelMode, FALSE, &timeout);
        ZwClose(thread);
        g_os_guest_test_active = 0;
        yghv_trace_u64("os guest counter", g_os_guest_counter);
        yghv_trace("bm os guest done");
        if (st12 == STATUS_TIMEOUT)
            return st12;
        return g_os_guest_counter > 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }

    if (step == 13) {
        ULONG i;
        ULONG j;
        ULONG bm_cores = 2;
        HANDLE threads[SVM_MAX_CORES] = { 0 };
        LARGE_INTEGER timeout;
        NTSTATUS st13 = STATUS_SUCCESS;

        yghv_trace("bm os guest multi start");
        g_os_guest_test_active = 1;
        g_os_guest_counter = 0;
        timeout.QuadPart = -60LL * 10000000LL;
        for (i = 1; i <= bm_cores; i++) {
            KeInitializeEvent(&g_os_guest_done_events[i], NotificationEvent, FALSE);
            st13 = PsCreateSystemThread(&threads[i], THREAD_ALL_ACCESS, NULL,
                                        NULL, NULL, yghv_os_guest_thread,
                                        (PVOID)(uintptr_t)i);
            if (!NT_SUCCESS(st13)) {
                LOG_ERROR("bm step 13: thread core %u failed 0x%x", i, st13);
                break;
            }
        }
        if (!NT_SUCCESS(st13)) {
            g_os_guest_test_active = 0;
            for (j = 1; j <= bm_cores; j++)
                if (threads[j]) ZwClose(threads[j]);
            return st13;
        }
        for (i = 1; i <= bm_cores; i++) {
            st13 = KeWaitForSingleObject(&g_os_guest_done_events[i], Executive,
                                         KernelMode, FALSE, &timeout);
            if (st13 == STATUS_TIMEOUT)
                break;
        }
        for (i = 1; i <= bm_cores; i++)
            if (threads[i]) ZwClose(threads[i]);
        g_os_guest_test_active = 0;
        for (i = 1; i <= bm_cores; i++) {
            if (!g_vcpus[i]) continue;
            yghv_trace_u64(i == 1 ? "os guest c1 exits" : "os guest c2 exits",
                           g_vcpus[i]->resident_exits);
        }
        yghv_trace_u64("os guest counter", g_os_guest_counter);
        yghv_trace("bm os guest multi done");
        if (st13 == STATUS_TIMEOUT)
            return st13;
        return g_os_guest_counter > 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }

    if (step == 14) {
        ULONG i;
        ULONG j;
        ULONG bm_cores = g_vcpu_count;
        HANDLE threads[SVM_MAX_CORES] = { 0 };
        LARGE_INTEGER timeout;
        NTSTATUS st14 = STATUS_SUCCESS;

        yghv_trace("bm os guest all start");
        g_os_guest_test_active = 1;
        g_os_guest_counter = 0;
        timeout.QuadPart = -120LL * 10000000LL;
        for (i = 0; i < bm_cores; i++) {
            KeInitializeEvent(&g_os_guest_done_events[i], NotificationEvent, FALSE);
            st14 = PsCreateSystemThread(&threads[i], THREAD_ALL_ACCESS, NULL,
                                        NULL, NULL, yghv_os_guest_thread,
                                        (PVOID)(uintptr_t)i);
            if (!NT_SUCCESS(st14)) {
                LOG_ERROR("bm step 14: thread core %u failed 0x%x", i, st14);
                break;
            }
        }
        if (!NT_SUCCESS(st14)) {
            g_os_guest_test_active = 0;
            for (j = 0; j < bm_cores; j++)
                if (threads[j]) ZwClose(threads[j]);
            return st14;
        }
        for (i = 0; i < bm_cores; i++) {
            st14 = KeWaitForSingleObject(&g_os_guest_done_events[i], Executive,
                                         KernelMode, FALSE, &timeout);
            if (st14 == STATUS_TIMEOUT)
                break;
        }
        for (i = 0; i < bm_cores; i++)
            if (threads[i]) ZwClose(threads[i]);
        g_os_guest_test_active = 0;
        for (i = 0; i < bm_cores; i++) {
            if (!g_vcpus[i]) continue;
            yghv_trace_u64("os guest exits", g_vcpus[i]->resident_exits);
        }
        yghv_trace_u64("os guest counter", g_os_guest_counter);
        yghv_trace("bm os guest all done");
        if (st14 == STATUS_TIMEOUT)
            return st14;
        return g_os_guest_counter > 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }

    if (step == 15) {
        HANDLE thread;
        NTSTATUS st15;
        LARGE_INTEGER timeout;

        yghv_trace("bm os seamless start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        g_os_guest_test_active = 1;
        g_os_guest_counter = 0;
        st15 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_seamless_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st15)) {
            LOG_ERROR("bm step 15: thread create failed 0x%x", st15);
            return st15;
        }
        timeout.QuadPart = -60LL * 10000000LL;
        st15 = KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                                     KernelMode, FALSE, &timeout);
        ZwClose(thread);
        g_os_guest_test_active = 0;
        yghv_trace_u64("os guest counter", g_os_guest_counter);
        yghv_trace("bm os seamless done");
        if (st15 == STATUS_TIMEOUT)
            return st15;
        return g_os_guest_counter > 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }

    if (step == 16) {
        ULONG i;
        ULONG j;
        ULONG bm_cores = g_vcpu_count;
        HANDLE threads[SVM_MAX_CORES] = { 0 };
        LARGE_INTEGER timeout;
        NTSTATUS st16 = STATUS_SUCCESS;

        yghv_trace("bm os seamless all start");
        g_os_guest_test_active = 1;
        g_os_guest_counter = 0;
        timeout.QuadPart = -120LL * 10000000LL;
        for (i = 0; i < bm_cores; i++) {
            KeInitializeEvent(&g_os_guest_done_events[i], NotificationEvent, FALSE);
            st16 = PsCreateSystemThread(&threads[i], THREAD_ALL_ACCESS, NULL,
                                        NULL, NULL,
                                        yghv_os_guest_seamless_thread,
                                        (PVOID)(uintptr_t)i);
            if (!NT_SUCCESS(st16)) {
                LOG_ERROR("bm step 16: thread core %u failed 0x%x", i, st16);
                break;
            }
        }
        if (!NT_SUCCESS(st16)) {
            g_os_guest_test_active = 0;
            for (j = 0; j < bm_cores; j++)
                if (threads[j]) ZwClose(threads[j]);
            return st16;
        }
        for (i = 0; i < bm_cores; i++) {
            st16 = KeWaitForSingleObject(&g_os_guest_done_events[i], Executive,
                                         KernelMode, FALSE, &timeout);
            if (st16 == STATUS_TIMEOUT)
                break;
        }
        for (i = 0; i < bm_cores; i++)
            if (threads[i]) ZwClose(threads[i]);
        g_os_guest_test_active = 0;
        for (i = 0; i < bm_cores; i++) {
            if (!g_vcpus[i]) continue;
            yghv_trace_u64("os seamless exits", g_vcpus[i]->resident_exits);
        }
        yghv_trace_u64("os guest counter", g_os_guest_counter);
        yghv_trace("bm os seamless all done");
        if (st16 == STATUS_TIMEOUT)
            return st16;
        return g_os_guest_counter > 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }

    if (step == 17) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st17;

        yghv_trace("bm os resident start");
        /* 9.217 单核方案: YGHV_SINGLECORE=1 时 step17 用 core0（VMX numvcpus=1 时
           g_vcpu_count=1 只有 core0）。单核 Windows 调度器不做跨核调度 → 消除
           跨核 IPI 死锁（9.216/9.217）。单核下 alive/watchdog 线程 pin core0 与
           guest 竞争同一 vCPU，无法运行——观测靠 guest 态交互/heartbeat。 */
#ifdef YGHV_SINGLECORE
        KeInitializeEvent(&g_os_guest_done_events[0], NotificationEvent, FALSE);
#else
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
#endif
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
#ifdef YGHV_HOST_ISR
        /* 9.215 变体 F: host ISR 服务时钟（step20 的 PASS 组合）——拦截 INTR/NMI，
           host 侧服务中断，guest 态不需要自己处理时钟中断（变体 E"有画面无响应"
           的可能根源 = guest 态时钟/中断服务断）。 */
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
#endif
#if defined(YGHV_CATCHALL) || defined(YGHV_V101_4VEC)
        /* 9.222 变体 J / 9.224 变体 K: 拦截 guest 异常，把 triple fault 变可诊断 VMEXIT */
        g_v102_catchall = TRUE;
#endif
#ifdef YGHV_APIC_SHADOW
        /* 9.226 裸机等价物: step17 用 APIC 影子（B-1full），非 passthrough */
        g_v98_apic_shadow = TRUE;
        yghv_trace("ctlM: apic shadow enabled for step17");
#endif
        st17 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_thread,
#ifdef YGHV_SINGLECORE
                                    (PVOID)(uintptr_t)0);
#else
                                    (PVOID)(uintptr_t)1);
#endif
        if (!NT_SUCCESS(st17)) {
            LOG_ERROR("bm step 17: thread create failed 0x%x", st17);
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            return st17;
        }
        st17 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st17)) {
            LOG_ERROR("bm step 17: alive thread create failed 0x%x", st17);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            return st17;
        }
        st17 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st17)) {
            LOG_ERROR("bm step 17: watchdog thread create failed 0x%x", st17);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            return st17;
        }
#ifdef YGHV_SINGLECORE
        KeWaitForSingleObject(&g_os_guest_done_events[0], Executive,
                              KernelMode, FALSE, NULL);
#else
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
#endif
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident running");
        return STATUS_SUCCESS;
    }

    if (step == 18) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st18;

        yghv_trace("bm os resident spin start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = FALSE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st18 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st18)) {
            LOG_ERROR("bm step 18: thread create failed 0x%x", st18);
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            return st18;
        }
        st18 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st18)) {
            LOG_ERROR("bm step 18: alive thread create failed 0x%x", st18);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            return st18;
        }
        st18 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st18)) {
            LOG_ERROR("bm step 18: watchdog thread create failed 0x%x", st18);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            return st18;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident spin running");
        return STATUS_SUCCESS;
    }

    if (step == 19) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st19;

        yghv_trace("bm os resident spin intr start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st19 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st19)) {
            LOG_ERROR("bm step 19: thread create failed 0x%x", st19);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_resident_log_active = FALSE;
            return st19;
        }
        st19 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st19)) {
            LOG_ERROR("bm step 19: alive thread create failed 0x%x", st19);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_resident_log_active = FALSE;
            return st19;
        }
        st19 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st19)) {
            LOG_ERROR("bm step 19: watchdog thread create failed 0x%x", st19);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_resident_log_active = FALSE;
            return st19;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident spin intr running");
        return STATUS_SUCCESS;
    }

    if (step == 20) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st20;

        yghv_trace("bm os resident spin host-isr start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st20 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st20)) {
            LOG_ERROR("bm step 20: thread create failed 0x%x", st20);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st20;
        }
        st20 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st20)) {
            LOG_ERROR("bm step 20: alive thread create failed 0x%x", st20);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st20;
        }
        st20 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st20)) {
            LOG_ERROR("bm step 20: watchdog thread create failed 0x%x", st20);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st20;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        /* 9.162: transfer handles to globals so DriverUnload can request stop
           and join the threads before teardown (fixes 0xCE on sc stop). */
        g_os_guest_resident_thread = thread;
        g_os_guest_alive_thread = alive;
        g_os_guest_watchdog_thread = watchdog;
        yghv_trace("bm os resident spin host-isr running");
        return STATUS_SUCCESS;
    }

    if (step == 21) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st21;

        yghv_trace("bm os resident spin inject start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_inject_intr = TRUE;
        g_os_guest_host_isr = FALSE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st21 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st21)) {
            LOG_ERROR("bm step 21: thread create failed 0x%x", st21);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_inject_intr = FALSE;
            g_os_resident_log_active = FALSE;
            return st21;
        }
        st21 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st21)) {
            LOG_ERROR("bm step 21: alive thread create failed 0x%x", st21);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_inject_intr = FALSE;
            g_os_resident_log_active = FALSE;
            return st21;
        }
        st21 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st21)) {
            LOG_ERROR("bm step 21: watchdog thread create failed 0x%x", st21);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_inject_intr = FALSE;
            g_os_resident_log_active = FALSE;
            return st21;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident spin inject running");
        return STATUS_SUCCESS;
    }

    if (step == 22) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st22;

        yghv_trace("bm os resident block host-isr start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = FALSE;
        g_os_guest_inject_intr = FALSE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st22 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st22)) {
            LOG_ERROR("bm step 22: thread create failed 0x%x", st22);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st22;
        }
        st22 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st22)) {
            LOG_ERROR("bm step 22: alive thread create failed 0x%x", st22);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st22;
        }
        st22 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st22)) {
            LOG_ERROR("bm step 22: watchdog thread create failed 0x%x", st22);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st22;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident block host-isr running");
        return STATUS_SUCCESS;
    }

    if (step == 23) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st23;

        yghv_trace("bm os resident block delay start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st23 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st23)) {
            LOG_ERROR("bm step 23: thread create failed 0x%x", st23);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st23;
        }
        st23 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st23)) {
            LOG_ERROR("bm step 23: alive thread create failed 0x%x", st23);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st23;
        }
        st23 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st23)) {
            LOG_ERROR("bm step 23: watchdog thread create failed 0x%x", st23);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            return st23;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident block delay running");
        return STATUS_SUCCESS;
    }

    if (step == 24) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st24;

        yghv_trace("bm os resident block delay quiet start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st24 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st24)) {
            LOG_ERROR("bm step 24: thread create failed 0x%x", st24);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            return st24;
        }
        st24 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st24)) {
            LOG_ERROR("bm step 24: alive thread create failed 0x%x", st24);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            return st24;
        }
        st24 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st24)) {
            LOG_ERROR("bm step 24: watchdog thread create failed 0x%x", st24);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            return st24;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident block delay quiet running");
        return STATUS_SUCCESS;
    }

    if (step == 25) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st25;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident block avic start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 25: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        st25 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st25)) {
            LOG_ERROR("bm step 25: avic prepare failed 0x%x", st25);
            return st25;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st25 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st25)) {
            LOG_ERROR("bm step 25: thread create failed 0x%x", st25);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st25;
        }
        st25 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st25)) {
            LOG_ERROR("bm step 25: alive thread create failed 0x%x", st25);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st25;
        }
        st25 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st25)) {
            LOG_ERROR("bm step 25: watchdog thread create failed 0x%x", st25);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st25;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident block avic running");
        return STATUS_SUCCESS;
    }

    if (step == 26) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st26;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident avic spin start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 26: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        st26 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st26)) {
            LOG_ERROR("bm step 26: avic prepare failed 0x%x", st26);
            return st26;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st26 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st26)) {
            LOG_ERROR("bm step 26: thread create failed 0x%x", st26);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st26;
        }
        st26 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st26)) {
            LOG_ERROR("bm step 26: alive thread create failed 0x%x", st26);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st26;
        }
        st26 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st26)) {
            LOG_ERROR("bm step 26: watchdog thread create failed 0x%x", st26);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st26;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident avic spin running");
        return STATUS_SUCCESS;
    }

    if (step == 27) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st27;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident block avic direct start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 27: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        st27 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st27)) {
            LOG_ERROR("bm step 27: avic prepare failed 0x%x", st27);
            return st27;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = FALSE;
        g_os_guest_host_isr = FALSE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st27 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st27)) {
            LOG_ERROR("bm step 27: thread create failed 0x%x", st27);
            g_os_resident_mode = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st27;
        }
        st27 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st27)) {
            LOG_ERROR("bm step 27: alive thread create failed 0x%x", st27);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st27;
        }
        st27 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st27)) {
            LOG_ERROR("bm step 27: watchdog thread create failed 0x%x", st27);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st27;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident block avic direct running");
        return STATUS_SUCCESS;
    }

    if (step == 28) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st28;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident avic irr spin start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 28: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        st28 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st28)) {
            LOG_ERROR("bm step 28: avic prepare failed 0x%x", st28);
            return st28;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_avic_irr_inject = TRUE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st28 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st28)) {
            LOG_ERROR("bm step 28: thread create failed 0x%x", st28);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st28;
        }
        st28 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st28)) {
            LOG_ERROR("bm step 28: alive thread create failed 0x%x", st28);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st28;
        }
        st28 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st28)) {
            LOG_ERROR("bm step 28: watchdog thread create failed 0x%x", st28);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st28;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident avic irr spin running");
        return STATUS_SUCCESS;
    }

    if (step == 29) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st29;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident avic scan spin start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 29: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        st29 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st29)) {
            LOG_ERROR("bm step 29: avic prepare failed 0x%x", st29);
            return st29;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_avic_irr_inject = TRUE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st29 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st29)) {
            LOG_ERROR("bm step 29: thread create failed 0x%x", st29);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st29;
        }
        st29 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st29)) {
            LOG_ERROR("bm step 29: alive thread create failed 0x%x", st29);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st29;
        }
        st29 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st29)) {
            LOG_ERROR("bm step 29: watchdog thread create failed 0x%x", st29);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st29;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident avic scan spin running");
        return STATUS_SUCCESS;
    }

    if (step == 30) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st30;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident avic scan block start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 30: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        st30 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st30)) {
            LOG_ERROR("bm step 30: avic prepare failed 0x%x", st30);
            return st30;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_avic_irr_inject = TRUE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st30 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st30)) {
            LOG_ERROR("bm step 30: thread create failed 0x%x", st30);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st30;
        }
        st30 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st30)) {
            LOG_ERROR("bm step 30: alive thread create failed 0x%x", st30);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st30;
        }
        st30 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st30)) {
            LOG_ERROR("bm step 30: watchdog thread create failed 0x%x", st30);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st30;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident avic scan block running");
        return STATUS_SUCCESS;
    }

    if (step == 31) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st31;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident avic stack block start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 31: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        st31 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st31)) {
            LOG_ERROR("bm step 31: avic prepare failed 0x%x", st31);
            return st31;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_avic_irr_inject = TRUE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st31 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st31)) {
            LOG_ERROR("bm step 31: thread create failed 0x%x", st31);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st31;
        }
        st31 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st31)) {
            LOG_ERROR("bm step 31: alive thread create failed 0x%x", st31);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st31;
        }
        st31 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st31)) {
            LOG_ERROR("bm step 31: watchdog thread create failed 0x%x", st31);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st31;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident avic stack block running");
        return STATUS_SUCCESS;
    }

    if (step == 32) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st32;
        svm_vcpu_t *av = svm_core_get_vcpu(1);

        yghv_trace("bm os resident avic eoi spin start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 32: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        av->resident_index = 1;
        st32 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st32)) {
            LOG_ERROR("bm step 32: avic prepare failed 0x%x", st32);
            return st32;
        }
        st32 = yghv_avic_install_eoi_idt(av);
        if (!NT_SUCCESS(st32)) {
            LOG_ERROR("bm step 32: eoi idt install failed 0x%x", st32);
            yghv_avic_cleanup(av);
            return st32;
        }

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = FALSE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_avic_irr_inject = TRUE;
        g_os_guest_avic_eoi_only = TRUE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st32 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_spin_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st32)) {
            LOG_ERROR("bm step 32: thread create failed 0x%x", st32);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_avic_eoi_only = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_restore_idt(av);
            yghv_avic_cleanup(av);
            return st32;
        }
        st32 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st32)) {
            LOG_ERROR("bm step 32: alive thread create failed 0x%x", st32);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_avic_eoi_only = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_restore_idt(av);
            yghv_avic_cleanup(av);
            return st32;
        }
        st32 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st32)) {
            LOG_ERROR("bm step 32: watchdog thread create failed 0x%x", st32);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_avic_eoi_only = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_restore_idt(av);
            yghv_avic_cleanup(av);
            return st32;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident avic eoi spin running");
        return STATUS_SUCCESS;
    }

    if (step == 33) {
        HANDLE thread;
        HANDLE alive;
        HANDLE watchdog;
        NTSTATUS st33;
        svm_vcpu_t *av = svm_core_get_vcpu(1);
        uint32_t lvtt;
        uint32_t clock_vector;

        yghv_trace("bm os resident avic timer block start");
        if (!av || g_vcpu_count <= 1) {
            LOG_ERROR("bm step 33: vcpu1 unavailable");
            return STATUS_NOT_FOUND;
        }
        av->resident_index = 1;
        st33 = yghv_avic_prepare(av, 1);
        if (!NT_SUCCESS(st33)) {
            LOG_ERROR("bm step 33: avic prepare failed 0x%x", st33);
            return st33;
        }
        svm_avic_timer_init(av);
        lvtt = READ_REGISTER_ULONG(
            (PULONG)((ULONG_PTR)av->avic_host_apic_va + APIC_OFFSET_LVTT));
        clock_vector = lvtt & 0xFF;
        svm_avic_start_timer(av, clock_vector, 10);
        WRITE_REGISTER_ULONG(
            (PULONG)((ULONG_PTR)av->avic_host_apic_va + APIC_OFFSET_TMICT), 0);
        WRITE_REGISTER_ULONG(
            (PULONG)((ULONG_PTR)av->avic_host_apic_va + APIC_OFFSET_TDCR), 0);

        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_avic_irr_inject = TRUE;
        g_os_guest_avic_eoi_only = FALSE;
        g_os_guest_avic_timer_emu = TRUE;
        g_os_guest_delay_quiet = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        st33 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st33)) {
            LOG_ERROR("bm step 33: thread create failed 0x%x", st33);
            if (av->avic_timer_initialized) {
                KeCancelTimer(&av->avic_timer);
                av->avic_timer_armed = FALSE;
            }
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_avic_timer_emu = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st33;
        }
        st33 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st33)) {
            LOG_ERROR("bm step 33: alive thread create failed 0x%x", st33);
            if (av->avic_timer_initialized) {
                KeCancelTimer(&av->avic_timer);
                av->avic_timer_armed = FALSE;
            }
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_avic_timer_emu = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st33;
        }
        st33 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st33)) {
            LOG_ERROR("bm step 33: watchdog thread create failed 0x%x", st33);
            if (av->avic_timer_initialized) {
                KeCancelTimer(&av->avic_timer);
                av->avic_timer_armed = FALSE;
            }
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_guest_avic_irr_inject = FALSE;
            g_os_guest_avic_timer_emu = FALSE;
            g_os_guest_delay_quiet = FALSE;
            g_os_resident_log_active = FALSE;
            yghv_avic_cleanup(av);
            return st33;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(watchdog);
        yghv_trace("bm os resident avic timer block running");
        return STATUS_SUCCESS;
    }

    if (step == 99) {
        ULONG i;
        ULONG j;
        ULONG bm_cores = g_vcpu_count;
        HANDLE threads[SVM_MAX_CORES] = { 0 };
        HANDLE alive = NULL;
        HANDLE watchdog = NULL;
        NTSTATUS st99 = STATUS_SUCCESS;

        yghv_trace("bm os allcore resident start");
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        InterlockedExchange(&g_v99_allcore_ready, 0);
        InterlockedExchange(&g_v99_allcore_go, 0);
        InterlockedExchange(&g_v99_allcore_abort, 0);
        InterlockedExchange(&g_v99_allcore_online, (LONG)bm_cores);

        for (i = 0; i < bm_cores; i++) {
            KeInitializeEvent(&g_os_guest_done_events[i], NotificationEvent, FALSE);
            st99 = PsCreateSystemThread(&threads[i], THREAD_ALL_ACCESS, NULL,
                                        NULL, NULL,
                                        yghv_os_guest_allcore_thread,
                                        (PVOID)(uintptr_t)i);
            if (!NT_SUCCESS(st99)) {
                LOG_ERROR("bm step 99: thread core %u failed 0x%x", i, st99);
                break;
            }
        }
        if (!NT_SUCCESS(st99)) {
            InterlockedExchange(&g_v99_allcore_abort, 1);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            for (j = 0; j < bm_cores; j++)
                if (threads[j]) ZwClose(threads[j]);
            return st99;
        }

        st99 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st99)) {
            LOG_ERROR("bm step 99: alive thread create failed 0x%x", st99);
            InterlockedExchange(&g_v99_allcore_abort, 1);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            for (j = 0; j < bm_cores; j++)
                if (threads[j]) ZwClose(threads[j]);
            return st99;
        }
        st99 = PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_watchdog_thread, NULL);
        if (!NT_SUCCESS(st99)) {
            LOG_ERROR("bm step 99: watchdog thread create failed 0x%x", st99);
            InterlockedExchange(&g_v99_allcore_abort, 1);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            for (j = 0; j < bm_cores; j++)
                if (threads[j]) ZwClose(threads[j]);
            return st99;
        }

        InterlockedExchange(&g_v99_allcore_go, 1);
        for (i = 0; i < bm_cores; i++) {
            KeWaitForSingleObject(&g_os_guest_done_events[i], Executive,
                                  KernelMode, FALSE, NULL);
        }
        ZwClose(alive);
        ZwClose(watchdog);
        for (i = 0; i < bm_cores; i++)
            if (threads[i]) ZwClose(threads[i]);
        yghv_trace("bm os allcore resident running");
        return STATUS_SUCCESS;
    }

    /* 9.232 step205: faithful SimpleSvm port (independent launch/VpData model,
     * see svm_simplevm_port.c). Runs after svm_core_init has enabled SVM + built
     * g_npt. Virtualizes every core sequentially via affinity; each core's host
     * loop persists in its own VpData-embedded stack. No observer, no poll thread,
     * zero NT calls in the VMEXIT island. Reboot to unload (no stop channel yet). */
    if (step == 205) {
        extern void yghv_sv205_start(ULONG cores);
        yghv_trace("bm step 205 SimpleSvm-port start");
        yghv_sv205_start(g_vcpu_count);
        yghv_trace("bm step 205 all cores virtualized, DriverEntry returns");
        return STATUS_SUCCESS;
    }

    if (step == 203 || step == 204) {
        ULONG i;
        ULONG j;
        ULONG bm_cores = g_vcpu_count;
        ULONG s203_base = 0;      /* first core index entering guest */
        HANDLE alive = NULL;
        HANDLE watchdog = NULL;
        NTSTATUS st203 = STATUS_SUCCESS;

        yghv_trace("bm os simplevm resident start");
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        /* 203 减法要点：不拦 INTR/NMI、不跑宿主 ISR、不注入——guest 原生服务
         * 中断（与 v99/step17 的分水岭，SimpleSvm/HelloAmdHv 同款语义）。 */
        g_os_guest_intr_intercept = FALSE;
        g_os_guest_host_isr = FALSE;
        g_os_guest_inject_intr = FALSE;
        g_os_guest_avic_irr_inject = FALSE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        /* 9.229: arm the self-diagnosis dispatch island (first-40 exits logged
         * write-through; any non-CPUID/VMRUN/VMMCALL exit fail-closes only the
         * guest core so the host survives and progress.log keeps the cause). */
        g_s203_lean = TRUE;
        InterlockedExchange(&g_s203_exit_log, 40);
        yghv_trace("s203 lean diag armed");
        g_s204_ret_immediately = (step == 204) ? TRUE : FALSE;
        if (step == 204)
            yghv_trace("s204 all-core ret-to-guest mode armed");
#ifdef YGHV_SINGLECORE
        /* 203-s 单变量隔离：只让 core1 进 guest（原生 tick/上下文切换照旧），
         * core0 保持宿主态——alive/watchdog/DriverEntry 尾日志都在 host 侧产出，
         * guest 停摆时 watchdog 能数到 3s 主动 0xE2 留 dump。
         * 区分"guest 原生中断处理即死" vs "全核/SMP 进入才是变量"。 */
        if (step == 203 && bm_cores >= 2) {
            bm_cores = 1;
            s203_base = 1;
            yghv_trace("s203 single-core mode (guest=core1, core0 host)");
        } else if (step == 203) {
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            LOG_ERROR("bm step 203 SINGLECORE: need >=2 cores");
            return STATUS_UNSUCCESSFUL;
        }
#endif
        InterlockedExchange(&g_s203_ready, 0);
        InterlockedExchange(&g_s203_go, 0);
        InterlockedExchange(&g_s203_abort, 0);
        InterlockedExchange(&g_s203_online, (LONG)bm_cores);
        for (i = 0; i < SVM_MAX_CORES; i++)
            g_s203_threads[i] = NULL;

        for (i = 0; i < bm_cores; i++) {
            ULONG c = s203_base + i;
            KeInitializeEvent(&g_os_guest_done_events[c], NotificationEvent, FALSE);
            st203 = PsCreateSystemThread(&g_s203_threads[i], THREAD_ALL_ACCESS,
                                         NULL, NULL, NULL,
                                         yghv_os_guest_simplevm_thread,
                                         (PVOID)(uintptr_t)c);
            if (!NT_SUCCESS(st203)) {
                LOG_ERROR("bm step 203: thread core %u failed 0x%x", c, st203);
                break;
            }
        }
        if (!NT_SUCCESS(st203)) {
            /* go 从未置位：已创建的线程都在 ready-spin 里，abort 后自行退出。
             * 必须 join（9.163 教训：失败路径也不能把线程留在模块里）。 */
            InterlockedExchange(&g_s203_abort, 1);
            g_os_resident_mode = FALSE;
            g_os_resident_log_active = FALSE;
            for (j = 0; j < SVM_MAX_CORES; j++) {
                if (g_s203_threads[j])
                    yghv_join_system_thread(g_s203_threads[j]);
                g_s203_threads[j] = NULL;
            }
            return st203;
        }

        /* alive/watchdog 都在 guest 态运行：alive 的周期输出 = "Windows 调度器
         * 在 VMM 之下仍活着"的直接证据（9.225 教训）；watchdog 停摆 3s 主动
         * 0xE2 留 dump。创建失败不致命，只降级为警告。 */
        if (!NT_SUCCESS(PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL,
                                             NULL, NULL,
                                             yghv_resident_alive_thread, NULL)))
            LOG_ERROR("bm step 203: alive thread create failed");
        else
            g_os_guest_alive_thread = alive;
        if (!NT_SUCCESS(PsCreateSystemThread(&watchdog, THREAD_ALL_ACCESS, NULL,
                                             NULL, NULL,
                                             yghv_resident_watchdog_thread, NULL)))
            LOG_ERROR("bm step 203: watchdog thread create failed");
        else
            g_os_guest_watchdog_thread = watchdog;

        /* 9.229c2 observer: pins to core0 (host in single-core mode), flushes the
         * guest core's latest exit evidence to progress.log from normal context.
         * The v100 monitor stays on so the ring records every exit in pure memory. */
        g_v100_monitor_active = TRUE;
        if (!NT_SUCCESS(PsCreateSystemThread(&g_s203_observer, THREAD_ALL_ACCESS,
                                             NULL, NULL, NULL,
                                             yghv_s203_observer_thread,
                                             (PVOID)(uintptr_t)s203_base)))
            LOG_ERROR("bm step 203: observer thread create failed");

        /* Release the barrier: every core enters guest mode together. */
        InterlockedExchange(&g_s203_go, 1);
        for (i = 0; i < bm_cores; i++) {
            KeWaitForSingleObject(&g_os_guest_done_events[s203_base + i], Executive,
                                  KernelMode, FALSE, NULL);
        }
        yghv_trace("bm os simplevm resident running");
        return STATUS_SUCCESS;
    }

    if (step == 100) {
        HANDLE thread;
        HANDLE alive;
        HANDLE monitor;
        NTSTATUS st100;
        svm_vcpu_t *mv;

        yghv_trace("bm os resident freeze-site start");
        KeInitializeEvent(&g_os_guest_done_events[1], NotificationEvent, FALSE);
        KeInitializeEvent(&g_os_resident_stop_event, NotificationEvent, FALSE);
        g_os_guest_intr_intercept = TRUE;
        g_os_guest_host_isr = TRUE;
        g_os_guest_inject_intr = FALSE;
        g_os_resident_mode = TRUE;
        g_os_resident_log_active = TRUE;
        g_v100_monitor_active = TRUE;
        g_v100_guest_entered = FALSE;
        /* step100 isolation: guest continuation performs no file I/O
           (no yghv_trace), leaving only a pure KeDelayExecutionThread
           block, to separate "any guest-mode scheduler switch" from
           "blocking file write triggers the 0x139".  (9.170: briefly set
           FALSE to validate the GS-selector fix against the v100 0x139
           shape — result was hard freeze, confirming GS selector is not
           the hard-freeze root cause; restored to TRUE.) */
        g_os_guest_delay_quiet = TRUE;
        mv = svm_core_get_vcpu(1);
        if (mv) {
            RtlZeroMemory(mv->v100_ring, sizeof(mv->v100_ring));
            mv->v100_seq = 0;
        }
        st100 = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_os_guest_resident_delay_thread,
                                    (PVOID)(uintptr_t)1);
        if (!NT_SUCCESS(st100)) {
            LOG_ERROR("bm step 100: thread create failed 0x%x", st100);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            g_v100_monitor_active = FALSE;
            return st100;
        }
        st100 = PsCreateSystemThread(&alive, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_resident_alive_thread, NULL);
        if (!NT_SUCCESS(st100)) {
            LOG_ERROR("bm step 100: alive thread create failed 0x%x", st100);
            ZwClose(thread);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            g_v100_monitor_active = FALSE;
            return st100;
        }
        st100 = PsCreateSystemThread(&monitor, THREAD_ALL_ACCESS, NULL, NULL,
                                    NULL, yghv_v100_monitor_thread, NULL);
        if (!NT_SUCCESS(st100)) {
            LOG_ERROR("bm step 100: monitor thread create failed 0x%x", st100);
            ZwClose(thread);
            ZwClose(alive);
            g_os_resident_mode = FALSE;
            g_os_guest_intr_intercept = FALSE;
            g_os_guest_host_isr = FALSE;
            g_os_resident_log_active = FALSE;
            g_v100_monitor_active = FALSE;
            return st100;
        }
        KeWaitForSingleObject(&g_os_guest_done_events[1], Executive,
                              KernelMode, FALSE, NULL);
        ZwClose(thread);
        ZwClose(alive);
        ZwClose(monitor);
        yghv_trace("bm os resident freeze-site running");
        return STATUS_SUCCESS;
    }

    v->regs.rcx = g_vmmcall_auth_cookie;
    v->regs.rax = 0;
    v->resident_index = 0;
    v->vmcb->control.general1_intercepts = 0;
    v->vmcb->control.general2_intercepts =
        INTR_GEN2(SVM_INTERCEPT_VMRUN) | INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    v->vmcb->control.exception_intercepts = 0;
    if (step >= 2) {
        v->vmcb->control.np_enable = SVM_NP_ENABLE;
        v->vmcb->control.ncr3 = g_npt.pml4_pa;
    } else {
        v->vmcb->control.np_enable = 0;
        v->vmcb->control.ncr3 = 0;
    }
    if (step == 4 || step == 5 || step == 7)
        v->vmcb->control.general1_intercepts = 0;
    else if (step >= 3)
        v->vmcb->control.general1_intercepts = INTERCEPT_CPUID;
    if (step == 5)
        v->vmcb->control.exception_intercepts = (1ULL << 1);

    if (step == 4) {
        npf_page = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE, YGHV_TAG);
        if (!npf_page) {
            LOG_ERROR("bm step 4: alloc failed");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(npf_page, HV_PAGE_SIZE);
        npf_pa = MmGetPhysicalAddress(npf_page).QuadPart;
        st = npt_split_2mb_to_4kb(&g_npt, npf_pa);
        if (!st)
            st = npt_set_page_perm(&g_npt, npf_pa, 0);
        if (st) {
            LOG_ERROR("bm step 4: setup failed 0x%x", st);
            ExFreePoolWithTag(npf_page, YGHV_TAG);
            return st;
        }
        g_npt_test_pa = npf_pa;
        g_npt_test_active = 1;
        v->regs.rdi = (uint64_t)npf_page;
        v->vmcb->state.rip = (uint64_t)svm_trampoline_test_npt_guest;
        yghv_trace_u64("bm npf pa", npf_pa);
    } else if (step == 7) {
        uint64_t dummy = (uint64_t)yghv_hook_test_dummy;
        st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
        if (!st)
            st = yghv_protect_install_hook(0, dummy);
        if (st) {
            LOG_ERROR("bm step 7: hook setup failed 0x%x", st);
            return st;
        }
        v->regs.rsi = dummy;
        v->vmcb->state.rip = (uint64_t)svm_trampoline_test_hook_guest;
        yghv_trace_u64("bm hook va", dummy);
    } else if (step == 5) {
        prot_page = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE, YGHV_TAG);
        if (!prot_page) {
            LOG_ERROR("bm step 5: alloc failed");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(prot_page, HV_PAGE_SIZE);
        st = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
        if (!st)
            st = yghv_protect_add_page((uint64_t)prot_page);
        if (!st)
            st = yghv_protect_start();
        if (st) {
            LOG_ERROR("bm step 5: setup failed 0x%x", st);
            ExFreePoolWithTag(prot_page, YGHV_TAG);
            return st;
        }
        v->regs.rdi = (uint64_t)prot_page;
        v->vmcb->state.rip = (uint64_t)svm_trampoline_test_prot_write;
        yghv_trace_u64("bm prot va", (uint64_t)prot_page);
    } else if (step == 1 || step == 2) {
        v->vmcb->state.rip = (uint64_t)svm_trampoline_test_min_guest;
    } else if (step >= 3) {
        v->vmcb->state.rip = (uint64_t)svm_trampoline_test_cpuid_guest;
    }
    v->vmcb->state.rax = 0;

    yghv_trace("bm vmrun");
    svm_core_enter_resident_current(0);
    if (step == 4) {
        g_npt_test_active = 0;
        npt_set_page_perm(&g_npt, npf_pa, NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
        ExFreePoolWithTag(npf_page, YGHV_TAG);
    }
    if (step == 5) {
        yghv_protect_stop();
        yghv_protect_remove_page((uint64_t)prot_page);
        ExFreePoolWithTag(prot_page, YGHV_TAG);
    }
    if (step == 7) {
        yghv_trace_u64("bm hook rdx", v->regs.rdx);
        yghv_protect_remove_hook(0);
    }
    yghv_trace("bm exit");
    yghv_trace_u64("bm rax", v->regs.rax);
    st = STATUS_SUCCESS;
    return st;
}

#if YGHV_HOOK_RENDEZVOUS_TEST
static VOID yghv_hook_rendezvous_thread(PVOID context) {
    void *page;
    uint64_t target_va;
    LARGE_INTEGER delay;
    ULONG pid;
    NTSTATUS st_install, st_remove;
    (void)context;

    delay.QuadPart = -8 * 10000000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    page = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE * 2, YGHV_TAG);
    if (!page) {
        LOG_ERROR("hook rendezvous test: alloc failed");
        PsTerminateSystemThread(STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    RtlZeroMemory(page, HV_PAGE_SIZE * 2);
    target_va = ((uint64_t)page + HV_PAGE_SIZE - 1) &
                ~(uint64_t)(HV_PAGE_SIZE - 1);
    RtlFillMemory((void *)target_va, 16, 0x90);
    *(uint8_t *)(target_va + 16) = 0xC3;

    yghv_protect_get_state(NULL, &pid, NULL);
    if (pid == 0)
        yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());

    st_install = yghv_protect_install_hook(1, target_va);
    LOG_ERROR("hook rendezvous test: install rc=0x%x", st_install);
    delay.QuadPart = -1000 * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    st_remove = yghv_protect_remove_hook(1);
    LOG_ERROR("hook rendezvous test: remove rc=0x%x", st_remove);
    LOG_ERROR("hook rendezvous test: %s",
        (NT_SUCCESS(st_install) && NT_SUCCESS(st_remove)) ? "PASS" : "FAIL");

    ExFreePoolWithTag(page, YGHV_TAG);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void yghv_hook_rendezvous_join(void) {
    PETHREAD thread_obj = NULL;
    NTSTATUS status;
    if (!g_hook_rendezvous_thread)
        return;
    status = ObReferenceObjectByHandle(
        g_hook_rendezvous_thread, THREAD_ALL_ACCESS, *PsThreadType,
        KernelMode, (PVOID *)&thread_obj, NULL);
    if (NT_SUCCESS(status) && thread_obj) {
        KeWaitForSingleObject(thread_obj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(thread_obj);
    } else {
        LOG_ERROR("hook rendezvous: [api] failed 0x%x",
            status);
    }
    ZwClose(g_hook_rendezvous_thread);
    g_hook_rendezvous_thread = NULL;
}
#endif

static void yghv_init_auth_cookie(void) {
    LARGE_INTEGER st, ticks;
    KeQuerySystemTime(&st);
    KeQueryTickCount(&ticks);
    g_vmmcall_auth_cookie = (uint64_t)st.QuadPart ^ (uint64_t)ticks.QuadPart;
    if (!g_vmmcall_auth_cookie)
        g_vmmcall_auth_cookie = 0x59484756ULL;
}

/* 9.258 (206-C3): process-create/destroy notification. Create=FALSE fires on
 * process teardown — forward to the protect-engine watchdog (disarm + free
 * the dead target's pages). Non-Ex variant: (ParentId, ProcessId, Create). */
static void yghv_s206_process_notify(HANDLE parent_id, HANDLE process_id,
                                     BOOLEAN create) {
    (void)parent_id;
    if (!create)
        yghv_protect_on_process_exit((uint32_t)(ULONG_PTR)process_id);
}

/* Write one diagnostic line to \SystemRoot\yghv_watchdog.log (append).  Uses its
   own handle so it works after yghv_trace_close() has NULLed g_trace_file
   (yghv_trace would silently drop the line). */
static void yghv_watchdog_log(const char *line) {
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    size_t len = 0;
    char buf[1600];

    RtlInitUnicodeString(&name, L"\\SystemRoot\\yghv_watchdog.log");
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_APPEND_DATA, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0)))
        return;
    if (NT_SUCCESS(RtlStringCchLengthA(line, sizeof(buf) - 2, &len))) {
        RtlCopyMemory(buf, line, len);
        buf[len] = '\r'; buf[len + 1] = '\n';
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)(len + 2), NULL, NULL);
        ZwFlushBuffersFile(h, &iosb);
    }
    ZwClose(h);
}

/* Append a 64-bit value as lowercase hex (0x...) to a buffer; no CRT printf
   is available in this kernel driver link. */
static void yghv_wd_hex(char *buf, size_t bufsz, size_t *off, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    int i;
    if (*off + 18 > bufsz - 1)   /* reserve 1 byte for the trailing NUL */
        return;
    buf[(*off)++] = '0';
    buf[(*off)++] = 'x';
    for (i = 15; i >= 0; i--)
        buf[(*off)++] = hex[(v >> (i * 4)) & 0xF];
}

/* 9.152: dedicated guest CR3 — build a page table mapping a list of pages at
   their kernel VAs (guest VA -> PA; the NPT identity map then PA -> PA).  The
   guest runs in its OWN address space instead of sharing the host kernel CR3,
   which is the freeze fix: every shared-CR3 configuration froze within 15-60 s,
   while the dedicated-CR3 minimal guest ran 5+ min stable. */
static npt_entry_t *yghv_guest_pt_alloc(void) {
    npt_entry_t *t = (npt_entry_t *)MmAllocateContiguousMemory(
        HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (t) RtlZeroMemory(t, HV_PAGE_SIZE);
    return t;
}

static uint64_t yghv_build_guest_cr3(uint64_t *vas, ULONG count) {
    npt_entry_t *pml4 = NULL, *pdpt = NULL, *pd = NULL, *pt = NULL;
    uint64_t pml4_pa = 0;
    ULONG i;

    for (i = 0; i < count; i++) {
        uint64_t va = vas[i] & ~(uint64_t)0xFFF;
        uint64_t pa = MmGetPhysicalAddress((PVOID)va).QuadPart;
        uint32_t p4, p2, p1, p0;
        if (!pa)
            return 0;
        p4 = (uint32_t)((va >> 39) & 0x1FF);
        p2 = (uint32_t)((va >> 30) & 0x1FF);
        p1 = (uint32_t)((va >> 21) & 0x1FF);
        p0 = (uint32_t)((va >> 12) & 0x1FF);

        if (!pml4) {
            pml4 = yghv_guest_pt_alloc();
            if (!pml4) return 0;
            pml4_pa = MmGetPhysicalAddress(pml4).QuadPart;
        }
        if (!(pml4[p4].all & 1)) {
            pdpt = yghv_guest_pt_alloc();
            if (!pdpt) return 0;
            pml4[p4].all = MmGetPhysicalAddress(pdpt).QuadPart | 0x3;
        } else {
            pdpt = (npt_entry_t *)MmGetVirtualForPhysical(
                (PHYSICAL_ADDRESS){ .QuadPart = pml4[p4].all & ~0xFFFULL });
        }
        if (!(pdpt[p2].all & 1)) {
            pd = yghv_guest_pt_alloc();
            if (!pd) return 0;
            pdpt[p2].all = MmGetPhysicalAddress(pd).QuadPart | 0x3;
        } else {
            pd = (npt_entry_t *)MmGetVirtualForPhysical(
                (PHYSICAL_ADDRESS){ .QuadPart = pdpt[p2].all & ~0xFFFULL });
        }
        if (!(pd[p1].all & 1)) {
            pt = yghv_guest_pt_alloc();
            if (!pt) return 0;
            pd[p1].all = MmGetPhysicalAddress(pt).QuadPart | 0x3;
        } else {
            pt = (npt_entry_t *)MmGetVirtualForPhysical(
                (PHYSICAL_ADDRESS){ .QuadPart = pd[p1].all & ~0xFFFULL });
        }
        pt[p0].all = pa | 0x3;   /* present|writable, no NX (code pages runnable) */
    }
    return pml4_pa;
}

/* 9.141 freeze watchdog: a host-side observer that logs the per-core resident
   VMEXIT counts every 5 s while the driver is loaded.  On a hard freeze the
   last marker tells us whether host code was still running (markers continue
   on the free cores) or every core was stuck in guest mode (markers stop),
   localizing the stall.  Purely observational; it does not touch the
   resident/NPF/VMMCALL paths. */
static VOID yghv_freeze_watchdog_thread(PVOID ctx) {
    LARGE_INTEGER delay;
    LARGE_INTEGER tick;
    char line[1600];
    size_t off;
    ULONG n, i;
    (void)ctx;
    delay.QuadPart = -5LL * 10 * 1000 * 1000;   /* 5 s */
    while (!g_watchdog_stop) {
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (g_watchdog_stop)
            break;
        KeQueryTickCount(&tick);
        n = g_vcpu_count;
        if (n > SVM_MAX_CORES) n = SVM_MAX_CORES;
        off = 0;
        line[off++] = 'w'; line[off++] = 'd';
        line[off++] = ' '; line[off++] = 't'; line[off++] = '=';
        yghv_wd_hex(line, sizeof(line), &off, (uint64_t)tick.QuadPart);
        /* per-core resident exit counts, in core order */
        line[off++] = ' '; line[off++] = 'e'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? g_vcpus[i]->resident_exits : 0);
        }
        /* per-core resident_state (ACTIVE=2 running, STOPPED=4 not running,
           STOPPING=3 stuck teardown, 0xEE = no vcpu) */
        line[off++] = ' '; line[off++] = 's'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? (uint64_t)(LONG)g_vcpus[i]->resident_state : 0xEE);
        }
        /* per-core last VMEXIT code (0x3E8 = SHUTDOWN, 0x1E0 = NPF, etc.) */
        line[off++] = ' '; line[off++] = 'x'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? g_vcpus[i]->last_exitcode : 0xEE);
        }
        /* per-core guest RIP at the last VMEXIT (where a fault/stop occurred) */
        line[off++] = ' '; line[off++] = 'r'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? g_vcpus[i]->last_rip : 0xEE);
        }
        /* per-core guest RSP and CR3 at the last VMEXIT (fault localization) */
        line[off++] = ' '; line[off++] = 'p'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? g_vcpus[i]->last_rsp : 0xEE);
        }
        line[off++] = ' '; line[off++] = 'g'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? g_vcpus[i]->last_cr3 : 0xEE);
        }
        /* 9.165 diag: per-core guest GS base / KERNEL_GS_BASE at the last
           VMEXIT — capture the KPCR/swapgs state before a context-switch
           freeze (v100/v100b 0x139 = MISSING_GSFRAME_STACKPTR_ERROR). */
        line[off++] = ' '; line[off++] = 'G'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? g_vcpus[i]->last_gs_base : 0xEE);
        }
        line[off++] = ' '; line[off++] = 'K'; line[off++] = '=';
        for (i = 0; i < n; i++) {
            if (i) line[off++] = ',';
            yghv_wd_hex(line, sizeof(line), &off,
                g_vcpus[i] ? g_vcpus[i]->last_kgs_base : 0xEE);
        }
        line[off] = 0;
        yghv_watchdog_log(line);
    }
}

/* 9.162: join a system thread by its HANDLE — wait on the thread OBJECT (via
   ObReferenceObjectByHandle), never on the raw handle (raw handle is not a
   dispatcher object and bugchecks 0xA — the 2EA24641 diagnostic build). */
static void yghv_join_system_thread(HANDLE h) {
    PETHREAD thread_obj = NULL;
    NTSTATUS jst;

    if (!h)
        return;
    jst = ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode,
                                    (PVOID *)&thread_obj, NULL);
    if (NT_SUCCESS(jst)) {
        KeWaitForSingleObject(thread_obj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(thread_obj);
    } else {
        LOG_ERROR("join thread: [api] failed 0x%x", jst);
    }
    ZwClose(h);
}

void DriverUnload(struct _DRIVER_OBJECT *d) {
    /*
     * 9.390: EVERY STEP BELOW IS STAMPED, because this is the path that has
     * killed the machine five times and nothing on it can be observed afterwards.
     *
     * The stamps go into a page that is never freed and never hidden (see
     * common/yghv_unload_trace.h). After a fatal unload the dump carries the last
     * stamp, which answers the only question that matters: how far did teardown
     * get before something freed memory a live core was still using.
     */
    yghv_ut_mark(YGHV_UT_START);

    /*
     * 9.404: SNAPSHOT BEFORE ANYTHING IS CLEARED.
     *
     * Every source below is destroyed by this function: g_resident_thread_count
     * goes to 0 in svm_core_stop_all_residents, g_PowerCallbackRegistration goes
     * to NULL in Sv206CoopUnload. Reading them after a crash therefore cannot
     * distinguish "never set" from "set, then cleared here" - which is why the
     * 9.401/9.402 readouts were ambiguous despite being correct.
     *
     * These six values are captured now, into a page that nothing else writes.
     * A later dump shows both: these (pre-teardown) and the globals (post). The
     * comparison is the measurement.
     *
     * externs instead of includes: these live in modules that pull kernel headers
     * in a different order, and main.c is the one place that can see them all.
     */
    /*
     * 9.406: the locals are declared OUTSIDE the block on purpose. The decision
     * further down ("was anything running when we entered?") has to be based on
     * the value captured HERE, before any teardown cleared it - not on the global,
     * which by then reads 0 either way.
     */
    ULONG_PTR resident_threads_at_entry = 0;
    {
        extern ULONG g_vcpu_count;
        extern volatile LONG g_s203_online;
        extern ULONG_PTR yghv_s206_power_cb_for_trace(void);
        extern ULONG_PTR svm_core_resident_threads_for_trace(void);
        extern ULONG_PTR yghv_s206_vmrun_stopped_for_trace(void);
        extern LONG yghv_s206_cpuid_view_for_trace(void);

        resident_threads_at_entry = svm_core_resident_threads_for_trace();

        /*
     * 9.591: put back every process this driver unlinked from
     * ActiveProcessLinks. FIRST, before anything else is torn down: an
     * EPROCESS left out of the list outlives the driver, and the next
     * enumeration that walks the list faults on the hole. There is no later
     * point in this function where the offsets are still valid.
     */
    /*
     * 9.592: drain queued user APCs before the image goes away. A KAPC holds
     * non-paged pool and its kernel routine lives in THIS image; unloading
     * first turns a pending injection into a jump into freed memory. The wait
     * is bounded, so a stuck APC delays the unload instead of hanging it, and
     * the count is left readable in yghv_inj_stats().
     */
    yghv_inj_cleanup();

    yghv_proc_cleanup();

    yghv_ut_snapshot_pre_unload(
            (UINT64)resident_threads_at_entry,
            (UINT64)yghv_s206_power_cb_for_trace(),
            (UINT64)g_vcpu_count,
            (UINT64)g_s203_online,
            (UINT64)yghv_s206_vmrun_stopped_for_trace(),
            (UINT64)yghv_s206_cpuid_view_for_trace());
    }
#if YGHV_BAREMETAL_STEP == 206 && defined(YGHV_206B_COEXIST)
    /* 9.258: unregister the target-exit watchdog BEFORE any teardown so no
     * callback fires into a half-dismantled protect engine. */
    (void)PsSetCreateProcessNotifyRoutine(yghv_s206_process_notify, TRUE);
    yghv_ut_mark(YGHV_UT_NOTIFY_OFF);
    /* 9.246: devirtualize all cores (upstream CPUID-backdoor loop) + unregister
     * the vendored power callback BEFORE any yghv teardown frees the NPT the
     * host loops still translate through. */
    /*
     * 9.441: THIS WAIT NOW WATCHES THE RIGHT COUNTER.
     *
     * 9.397 waited for `g_VmrunStopped` to reach the core count. 9.440 proved that
     * counter has NO WRITER in this build: its only incrementer is
     * Sv206VmrunShouldExit, and 9.419 left that function with zero callers when it
     * deleted the ASM loop's check. So the wait below could not succeed - it could
     * only burn its full 2000 iterations and fall through, every single time,
     * looking exactly like "the loops refused to stop".
     *
     * The number that actually answers "is anybody still inside vmrun" is
     * g_inVmrunCount: `lock inc` immediately before vmrun, `lock dec` immediately
     * after vmsave, both unconditional, both present in the artifact. It is the
     * only counter in this tree that is both written and reachable.
     *
     * The order is also inverted from 9.397, and that is the substance of the fix:
     * the stop flag is requested first (cheap, harmless), then the CORE WALK runs -
     * because the walk, not the flag, is what reaches a running loop with legal
     * register clobbering (see the note at the walk below). The wait comes after
     * both, so it measures the effect of the thing that works.
     */
    {
        extern void yghv_s206_vmrun_stop_request(void);
        extern int  yghv_s206_vmrun_stopped_count(void);

        ULONG active = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
        int stopped;

        yghv_s206_vmrun_stop_request();

        /*
         * 9.443: `stopped` IS READ AFTER THE WAIT BELOW, NOT HERE.
         *
         * It used to be read immediately after the request, which under the new
         * protocol would always be 0: each core only increments g_S206CoresExited
         * when its guest stub reaches Sv206GuestExited, and that takes one vmmcall
         * tick per core. Reading it before giving the cores that tick would report
         * "0 of 12 stopped" on a perfectly healthy unload and make the trace
         * unreadable.
         */
        stopped = 0;

        yghv_ut_mark_val(45 + 1, (uint64_t)active);

        /*
         * 9.441/9.442: WHY THE WALK IS BACK AS THE DEFAULT - AND WHAT IT IS NOT.
         *
         * What the old dumps said (9.399/9.406 read them correctly):
         *   - the last stamp was COOP_UNLOAD, so Sv206CoopUnload never returned;
         *   - the bugcheck was 0x7F double fault at
         *     SvDevirtualizeAllProcessors+0xC0, i.e. inside the walk.
         *
         * Their conclusion - "pinning to a core and running CPUID there is unsafe,
         * so skip the walk" - was wrong for a reason they could not see at the
         * time: the walk was not the broken part, the EXIT it triggered was.
         * Every core that took a CPUID exit went through SvLV20, whose first
         * instruction is `mov rsp, rcx`, and RCX held 'SSVM' because of 9.383.
         * A double fault inside the walk is exactly what an exit path with a
         * garbage stack pointer looks like from outside. 9.441 reverted it.
         *
         * But 9.442 has to be honest about the other half: the walk does NOT stop
         * a running VMRUN loop, and never could. SvDevirtualizeProcessor runs
         * `__cpuidex` on the unload thread, which is not in guest mode, so the
         * CPUID is not intercepted and the live loops never see it. 213,601 exits
         * with 12 loops still cycling is the measurement that proves it.
         *
         * So the walk is here for what it actually does - freeing each core's VPD
         * and letting each core clear EFER.SVME in the legal window - and the
         * stopping is done by the flag above plus the wait below. Skipping the
         * walk (as 9.399/9.406 did) leaked every VPD on every unload, which is
         * why it must run unconditionally even though it is not the stop.
         */
        yghv_ut_mark_val(47, stopped >= (int)active ? 1 : 0);

        /*
         * 9.442: THE WALK'S JOB IS NOT TO STOP THE LOOPS - IT NEVER COULD.
         *
         * 9.441 restored this walk as the default on the theory that it is the
         * only way to reach a running VMRUN loop. The first measurement killed
         * that theory:
         *
         *   SvDevirtualizeProcessor runs `__cpuidex(CPUID_UNLOAD_SIMPLE_SVM)` on
         *   the UNLOAD THREAD. That thread is not in guest mode - its VMRUN loop
         *   was abandoned long ago. So the CPUID is NOT intercepted, SvHandleVmExit
         *   never sees it, and the 12 live loops do not notice anything. The walk
         *   cannot stop them, by construction, and never could.
         *
         * What the walk DOES do is real and still needed: it frees each core's
         * VPD and lets each core clear EFER.SVME in the one legal window. That is
         * teardown, not stopping.
         *
         * The loops are stopped by the flag above, which SvHandleVmExit now checks
         * on every exit (9.442), plus the wait below. Order matters: flag, walk,
         * wait. The walk runs before the wait so that VPDs are freed while the
         * cores are still leaving, not after they have gone quiet.
         */
        yghv_ut_mark_val(47, stopped >= (int)active ? 1 : 0);

        /*
         * 9.442: THE WALK RUNS UNCONDITIONALLY, BUT IT IS NO LONGER THE STOP.
         *
         * The two branches this replaces both keyed on counters that 9.440 proved
         * are identically zero in this build:
         *   `stopped == 0`                  - g_VmrunStopped had no writer;
         *   `resident_threads_at_entry == 0` - 206-B never starts residents.
         * So the walk was skipped every single time - which mattered, because the
         * walk frees the VPDs. Skipping it leaked every VPD on every unload.
         *
         * (9.443: g_S206CoresExited now has exactly one writer - Sv206GuestExited -
         * and it is incremented once per core after SvLV20 has taken that core out
         * of the guest for good. So it is a real reading, and it is read AFTER the
         * wait below. An earlier version of this comment said it would read 0 here;
         * that was true only while it was being read too early.)
         *
         * The callback release happens first and unconditionally: it has nothing
         * to do with cores, and skipping it would leave a registration pointing
         * into an unloaded image.
         */
        {
            extern int yghv_s206_power_callback_release(void);
            (void)yghv_s206_power_callback_release();
        }
        yghv_ut_mark(21);
        yghv_ut_mark(YGHV_UT_COOP_UNLOAD);
        { extern void Sv206CoopUnload(void); Sv206CoopUnload(); }
        yghv_ut_mark(YGHV_UT_COOP_UNLOAD + 5);
        yghv_ut_mark_val(22, 1);   /* 22/1 = walk ran (was: 22/0 = skipped) */

        /*
         * 9.443: WAIT FOR CORES THAT ACTUALLY LEFT - NOT FOR A FRAME COUNT.
         *
         * 9.442 waited on g_inVmrunCount, which can never reach zero by design: the
         * VMRUN frame a core abandons when its guest exits stays suspended on that
         * thread's stack forever, so `lock dec` never runs for it. Waiting on it was
         * guaranteed to burn the whole budget on every single unload.
         *
         * g_S206CoresExited is the number that means what this function needs: how
         * many cores have reached Sv206GuestExited, i.e. have left the guest for
         * good and are no longer executing anything of ours. When it equals the
         * active core count, nothing of this driver is running anywhere, and the
         * image can be unmapped without a PAGED_IP.
         *
         * The wait is BEFORE DriverUnload returns because the unmap happens after
         * the function: a core still in the stub at that point cannot be saved by
         * anything later in this function.
         */
        {
            extern int yghv_s206_vmrun_stopped_count(void);
            extern int yghv_s206_invmrun_count(void);

            ULONG waited = 0;

            yghv_ut_mark_val(55, (uint64_t)yghv_s206_invmrun_count());

            stopped = yghv_s206_vmrun_stopped_count();

            /*
             * 9.556: WAIT FOR THE CORES THAT EXIST, NOT FOR EVERY LOGICAL CPU.
             *
             * 9.555 made the stopped counter honest - the ASM exit path now
             * increments it before terminating the thread - and unload time was
             * UNCHANGED (2.844 / 2.137 / 4.118 s). So the counter was never the
             * problem; the bound was.
             *
             * `active` is KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) = 12
             * here, while 206 virtualizes SIX cores. `stopped` cannot reach 12, so
             * the condition was unsatisfiable and the waiter always ran out its
             * 20000 passes. This is why every unload cost ~2-4 s regardless of how
             * correct any counter was.
             *
             * Comparing against the registered VPD count asks the question that
             * has an answer. A count of 0 means no core ever virtualized, and the
             * loop then leaves at once - the outcome 9.442 already calls normal.
             */
            {
                extern int yghv_s206_vpd_count(void);
                int vpds = yghv_s206_vpd_count();
                ULONG waitFor = (vpds > 0) ? (ULONG)vpds : 0;
                yghv_ut_mark_val(58, (uint64_t)waitFor);

            while ((ULONG)stopped < waitFor && waited < 20000) {
                volatile ULONG spin;

                /*
                 * 9.462: KICK AGAIN ON EVERY PASS, NOT ONCE.
                 *
                 * The stub parks in `hlt` now, so a core that misses the first
                 * wake sleeps until something else interrupts it - and nothing
                 * else will, because Windows does not know it has a core sitting
                 * inside our guest. Measured without this loop: invmrun = 0xc after
                 * the stop, i.e. all twelve cores still parked; DriverUnload then
                 * returned, the image was unmapped, and the parked cores executed
                 * `hlt` on dead code pages -> 0xD1 PAGED_IP at Sv206LaunchVm+0x1e
                 * with rcx = 0xa (the value `mov rsp, rcx` had written).
                 *
                 * A repeated injection is safe by construction: a core that already
                 * woke and left increments g_S206CoresExitedAsm - the same object
                 * yghv_s206_vmrun_stopped_count() reads - and is not in the
                 * registry's parked set next pass, and a core that is still parked
                 * gets a fresh EventInj.
                 *
                 * 9.565: this comment named g_S206CoresExited, which has had no
                 * writer since 9.555 (that name is now a dead static; the live one
                 * is the _Asm alias the ASM increments directly). Fixed here rather
                 * than left on the audit's list: it is one line, it is the exact
                 * sentence a reader would trust when reasoning about this loop, and
                 * the same-name family already cost us the 4-second unload.
                 */
                {
                    extern LONG yghv_s206_kick_parked_cores(void);
                    (void)yghv_s206_kick_parked_cores();
                }

                for (spin = 0; spin < 200000UL; spin++) { }
                waited += 1;
                stopped = yghv_s206_vmrun_stopped_count();
            }
            }

            yghv_ut_mark_val(56, (uint64_t)(ULONG)stopped);
            yghv_ut_mark_val(57, (uint64_t)waited);
            yghv_ut_mark_val(48, (uint64_t)(ULONG)stopped);
        }
    }
    /* The mark after it is the one that matters: if the dump's last stamp is
     * COOP_UNLOAD and not this, the de-virtualize walk never returned. */
    yghv_ut_mark(YGHV_UT_COOP_UNLOAD + 5);
    /* 9.251: flush the island's DENY telemetry (bare-metal now; file I/O safe).
     * The file handle must still be open — yghv_trace_close runs later. */
    { extern void yghv_s206_flush_deny_log(void); yghv_s206_flush_deny_log(); }
    /* 9.253: flush the armed-page ring (same bare-metal-now contract; the
     * runtime add-page path must not touch the file system, see run_c10). */
    { extern void yghv_s206_flush_addlog(void); yghv_s206_flush_addlog(); }
    /*
     * 9.441: DUMP THE EXIT COUNTERS BEFORE THE STOP REQUEST.
     *
     * This is the readout 9.440 said the tree was missing. Taken HERE because
     * everything after it changes what the numbers mean: the stop request below
     * makes every live loop take an exit, so a counter read afterwards would
     * conflate "exits happen" with "the stop worked".
     *
     * Three numbers, three different facts:
     *   s206 exits at unload  == 0  -> the guest never trapped at all
     *   s206 exits at unload  > 0  -> it did trap; the problem is downstream
     *   s206 vmmcall at unload      -> whether the VMMCALL bit ever fired
     */
    {
        extern LONG64 yghv_s206_exits_count(void);
        extern LONG64 yghv_s206_vmmcalls_count(void);
        extern int yghv_s206_invmrun_count(void);
        yghv_trace_u64("s206 exits at unload",
                       (UINT64)yghv_s206_exits_count());
        yghv_trace_u64("s206 vmmcall at unload",
                       (UINT64)yghv_s206_vmmcalls_count());
        yghv_trace_u64("s206 invmrun at unload",
                       (UINT64)yghv_s206_invmrun_count());
    }
#endif
#if YGHV_HOOK_RENDEZVOUS_TEST
    yghv_hook_rendezvous_join();
#endif
    g_npt_test_active = 0;
    /* 9.162: clean-unload for OS-as-guest resident.  Request guest stop first —
       the spin/block guest exits on its next VMEXIT (svm_dispatch_exit returns 1
       -> trampoline jnz host_done -> yghv_os_guest_host_done -> thread exits).
       alive/watchdog threads break out on the same flag.  Join all three before
       any teardown so no thread runs in the unloaded module (fixes 0xCE). */
    g_os_guest_stop_requested = TRUE;
    yghv_ut_mark(YGHV_UT_STOP_REQUESTED);
    yghv_join_system_thread(g_os_guest_resident_thread);
    yghv_join_system_thread(g_os_guest_alive_thread);
    yghv_join_system_thread(g_os_guest_watchdog_thread);
    /* 9.227 step203: join the all-core simplevm threads.  Each core's guest
     * continuation VMEXITs on its ~10ms CPUID poll once stop_requested is set
     * (svm_dispatch_exit returns 1 -> host_done -> PsTerminateSystemThread),
     * so these joins complete in bounded time on a live system. */
    yghv_ut_mark(YGHV_UT_JOIN_GUEST);
    {
        ULONG s203;
        for (s203 = 0; s203 < g_s203_online && s203 < SVM_MAX_CORES; s203++) {
            if (g_s203_threads[s203])
                yghv_join_system_thread(g_s203_threads[s203]);
            g_s203_threads[s203] = NULL;
        }
    }
    yghv_ut_mark_val(YGHV_UT_JOIN_203, (uint64_t)g_s203_online);
    /* observer breaks on stop_requested and exits ~5ms later; join in host ctx */
    yghv_join_system_thread(g_s203_observer);
    g_s203_observer = NULL;
    g_s203_lean = FALSE;
    g_s203_unexpected_exit = 0;
    g_v100_monitor_active = FALSE;
    g_os_guest_resident_thread = NULL;
    g_os_guest_alive_thread = NULL;
    g_os_guest_watchdog_thread = NULL;
    g_os_guest_stop_requested = FALSE;
    yghv_ut_mark(YGHV_UT_STOP_RESIDENTS);
    svm_core_stop_all_residents();
    svm_core_wait_all_stopped(g_vcpu_count);
    yghv_ut_mark_val(YGHV_UT_WAIT_RESIDENTS, (uint64_t)g_vcpu_count);
    yghv_ut_mark(YGHV_UT_CONTROL_CLEANUP);
    yghv_control_device_cleanup(d);
    /*
     * 9.487: RELEASE THE TIER 2.4 SHARED FRAME.
     *
     * `yghv_share_init` allocates one non-paged page and nothing freed it, so
     * every load/unload cycle leaked 4 KB of a pool the OS cannot reclaim. Placed
     * here, after the control device is gone (no caller can issue `share init`
     * from here on) and before the protection teardown.
     */
    {
        extern void yghv_share_down(void);
        yghv_share_down();
    }
    KeSetSystemAffinityThread((KAFFINITY)1);
    yghv_ut_mark(YGHV_UT_PROTECT_CLEANUP);
    yghv_protect_cleanup();
    /*
     * 9.487: RELEASE THE DECOY FRAME - AND ONLY HERE.
     *
     * Every DECOY-hidden page has its NPT leaf pointing at this frame, so freeing
     * it while any leaf still points here would leave the guest reading memory
     * that now belongs to somebody else. `yghv_protect_cleanup` above is what
     * restores those leaves, which is why this must come AFTER it and not before.
     *
     * Nothing freed it at all before this line, so each load/unload leaked 4 KB of
     * non-paged pool - the one pool a driver cannot ask the OS to reclaim.
     */
    {
        extern void yghv_guard_decoy_free(void);
        yghv_guard_decoy_free();
    }
    if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
    g_guest_code_page = NULL;
    if (g_resident_workload_page) MmFreeContiguousMemory(g_resident_workload_page);
    g_resident_workload_page = NULL;
    /*
     * 9.426: THE SAME GUARD MUST COVER npt_cleanup, AND IT HAS TO RUN FIRST.
     *
     * npt_cleanup frees the NPT tables - the very tables a core still inside vmrun
     * is translating through (NCr3 points at them). It used to run BEFORE the
     * vmrun check, so the guard added in 9.425 protected only half the problem:
     * the VMCB write was deferred but the page tables it walks were already gone.
     *
     * So the check moves up and now decides both frees. The wait is shared: one
     * grace period, then one decision, then both cleanups follow it together.
     */
    {
        extern int yghv_s206_invmrun_count(void);

        ULONG waited = 0;
        int inside = 0;

        for (;;) {
            inside = yghv_s206_invmrun_count();
            if (inside == 0)
                break;
            if (waited >= 600)
                break;
            {
                volatile ULONG spin;
                for (spin = 0; spin < 200000UL; spin++) { }
            }
            waited += 1;
        }

        yghv_ut_mark_val(51, (uint64_t)inside);
        yghv_ut_mark_val(52, (uint64_t)waited);

        if (inside != 0) {
            /*
             * Nobody may touch NPT or VMCB from here: a core is still inside
             * vmrun and will write both. Leak them, and say so.
             *
             * The NPT leak matters more than the VMCB leak: g_npt.pml4_pa is
             * written into every VCPU's NCr3, so freeing it under a live core
             * would turn the next guest memory access into a fault through a
             * freed table - a wider blast radius than the vmsave write alone.
             */
            yghv_ut_mark_val(53, 1);
            yghv_ut_mark(54);          /* 54 = both cleanups skipped */
        } else {
            yghv_ut_mark_val(53, 0);
            yghv_ut_mark(YGHV_UT_NPT_CLEANUP);
            npt_cleanup(&g_npt);
            yghv_ut_mark(YGHV_UT_CORE_CLEANUP);
            svm_core_cleanup();
        }
    }
    goto after_vmrun_guard;
after_vmrun_guard:;
    KeRevertToUserAffinityThread();
    g_watchdog_stop = TRUE;
    if (g_watchdog_thread) {
        /* Wait on the thread OBJECT (via the handle), never on the raw handle:
           passing the HANDLE to KeWaitForSingleObject treats it as a dispatcher
           pointer and bugchecks 0xA (seen in the 2EA24641 diagnostic build). */
        PETHREAD thread_obj = NULL;
        NTSTATUS jst = ObReferenceObjectByHandle(
            g_watchdog_thread, SYNCHRONIZE, *PsThreadType, KernelMode,
            (PVOID *)&thread_obj, NULL);
        if (NT_SUCCESS(jst)) {
            KeWaitForSingleObject(thread_obj, Executive, KernelMode, FALSE,
                                  NULL);
            ObDereferenceObject(thread_obj);
        } else {
            LOG_ERROR("watchdog join: [api] failed 0x%x",
                      jst);
        }
        ZwClose(g_watchdog_thread);
        g_watchdog_thread = NULL;
    }
    /* 9.292 (defect C): close the progress.log handle on the NORMAL unload path.
     * DriverEntry's every failure path calls yghv_trace_close(), but
     * DriverUnload never did — so an unloaded driver left an open
     * \SystemRoot\yghv_progress.log handle owned by the kernel (Restart
     * Manager reports HOLDER pid=4 = System), pinning the file against
     * archive/rotation and accumulating one leaked handle per load/unload
     * cycle. Placed AFTER the 206 telemetry flushes (they write through
     * g_trace_file) and after the watchdog join, immediately before the
     * final LOG_INFO. */
    /*
     * 9.432: THE UNBOUNDED WAIT IS GONE - ITS PREMISE WAS THE BUG.
     *
     * 9.428 waited forever here on the reasoning "a hang beats a bugcheck". That
     * reasoning was sound given what was known then, and it cost one 0x101
     * CLOCK_WATCHDOG_TIMEOUT to learn the actual cause: the counter could never
     * reach zero because the guest's only exit instruction, vmmcall, had no
     * intercept. Waiting forever on a counter that cannot move is not patience,
     * it is a hang with extra steps.
     *
     * With VMMCALL armed (9.432), vmmcall traps, vmrun returns, vmsave runs,
     * lock dec runs, and the counter reaches zero on its own within one guest
     * iteration. So the wait needs to be a BOUNDED grace period again - long
     * enough to cover one iteration, short enough that a genuinely stuck core
     * cannot turn unload into a watchdog.
     *
     * If it does time out, the value is recorded and unload proceeds: the
     * VMCB/NPT guard of 9.425/9.426 still decides whether to free, so a stuck
     * core leaks memory rather than being handed freed pages.
     */
    /*
     * 9.441: THE SECOND WAIT IS GONE - THERE IS NOW EXACTLY ONE.
     *
     * This block used to repeat the same bounded spin on g_inVmrunCount that the
     * de-virtualize section already performs (and would have overwritten the same
     * trace slots 55/56/57 a second time, so the readout could not say WHICH wait
     * produced the number). The wait now lives immediately after the core walk,
     * where its result is still meaningful: the walk is what asks the cores to
     * leave, so the count is read while that request is as fresh as it will ever
     * be. Reading it again here, after the watchdog join and the file flush, would
     * only measure elapsed time.
     */

    yghv_trace_close();
    LOG_INFO("DriverUnload");    yghv_trace_close();
    LOG_INFO("DriverUnload");
}

NTSTATUS DriverEntry(struct _DRIVER_OBJECT*d,PUNICODE_STRING r){
    (void)d;(void)r;
    int sv;
    void *npt_test_buf = NULL;
    uint64_t guest_cr3 = 0;
    ULONG i;
    ULONG online;

#if YGHV_BAREMETAL_STEP == 206 && !defined(YGHV_206B_COEXIST)
    /* step206-A (9.244) baseline: run the VERBATIM vendored upstream SimpleSvm
     * entry body as the OS-as-guest vehicle, dispatched BEFORE yghv's own init
     * (so it is behavior-equivalent to loading C0's SimpleSvm.sys standalone;
     * uses upstream's own NPT). Proven on HW 2026-10-04 (9.245): entered +
     * survived 4x100s stress. The COEXIST variant (9.246, dispatched after the
     * full yghv init further below) instead shares g_npt. */
    {
        extern NTSTATUS Sv206Entry(struct _DRIVER_OBJECT *, PUNICODE_STRING);

        /*
         * 9.454: the loader_stealth call for THIS branch only.
         *
         * The real fix for the shipped build is at the YGHV_206B_COEXIST site
         * further down - this branch is compiled out under COEXIST, which is why
         * putting the call only here left it unreachable. Both sites now carry it
         * so the switch is honest in either configuration.
         */
#if YGHV_LOADER_STEALTH
        yghv_loader_stealth(d, r);
#endif
        return Sv206Entry(d, r);
    }
#endif

#if YGHV_LOADER_STEALTH && !defined(YGHV_206B_COEXIST)
    /*
     * The non-coexist build reaches its own init below and needs the call here.
     * Two sites rather than one because the coexist path returns above and a
     * single call could not reach both without a branch.
     */
    yghv_loader_stealth(d, r);
#endif
#if !YGHV_UNLOAD_GUARD
    d->DriverUnload = DriverUnload;
#endif
    d->Flags |= DRVO_LEGACY_DRIVER;
    LOG_INFO("DriverEntry start");

    KeSetSystemAffinityThread((KAFFINITY)1);
    yghv_init_auth_cookie();
    yghv_trace_init();
    /*
     * 9.589: derive the EPROCESS/ETHREAD offsets before anything could want them.
     *
     * These come from ntoskrnl own exported accessors, not from a hardcoded table:
     * each PsGetXxx is "load one field of RCX and return", so disassembling its
     * body yields the offset for the build that is actually running. A table would
     * be a delayed fuse - right on the build it was typed for, silently reading the
     * neighbouring field afterwards, with correct width and plausible value.
     *
     * Failure here is NOT fatal to load: a driver that cannot derive an offset also
     * cannot use it, so the features that need one fail and the rest carry on. What
     * is rejected is the alternative - writing a guess and letting it read memory
     * that looks like a process.
     */
    {
        NTSTATUS offSt = yghv_offsets_init();
        /* 9.608: yghv_offsets_stats writes EIGHT entries (the links-probe
         * diagnostics were added), so a [4] here would be a 32-byte stack
         * overflow the first time it ran. Sized to the contract, not to the
         * fields this particular caller reads. */
        uint64_t offStats[8];

        yghv_offsets_stats(offStats);
        if (!NT_SUCCESS(offSt) || !yghv_offsets_ready()) {
            LOG_ERROR("offsets: derivation FAILED 0x%x derived=0x%llx failures=%llu",
                      offSt, offStats[0], offStats[2]);
        } else {
            LOG_INFO("offsets: derived=0x%llx validated=0x%llx build=%llu",
                     offStats[0], offStats[1], offStats[3]);
        }
    }
    /*
     * 9.589: the signature engine proving itself against the image it is running in.
     *
     * It rebuilds each accessor pattern from the derived offset and demands exactly
     * one hit sitting on the address MmGetSystemRoutineAddress reports. Two facts at
     * once: the derivation and the search agree, and the pattern really is unique.
     * Non-fatal for the same reason as above.
     */
    {
        int sigOk = yghv_sig_selftest();
        uint64_t sigStats[10];

        yghv_sig_stats(sigStats);
        if (sigOk)
            LOG_INFO("sig: self-test ok, %llu case(s)", sigStats[5]);
        else
            LOG_ERROR("sig: self-test FAILED ok=%llu fail=%llu hits=%llu",
                      sigStats[5], sigStats[6], sigStats[2]);
    }
    /*
     * 9.591: the two structures no export can describe -- the thread list pair
     * and EPROCESS.Token. Runs after the offsets because the probes read through
     * the accessors that pass fills in; reports how many it found, and the ops
     * built on a missing one answer STATUS_NOT_IMPLEMENTED instead of guessing.
     */
    {
        int procOk = yghv_proc_init();
        LOG_INFO("proc: field probes %d of 3 ok (thread list, token, protection export)", procOk);
    }
    /* 9.304: hand the service's own registry path to the persistence layer
     * BEFORE protect_init, which loads the saved settings. */
    {
        int winOk = yghv_win_init();
        LOG_INFO("win: %d piece(s) resolved (win32kfull exports)", winOk);
    }

    yghv_protect_set_registry_path(r);
    sv = yghv_protect_init();
    if (sv) {
        LOG_ERROR("yghv_protect_init failed 0x%x", sv);
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return sv;
    }
    yghv_trace("entry");
    /*
     * 9.390: announce the unload-trace page's physical address at load.
     *
     * A crash dump has no symbol for a static in an unloaded driver, so the only
     * way to find this page afterwards is to have written down where it is. This
     * is that: one line, at load, while the driver still exists.
     */
    /*
     * 9.392: allocate the unload-trace page HERE, at load, PASSIVE.
     *
     * It must exist before DriverUnload runs, and it must not be freed by it - the
     * whole point is that it outlives the module (see yghv_unload_trace.c). So it
     * is created once, early, and never released.
     */
    if (!yghv_ut_init())
        LOG_ERROR("unload-trace: allocation FAILED - unload progress will not be recorded");
    yghv_trace_u64("unload-trace pa", yghv_ut_page_pa());
    LOG_ERROR("driver entry pre");

    /* The direct-map pages returned by MmAllocateContiguousMemory are NX in
       Windows, so the test guest keeps executing from the driver image. */
    g_guest_hb_va = (uint64_t)svm_trampoline_test_guest;
    g_guest_npt_va = (uint64_t)svm_trampoline_test_npt_guest;
    LOG_ERROR("guest code driver va hb=0x%llx npt=0x%llx", g_guest_hb_va, g_guest_npt_va);

    sv = svm_core_init();
    if (sv) {
        LOG_ERROR("svm_core_init failed 0x%x", sv);
        yghv_trace("fail svm_core_init");
        yghv_trace_close();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        KeRevertToUserAffinityThread();
        return (NTSTATUS)sv;
    }
    LOG_INFO("svm_core_init ok");
    yghv_trace("svm_core_init ok");
    g_control_cr3 = g_vcpus[0]->vmcb->state.cr3;
    LOG_ERROR("control cr3=0x%llx", g_control_cr3);

    online = KeQueryActiveProcessorCount(NULL);
    if (online > SVM_MAX_CORES) online = SVM_MAX_CORES;
    LOG_ERROR("active processors: %u", online);
    yghv_trace("cores counted");

    for (i = 1; i < online; i++) {
        sv = svm_alloc_vcpu(i, &g_vcpus[i]);
        if (sv) {
            LOG_ERROR("svm_alloc_vcpu core %u failed 0x%x", i, sv);
            svm_core_cleanup();
            if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
            g_guest_code_page = NULL;
            yghv_trace_close();
            KeRevertToUserAffinityThread();
            return (NTSTATUS)sv;
        }
        g_vcpu_count++;
    }

    KeIpiGenericCall(svm_core_ipi_prepare_vcpu, 0);
    yghv_trace("ipi prepare done");
    for (i = 0; i < online; i++) {
        if (!g_vcpus[i]) continue;
        g_vcpus[i]->regs.rcx = g_vmmcall_auth_cookie;
        g_vcpus[i]->vmcb->state.rip = g_guest_hb_va;
    }

    sv = npt_init(&g_npt, 0);
    if (!sv)
        sv = yghv_npt_map_ram(&g_npt);
    if (sv) {
        LOG_ERROR("npt_init/map_ram failed 0x%x", sv);
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return (NTSTATUS)sv;
    }
    LOG_INFO("npt_init ok");
    yghv_trace("npt_init ok");

#if YGHV_R1_NPT_UNIT_TEST
    sv = yghv_r1_npt_unit_test();
    if (sv) {
        LOG_ERROR("r1 npt unit test failed 0x%x", sv);
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return sv;
    }
#endif

    yghv_trace("map ram n/a");

#if YGHV_R1_EXCLUDE_PRIVATE
    yghv_exclude_hv_private(d);
    yghv_trace("exclude private ok");
    sv = yghv_r1_exclude_check();
    if (sv) {
        LOG_ERROR("r1 exclude check failed 0x%x", sv);
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return sv;
    }
    yghv_trace("exclude check ok");
#else
    yghv_trace("exclude private n/a");
#endif
    /*
     * 9.343 (Tier 7.1): register our code range and allocate the decoy frame.
     *
     * MUST BE OUTSIDE the YGHV_R1_EXCLUDE_PRIVATE conditional. The first version
     * of this block was placed inside yghv_exclude_hv_private(), which the
     * default build does not compile (the macro defaults to 0) — so the code
     * range stayed 0, yghv_guard_hide_core() refused to hide anything, and the
     * feature silently did nothing while reporting success. Measured: hidecore
     * returned "hid 0 page(s)".
     *
     * The code range is what lets the NPF handler tell "our own access to a page
     * we hid" (RIP inside this image, CPL 0) from "a guest reaching for it"
     * (guest RIP, CPL 3). Without it, hiding our own tables takes the driver out
     * with them — the 13th crash. Without the decoy, a hidden page leaves a hole
     * an inventory comparison can see.
     */
    {
        extern void yghv_guard_set_code_range(uint64_t start, uint64_t size);
        extern int yghv_guard_decoy_init(void);
        /*
         * 9.455 (Tier 7.1): remember the image range for the sections report.
         *
         * `yghv_guard_set_code_range` already takes these two numbers, but that
         * stores them as VAs for the RIP whitelist - a different consumer with a
         * different question. The sections command needs the BASE POINTER + SIZE
         * to walk PE headers, and it is registered here so both readers see the
         * same image. A second place computing "our image" is a second answer
         * waiting to disagree.
         */
        {
            extern void yghv_pa_set_driver_range(PVOID start, uint32_t size);
            yghv_pa_set_driver_range(d->DriverStart, d->DriverSize);
        }

        yghv_guard_set_code_range((uint64_t)d->DriverStart,
                                  (uint64_t)d->DriverSize);
        (void)yghv_guard_decoy_init();
        yghv_trace("guard: code range + decoy registered");
    }

    yghv_trace("guest code exec n/a");

    for (i = 0; i < online; i++) {
        sv = svm_core_set_npt(i, g_npt.pml4_pa);
        if (sv) {
            LOG_ERROR("svm_core_set_npt core %u failed 0x%x", i, sv);
            npt_cleanup(&g_npt);
            svm_core_cleanup();
            if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
            g_guest_code_page = NULL;
            yghv_trace_close();
            KeRevertToUserAffinityThread();
            return (NTSTATUS)sv;
        }
    }
    LOG_ERROR("NPT enabled for %u cores: pml4=0x%llx", online, g_npt.pml4_pa);
    yghv_trace("npt set ok");

#if YGHV_BAREMETAL_NO_RESIDENT
    /* Bare-metal smoke: this host hard-freezes on the first VMRUN, so validate
       only non-resident paths (r1 NPT API, hook boundary, hook install/remove)
       and hand off to the control device. */
    sv = yghv_r1_npt_unit_test();
    if (!sv)
        sv = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
    if (!sv)
        sv = yghv_hook_boundary_test();
    if (!sv)
        sv = yghv_hook_test();
    if (sv) {
        LOG_ERROR("bare-metal smoke: non-resident tests failed 0x%x", sv);
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return sv;
    }
    sv = yghv_control_device_init(d);
    if (sv) {
        LOG_ERROR("bare-metal smoke: control device init failed 0x%x", sv);
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return sv;
    }
    LOG_ERROR("bare-metal smoke: non-resident tests PASS, control device ready");
    KeRevertToUserAffinityThread();
    return STATUS_SUCCESS;
#endif

#if YGHV_BAREMETAL_STEP >= 1 && YGHV_BAREMETAL_STEP != 206
    {
        NTSTATUS st2 = yghv_baremetal_step_test(YGHV_BAREMETAL_STEP);
        yghv_trace("bm done");
        if (YGHV_BAREMETAL_STEP < 17 || !g_os_resident_log_active)
            yghv_trace_close();
        KeRevertToUserAffinityThread();
        return st2;
    }
#endif

#if YGHV_BAREMETAL_STEP == 206 && defined(YGHV_206B_COEXIST)
    /* 9.247 step206-B dispatch (RELOCATED here from the control-device site):
     * yghv's core init is complete (svm_core_init + g_npt + per-core NPT set —
     * "npt set ok" is in the log) but the default path's resident/hook test
     * sequence has NOT run yet. That sequence must be skipped in 206-B mode:
     * its synthetic guests VMRUN too, and the trace file is closed at
     * "all stopped" (5336) — dispatching after it left the s206b traces
     * unsighted (the 9.246 run proved the dispatch itself works: RUNNING +
     * 5.5min + sc stop 78ms, but we want the evidence IN the log).
     * Here: extend g_npt to upstream-parity identity 0-512GB (MMIO included),
     * then run the verbatim vendored upstream entry with NCr3=g_npt.
     * Residents are not started; control device is NOT created yet (206-C
     * will decide where it belongs). DriverUnload (registered at entry top,
     * re-registered after Sv206Entry) calls Sv206CoopUnload before teardown. */
    {
        extern NTSTATUS Sv206Entry(struct _DRIVER_OBJECT *, PUNICODE_STRING);
        npt_identity_map_range(&g_npt, 0, 0x8000000000ULL);
        yghv_trace_u64("s206b ncr3", g_npt.pml4_pa);
        /* 206-C1: create the control device BEFORE virtualizing (PASSIVE_LEVEL
         * IoCreateDevice/SymbolicLink). Once the OS runs as guest, the client
         * still reaches \\.\YuanGuardHV — the IRP path is ordinary Windows
         * execution in guest mode (no VMEXIT), and set-target/add-page IOCTLs
         * only touch g_protect + g_npt (pure, no VMRUN). DriverUnload already
         * runs yghv_control_device_cleanup. */
        sv = yghv_control_device_init(d);
        if (sv) {
            LOG_ERROR("s206b: control device init failed 0x%x", sv);
            yghv_protect_cleanup();
            npt_cleanup(&g_npt);
            svm_core_cleanup();
            if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
            g_guest_code_page = NULL;
            yghv_trace_close();
            KeRevertToUserAffinityThread();
            return (NTSTATUS)sv;
        }
        yghv_trace("s206b coexist dispatch");
        /* 9.258 (206-C3): target-exit watchdog — on process teardown, disarm +
         * remove its protected pages (a dead target otherwise leaks armed NPT
         * state onto freed physical pages = the c12 victim-storm amplifier).
         * The callback runs at PASSIVE in normal thread context (never in the
         * island), mutex-protected, memory/NPT ops only. Non-fatal on failure:
         * reopen_page_bare self-heal still bounds the leak damage. */
        sv = PsSetCreateProcessNotifyRoutine(yghv_s206_process_notify, FALSE);
        yghv_trace_u64("s206b notify reg rc", (uint64_t)(NTSTATUS)sv);
        if (sv)
            sv = 0;

        /*
         * 9.454 (Tier 7.1): THIS IS THE SITE THAT MATTERS, AND IT WAS MISSING.
         *
         * The yghv_loader_stealth call in this file lives inside
         * `#if YGHV_BAREMETAL_STEP == 206 && !defined(YGHV_206B_COEXIST)` -
         * a branch that is EMPTY for the configuration this tree actually ships.
         * Measured: with YGHV_LOADER_STEALTH=1 the function was emitted into the
         * binary (`yghv_loader_stealth @ 0x1400137b0` is in the map) but NOTHING
         * CALLED IT, because the only call site was in the branch we compile out.
         * So "the module unlink feature exists and is gated off" was true of the
         * source and false of every binary this tree has ever produced.
         *
         * IT MUST RUN HERE, BEFORE Sv206Entry. yghv_unlink_module walks
         * PsLoadedModuleList looking for DriverObject->DriverStart - that only
         * finds us while we are still linked, and Sv206Entry is what takes over
         * the entry point. After it, the window is gone.
         */
#if YGHV_LOADER_STEALTH
        yghv_loader_stealth(d, r);
#endif

        sv = Sv206Entry(d, r);
        yghv_trace_u64("s206b entry rc", (uint64_t)(NTSTATUS)sv);
        if (sv) {
            yghv_protect_cleanup();
            npt_cleanup(&g_npt);
            svm_core_cleanup();
            if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
            g_guest_code_page = NULL;
            yghv_trace_close();
            KeRevertToUserAffinityThread();
            return (NTSTATUS)sv;
        }
        /* upstream overwrote d->DriverUnload with its own half-teardown; keep
         * yghv's (which runs Sv206CoopUnload first, then full yghv teardown). */
        d->DriverUnload = DriverUnload;
        KeRevertToUserAffinityThread();
        return STATUS_SUCCESS;
    }
#endif

#if !YGHV_R1_SKIP_NPT_TEST
    /* Single-core NPT permission test first. */
    npt_test_buf = MmAllocateContiguousMemory(
        HV_LARGE_PAGE_SIZE * 2,
        (PHYSICAL_ADDRESS){ .QuadPart = -1 });
    if (!npt_test_buf) {
        LOG_ERROR("npt_test_buf allocation failed");
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(npt_test_buf, HV_LARGE_PAGE_SIZE * 2);
    uint64_t buf_pa = MmGetPhysicalAddress(npt_test_buf).QuadPart;
    uint64_t page_pa = (buf_pa + HV_LARGE_PAGE_SIZE - 1) & ~(uint64_t)(HV_LARGE_PAGE_SIZE - 1);
    if (page_pa + HV_LARGE_PAGE_SIZE > buf_pa + HV_LARGE_PAGE_SIZE * 2) {
        LOG_ERROR("npt_test_buf has no aligned 2MB slot");
        MmFreeContiguousMemory(npt_test_buf);
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    uint64_t test_va = (uint64_t)npt_test_buf + (page_pa - buf_pa);
    g_npt_test_pa = page_pa;
    g_npt_test_active = 1;
    LOG_ERROR("NPT test page: va=0x%llx pa=0x%llx", test_va, page_pa);
    yghv_trace("test page ready");

    sv = npt_split_2mb_to_4kb(&g_npt, page_pa);
    if (sv) {
        LOG_ERROR("npt_split test page failed 0x%x", sv);
        MmFreeContiguousMemory(npt_test_buf);
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return (NTSTATUS)sv;
    }
    sv = npt_set_page_perm(&g_npt, page_pa, 0);
    if (sv) {
        LOG_ERROR("npt_set_page_perm failed 0x%x", sv);
        MmFreeContiguousMemory(npt_test_buf);
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return (NTSTATUS)sv;
    }
    svm_vcpu_t *npt_vcpu = svm_core_get_vcpu(0);
    npt_vcpu->regs.rdi = test_va;
    npt_vcpu->regs.rcx = g_vmmcall_auth_cookie;
    npt_vcpu->vmcb->state.rip = g_guest_npt_va;
    npt_vcpu->vmcb->state.rax = 0;
    LOG_ERROR("NPT guest: rip=0x%llx code_pa=0x%llx rdi=0x%llx rdi_pa=0x%llx cr3=0x%llx",
        npt_vcpu->vmcb->state.rip,
        MmGetPhysicalAddress((PVOID)g_guest_npt_va).QuadPart,
        npt_vcpu->regs.rdi,
        MmGetPhysicalAddress((PVOID)npt_vcpu->regs.rdi).QuadPart,
        npt_vcpu->vmcb->state.cr3);
    LOG_ERROR("NPT diag: test_pa=0x%llx trans_test=0x%llx trans_code=0x%llx",
        page_pa,
        npt_translate(&g_npt, page_pa),
        npt_translate(&g_npt, MmGetPhysicalAddress((PVOID)g_guest_npt_va).QuadPart));

    LOG_INFO("entering NPT test resident");
    yghv_trace("before npt test");
    svm_core_enter_resident_current(0);
    LOG_INFO("NPT test resident exited");
    yghv_trace("after npt test");
    g_npt_test_active = 0;
#else
    yghv_trace("npt test skipped");
#endif

    if (!NT_SUCCESS(yghv_protect_test())) {
        LOG_ERROR("protect test failed");
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(yghv_hook_test())) {
        LOG_ERROR("protect hook test failed");
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(yghv_hook_resident_test())) {
        LOG_ERROR("protect hook resident test failed");
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(yghv_hook_boundary_test())) {
        LOG_ERROR("protect hook boundary test failed");
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_UNSUCCESSFUL;
    }

#if YGHV_REAL_HOOK_TEST
    if (!NT_SUCCESS(yghv_real_hook_test())) {
        LOG_ERROR("real hook test failed");
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_UNSUCCESSFUL;
    }
#endif

    if (!NT_SUCCESS(yghv_cpuid_stealth_test())) {
        LOG_ERROR("cpuid stealth test failed");
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_UNSUCCESSFUL;
    }

    /* Reset CPU0 to the heartbeat guest for the multi-core resident test. */
    svm_core_prepare_vcpu_other(0);
    yghv_trace("cpu0 reset");
    g_vcpus[0]->regs.rcx = g_vmmcall_auth_cookie;
    g_vcpus[0]->vmcb->state.rip = g_guest_hb_va;
    svm_core_set_npt(0, g_npt.pml4_pa);

    sv = svm_core_start_remote_residents(online);
    if (sv) {
        LOG_ERROR("svm_core_start_remote_residents failed 0x%x", sv);
        svm_core_stop_all_residents();
        svm_core_wait_all_stopped(online);
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        g_npt_test_active = 0;
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return (NTSTATUS)sv;
    }
    yghv_trace("remote started");
    svm_core_wait_remote_ready(online);
    yghv_trace("remote ready");

    LOG_INFO("entering multi-core resident on CPU0");
    yghv_trace("before heartbeat");
    svm_core_enter_resident_current(0);
    LOG_INFO("CPU0 resident exited");
    yghv_trace("after heartbeat");
    svm_core_wait_all_stopped(online);
    yghv_trace("all stopped");

    if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
    npt_test_buf = NULL;
    g_npt_test_active = 0;
    if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
    g_guest_code_page = NULL;
    yghv_trace_close();

#if YGHV_RESIDENT_WORKLOAD_TEST
    g_resident_workload_page = MmAllocateContiguousMemory(
        HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!g_resident_workload_page) {
        LOG_ERROR("resident workload: page alloc failed");
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(g_resident_workload_page, HV_PAGE_SIZE);
    sv = yghv_protect_set_target((uint32_t)(ULONG_PTR)PsGetCurrentProcessId());
    if (!sv)
        sv = yghv_protect_add_page((uint64_t)g_resident_workload_page);
    if (!sv)
        sv = yghv_protect_install_hook(0, (uint64_t)yghv_hook_test_dummy);
    if (sv) {
        LOG_ERROR("resident workload: setup failed 0x%x", sv);
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_resident_workload_page) MmFreeContiguousMemory(g_resident_workload_page);
        g_resident_workload_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return sv;
    }
#endif

    sv = yghv_protect_start();
    if (sv) {
        LOG_ERROR("persistent protect start failed 0x%x", sv);
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_resident_workload_page) MmFreeContiguousMemory(g_resident_workload_page);
        g_resident_workload_page = NULL;
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        g_npt_test_active = 0;
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return (NTSTATUS)sv;
    }
    KeIpiGenericCall(svm_core_ipi_prepare_vcpu, 0);
    svm_core_prepare_vcpu_other(0);
    /* 9.152: dedicated guest CR3 — map every page the persistent guests touch
       (workload code, heartbeat code, hook dummy, generated stub, g_protect,
       workload page) so the guests run in their own address space instead of
       sharing the host kernel CR3 (the freeze fix: shared-CR3 configs froze
       15-60 s; dedicated-CR3 minimal guest ran 5+ min stable). */
    {
        /* The guest needs its FULL stack region mapped — host_stack is
           SVM_HOST_STACK_PAGES (4) pages and guest RSP starts at
           host_stack + 4*HV_PAGE_SIZE - 8 (the 4th page); mapping only the
           first page made any guest CALL (stack push) NPF -> triple fault. */
        uint64_t gv[64];
        ULONG gvc = 0;
        ULONG j;
        gv[gvc++] = (uint64_t)svm_trampoline_test_resident_guest;
        gv[gvc++] = (uint64_t)svm_trampoline_test_guest;
        gv[gvc++] = (uint64_t)yghv_hook_test_dummy;
        gv[gvc++] = yghv_protect_get_hook_stub_va(0);
        /* g_protect spans ~6.3 KB (targets[4] + config) — map BOTH pages; the
           stub reads targets[3].cr3 and config.deny_status in the 2nd page,
           and an unmapped page would NPF -> guest #PF -> triple fault. */
        gv[gvc++] = (uint64_t)&g_protect;
        gv[gvc++] = (uint64_t)((uint8_t *)&g_protect + 0x1000);
        gv[gvc++] = (uint64_t)g_resident_workload_page;
        for (j = 0; j < online && j < SVM_MAX_CORES; j++) {
            if (g_vcpus[j] && g_vcpus[j]->host_stack) {
                uint64_t hs = (uint64_t)g_vcpus[j]->host_stack;
                ULONG k;
                for (k = 0; k < SVM_HOST_STACK_PAGES; k++)
                    gv[gvc++] = hs + k * HV_PAGE_SIZE;
            }
        }
        guest_cr3 = yghv_build_guest_cr3(gv, gvc);
        if (!guest_cr3) {
            LOG_ERROR("persistent: dedicated guest CR3 build failed, falling back");
            guest_cr3 = g_protect.targets[0].cr3;
        }
    }
    for (i = 0; i < online; i++) {
        if (!g_vcpus[i]) continue;
        g_vcpus[i]->regs.rcx = g_vmmcall_auth_cookie;
        /* 9.154: c0 runs the FULL workload guest (write + hooked-call) under the
           dedicated CR3 — with the full host_stack (4 pages) now mapped, the
           guest's call+ret should work.  c1-c11 stay pure heartbeat. */
        if (i == 0) {
            g_vcpus[i]->vmcb->state.rip =
                (uint64_t)svm_trampoline_test_resident_guest;
            g_vcpus[i]->regs.rdi = (uint64_t)g_resident_workload_page;
            g_vcpus[i]->regs.rsi = (uint64_t)yghv_hook_test_dummy;
        } else {
            g_vcpus[i]->vmcb->state.rip = g_guest_hb_va;
            g_vcpus[i]->regs.rdi = 0;
            g_vcpus[i]->regs.rsi = 0;
        }
        g_vcpus[i]->vmcb->state.cr3 = guest_cr3;   /* dedicated guest CR3 (9.152) */
        sv = svm_core_set_npt(i, g_npt.pml4_pa);
        if (sv) {
            LOG_ERROR("persistent vcpu prepare core %u failed 0x%x", i, sv);
            yghv_protect_cleanup();
            npt_cleanup(&g_npt);
            svm_core_cleanup();
            if (g_resident_workload_page) MmFreeContiguousMemory(g_resident_workload_page);
            g_resident_workload_page = NULL;
            if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
            npt_test_buf = NULL;
            g_npt_test_active = 0;
            if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
            g_guest_code_page = NULL;
            yghv_trace_close();
            KeRevertToUserAffinityThread();
            return (NTSTATUS)sv;
        }
    }
    sv = yghv_control_device_init(d);
    if (!sv)
        sv = svm_core_start_persistent_residents(online);
    if (sv) {
        LOG_ERROR("persistent residents/control device start failed 0x%x", sv);
        svm_core_wait_remote_ready(online);
        svm_core_stop_all_residents();
        svm_core_wait_all_stopped(online);
        yghv_control_device_cleanup(d);
        yghv_protect_cleanup();
        npt_cleanup(&g_npt);
        svm_core_cleanup();
        if (g_resident_workload_page) MmFreeContiguousMemory(g_resident_workload_page);
        g_resident_workload_page = NULL;
        if (npt_test_buf) MmFreeContiguousMemory(npt_test_buf);
        npt_test_buf = NULL;
        g_npt_test_active = 0;
        if (g_guest_code_page) MmFreeContiguousMemory(g_guest_code_page);
        g_guest_code_page = NULL;
        yghv_trace_close();
        KeRevertToUserAffinityThread();
        return (NTSTATUS)sv;
    }
    svm_core_wait_remote_ready(online);
    {
        /* 9.141: start the freeze watchdog as persistent mode begins. */
        NTSTATUS wst = PsCreateSystemThread(
            &g_watchdog_thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
            yghv_freeze_watchdog_thread, NULL);
        if (!NT_SUCCESS(wst))
            g_watchdog_thread = NULL;
    }
    g_persistent_mode = TRUE;
#if YGHV_HOOK_RENDEZVOUS_TEST
    {
        NTSTATUS status = PsCreateSystemThread(
            &g_hook_rendezvous_thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
            yghv_hook_rendezvous_thread, NULL);
        if (!NT_SUCCESS(status)) {
            LOG_ERROR("hook rendezvous: thread create failed 0x%x", status);
            g_hook_rendezvous_thread = NULL;
        }
    }
#endif
    LOG_ERROR("persistent protect mode active: %u cores", online);
    KeRevertToUserAffinityThread();
    return STATUS_SUCCESS;
}
