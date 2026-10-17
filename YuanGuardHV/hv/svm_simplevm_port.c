/* svm_simplevm_port.c — faithful port of SimpleSvm's OS-as-guest enter/exit model
 * for YuanGuardHV step205 (option A). Compiled ONLY when YGHV_BAREMETAL_STEP==205;
 * otherwise an empty translation unit (default build and step203/204 unaffected).
 *
 * Rationale (docs/YGHV_SIMPLEVM_LEVERAGE_20260918.md §7): every YGHV variant that
 * keeps our own svm_trampoline_os_enter model corrupts guest kernel structures,
 * while upstream SimpleSvm runs 600s+ on THIS machine. So we port SimpleSvm's exact
 * mechanism: the per-CPU virtualize routine IS the host loop (VpData-embedded host
 * stack); the guest resumes at the captured RIP (the installed-check line), sees
 * "hypervisor installed" on the 2nd pass and returns so the affinity loop advances.
 * The VMEXIT handler is synchronous-only (CPUID/VMRUN), zero NT calls, zero I/O.
 * NPT reuses g_npt.
 */
#include <ntddk.h>

#if defined(YGHV_BAREMETAL_STEP) && (YGHV_BAREMETAL_STEP == 205)

#include "vmcb.h"        /* YGHV vmcb_t { control; state; } */
#include "svm_defs.h"    /* MSR_EFER, SVM_EXIT_*, INTERCEPT_* */
#include "npt.h"         /* npt_mgr_t (g_npt) */

extern npt_mgr_t g_npt;
extern ULONG     g_vcpu_count;

/* 9.234 root-cause fix: the ntoskrnl-exported RtlCaptureContext captures the
 * CALLER's rip/rsp/gprs (SimpleSvm relies on exactly this contract for its
 * seamless 2nd-pass entry). Our previous p_capture was a REAL C call, so it
 * captured its own frame: guest rip landed inside the helper and guest rsp was
 * the helper's stack -> on the guest 2nd pass the frame was garbage -> ret to
 * a wild address -> triple fault (no SHUTDOWN intercept) -> CPU shutdown state
 * = the observed silent hard hang with no dump. NEVER wrap the capture in a
 * helper function. */
NTKERNELAPI VOID RtlCaptureContext(PCONTEXT ContextRecord);

/* step205b diagnostics: write-through traces, used ONLY in bare-metal / guest
 * syscall context (never inside the VMEXIT island). */
void yghv_trace(const char *msg);
void yghv_trace_u64(const char *label, uint64_t v);

/* ---- low-level helpers (clang-cl; we avoid intrin.h / RtlCaptureContext) ---- */
static uint64_t p_read_msr(uint32_t msr) {
    uint32_t lo, hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static void p_write_msr(uint32_t msr, uint64_t v) {
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr"::"c"(msr),"a"(lo),"d"(hi));
}
static uint64_t p_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
static uint64_t p_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2,%0":"=r"(v)); return v; }
static uint64_t p_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v; }
static uint64_t p_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4,%0":"=r"(v)); return v; }
static uint16_t p_tr(void)  { uint16_t t; __asm__ volatile("str %w0":"=r"(t)); return t; }
/* step205h: read SS from the register itself. RtlCaptureContext's SegSs is NOT
 * reliable on x64 (205e/f read 0x0000 vs 205g 0x493 across boots with identical
 * code — the field is stale/garbage), and VMRUN validates guest CPL from the SS
 * descriptor, so the VMCB must get the hardware value. A plain value read has
 * no frame problem (unlike the old p_capture). */
static uint16_t p_read_ss(void) { uint16_t s; __asm__ volatile("mov %%ss,%0":"=r"(s)); return s; }
static void p_vmsave(uint64_t pa) { __asm__ volatile("vmsave %0"::"r"(pa):"memory"); }
static void p_vmload(uint64_t pa) { __asm__ volatile("vmload %0"::"r"(pa):"memory"); }
static void p_stgi(void)    { __asm__ volatile("stgi"); }
static void p_writeeflags(uint64_t f) { __asm__ volatile("push %0\n\tpopfq"::"r"(f):"cc","memory"); }

static uint32_t p_seg_limit(uint16_t sel) {
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) g;
    __asm__ volatile("sgdt %0":"=m"(g));
    uint64_t lo = ((uint64_t *)(g.b + (sel & ~7u)))[0];
    uint32_t lim = (uint32_t)((lo & 0xFFFFu) | (((lo >> 32) & 0xFu) << 16));
    if (lo & (1ULL << 55)) lim = (lim << 12) | 0xFFF;
    return lim;
}
static uint16_t p_seg_attrib(uint16_t sel) {
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) g;
    __asm__ volatile("sgdt %0":"=m"(g));
    uint64_t lo = ((uint64_t *)(g.b + (sel & ~7u)))[0];
    /* FULL 16-bit VMCB attribute (SimpleSvm SvGetSegmentAccessRight contract):
     * byte0 = type/S/DPL/P (desc bits 40-47), bits 8-11 = AVL/L/D/B/G (desc
     * bits 52-55). Missing the L bit on CS drops the guest out of 64-bit mode
     * on the first fetch -> RIP truncates to 32 bits (bugcheck 0x1E showed the
     * fetch at 0x00000000BF95D7C7 = truncated guest rip). */
    return (uint16_t)(((lo >> 40) & 0xFFu) | (((lo >> 52) & 0xFu) << 8));
}

/* ---- VpData layout: MUST match svm_simplevm_port.S (self at HostRsp+0x10) ---- */
/* step205d: one shared fault record (memory-only writes in the VMEXIT island;
 * main.c traces it from bare-metal context after the entry loop returns). */
typedef struct _SVP_SHARED {
    volatile LONG ProcessorsVirtualized;
    volatile LONG Abort;
    volatile LONG FaultSeen;        /* set by the island on the FIRST bad exit */
    volatile LONG FaultCpu;
    volatile ULONG64 FaultExitcode;
    volatile ULONG64 FaultRip;
    volatile ULONG64 FaultInfo1;
    volatile ULONG64 FaultInfo2;
} SVP_SHARED;

#define SVP_HOST_STACK 0x4000
#define SVP_TRAPFRAME  0x190

typedef struct _SVP_VPD {
    union {
        DECLSPEC_ALIGN(PAGE_SIZE) uint8_t HostStackLimit[SVP_HOST_STACK];
        struct {
            uint8_t  StackContents[SVP_HOST_STACK - 6*8 - SVP_TRAPFRAME];
            uint8_t  TrapFrame[SVP_TRAPFRAME];
            uint64_t GuestVmcbPa;               /* HostRsp +0x00 */
            uint64_t HostVmcbPa;                /* +0x08 */
            struct _SVP_VPD *Self;              /* +0x10 (asm reads) */
            SVP_SHARED *Shared;                 /* +0x18 */
            uint64_t Padding1;
            uint64_t Reserved1;
        } hs;
    };
    DECLSPEC_ALIGN(PAGE_SIZE) vmcb_t GuestVmcb;   /* page-aligned */
    DECLSPEC_ALIGN(PAGE_SIZE) vmcb_t HostVmcb;
    DECLSPEC_ALIGN(PAGE_SIZE) uint8_t HostStateArea[PAGE_SIZE];
    /* step205f: valid MSRPM (2 pages) + IOPM (3 pages), all-zero = permit every
     * MSR / IO port natively. Every proven-good config on this machine (upstream
     * SimpleSvm, YGHV 203) sets NON-ZERO page-aligned bases; leaving them 0 is
     * the one remaining delta and a candidate VMEXIT_INVALID source. We still do
     * NOT enable the MSR_PROT / IOIO intercepts, so the maps are simply valid
     * and unused. */
    DECLSPEC_ALIGN(PAGE_SIZE) uint8_t Msrpm[PAGE_SIZE * 2];
    DECLSPEC_ALIGN(PAGE_SIZE) uint8_t Iopm[PAGE_SIZE * 3];
    /* step205b diagnostics (appended after page-aligned regions): */
    ULONG CpuIndex;
    volatile LONG BadExits;
} SVP_VPD, *PSVP_VPD;

_Static_assert(offsetof(SVP_VPD, GuestVmcb) % PAGE_SIZE == 0, "GuestVmcb page-aligned");
_Static_assert(offsetof(SVP_VPD, HostVmcb) % PAGE_SIZE == 0, "HostVmcb page-aligned");
_Static_assert(offsetof(SVP_VPD, Msrpm) % PAGE_SIZE == 0, "Msrpm page-aligned");
_Static_assert(offsetof(SVP_VPD, Iopm) % PAGE_SIZE == 0, "Iopm page-aligned");
_Static_assert(offsetof(SVP_VPD, hs.Self) - offsetof(SVP_VPD, hs.GuestVmcbPa) == 0x10,
               "Self at HostRsp+0x10");

typedef struct _GUEST_REGS {
    uint64_t R15, R14, R13, R12, R11, R10, R9, R8;
    uint64_t Rdi, Rsi, Rbp;
    uint64_t RspDummy;
    uint64_t Rbx, Rdx, Rcx, Rax;
} GUEST_REGS;

void yghv_sv_launch(void *HostRsp);   /* svm_simplevm_port.S */

#define SVP_CPUID_INSTALLED   0x4FFFFFFCu
#define SVP_CPUID_UNLOAD      0x41414141u
#define SVP_MAGIC             0x504D5653u   /* 'SVMP' */

/* Devirtualize THIS cpu (pure memory + privileged ops; safe inside the GIF=0
 * VMEXIT island — no NT calls). Sets the SvLV20 return contract so the asm
 * resumes bare metal at resume_rip (for the CPUID backdoor that is nRIP; for
 * fault-like exits nRIP is INVALID, so the caller must pass the faulting
 * state.rip instead — never trust nRIP on a fault). */
static void p_devirt(PSVP_VPD Vpd, GUEST_REGS *Regs, uint64_t resume_rip) {
    Regs->Rax = (uint32_t)(UINT_PTR)Vpd;
    Regs->Rdx = (uint64_t)((UINT_PTR)Vpd >> 32);
    Regs->Rbx = resume_rip;
    Regs->Rcx = Vpd->GuestVmcb.state.rsp;
    p_vmload(Vpd->hs.GuestVmcbPa);           /* load guest segs */
    p_write_msr(MSR_EFER, p_read_msr(MSR_EFER) & ~(uint64_t)EFER_SVME);
    p_stgi();                                /* re-enable interrupts */
    p_writeeflags(Vpd->GuestVmcb.state.rflags);
}

/* step205f: park THIS cpu forever (host mode, GIF=0 already cleared by VMEXIT,
 * cli+hlt -> only NMI/SMI could wake and the loop re-halts). Used for exits we
 * cannot resume safely (VMEXIT_INVALID / unknown exitcode, where state.rip and
 * nRIP are untrustworthy) — resuming there is what produced the wild jumps that
 * corrupted unrelated kernel threads (stornvme 0xD1). A parked cpu keeps the
 * machine ALIVE so the write-through progress log (VMCB dump) stays readable. */
__declspec(noreturn) static void p_park_cpu(void) {
    for (;;) {
        __asm__ volatile("cli\n\thlt");
    }
}

/* step205i: dedicated magic-tagged record page. Its PA is logged (host context)
 * at start; after a failed boot we grep the full memory dump for 'SVPRCD1' and
 * read the fields — deterministic recovery, no needle-in-haystack. Island-safe:
 * pure memory writes. */
static unsigned char *g_svp_rec_page = NULL;
/* step205j: pure-memory VMEXIT heartbeat, advanced by the handler on EVERY exit
 * and by the 2nd-pass return. The bare-metal watchdog (reserved core) bugs
 * checks 0xE2 when it stalls -> full dump with the record page + counters. */
volatile LONG64 g_svp_hb = 0;

/* step205j: reserved bare-metal watchdog. One core (the highest index) NEVER
 * enters guest mode and stays native forever, so it can still bug check the
 * machine when every guest-mode core wedges silently (the 205f..205i freezes
 * produced NO dump because nothing bug checked). 10s without a heartbeat
 * (driven by the stress CPUID storm) -> 0xE2 -> full RAM dump containing the
 * SVPRCD1 record page and all counters. */
static VOID yghv_svp_watchdog(PVOID ctx) {
    LARGE_INTEGER delay;
    LONG64 last = 0;
    ULONG stall = 0;
    (void)ctx;
    delay.QuadPart = -1LL * 10000000LL;   /* 1s */
    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (g_svp_hb != last) {
            last = g_svp_hb;
            stall = 0;
        } else if (++stall >= 10) {
        } else if (++stall >= 10) {
            /*
             * 9.334 (Tier 7.2/7.7): a stalled heartbeat is a real problem but
             * not necessarily a dead machine. Report it through the failure
             * framework and keep polling; only bugcheck if it says there is
             * nothing safe left.
             */
            extern int yghv_fail_degrade(int reason, uint64_t a,
                                         uint64_t b, uint64_t c);
            if (!yghv_fail_degrade(2 /* WATCHDOG */,
                                   (uint64_t)g_svp_hb,
                                   (uint64_t)last, 0)) {
                KeBugCheckEx(0xE2, 0x53565052 /* 'SVPR' */,
                             (ULONG_PTR)g_svp_hb, (ULONG_PTR)last, 0);
            }
            stall = 0;
        }
    }
}

static void p_record_fatal(uint64_t exitcode, PSVP_VPD Vpd) {
    volatile ULONG64 *r;
    if (!g_svp_rec_page) return;
    r = (volatile ULONG64 *)g_svp_rec_page;
    r[0] = 0x3131444352505653ULL;    /* memory bytes: 'S','V','P','R','C','D','1',0 */
    r[1] = exitcode;
    r[2] = Vpd->GuestVmcb.state.rip;
    r[3] = Vpd->GuestVmcb.control.exitinfo1;
    r[4] = Vpd->GuestVmcb.control.exitinfo2;
    r[5] = Vpd->CpuIndex;
    r[6] = Vpd->GuestVmcb.control.exitintinfo;
}

/* VMEXIT island: synchronous CPUID/VMRUN only, zero NT calls, zero I/O. */
BOOLEAN NTAPI yghv_sv_handle_vmexit(PSVP_VPD Vpd, GUEST_REGS *Regs);
BOOLEAN NTAPI yghv_sv_handle_vmexit(PSVP_VPD Vpd, GUEST_REGS *Regs) {
    BOOLEAN exit_vm = FALSE;
    uint64_t exitcode = Vpd->GuestVmcb.control.exitcode;
    uint64_t leaf;

    p_vmload(Vpd->hs.HostVmcbPa);          /* restore host segments/GS (KPCR) */
    Regs->Rax = Vpd->GuestVmcb.state.rax;  /* reflect guest RAX (host overwrote it) */
    InterlockedIncrement64(&g_svp_hb);     /* step205j: island heartbeat (pure memory) */

    switch (exitcode) {
    case SVM_EXIT_CPUID:
        leaf = Regs->Rax;
        /* step205h: the guest is demonstrably executing — drop the entry-window
         * exception interception so native page faults flow. */
        Vpd->GuestVmcb.control.exception_intercepts = 0;
        if (leaf == SVP_CPUID_INSTALLED) {
            Regs->Rax = SVP_MAGIC; Regs->Rbx = 0; Regs->Rcx = 0; Regs->Rdx = 0;
        } else if (leaf == SVP_CPUID_UNLOAD) {
            if ((Vpd->GuestVmcb.state.ss_attrib & 0x60u) == 0) {   /* DPL0 */
                p_devirt(Vpd, Regs, Vpd->GuestVmcb.control.next_rip);
                exit_vm = TRUE;
                break;
            }
            { int o[4]; __cpuidex(o, (int)leaf, (int)Regs->Rcx);
              Regs->Rax=o[0]; Regs->Rbx=o[1]; Regs->Rcx=o[2]; Regs->Rdx=o[3]; }
        } else {
            int o[4]; __cpuidex(o, (int)leaf, (int)Regs->Rcx);
            Regs->Rax=o[0]; Regs->Rbx=o[1]; Regs->Rcx=o[2]; Regs->Rdx=o[3];
        }
        Vpd->GuestVmcb.state.rip = Vpd->GuestVmcb.control.next_rip;
        Vpd->GuestVmcb.state.rax = Regs->Rax;
        Vpd->BadExits = 0;                    /* healthy exit resets the bail */
        break;
    case SVM_EXIT_VMRUN:
        /* guest executed VMRUN (would nest) — inject #GP. */
        Vpd->GuestVmcb.control.event_injection =
            (1ULL << 31) | (3ULL << 8) | 13ULL;
        break;
    case SVM_EXIT_MSR:
        /* Only the EFER WRITE is intercepted (MSRPM quadrant 0x1800). Inject #GP
         * so the guest can never clear SVME out from under the hypervisor —
         * SimpleSvm's exact protection intent. */
        Vpd->GuestVmcb.control.event_injection =
            (1ULL << 31) | (3ULL << 8) | 13ULL;
        break;
    default:
        /* step205f: whitelist handling. ANY guest exception during the entry
         * window means broken guest state: record it memory-only (island-safe —
         * NO file I/O, see 9.229), then devirtualize resuming at the FAULTING
         * instruction (state.rip — nRIP is invalid on fault-like exits) so the
         * OS raises the fault observably (bugcheck + dump). For ANY other
         * unexpected exitcode (VMEXIT_INVALID, NPF, INTR, ...) state.rip and
         * nRIP are untrustworthy — resuming there is what produced the wild
         * jumps that corrupted unrelated kernel threads (stornvme 0xD1) — so
         * park THIS cpu instead: the machine stays alive and the write-through
         * VMCB dump in progress.log stays readable. */
        {
            SVP_SHARED *sh = Vpd->hs.Shared;
            if (sh && InterlockedCompareExchange(&sh->FaultSeen, 1, 0) == 0) {
                sh->FaultCpu = (LONG)Vpd->CpuIndex;
                sh->FaultExitcode = exitcode;
                sh->FaultRip = Vpd->GuestVmcb.state.rip;
                sh->FaultInfo1 = Vpd->GuestVmcb.control.exitinfo1;
                sh->FaultInfo2 = Vpd->GuestVmcb.control.exitinfo2;
            }
            /* step205i: ALSO mirror the record into the dedicated magic-tagged
             * page (pure memory, island-safe). After the reboot, the full dump
             * is searched for the 'SVPRCD1' magic — deterministic record
             * recovery, no needle-in-haystack scanning. */
            p_record_fatal(exitcode, Vpd);
        }
        if (exitcode >= SVM_EXIT_EXCEPTION_BASE &&
            exitcode <  SVM_EXIT_EXCEPTION_BASE + 32) {
            p_devirt(Vpd, Regs, Vpd->GuestVmcb.state.rip);
            exit_vm = TRUE;
        } else {
            /* step205i: park SILENTLY. The 205g/h boots proved that even a
             * "terminal" ZwWriteFile from the VMEXIT island deadlocks the whole
             * box before a single byte reaches the disk (the island thread is
             * the only one running the island; blocking it wedges the watchdog
             * too). The record page (above) + the 0x101 dump carry the
             * evidence instead. */
            p_park_cpu();   /* noreturn */
        }
        break;
    }
    return exit_vm;
}

static void p_prepare(PSVP_VPD v, PCONTEXT c, uint64_t ncr3) {
    vmcb_t *g = &v->GuestVmcb;
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) gdtr, idtr;
    __asm__ volatile("sgdt %0":"=m"(gdtr));
    __asm__ volatile("sidt %0":"=m"(idtr));

    g->control.general1_intercepts  = INTERCEPT_CPUID | INTERCEPT_MSR_PROT |
                                      INTR_GEN1(SVM_INTERCEPT_SHUTDOWN);
    g->control.general2_intercepts  = INTR_GEN2(SVM_INTERCEPT_VMRUN);
    /* step205h: intercept every exception EXCEPT #PF during the ENTRY window.
     * Lessons across 205c..205g: with NO exception interception (205g) the fatal
     * fault chain (#GP/#UD -> #DF -> triple fault -> cpu shutdown) is invisible
     * = the silent hard freeze; with ALL interception (205d/e) we caught the
     * CS.L bug but ALSO legit #PFs (stornvme 0xD1). Sweet spot: #PF flows
     * natively (the OS pages constantly), every other fault is captured and
     * devirtualized observably. The first healthy CPUID exit clears this back
     * to 0 (VMCB clean bits are 0 -> control area re-read on every VMRUN).
     * MSRPM intercepts only the EFER write (protect SVME), APM quadrant 0x1800
     * layout: byte 0x1800 + (0x80/8) = 0x1810, bit 0. */
    g->control.exception_intercepts = 0xFFFFFFFFu & ~(1u << 14);
    v->Msrpm[0x1810] |= 0x01;
    g->control.cr_read_intercepts   = 0;
    g->control.cr_write_intercepts  = 0;
    g->control.guest_asid           = 1;
    g->control.tlb_control          = 0;
    g->control.ncr3                 = ncr3;
    g->control.np_enable            = 1;
    /* step205f: valid, page-aligned, all-zero (permit-all) maps — matches every
     * proven-good config (upstream SimpleSvm sets MSRPM; YGHV 203 sets MSRPM).
     * The MSR_PROT/IOIO intercepts stay CLEAR, so these are valid but unused. */
    g->control.msrpm_base_pa = MmGetPhysicalAddress(v->Msrpm).QuadPart;
    g->control.iopm_base_pa  = MmGetPhysicalAddress(v->Iopm).QuadPart;

    g->state.gdtr_base = gdtr.b; g->state.gdtr_limit = gdtr.l;
    g->state.idtr_base = idtr.b; g->state.idtr_limit = idtr.l;
    g->state.cs_selector = (uint16_t)c->SegCs; g->state.ds_selector = (uint16_t)c->SegDs;
    g->state.es_selector = (uint16_t)c->SegEs; g->state.fs_selector = (uint16_t)c->SegFs;
    g->state.gs_selector = (uint16_t)c->SegGs; g->state.ss_selector = p_read_ss();
    g->state.cs_attrib = p_seg_attrib((uint16_t)c->SegCs); g->state.cs_limit = p_seg_limit((uint16_t)c->SegCs);
    g->state.ds_attrib = p_seg_attrib((uint16_t)c->SegDs); g->state.ds_limit = p_seg_limit((uint16_t)c->SegDs);
    g->state.es_attrib = p_seg_attrib((uint16_t)c->SegEs); g->state.es_limit = p_seg_limit((uint16_t)c->SegEs);
    g->state.ss_attrib = p_seg_attrib(p_read_ss()); g->state.ss_limit = p_seg_limit(p_read_ss());
    g->state.tr_selector = p_tr();
    g->state.cr0 = p_cr0(); g->state.cr2 = p_cr2();
    g->state.cr3 = p_cr3(); g->state.cr4 = p_cr4();
    g->state.efer = p_read_msr(MSR_EFER);
    g->state.rflags = c->EFlags;
    g->state.rsp = c->Rsp;
    g->state.rip = c->Rip;
    g->state.g_pat = p_read_msr(0x277);
    p_vmsave(v->hs.GuestVmcbPa);           /* capture FS/GS/TR/KernelGs into guest VMCB */
    v->hs.Reserved1 = ~0ULL;
}

static NTSTATUS p_virtualize_one(SVP_SHARED *shared) {
    CONTEXT ctx;
    PSVP_VPD v;
    SIZE_T total;
    int cp[4];
    ULONG cpu = KeGetCurrentProcessorNumber();

    /* SvVirtualizeProcessor-exact shape. RtlCaptureContext (ntoskrnl export)
     * captures THIS function's rip/rsp/gprs; the guest 2nd pass resumes at the
     * installed-check line below with callee-saved registers intact, so the
     * plain `return` unwinds the affinity loop correctly. */
    RtlCaptureContext(&ctx);

    __cpuidex(cp, SVP_CPUID_INSTALLED, 0);
    if ((uint32_t)cp[0] == SVP_MAGIC) {     /* already virtualized (guest 2nd pass) */
        /* step205j: NO NT call here. 205c..205i never once reached this point's
         * trace, and the guest-return ZwWriteFile was the ONLY NT call in the
         * VMRUN->2nd-pass window — the prime freeze suspect. Advance the pure-
         * memory heartbeat instead; the bare-metal watchdog (below) turns a
         * stalled heartbeat into a 0xE2 dump. */
        InterlockedIncrement(&g_svp_hb);
        return STATUS_SUCCESS;
    }

    total = (sizeof(SVP_VPD) + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    v = (PSVP_VPD)MmAllocateContiguousMemory(total, (PHYSICAL_ADDRESS){ .QuadPart = -1 });
    if (!v) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(v, total);
    v->CpuIndex = cpu;

    p_write_msr(MSR_EFER, p_read_msr(MSR_EFER) | EFER_SVME);

    v->hs.GuestVmcbPa = MmGetPhysicalAddress(&v->GuestVmcb).QuadPart;
    v->hs.HostVmcbPa  = MmGetPhysicalAddress(&v->HostVmcb).QuadPart;
    v->hs.Self        = v;
    v->hs.Shared      = shared;
    p_write_msr(0xC0010117, MmGetPhysicalAddress(v->HostStateArea).QuadPart); /* VM_HSAVE_PA */
    p_prepare(v, &ctx, g_npt.pml4_pa);
    p_vmsave(v->hs.HostVmcbPa);             /* snapshot host segs for VMLOAD on exit */
    InterlockedIncrement(&shared->ProcessorsVirtualized);

    /* step205d: full VMCB dump (bare-metal context, write-through) for offline
     * diff against SimpleSvm's known-good values before this cpu's first VMRUN. */
    yghv_trace_u64("s205 pre-vmrun cpu", (uint64_t)cpu);
    yghv_trace_u64("s205 V g1", v->GuestVmcb.control.general1_intercepts);
    yghv_trace_u64("s205 V g2", v->GuestVmcb.control.general2_intercepts);
    yghv_trace_u64("s205 V exc", v->GuestVmcb.control.exception_intercepts);
    yghv_trace_u64("s205 V asid", v->GuestVmcb.control.guest_asid);
    yghv_trace_u64("s205 V ncr3", v->GuestVmcb.control.ncr3);
    yghv_trace_u64("s205 V npen", v->GuestVmcb.control.np_enable);
    yghv_trace_u64("s205 V cr0", v->GuestVmcb.state.cr0);
    yghv_trace_u64("s205 V cr3", v->GuestVmcb.state.cr3);
    yghv_trace_u64("s205 V cr4", v->GuestVmcb.state.cr4);
    yghv_trace_u64("s205 V efer", v->GuestVmcb.state.efer);
    yghv_trace_u64("s205 V rip", v->GuestVmcb.state.rip);
    yghv_trace_u64("s205 V rsp", v->GuestVmcb.state.rsp);
    yghv_trace_u64("s205 V rflags", v->GuestVmcb.state.rflags);
    yghv_trace_u64("s205 V cs", v->GuestVmcb.state.cs_attrib);
    yghv_trace_u64("s205 V ss", v->GuestVmcb.state.ss_attrib);
    yghv_trace_u64("s205 V gdtb", v->GuestVmcb.state.gdtr_base);
    yghv_trace_u64("s205 V idtb", v->GuestVmcb.state.idtr_base);
    yghv_trace_u64("s205 V pat", v->GuestVmcb.state.g_pat);
    yghv_sv_launch(&v->hs.GuestVmcbPa);     /* host loop; returns only after unload */
    /* Unreachable in normal operation (the SvLV20 unload tail jumps back into the
     * guest's flow, not here). Mirror SimpleSvm's fail-loud: */
    KeBugCheck(MANUALLY_INITIATED_CRASH);
    return STATUS_SUCCESS;
}

static void p_run_on_each(SVP_SHARED *shared, ULONG n) {
    GROUP_AFFINITY old, ga;
    ULONG i;
    PROCESSOR_NUMBER pn;
    for (i = 0; i < n; i++) {
        if (KeGetProcessorNumberFromIndex(i, &pn) != STATUS_SUCCESS) continue;
        RtlZeroMemory(&ga, sizeof(ga));
        ga.Group = pn.Group; ga.Mask = (KAFFINITY)(1ULL << pn.Number);
        RtlZeroMemory(&old, sizeof(old));
        KeSetSystemGroupAffinityThread(&ga, &old);
        (void)p_virtualize_one(shared);
        KeRevertToUserGroupAffinityThread(&old);
    }
}

void yghv_sv205_start(ULONG cores);
void yghv_sv205_start(ULONG cores) {
    SVP_SHARED shared;
    HANDLE wd = NULL;
    ULONG vcores;
    shared.ProcessorsVirtualized = 0; shared.Abort = 0; shared.FaultSeen = 0;
    /* step205i: record page + log its PA so a failed boot's full dump can be
     * searched for 'SVPRCD1' deterministically. */
    g_svp_rec_page = (unsigned char *)MmAllocateContiguousMemory(
        PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = -1 });
    if (g_svp_rec_page) {
        RtlZeroMemory(g_svp_rec_page, PAGE_SIZE);
        yghv_trace_u64("s205 rec pa",
                       MmGetPhysicalAddress(g_svp_rec_page).QuadPart);
    }
    /* step205j: reserve the HIGHEST core as a bare-metal watchdog that never
     * enters guest mode; it converts any silent freeze into a 0xE2 dump. */
    vcores = (cores > 1) ? (cores - 1) : cores;
    if (cores > 1) {
        GROUP_AFFINITY wga, wold;
        PROCESSOR_NUMBER wpn;
        if (NT_SUCCESS(KeGetProcessorNumberFromIndex(cores - 1, &wpn))) {
            RtlZeroMemory(&wga, sizeof(wga));
            wga.Group = wpn.Group; wga.Mask = (KAFFINITY)(1ULL << wpn.Number);
            RtlZeroMemory(&wold, sizeof(wold));
            KeSetSystemGroupAffinityThread(&wga, &wold);
            PsCreateSystemThread(&wd, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                 yghv_svp_watchdog, NULL);
            KeRevertToUserGroupAffinityThread(&wold);
        }
    }
    yghv_trace_u64("s205 vcores", (uint64_t)vcores);
    p_run_on_each(&shared, vcores);
    /* step205d: if any core bailed on a guest fault during entry, the record is
     * in memory and we are back in bare-metal context — trace it now (this is
     * what turns the old silent shutdown into a diagnosable run). */
    if (shared.FaultSeen) {
        yghv_trace("s205 FAULT during entry (core devirtualized)");
        yghv_trace_u64("s205 fault cpu", (uint64_t)(LONG)shared.FaultCpu);
        yghv_trace_u64("s205 fault exit", shared.FaultExitcode);
        yghv_trace_u64("s205 fault rip", shared.FaultRip);
        yghv_trace_u64("s205 fault info1", shared.FaultInfo1);
        yghv_trace_u64("s205 fault info2", shared.FaultInfo2);
    }
}

#else
/* Non-205 builds still compile+link svm_simplevm_port.S (the launch loop), which
 * references yghv_sv_handle_vmexit. Nothing ever calls yghv_sv_launch here, so
 * provide a stub so the (dead) asm object links; the real handler is above. */
typedef int svp_unused_t;
unsigned char yghv_sv_handle_vmexit(void *vpd, void *regs);
unsigned char yghv_sv_handle_vmexit(void *vpd, void *regs) { (void)vpd; (void)regs; return 0; }
#endif
