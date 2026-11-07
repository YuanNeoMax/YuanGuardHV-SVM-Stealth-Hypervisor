/*!
    @file       SimpleSvm.cpp

    @brief      All C code.

    @author     Satoshi Tanda

    @copyright  Copyright (c) 2017-2020, Satoshi Tanda. All rights reserved.
 */
#define POOL_NX_OPTIN   1
#include "common/npt.h"
/* 9.375 (Tier 9.1): device-free control channel. Included here rather than via a
 * common header because this file has its own include scheme (see the note in
 * mem_bp.c: pulling <ntddk.h> in the wrong order redefines PEPROCESS). */
#include "common/yghv_channel.h"
#include "svm_simplevm206.hpp"

#include <intrin.h>
#include <ntifs.h>
#include <stdarg.h>

/* 9.443: the 206 guest stub, defined in svm_simplevm206_x64.asm.
 *
 * Declared at file scope because extern "C" cannot be spelled inside a block.
 * This is the guest the 206 line runs: a loop whose only intercepted instruction
 * is `vmmcall`, so the handler gets a vote on every iteration and can end the
 * guest at a point we chose. See the long note at the stub itself. */
extern "C" void Sv206GuestStub(VOID);

/*
 * 9.444: the exit thunk. Defined in svm_simplevm206_x64.asm; it only does `ret`,
 * and it must be entered with RSP already pointing at the thread's real stack.
 * See the long note at the thunk for why a C function cannot be the exit target.
 */
extern "C" void Sv206LeaveToHost(VOID);

/*
 * 9.443: per-core VMRUN threads.
 *
 * Each core gets its own system thread (see Sv206CoreThread). These hold the thread
 * handles and the started/exited counts, so DriverUnload can report how many were
 * started and wait for how many have genuinely left the guest.
 *
 * SVM206_MAX_THREADS is generous: this machine has 12 cores, and the array costs
 * 64 * 8 = 512 bytes. Sizing it to the hardware would make the driver's behaviour
 * depend on the build machine, which is exactly the kind of assumption this tree
 * has already paid for.
 *
 * Declared HERE, at the top, because the exit path needs the limit and the counter.
 */
#define SVM206_MAX_THREADS 64
static HANDLE g_S206CoreThreads[SVM206_MAX_THREADS] = { 0 };
static volatile LONG g_S206CoresStarted = 0;
static volatile LONG g_S206CoresExited = 0;

/*
 * 9.445: WHERE THE EXIT STACK POINTER COMES FROM - AND WHY NOTHING HERE TRACKS IT.
 *
 * Four earlier attempts each maintained some piece of state to reconstruct the
 * thread's real stack on exit: a driver-allocated stack, a compiler intrinsic, a
 * global array indexed by core, a value handed through an entry trampoline. Each was
 * wrong in a different way (0x1E / 0x0E / 0x50 / 0xF7).
 *
 * None of it is needed. Sv206LaunchVm captures RSP with one instruction before its
 * `mov rsp, rcx` switches stacks, and stores it in GuestVmcb.ControlArea.HostExitRsp.
 * That register value is the thread's own stack at the call - no reconstruction, no
 * index, no ordering question. So there is no bookkeeping to keep in this file.
 */

//
// 9.246 step206-B seams (both gated; with the gates OFF this file is the
// 206-A baseline verbatim). YGHV_206B_GNPT: take the nested page table root
// from YuanGuardHV's own NPT manager (g_npt) instead of building SimpleSvm's
// private 1TB identity map, so protection semantics (NPF perm/split/exclude)
// apply to the guest. YGHV_206B_COEXIST: main.c dispatches us AFTER its full
// init sequence, so yghv's svm_core/npt state exists when we virtualize.
//
#if defined(YGHV_206B_GNPT)
extern "C" {
typedef struct { UINT64 pml4_pa; void *pml4_va; UINT64 total_mapped_pages; UINT64 total_mapped_2mb_pages; } yghv_npt_mgr_t;
extern yghv_npt_mgr_t g_npt;
}
#if !defined(YGHV_206B_COEXIST)
#error "YGHV_206B_GNPT requires YGHV_206B_COEXIST: g_npt is only initialized after yghv's init sequence; the early-dispatch (206-A) path would run with NCr3=0"
#endif
#endif

//
// 9.249 step206-C2: YuanGuardHV protection bridge (lock-free in-island NPF
// verdict + page re-arm), plus a last-hit record for the control plane to
// query. These are YGHV functions (protect.c); the vendored file keeps its
// own include set, so re-declare the two entry points here with C linkage.
// Ordinals match protect.h's yghv_npf_result_t: 0=NONE, 1=ALLOW, 2=DENY.
//
extern "C" {
// ordinals match protect.h's yghv_npf_result_t: 0=NONE, 1=ALLOW, 2=DENY
int yghv_protect_on_npf_write_bare(UINT64 guest_cr3, UINT32 cpl,
                                   UINT64 gpa, UINT64 *rearm_gpa, int *flip);
int yghv_protect_arm_page_bare(UINT64 gpa);
void *yghv_protect_find_page_bare(UINT64 gpa);
int yghv_protect_fake_bare(UINT64 gpa, UINT64 *rearm_out);
void yghv_protect_fake_restore(UINT64 gpa);
void yghv_protect_fake_invpga_armeds(UINT32 asid);
LONG yghv_protect_fake_alt_active(void);   /* 9.289 C6.1 */
void SvInvlpgaByVa(UINT64 gva, UINT32 asid);         // asm helper
UINT64 yghv_protect_fake_alt_pa(void);
/* Tier 4.1: split-view NPT selection (island-safe, no calls). */
extern UINT64 yghv_view_pick_ncr3(UINT64 guest_cr3);
/* Tier 4.6: NPF watchpoints (island-safe). */
extern int  yghv_watch_match(UINT64 gpa, UINT32 kind);
extern void yghv_watch_record(UINT64 cr3, UINT64 rip, UINT64 gpa, UINT32 kind, UINT32 cpl);
/* Tier 4.4: instruction-level breakpoints (returns 1 = take the ALLOW path). */
extern int yghv_bp_on_npf(UINT64 gpa, UINT64 rip, UINT32 kind, UINT32 cpl, UINT64 cr3);
/* Tier 7.1 (9.336): a page we hid from the guest - never restore it. */
extern int yghv_guard_is_hidden(UINT64 gpa);
/* Tier 7.1 (9.343): hide-our-own-tables support. */
extern int yghv_guard_fault_is_ours(UINT64 rip, UINT32 cpl);
/* Tier 7.4 (9.343): exit-latency self-measurement. */
extern void yghv_sentry_exit_begin(UINT64 tsc);
extern void yghv_sentry_exit_end(UINT64 tsc);
/*
 * 9.366 (Tier 7.5): escape recording — every rejected guest input, with context.
 * Island-safe by construction (counters only; see yghv_sentry.c).
 */
extern void yghv_sentry_escape(UINT32 kind, UINT64 detail, UINT64 a, UINT64 b);
/* Kinds, mirroring common/yghv_sentry.h (the vendored file keeps its own
 * include set, so re-state the ordinals here exactly as the enum declares them). */
#define YGHV_ESC_MSR_INDEX   0
#define YGHV_ESC_IO_PORT     1
#define YGHV_ESC_CR_VALUE    2
#define YGHV_ESC_VMCB_FIELD  3
#define YGHV_ESC_IOCTL_ARG   4
#define YGHV_ESC_GPA_GUARD   5
/*
 * 9.366 (Tier 7.6): the intercept inventory, filled from where it is armed.
 * PASSIVE-only; called from SvPrepareForVirtualization, which is where the mask
 * is actually set.
 *
 * 9.368 (A2): the FUNCTION that filled it (yghv_sentry_surface_arm_intercepts)
 * is gone — it carried a hand-written list of nine names, which is exactly the
 * two-places-must-agree shape this project keeps finding. The inventory is now
 * derived from the mask on the spot (SvArmSurfaceFromMask below), so this file
 * still needs the two sentry entry points but no longer needs the one that
 * guessed.
 */
extern int  yghv_sentry_surface_armed(void);
extern int  yghv_sentry_surface_add(const char *name);
extern void yghv_sentry_surface_mark_armed(void);

extern int  yghv_sentry_surface_armed(void);
extern int yghv_guard_remap_bare(UINT64 gpa);
extern int yghv_guard_reopen_bare(UINT64 gpa);
/* 9.350: re-hide using the entry's recorded mechanism. */
extern int yghv_guard_rehide_bare(UINT64 gpa);
extern int yghv_guard_decoy_bare(UINT64 gpa);
extern int yghv_guard_hiding_armed(void);
extern int yghv_guard_decoy_ready(void);
extern UINT64 yghv_guard_decoy_pa(void);
extern int yghv_protect_npt_remap_flags(UINT64 gpa, UINT64 spa, int use_alt, UINT64 flags);
/* Tier 4.4: re-arm an instruction-level breakpoint page (PASSIVE). */
extern int yghv_bp_rearm_bare(UINT64 gpa);
/* Tier 4.4: change one page's NPT permission (PASSIVE). */
extern int yghv_protect_npt_set_perm(UINT64 gpa, UINT64 perm);
/* Tier 4.4 (9.327, INT3 mode): #BP handling + the byte we displaced. */
extern int yghv_bp_on_int3(UINT64 va, UINT32 cpl, UINT64 cr3);
extern int yghv_bp_int3_orig(UINT64 va, UINT8 *out);
/* 9.373: island-safe shadow switch (leaf write only). */
extern int yghv_bp_shadow_switch(UINT64 gpa, int to_shadow);
/* Tier 4.4 (9.327, DR mode): which breakpoint fired, per DR6. */
extern int yghv_bp_on_dr(UINT64 dr6, UINT64 rip, UINT32 cpl, UINT64 cr3);
/* Physical write, used to restore/patch the instruction byte. */
extern int yghv_pa_write(UINT64 pa, const void *src, size_t len);
extern UINT64 yghv_protect_fake_main_pa(void);
extern void yghv_protect_reopen_page_bare(UINT64 gpa);
extern UINT64 yghv_protect_guest_va_to_pa(UINT64 cr3, UINT64 va);
extern UINT64 yghv_protect_control_walk_gpa(void);
/* 9.297 (A'): runtime TLB-flush strategy for the fake path (see protect.c).
 * plan() returns the TlbControl value; *use_invlpga says whether to INVLPGA. */
int yghv_protect_fake_tlb_plan(int *use_invlpga);
int yghv_protect_fake_tlb_get(void);
int yghv_protect_fake_reopen_main(UINT64 gpa);
}
static volatile UINT64 g_S206LastProtectHit = 0;
/* 9.258 (206-C3): control-plane read of the last protection verdict hit. */
extern "C" UINT64 yghv_s206_last_hit(void) { return g_S206LastProtectHit; }
// 9.251: DENY telemetry ring (cr3/gpa/cpl triplets, island-memory only).
// Flushed to progress.log by the yghv DriverUnload path AFTER Sv206CoopUnload
// (bare-metal context, file I/O safe).
extern "C" void yghv_trace(const char *msg);
extern "C" void yghv_trace_u64(const char *label, UINT64 v);
static volatile UINT64 g_S206DenyLog[192] = { 0 };
static volatile ULONG g_S206DenyIdx = 0;
/* 9.252: NPF-entry telemetry (write-fault NPFs reaching the handler) */
static volatile UINT64 g_S206NpfLog[96] = { 0 };
static volatile ULONG g_S206NpfIdx = 0;
/* 9.255: verdict ring — (gpa, vr|TF<<3) per write-NPF verdict. The deny ring
 * only records DENYs and c11/c12 left the ALLOW aftermath invisible; this
 * records EVERY verdict so the ALLOW/#DB window becomes observable. */
static volatile UINT64 g_S206VrLog[64] = { 0 };
static volatile ULONG g_S206VrIdx = 0;
/* 9.255: #DB ring — (rip, rearmSlot, rflags); 9.258: + DR6 (4-tuple, cap 8)
 * to identify WHY the c14 run saw 3 #DBs per round (BS vs breakpoint bits). */
static volatile UINT64 g_S206DbgLog[128] = { 0 };
static volatile ULONG g_S206DbgIdx = 0;
extern "C" void yghv_s206_flush_deny_log(void) {
    /* 9.252: NPF-entry records (write faults reaching the handler) — dumps
     * even when DENY was never taken (find_page-miss path). */
    if (g_S206NpfIdx == 0)
        yghv_trace("s206 npf-log empty");
    else {
        yghv_trace_u64("s206 npf count", (UINT64)g_S206NpfIdx);
        for (ULONG q = 0; q < g_S206NpfIdx && q < 32; q++) {
            yghv_trace_u64("s206 npf cr3", g_S206NpfLog[q * 3]);
            yghv_trace_u64("s206 npf gpa", g_S206NpfLog[q * 3 + 1]);
            yghv_trace_u64("s206 npf cpl", g_S206NpfLog[q * 3 + 2]);
        }
        g_S206NpfIdx = 0;
    }
    /* 9.255: verdict records */
    if (g_S206VrIdx == 0)
        yghv_trace("s206 vr-log empty");
    else {
        /*
         * 9.448c: THIS LOOP HAD NO ARRAY-BOUND GUARD AT ALL.
         *
         * `q * 2 < g_S206VrIdx` was the only condition, so a corrupted or
         * over-counted index walked off the end of g_S206VrLog with no second
         * check. Bounding it by sizeof() - rather than by a number that happens
         * to match today - is what keeps the next resizing from reintroducing the
         * 0x1E this step exists to fix.
         */
        const ULONG entries = InterlockedCompareExchange(
            (volatile LONG *)&g_S206VrIdx, 0, 0);
        const ULONG slots = (ULONG)(sizeof(g_S206VrLog) / sizeof(g_S206VrLog[0]));
        ULONG q, qmax;

        yghv_trace_u64("s206 vr count", (UINT64)entries);
        qmax = (entries / 2 < slots / 2) ? entries / 2 : slots / 2;

        for (q = 0; q < qmax; q++) {
            yghv_trace_u64("s206 vr gpa", g_S206VrLog[q * 2]);
            yghv_trace_u64("s206 vr verdict", g_S206VrLog[q * 2 + 1]);
        }
        g_S206VrIdx = 0;
    }
    /* 9.255: #DB handler hits; 9.258: +DR6 */
    if (g_S206DbgIdx == 0)
        yghv_trace("s206 dbg-log empty");
    else {
        const ULONG entries = InterlockedCompareExchange(
            (volatile LONG *)&g_S206DbgIdx, 0, 0);
        const ULONG slots = (ULONG)(sizeof(g_S206DbgLog) / sizeof(g_S206DbgLog[0]));
        ULONG q, qmax;

        yghv_trace_u64("s206 dbg count", (UINT64)entries);
        qmax = (entries / 4 < slots / 4) ? entries / 4 : slots / 4;

        for (q = 0; q < qmax; q++) {
            yghv_trace_u64("s206 dbg rip", g_S206DbgLog[q * 4]);
            yghv_trace_u64("s206 dbg rearm", g_S206DbgLog[q * 4 + 1]);
            yghv_trace_u64("s206 dbg rflags", g_S206DbgLog[q * 4 + 2]);
            yghv_trace_u64("s206 dbg dr6", g_S206DbgLog[q * 4 + 3]);
        }
        g_S206DbgIdx = 0;
    }
    if (g_S206DenyIdx == 0) {
        yghv_trace("s206 deny-log empty");
        return;
    }
    /*
     * 9.448c: SNAPSHOT THE INDEX ONCE, AND BOUND IT BY THE ARRAY, NOT BY A
     * CONSTANT THAT HAPPENS TO MATCH IT TODAY.
     *
     * Two separate defects lived in the loop that follows:
     *
     *  1. `q * 6 < g_S206DenyIdx` re-reads a volatile that other cores are
     *     still incrementing, so the bound moves while the loop runs.
     *  2. `q < 32` was the real guard, and 32 is only right because 32*6 = 192
     *     equals the array size. Change the array and the guard is silently
     *     wrong - the kind of coupling a size expression removes.
     *
     * The crash this step fixes was NOT in this loop (the guard held), but this
     * loop is where the corrupt index became a faulting address, so it is the
     * one place a reader will look first. Making it depend on sizeof() means the
     * next person to resize the ring cannot reintroduce the fault here.
     */
    {
        const ULONG entries = InterlockedCompareExchange(
            (volatile LONG *)&g_S206DenyIdx, 0, 0);
        const ULONG slots = (ULONG)(sizeof(g_S206DenyLog) / sizeof(g_S206DenyLog[0]));
        ULONG q, qmax;

        yghv_trace_u64("s206 deny count", (UINT64)entries);

        /* A torn/over-counted index must still not walk past the array. */
        qmax = (entries / 6 < slots / 6) ? entries / 6 : slots / 6;

        for (q = 0; q < qmax; q++) {
            yghv_trace_u64("s206 deny cr3", g_S206DenyLog[q * 6]);
            yghv_trace_u64("s206 deny gpa", g_S206DenyLog[q * 6 + 1]);
            yghv_trace_u64("s206 deny cpl", g_S206DenyLog[q * 6 + 2]);
            yghv_trace_u64("s206 deny gva", g_S206DenyLog[q * 6 + 3]);
            yghv_trace_u64("s206 deny rip", g_S206DenyLog[q * 6 + 4]);
            yghv_trace_u64("s206 deny aux", g_S206DenyLog[q * 6 + 5]);
        }
    }
    g_S206DenyIdx = 0;
}
// 9.250: the re-arm slot moved INTO the per-VCPU VMCB (ControlArea.GuestPaOfGhcb,
// repurposed — SEV-ES only, we are not SEV). The old machine-global
// g_S206RearmGpa raced when two cores had concurrent protected-write cycles.

// 9.250c: fatal-state record, stored per-VCPU in the VPD's Padding1..Reserved1
// pair is too small; instead we keep a small static record (one crash at a
// time is enough) AND the caller KeBugCheckEx's with the details, so the full
// dump carries everything. Pure memory, island-safe.
static volatile UINT64 g_S206Fatal[4] = { 0 };
static VOID S206RecordFatal(UINT64 exitcode, UINT64 rip, UINT64 extra) {
    g_S206Fatal[0] = 0x3156433252505653ULL; /* 'SVPR2CV1' */
    g_S206Fatal[1] = exitcode;
    g_S206Fatal[2] = rip;
    g_S206Fatal[3] = extra;
}

EXTERN_C DRIVER_INITIALIZE Sv206Entry;
static DRIVER_UNLOAD SvDriverUnload;
static CALLBACK_FUNCTION SvPowerCallbackRoutine;

EXTERN_C
VOID
_sgdt (
    _Out_ PVOID Descriptor
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
DECLSPEC_NORETURN
EXTERN_C
VOID
NTAPI
Sv206LaunchVm (
    _In_ PVOID HostRsp
    );

//
// x86-64 defined structures.
//

//
// See "2-Mbyte PML4E-Long Mode" and "2-Mbyte PDPE-Long Mode".
//
typedef struct _PML4_ENTRY_2MB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Reserved1 : 3;           // [6:8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 PageFrameNumber : 40;    // [12:51]
            UINT64 Reserved2 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PML4_ENTRY_2MB, *PPML4_ENTRY_2MB,
  PDPT_ENTRY_2MB, *PPDPT_ENTRY_2MB;
static_assert(sizeof(PML4_ENTRY_2MB) == 8,
              "PML4_ENTRY_1GB Size Mismatch");

//
// See "2-Mbyte PDE-Long Mode".
//
typedef struct _PD_ENTRY_2MB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Dirty : 1;               // [6]
            UINT64 LargePage : 1;           // [7]
            UINT64 Global : 1;              // [8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 Pat : 1;                 // [12]
            UINT64 Reserved1 : 8;           // [13:20]
            UINT64 PageFrameNumber : 31;    // [21:51]
            UINT64 Reserved2 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PD_ENTRY_2MB, *PPD_ENTRY_2MB;
static_assert(sizeof(PD_ENTRY_2MB) == 8,
              "PDE_ENTRY_2MB Size Mismatch");

//
// See "GDTR and IDTR Format-Long Mode"
//
#include <pshpack1.h>
typedef struct _DESCRIPTOR_TABLE_REGISTER
{
    UINT16 Limit;
    ULONG_PTR Base;
} DESCRIPTOR_TABLE_REGISTER, *PDESCRIPTOR_TABLE_REGISTER;
static_assert(sizeof(DESCRIPTOR_TABLE_REGISTER) == 10,
              "DESCRIPTOR_TABLE_REGISTER Size Mismatch");
#include <poppack.h>

//
// See "Long-Mode Segment Descriptors" and some of definitions
// (eg, "Code-Segment Descriptor-Long Mode")
//
typedef struct _SEGMENT_DESCRIPTOR
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT16 LimitLow;        // [0:15]
            UINT16 BaseLow;         // [16:31]
            UINT32 BaseMiddle : 8;  // [32:39]
            UINT32 Type : 4;        // [40:43]
            UINT32 System : 1;      // [44]
            UINT32 Dpl : 2;         // [45:46]
            UINT32 Present : 1;     // [47]
            UINT32 LimitHigh : 4;   // [48:51]
            UINT32 Avl : 1;         // [52]
            UINT32 LongMode : 1;    // [53]
            UINT32 DefaultBit : 1;  // [54]
            UINT32 Granularity : 1; // [55]
            UINT32 BaseHigh : 8;    // [56:63]
        } Fields;
    };
} SEGMENT_DESCRIPTOR, *PSEGMENT_DESCRIPTOR;
static_assert(sizeof(SEGMENT_DESCRIPTOR) == 8,
              "SEGMENT_DESCRIPTOR Size Mismatch");

typedef struct _SEGMENT_ATTRIBUTE
{
    union
    {
        UINT16 AsUInt16;
        struct
        {
            UINT16 Type : 4;        // [0:3]
            UINT16 System : 1;      // [4]
            UINT16 Dpl : 2;         // [5:6]
            UINT16 Present : 1;     // [7]
            UINT16 Avl : 1;         // [8]
            UINT16 LongMode : 1;    // [9]
            UINT16 DefaultBit : 1;  // [10]
            UINT16 Granularity : 1; // [11]
            UINT16 Reserved1 : 4;   // [12:15]
        } Fields;
    };
} SEGMENT_ATTRIBUTE, *PSEGMENT_ATTRIBUTE;
static_assert(sizeof(SEGMENT_ATTRIBUTE) == 2,
              "SEGMENT_ATTRIBUTE Size Mismatch");

//
// SimpleSVM specific structures.
//

typedef struct _PML4E_TREE
{
    DECLSPEC_ALIGN(PAGE_SIZE) PDPT_ENTRY_2MB PdptEntries[512];
    DECLSPEC_ALIGN(PAGE_SIZE) PD_ENTRY_2MB PdEntries[512][512];
} PML4E_TREE, *PPML4E_TREE;

typedef struct _SHARED_VIRTUAL_PROCESSOR_DATA
{
    PVOID MsrPermissionsMap;
    DECLSPEC_ALIGN(PAGE_SIZE) PML4_ENTRY_2MB Pml4Entries[512];
    DECLSPEC_ALIGN(PAGE_SIZE) PML4E_TREE Pml4eTrees[2];    // For 1TB
} SHARED_VIRTUAL_PROCESSOR_DATA, *PSHARED_VIRTUAL_PROCESSOR_DATA;

typedef struct _VIRTUAL_PROCESSOR_DATA
{
    union
    {
        //
        //  Low     HostStackLimit[0]                        StackLimit
        //  ^       ...
        //  ^       HostStackLimit[KERNEL_STACK_SIZE - 2]    StackBase
        //  High    HostStackLimit[KERNEL_STACK_SIZE - 1]    StackBase
        //
        DECLSPEC_ALIGN(PAGE_SIZE) UINT8 HostStackLimit[KERNEL_STACK_SIZE];
        struct
        {
            UINT8 StackContents[KERNEL_STACK_SIZE - (sizeof(PVOID) * 6) - sizeof(KTRAP_FRAME)];
            KTRAP_FRAME TrapFrame;
            UINT64 GuestVmcbPa;     // HostRsp
            UINT64 HostVmcbPa;
            struct _VIRTUAL_PROCESSOR_DATA* Self;
            PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData;
            UINT64 Padding1;        // To keep HostRsp 16 bytes aligned
            UINT64 Reserved1;
        } HostStackLayout;
    };

    DECLSPEC_ALIGN(PAGE_SIZE) VMCB GuestVmcb;
    DECLSPEC_ALIGN(PAGE_SIZE) VMCB HostVmcb;
    DECLSPEC_ALIGN(PAGE_SIZE) UINT8 HostStateArea[PAGE_SIZE];
} VIRTUAL_PROCESSOR_DATA, *PVIRTUAL_PROCESSOR_DATA;
/*
 * 9.542: THE IRETQ FRAME, OVERLAID ON THE FREE TAIL OF StackContents.
 *
 * 9.541 tried to APPEND a field to HostStackLayout and crashed with
 * 0x1E / C000001D (illegal instruction). Cause: HostStackLimit carries
 * DECLSPEC_ALIGN(PAGE_SIZE), so any growth rounds the union up by a WHOLE PAGE and
 * GuestVmcb / HostVmcb / HostStateArea each move one page later - while the asm
 * addresses them with constants computed from the OLD layout (5FD0h, 6110h,
 * KTRAP_FRAME_SIZE equ 190h). The static_assert failed and reported it; the assert
 * was edited instead of read, and the crash followed.
 *
 * THIS VERSION ADDS NO MEMBER. StackContents is 11.6 KB that nothing in this tree
 * references (no reads, no writes - only the definition). RSP points at
 * GuestVmcbPa, i.e. at the top of TrapFrame, so the LAST bytes of StackContents are
 * the free stack immediately below RSP. The frame overlays them:
 *
 *     StackContents[ .................... ][ Rip Cs Rflags Rsp Ss ]
 *                                          ^ sizeof(StackContents) - sizeof(slot)
 *
 * sizeof(VIRTUAL_PROCESSOR_DATA): unchanged. Every offset: unchanged.
 *
 * ORDER IS THE ARCHITECTURAL IRETQ ORDER. A struct with the right fields in the
 * wrong order loads CS out of what it believed was RSP - a fault that looks like
 * memory corruption rather than like a layout mistake.
 *
 * NOTHING USES IT YET: step 1 is layout only, and the exit is still
 * `mov rsp,rcx ; jmp rbx`. A slot that changes nothing is the only way to prove the
 * layout before anything depends on it.
 */
typedef struct _S206_IRET_SLOT
{
    UINT64 Rip;
    UINT64 Cs;
    UINT64 Rflags;
    UINT64 Rsp;
    UINT64 Ss;
} S206_IRET_SLOT, *PS206_IRET_SLOT;

/*
 * The address is COMPUTED from the array, never hand-written: if StackContents ever
 * changes size, this follows it. A literal offset would silently point into the
 * middle of something else - which is 9.541's failure mode in miniature.
 */
static PS206_IRET_SLOT Sv206IretSlot(PVIRTUAL_PROCESSOR_DATA vp)
{
    return reinterpret_cast<PS206_IRET_SLOT>(
                &vp->HostStackLayout.StackContents[
                    sizeof(vp->HostStackLayout.StackContents) - sizeof(S206_IRET_SLOT)]);
}

static_assert(sizeof(VIRTUAL_PROCESSOR_DATA) == KERNEL_STACK_SIZE + PAGE_SIZE * 3,
              "VIRTUAL_PROCESSOR_DATA Size Mismatch");

/*
 * 9.368 (A2): THE INTERCEPT INVENTORY IS DERIVED FROM THE MASK.
 *
 * 9.366 listed nine intercept names by hand, right here, in a comment that said
 * "this is what the mask arms". The two facts were in two statements separated
 * by 70 lines: adding an intercept to the mask left the list unchanged, and the
 * inventory then under-reported what an attacker could reach while still looking
 * complete. An inventory that can silently under-report is worse than none,
 * because it reads as "we checked".
 *
 * So the mask is scanned. Each bit that is SET and that this handler actually
 * HAS A CASE FOR becomes an entry. Two properties fall out of that:
 *
 *   - a bit armed without a case cannot appear (it is not a surface, it is a
 *     bug that would bugcheck on the first exit), and
 *   - a case without its bit cannot appear either (dead code, 9.367.3's shape),
 *
 * because the table below lists BOTH facts side by side and the code asks the
 * mask about one while the reader can see the other.
 *
 * This lives in the file that owns the VMCB, on purpose: yghv_sentry.c stores
 * names and must not learn what a control area is.
 */
typedef struct {
    UINT32 bit;         /* position inside the field below */
    const char *name;   /* what sys-sentry prints */
    UINT8  field;       /* 0=Misc1 1=Misc2 2=Misc3 3=Exception 4=CrWrite */
} S206_INTERCEPT_ENTRY;

/*
 * Every intercept this hypervisor can name, with the field and bit that arms it
 * and the exit code its case handles. The two comments on each row are the point:
 * if `bit` is not set, no entry appears; if the case is missing, the row should
 * not be here at all.
 */
static const S206_INTERCEPT_ENTRY g_S206Intercepts[] = {
    { 18, "cpuid",        0 },   /* Misc1 bit18 CPUID        -> case VMEXIT_CPUID    */
    { 28, "msr",          0 },   /* Misc1 bit28 MSR_PROT     -> case VMEXIT_MSR      */
    { 31, "shutdown",     0 },   /* Misc1 bit31 SHUTDOWN     -> case VMEXIT_SHUTDOWN */
    {  0, "vmrun",        1 },   /* Misc2 bit0  VMRUN        -> case VMEXIT_VMRUN    */
    {  1, "exception-db", 3 },   /* Exception bit1 #DB       -> case VMEXIT_EXCEPTION_DB */
    {  3, "exception-bp", 3 },   /* Exception bit3 #BP       -> case VMEXIT_EXCEPTION_BP
                                  *    9.367.3: NOT SET TODAY. This row exists so the
                                  *    gap is visible: the case is present, the bit is
                                  *    not, so no entry is emitted — which is the honest
                                  *    description of an INT3 path with no hot evidence. */
    {  3, "cr3",          4 },   /* CrWrite bit3 CR3         -> case VMEXIT_CR3_WRITE */
    {  4, "cr4",          4 },   /* CrWrite bit4 CR4         -> case VMEXIT_CR4_WRITE */
    {  0, "npf",          5 }    /* NpEnable bit0 NPT        -> case VMEXIT_NPF
                                  *    NPT is not a "bit 0 of Misc1" fact; it is its own
                                  *    field, which is why it gets field 5 rather than a
                                  *    fake bit position. */
};
#define S206_INTERCEPT_ROWS \
    ((UINT32)(sizeof(g_S206Intercepts) / sizeof(g_S206Intercepts[0])))

_IRQL_requires_same_
static
VOID
SvArmSurfaceFromMask (
    _In_ PVIRTUAL_PROCESSOR_DATA VpData
    )
{
    UINT32 i;
    UINT32 misc1 = VpData->GuestVmcb.ControlArea.InterceptMisc1;
    UINT32 misc2 = VpData->GuestVmcb.ControlArea.InterceptMisc2;
    UINT32 exc   = VpData->GuestVmcb.ControlArea.InterceptException;
    UINT16 crw   = VpData->GuestVmcb.ControlArea.InterceptCrWrite;
    int    np    = (VpData->GuestVmcb.ControlArea.NpEnable & SVM_NP_ENABLE_NP_ENABLE) ? 1 : 0;

    for (i = 0; i < S206_INTERCEPT_ROWS; i++) {
        int armed = 0;
        switch (g_S206Intercepts[i].field) {
        case 0: armed = (misc1 >> g_S206Intercepts[i].bit) & 1; break;
        case 1: armed = (misc2 >> g_S206Intercepts[i].bit) & 1; break;
        case 3: armed = (exc   >> g_S206Intercepts[i].bit) & 1; break;
        case 4: armed = (crw   >> g_S206Intercepts[i].bit) & 1; break;
        case 5: armed = np; break;
        default: armed = 0; break;
        }
        if (armed)
            (void)yghv_sentry_surface_add(g_S206Intercepts[i].name);
    }

    /* 9.368 (A2): the FILLER marks it, not the storage module — otherwise a
     * second core would re-register every name and inflate the hit counts. */
    yghv_sentry_surface_mark_armed();
}


typedef struct _GUEST_REGISTERS
{
    UINT64 R15;
    UINT64 R14;
    UINT64 R13;
    UINT64 R12;
    UINT64 R11;
    UINT64 R10;
    UINT64 R9;
    UINT64 R8;
    UINT64 Rdi;
    UINT64 Rsi;
    UINT64 Rbp;
    UINT64 Rsp;
    UINT64 Rbx;
    UINT64 Rdx;
    UINT64 Rcx;
    UINT64 Rax;
} GUEST_REGISTERS, *PGUEST_REGISTERS;

typedef struct _GUEST_CONTEXT
{
    PGUEST_REGISTERS VpRegs;
    BOOLEAN ExitVm;
} GUEST_CONTEXT, *PGUEST_CONTEXT;


//
// x86-64 defined constants.
//
#define IA32_MSR_PAT    0x00000277
#define IA32_MSR_EFER   0xc0000080

/*
 * 9.476: 64-BIT, AND THE WIDTH IS THE POINT - NOT THE VALUE.
 *
 * This was `(1UL << 12)`. On x64 Windows `unsigned long` is 32 bits, so `~EFER_SVME`
 * is `0x00000000FFFFEFFF`, and `__readmsr(x) & ~EFER_SVME` therefore clears every
 * bit ABOVE 32 in the EFER it is writing back.
 *
 * Measured: with a realistic EFER (`0x0D01`) the 32-bit and 64-bit masks give the
 * same answer, which is why this has never shown up as a defect. But `__readmsr`
 * returns a full 64-bit register, and any bit above 32 that AMD defines (or any
 * future one) would be silently discarded by a 32-bit complement. A mask that
 * happens to be harmless today because the top half is zero is still the wrong
 * mask - the failure it causes would be a reserved-bit fault on EFER, i.e. #GP
 * inside our own unload path, and it would look like something else entirely.
 *
 * The tree already had the correct form in common/svm_defs.h (`1ULL << 12`), so
 * this was also a duplicate definition that disagreed in TYPE while agreeing in
 * VALUE. Two spellings of one constant is how a value and a type drift apart.
 */
#define EFER_SVME       (1ULL << 12)

#define RPL_MASK        3
#define DPL_SYSTEM      0

#define CPUID_FN8000_0001_ECX_SVM                   (1UL << 2)
#define CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT    (1UL << 31)
#define CPUID_FN8000_000A_EDX_NP                    (1UL << 0)

#define CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING          0x00000000
#define CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS       0x00000001
#define CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX    0x80000001
#define CPUID_SVM_FEATURES                                      0x8000000a
//
// The Microsoft Hypervisor interface defined constants.
//
#define CPUID_HV_VENDOR_AND_MAX_FUNCTIONS   0x40000000
#define CPUID_HV_INTERFACE                  0x40000001

//
// SimpleSVM specific constants.
//
#define CPUID_UNLOAD_SIMPLE_SVM     0x41414141
// 9.288 (C6 v5): returns EDX:EAX = the per-core VPD physical address WITHOUT
// de-virtualizing (kernel-mode only; used by the fake-write NCr3 resync).
#define CPUID_FAKE_VPD              0x41414142
#define CPUID_HV_MAX                CPUID_HV_INTERFACE

/*!
    @brief      Tier 5.4 (9.368/A4): what the guest is ALLOWED to see about us.

    @details    Every CPUID answer below is a claim about the hardware the guest
                thinks it is running on. Until this existed the hypervisor bit
                was written unconditionally:

                    registers[2] |= CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT;

                which is an honest answer ("a hypervisor is present") and the
                single easiest thing for a detector to check. The bit cannot be
                hidden by the guest itself — CPUID leaves 0x40000000+ are how it
                is discovered — so the decision has to live here, on our side.

                5.4 calls this "hardware view faking"; the whole capability is a
                POLICY about what to admit, and the policy has to be reachable
                from the control plane rather than recompiled. Three levels:

                  HONEST (default)  hypervisor bit set, vendor string says
                                    "SimpleSvm". This is what the code always
                                    did, kept as the default so nothing about
                                    existing tests changes silently.
                  NO_VENDOR_BIT     hypervisor bit CLEARED, but leaves
                                    0x40000000+ still answer a vendor string.
                                    A guest reading 0x40000000 sees us anyway —
                                    this level is for measuring which detectors
                                    look at bit 31 versus which probe the leaves.
                  OFF               hypervisor bit cleared AND leaves
                                    0x40000000+ answer zero (leaf 0 as if
                                    unsupported). The guest is told there is no
                                    hypervisor at all.

                HONEST LIMIT, stated where it is load-bearing: OFF is a lie the
                guest can still catch — TSC latency and the leaf-0 max-standard
                answer are not adjusted here (that is Tier 5.1 / 6.5 territory),
                so a timing or a max-leaf comparison still sees something. This
                level changes what CPUID *says*, not what the CPU *does*.

                Island-safe: three volatile LONGs, read once per CPUID exit.
*/
typedef enum {
    YGHV_CPUID_VIEW_HONEST = 0,
    YGHV_CPUID_VIEW_NO_VENDOR_BIT = 1,
    YGHV_CPUID_VIEW_OFF = 2,
    YGHV_CPUID_VIEW_MAX = 3
} YGHV_CPUID_VIEW;

static volatile LONG g_CpuidView = YGHV_CPUID_VIEW_HONEST;
static volatile LONG64 g_CpuidViewSets = 0;      /* how many times it was changed */
static volatile LONG64 g_CpuidViewSuppressed = 0; /* answers actually altered */

/*!
    @brief      Set the view. PASSIVE (control plane only, never the island).
    @result     1 on success, 0 for an out-of-range level (nothing is changed).
*/
extern "C" int yghv_s206_cpuid_view_set(UINT32 level)
{
    if (level >= YGHV_CPUID_VIEW_MAX)
        return 0;
    InterlockedExchange(&g_CpuidView, (LONG)level);
    InterlockedIncrement64(&g_CpuidViewSets);
    return 1;
}

extern "C" UINT32 yghv_s206_cpuid_view_get(VOID)
{
    return (UINT32)InterlockedCompareExchange(&g_CpuidView, 0, 0);
}

/*! Read-only counters, so a test can prove the policy actually fired rather than
    assuming it did. out[0]=level out[1]=sets out[2]=suppressed */
extern "C" void yghv_s206_cpuid_view_stats(UINT64 *out)
{
    if (!out)
        return;
    out[0] = (UINT64)yghv_s206_cpuid_view_get();
    out[1] = (UINT64)InterlockedCompareExchange64(&g_CpuidViewSets, 0, 0);
    out[2] = (UINT64)InterlockedCompareExchange64(&g_CpuidViewSuppressed, 0, 0);
}

/*!
    @brief      Tier 5.1 (9.368/B1): TIME as seen by the guest.

    @details    TSC is the one clock a guest can always read, and the only one
                whose value it cannot verify against anything outside itself. AMD
                gives us two tools, and they are NOT equivalent:

                  TscOffset (VMCB +0x050)   added by hardware to every RDTSC /
                                            RDTSCP the guest executes. Costs
                                            nothing per read — no #VMEXIT at all.
                                            It SHIFTS time; it cannot make it run
                                            faster or slower.
                  RDTSC / RDTSCP intercept  every read becomes a #VMEXIT, so the
                                            handler may return anything, including
                                            a frozen or reversed value. Costs an
                                            exit per read.

                B1 wires the first, B2 the second. Keeping them as separate
                levels is deliberate: a test that only proved "time is wrong" could
                be passed by either mechanism, and the two have completely
                different costs and failure modes.

                WHAT OFFSET CANNOT DO, stated here because it is the thing most
                easily over-claimed: with a constant offset the guest's measured
                INTERVALS stay exactly right. A detector that times an operation
                (rdtsc, work, rdtsc, subtract) sees the true duration plus a
                constant that cancels out. Offsetting hides WHEN, not HOW LONG.

                Island-safe: one volatile LONG64 read per RDTSC exit, and the
                VMRUN path just copies it into the VMCB.
*/
static volatile LONG64 g_TscOffset = 0;              /* what the guest is shifted by */
/*
 * 9.369 (B2 RE-DO): FREEZE IS SCOPED, NOT GLOBAL, AND IT EXPIRES.
 *
 * MEASURED WHY THIS SHAPE AND NOT THE OBVIOUS ONE. The first implementation
 * froze every RDTSC the hypervisor intercepted. But the guest here IS Windows:
 * KeQueryPerformanceCounter, KeQuerySystemTimePrecise, DPC scheduling and timer
 * expiry all read TSC continuously. Stopping time stopped the scheduler, and the
 * machine hung with no bugcheck at all - no dump, no event, just no progress.
 * Recovering it took a manual reboot.
 *
 * So a freeze is now (a) only for reads made by a CR3 we were asked to watch,
 * and (b) armed with a wall-clock deadline in REAL ticks, after which it thaws
 * itself. Both are load-bearing:
 *
 *   (a) the OS's own reads pass through, so the scheduler keeps running;
 *   (b) a forgetful operator cannot leave a machine that needs a power cycle.
 *
 * `g_TscFreezeExpiresAt` is compared against the REAL TSC, not the frozen one -
 * a deadline measured on the clock being frozen never advances.
 */
static volatile LONG64 g_TscMode = 0;                /* 0 = normal, 1 = frozen */
static volatile LONG64 g_TscFrozenAt = 0;            /* the value frozen mode holds */
static volatile LONG64 g_TscFreezeExpiresAt = 0;     /* REAL tsc at which it thaws */
static volatile LONG64 g_TscScopeCr3 = 0;            /* only this CR3 is frozen; 0 = none */
static volatile LONG64 g_TscExpired = 0;             /* how many times the deadline fired */
static volatile LONG64 g_TscScopedOut = 0;           /* reads let through because CR3 != scope */
/*
 * 9.369b: EXIT-BUDGET, because the intercept can starve the core that owns it.
 *
 * 0x101 CLOCK_WATCHDOG_TIMEOUT is the OS noticing a core has not serviced its
 * clock interrupt in time. RDTSC is hot enough that turning it into an exit can
 * do exactly that: measured before the crash, 369526 RDTSC exits accumulated in
 * a couple of seconds on a 12-thread machine.
 *
 * A deadline bounds how LONG a freeze lasts; this bounds how MANY reads it may
 * charge for. When the budget is spent the freeze ends, so a pathological guest
 * (a tight rdtsc loop is a normal thing for a detector to write) cannot keep the
 * core in our handler. The limit is deliberately small: the feature exists to
 * blind a detector that samples time, and a detector sampling millions of times
 * per second has already found us by another route.
 */
#define YGHV_TSC_EXIT_BUDGET 20000
static volatile LONG64 g_TscBudgetLeft = YGHV_TSC_EXIT_BUDGET;
static volatile LONG64 g_TscBudgetHits = 0;          /* freezes cut short by the budget */
static volatile LONG64 g_TscSelfDisarmed = 0;        /* RDTSC handlers that disarmed their own core */
static volatile LONG64 g_TscApplyFailures = 0;       /* per-core walks that did not complete */
static volatile LONG64 g_TscCoreHits = 0;            /* cores where a walk reached a VPD */
static volatile LONG64 g_TscCoreMisses = 0;          /* cores where the handshake answered no */
static volatile LONG64 g_TscIntercepts = 0;          /* RDTSC/RDTSCP exits handled */
static volatile LONG64 g_TscSuppressed = 0;          /* answers changed from hardware */
static volatile LONG   g_TscInterceptArmed = 0;      /* is the intercept bit set? */

/*
 * 9.372 (C1): is the #BP exception intercept armed?
 *
 * Default 0. See the note at the arming site: an exception intercept that was
 * never measured is exactly how the 205k #GP block produced a 0xE2, and this
 * switch keeps the default boot path identical to what the 13/13 suite verified.
 */
static volatile LONG   g_BpInt3Intercept = 0;

/*
 * 9.396: A STOP FLAG THE VMRUN LOOP CAN ACTUALLY SEE.
 *
 * THE DEFECT THIS ADDRESSES (9.395, confirmed against tandasat/SimpleSvm)
 *
 * This tree virtualises in TWO places on the same cores:
 *
 *   svm_core_init()   -> resident threads            (wait_all_stopped waits for these)
 *   Sv206Entry()      -> SvVirtualizeProcessor -> Sv206LaunchVm  (a VMRUN loop that
 *                        never returns until ExitVm)
 *
 * DriverUnload stops the FIRST set (svm_core_stop_all_residents +
 * svm_core_wait_all_stopped) and then frees npt_cleanup() / svm_core_cleanup()
 * state that the SECOND set is still running on. The crash point
 * Sv206LaunchVm+0xD is `mov rax,[rsp]` reading VpData->HostStackLayout - the loop
 * is alive and its VpData is gone.
 *
 * WHY THE EXISTING MECHANISMS DO NOT WORK
 *
 *   SvDevirtualizeAllProcessors: its __cpuidex runs in the CONTROL thread. CPUID's
 *     back door only makes the CALLER leave guest mode, and the caller is not in
 *     guest mode - the VMRUN loop is in a different execution context entirely.
 *     That is why writing the 'SSVM' ack (9.378, 9.383) changed nothing.
 *
 *   YGHV_CMD_STOP_INTERNAL: a VMMCALL, and the 206 guest does not issue VMMCALL.
 *
 * So the only lever that reaches the loop is a flag read ON EVERY VMEXIT, inside
 * the exit handler the loop itself calls. That is this flag.
 *
 * The exit handler already decides "return to the host" through guestContext.ExitVm.
 * Setting that from here means the loop takes the SAME path a CPUID_UNLOAD takes:
 * __svm_vmload, __svm_stgi, clear EFER.SVME, return. One exit path, not two.
 */
/*
 * 9.416: NOT static any more - the ASM loop reads it directly.
 *
 * 9.414 called a C helper from SvLV10 and the machine died with 0x139 / 0x7F
 * double fault. 9.415 worked out why: a C function's prolog (stack probe, SEH
 * frame) expects a normal kernel context, and a VM-exit frame has no valid SEH
 * chain. So the check cannot be a call - it has to be inline instructions against
 * memory whose address the assembler knows at link time.
 *
 * That is the only reason these two lost their `static`: an external symbol is
 * addressable from the .asm. Nothing else about them changed, and the semantics
 * (request stays set until a fresh load) are unchanged from 9.412.
 */
/* 9.435: how many times the guest's vmmcall was actually trapped. Zero would
 * mean the intercept bit is still not taking effect - a different fix from
 * "the bit works and the guest left". */
static volatile LONG64 g_S206Vmmcalls = 0;

/*
 * 9.448: A PER-EXIT-CODE COUNTER TABLE, so "35 exit codes implemented" stops
 * being a claim about source text and becomes a reading.
 *
 * THE WINDOW IS 0x40..0x8D, NOT 0x60..0x8D, AND THAT IS A CORRECTION.
 *
 * The first version counted only 0x60..0x8D, which is the range the Misc1/Misc2
 * intercept bits address. Measured on the first run: `total exits = 803,776,070`
 * but `distinct observed = 0`. The counter was not broken - the window was. The
 * exits actually happening on a live Windows guest are CPUID (0x72, in range) and
 * #DB/#BP (0x41/0x43, EXCEPTION_BASE 0x40 + vector), plus NPF (0x400, its own
 * ring). The exception codes start at 0x40 and fell outside the window entirely,
 * so the one measurement that mattered read zero while 800 million exits
 * demonstrably happened.
 *
 * That is the same mistake 9.440 recorded in another form: a counter that cannot
 * observe the event it claims to count reads as "nothing happened".
 */
#define S206_EXITCODE_LO 0x40
#define S206_EXITCODE_HI 0x8d
static volatile LONG64 g_S206ExitByCode[S206_EXITCODE_HI - S206_EXITCODE_LO + 1] = { 0 };

static __forceinline void S206ExitCount(UINT64 code)
{
    if (code >= S206_EXITCODE_LO && code <= S206_EXITCODE_HI)
    {
        InterlockedIncrement64(&g_S206ExitByCode[code - S206_EXITCODE_LO]);
    }
}

extern "C" LONG64 yghv_s206_exit_count_of(UINT64 code)
{
    if (code < S206_EXITCODE_LO || code > S206_EXITCODE_HI)
        return -1;
    return InterlockedCompareExchange64(
        &g_S206ExitByCode[code - S206_EXITCODE_LO], 0, 0);
}

/*
 * 9.448: THE CALLER-SELECTED EXTRA INTERCEPT BITS.
 *
 * These are OR'd into every VMCB at prepare time. Empty by default, which is what
 * keeps the build's behaviour identical to the pre-9.448 set - an armed HLT turns
 * every idle transition into an exit, and on a 12-core guest that is a real cost,
 * not a theoretical one.
 *
 * The "quiet" preset arms exactly HLT and PAUSE: the two instructions whose exits
 * prove that idle-time interception works at all. The "full" preset arms everything
 * this tree defines in Misc1/Misc2, which is what makes the 35-code claim
 * measurable in one run instead of by argument.
 */
static volatile LONG g_S206ExtraMisc1 = 0;
static volatile LONG g_S206ExtraMisc2 = 0;

/*
 * The set of bits this IOCTL is allowed to own. It is NOT "all bits": the
 * default-armed bits (CPUID bit18, MSR_PROT bit28, SHUTDOWN bit31, VMRUN Misc2
 * bit0, VMMCALL Misc2 bit1) must stay armed no matter what a caller asks for,
 * because the guest stub needs VMMCALL to ever return and SHUTDOWN is how a
 * triple fault becomes observable.
 *
 * Declared before use by yghv_s206_apply_extra_intercepts below.
 */
#define S206_MISC1_EXTRA_MASK \
    (SVM_INTERCEPT_MISC1_INTR | SVM_INTERCEPT_MISC1_NMI | SVM_INTERCEPT_MISC1_SMI | \
     SVM_INTERCEPT_MISC1_INIT | SVM_INTERCEPT_MISC1_VINTR | SVM_INTERCEPT_MISC1_CR0_SEL | \
     SVM_INTERCEPT_MISC1_IDTR_READ | SVM_INTERCEPT_MISC1_GDTR_READ | \
     SVM_INTERCEPT_MISC1_LDTR_READ | SVM_INTERCEPT_MISC1_TR_READ | \
     SVM_INTERCEPT_MISC1_IDTR_WRITE | SVM_INTERCEPT_MISC1_GDTR_WRITE | \
     SVM_INTERCEPT_MISC1_LDTR_WRITE | SVM_INTERCEPT_MISC1_TR_WRITE | \
     SVM_INTERCEPT_MISC1_RDPMC | SVM_INTERCEPT_MISC1_PUSHF | SVM_INTERCEPT_MISC1_POPF | \
     SVM_INTERCEPT_MISC1_RSM | SVM_INTERCEPT_MISC1_IRET | SVM_INTERCEPT_MISC1_SWINT | \
     SVM_INTERCEPT_MISC1_INVD | SVM_INTERCEPT_MISC1_PAUSE | SVM_INTERCEPT_MISC1_HLT | \
     SVM_INTERCEPT_MISC1_INVLPG | SVM_INTERCEPT_MISC1_INVLPGA | SVM_INTERCEPT_MISC1_IOIO | \
     SVM_INTERCEPT_MISC1_TASKSR | SVM_INTERCEPT_MISC1_FERR)
#define S206_MISC2_EXTRA_MASK \
    (SVM_INTERCEPT_MISC2_VMLOAD | SVM_INTERCEPT_MISC2_VMSAVE | SVM_INTERCEPT_MISC2_STGI | \
     SVM_INTERCEPT_MISC2_CLGI | SVM_INTERCEPT_MISC2_SKINIT | SVM_INTERCEPT_MISC2_ICEBP | \
     SVM_INTERCEPT_MISC2_WBINVD | SVM_INTERCEPT_MISC2_MONITOR | SVM_INTERCEPT_MISC2_MWAIT | \
     SVM_INTERCEPT_MISC2_MWAIT_COND | SVM_INTERCEPT_MISC2_XSETBV)

/*
 * 9.448b: THE REGISTER OF LIVE VPDs, SO A MASK CHANGE CAN REACH THE RUNNING VMCBs.
 *
 * 9.448 shipped "takes effect on the next load" and MEASURED it: set quiet,
 * reload, read back -> 0x00000000. The mask was a static, the static is reset by
 * the unload, and prepare runs before any of it can be re-armed. So "next load"
 * was not slow, it was unreachable, and the command's own output was the only
 * thing that said otherwise.
 *
 * The fix is not to persist the static - it is to STOP WAITING. Each core's VPD
 * is already registered here by its own thread (it is the only place that knows
 * the mapping, since the thread owns the frame), so a mask change can walk the
 * list and rewrite the live VMCB control area directly.
 *
 * IMPLEMENTATION NOTE: this crosses cores, and VMCB control area is ordinary
 * memory that only the owning core writes during an exit. The intercept word is
 * 4-byte aligned, so a 4-byte store is atomic; the guest cannot be inside an
 * exit that already read the old value at the moment we store the new one, and an
 * exit either sees the old mask or the new one - never a torn mix. That is the
 * whole safety argument, and it is why only the two intercept words are touched
 * here and nothing else in the VMCB.
 */
#define SVM206_MAX_VPD 64
static PVIRTUAL_PROCESSOR_DATA g_S206LiveVpd[SVM206_MAX_VPD] = { 0 };

extern "C" VOID yghv_s206_register_vpd(PVOID vpData)
{
    for (UINT32 i = 0; i < SVM206_MAX_VPD; i++)
    {
        if (InterlockedCompareExchangePointer(
                (PVOID volatile *)&g_S206LiveVpd[i], vpData, NULL) == NULL)
        {
            return;
        }
        if (g_S206LiveVpd[i] == (PVIRTUAL_PROCESSOR_DATA)vpData)
            return;
    }
}

extern "C" VOID yghv_s206_unregister_vpd(PVOID vpData)
{
    for (UINT32 i = 0; i < SVM206_MAX_VPD; i++)
    {
        if (g_S206LiveVpd[i] == (PVIRTUAL_PROCESSOR_DATA)vpData)
            InterlockedExchangePointer((PVOID volatile *)&g_S206LiveVpd[i], NULL);
    }
}

/*
 * Apply the CURRENT extra-mask to every live VMCB right now.
 *
 * Returns how many VMCBs were touched. Zero is a meaningful answer: it means no
 * core is virtualized yet, so the mask is only in the statics and will be folded
 * in by the next prepare - which is the pre-9.448b behaviour, now distinguishable
 * from success.
 */
/*
 * 9.462: WAKE EVERY PARKED CORE, FROM THE HOST SIDE.
 *
 * The stub now parks in `hlt` instead of spinning on `vmmcall`, which removes the
 * ~56 million VMEXITs per second that kept the machine at 100% CPU. The cost of
 * that change is that a parked core performs NO exits, so the old stop checkpoint
 * (`Sv206VmrunShouldExit` inside `case VMEXIT_VMMCALL`) would never run - the
 * exact failure 9.432 documented when it said removing vmmcall stops everything
 * downstream.
 *
 * So the stop is INITIATED from here instead: write an event into each parked
 * core's VMCB EventInj. A parked core takes it at its next instruction boundary
 * (hlt is not interrupt-shadowable), the resulting VMEXIT runs the SAME handler,
 * and that handler runs the SAME `Sv206VmrunShouldExit` check. One stop path,
 * one exit protocol - only the initiator moved.
 *
 * WHICH EVENT, AND WHY NMI
 * Vector 2 (NMI). Chosen over a maskable vector because the guest's IF is not
 * something this function can assert about a core it does not own: IF is forced
 * to 1 at prepare (see the Rflags line), but an injected maskable vector would be
 * held pending if that ever became false, and the core would never wake. NMI
 * cannot be masked, so delivery does not depend on a flag we do not control.
 *
 * TYPE 4 = NMI in the VMCB EventInj encoding (bits 7:0 vector, 10:8 type, 11
 * error-code-valid). Vector 2 with type 4 and no error code is 0x402.
 *
 * VmcbClean = 0 after each write: the processor caches the control area, and an
 * injected event written without dirtying the VMCB would simply not be seen -
 * the same lesson 9.448e paid two reboots for.
 *
 * Returns the number of VMCBs written. Zero means no core is virtualized, which
 * is a normal answer for a stop that arrives before any core started.
 */
extern "C" LONG yghv_s206_kick_parked_cores(VOID)
{
    LONG kicked = 0;

    for (UINT32 i = 0; i < SVM206_MAX_VPD; i++)
    {
        PVIRTUAL_PROCESSOR_DATA vp = g_S206LiveVpd[i];

        if (vp == NULL)
            continue;

        /*
         * 9.462: OVERWRITE, DO NOT ASK "IS THE SLOT FREE".
         *
         * The first version skipped a core whose EventInj was non-zero, reasoning
         * that a pending event would be taken anyway. Measured on the unload path:
         * invmrun stayed at 0xc - all twelve cores still parked - and the machine
         * bugchecked 0xD1 with PAGED_IP once the image was unmapped. A slot holding
         * a stale or already-consumed event is exactly the case where "something
         * is pending" and "the core will wake" are NOT the same statement.
         * Overwriting with our own NMI makes the wake deterministic.
         */
        vp->GuestVmcb.ControlArea.EventInj = 0x402ULL;   /* NMI, vector 2 */
        vp->GuestVmcb.ControlArea.VmcbClean = 0;
        kicked += 1;
    }
    return kicked;
}

extern "C" LONG yghv_s206_apply_extra_intercepts(VOID)
{
    LONG applied = 0;
    UINT32 m1 = (UINT32)InterlockedCompareExchange(&g_S206ExtraMisc1, 0, 0);
    UINT32 m2 = (UINT32)InterlockedCompareExchange(&g_S206ExtraMisc2, 0, 0);

    for (UINT32 i = 0; i < SVM206_MAX_VPD; i++)
    {
        PVIRTUAL_PROCESSOR_DATA vp = g_S206LiveVpd[i];

        if (vp == NULL)
            continue;
        /*
         * SET, NOT OR. The caller's mask is the whole truth for the extra bits:
         * clear must be able to take a bit back out, and OR-ing only ever adds.
         * The default bits (CPUID, MSR_PROT, SHUTDOWN, VMRUN, VMMCALL) are
         * separate and are not in these two words, so replacing them cannot
         * disarm anything the build needs.
         */
        vp->GuestVmcb.ControlArea.InterceptMisc1 =
            (UINT32)((vp->GuestVmcb.ControlArea.InterceptMisc1 & ~S206_MISC1_EXTRA_MASK) | m1);
        vp->GuestVmcb.ControlArea.InterceptMisc2 =
            (UINT32)((vp->GuestVmcb.ControlArea.InterceptMisc2 & ~S206_MISC2_EXTRA_MASK) | m2);

        /*
         * 9.448e: AND INVALIDATE THE CACHED VMCB - the step that explains TWO
         * reboots.
         *
         * VmcbClean is how the processor knows whether the control area it holds
         * internally is still valid. Write an intercept word without clearing it
         * and the CPU keeps USING THE OLD MASK: the new bits never take effect
         * until something else happens to dirty the VMCB. The tree already knows
         * this - every other site that rewrites an intercept sets VmcbClean = 0
         * for exactly this reason (9.285, 9.297, 9.369h).
         *
         * Measured shape of getting it wrong: `quiet` (HLT+PAUSE) worked, because
         * those bits are rarely exercised so a stale mask changes nothing visible.
         * `full` rebooted twice within seconds, because it arms bits that fire
         * constantly - and with a half-applied mask the handler ran against a
         * VMCB whose armed set and whose clean flag disagreed.
         *
         * A single byte store, so no cross-core atomicity issue of its own; the
         * pair (intercept words, then this) is ordered by the fact that only the
         * owning core reads them, and it reads them at its next VMRUN.
         */
        vp->GuestVmcb.ControlArea.VmcbClean = 0;

        applied += 1;
    }
    return applied;
}

extern "C" VOID yghv_s206_set_extra_intercepts(UINT32 m1, UINT32 m2)
{
    InterlockedExchange(&g_S206ExtraMisc1, (LONG)m1);
    InterlockedExchange(&g_S206ExtraMisc2, (LONG)m2);

    /*
     * 9.448b: APPLY NOW, not "on the next load".
     *
     * Measured: the first version stored only, and the reload that was supposed
     * to pick it up read back 0x00000000 because the unload reset the static.
     * A setting that cannot take effect is worse than no setting, because the
     * command still prints a success-looking mask.
     *
     * Applying returns the count of live VMCBs touched; the IOCTL reports it so
     * "it did not reach anything" is visible instead of implied.
     */
    (void)yghv_s206_apply_extra_intercepts();
}

/*
 * Read back the mask AS IT IS ARMED IN THE LIVE VMCBs, falling back to the
 * statics when no core is virtualized. A caller comparing this against what it
 * set is then comparing against reality, not against its own request echoed
 * back - which is the failure mode this whole step exists to remove.
 */
extern "C" VOID yghv_s206_get_extra_intercepts(UINT32 *m1, UINT32 *m2)
{
    UINT32 rm1 = 0, rm2 = 0;

    for (UINT32 i = 0; i < SVM206_MAX_VPD; i++)
    {
        PVIRTUAL_PROCESSOR_DATA vp = g_S206LiveVpd[i];

        if (vp == NULL)
            continue;
        rm1 = (UINT32)(vp->GuestVmcb.ControlArea.InterceptMisc1 & S206_MISC1_EXTRA_MASK);
        rm2 = (UINT32)(vp->GuestVmcb.ControlArea.InterceptMisc2 & S206_MISC2_EXTRA_MASK);
        break;
    }

    if (rm1 == 0 && rm2 == 0)
    {
        rm1 = (UINT32)InterlockedCompareExchange(&g_S206ExtraMisc1, 0, 0);
        rm2 = (UINT32)InterlockedCompareExchange(&g_S206ExtraMisc2, 0, 0);
    }

    if (m1) *m1 = rm1;
    if (m2) *m2 = rm2;
}

/* The two presets, named so the IOCTL does not hand-assemble bits. */
#define S206_MISC1_QUIET (SVM_INTERCEPT_MISC1_HLT | SVM_INTERCEPT_MISC1_PAUSE)
#define S206_MISC2_QUIET (0u)
/*
 * 9.448d: `full` DELIBERATELY EXCLUDES THE EVENT-DELIVERY BITS.
 *
 * The first version armed all of them and the machine rebooted: the `quiet`
 * preset succeeded at 04:12:32, `full` followed and the system restarted at
 * 04:14:44 with no dump ("此次重启没有生成转储").
 *
 * The cause is not a ring race (that was 9.448c and it is fixed). It is the
 * SIX bits that do not describe an instruction boundary at all:
 *
 *     INTR  0x60   a physical interrupt arrived
 *     NMI   0x61   a non-maskable interrupt arrived
 *     SMI   0x62   the CPU entered system management mode
 *     INIT  0x63   an INIT was signalled
 *     VINTR 0x64   the virtual interrupt fired
 *     RSM   0x73   the CPU resumed from SMM
 *
 * Arming any of these means every hardware event becomes an exit and must be
 * RE-DELIVERED correctly, by vector, by type, with the right error code, at the
 * right RIP, without double-delivery. My handler writes ExitIntInfo into EventInj
 * when the slot is free - which is a plausible-looking pass-through that I
 * cannot prove correct. On a machine where these arrive thousands of times a
 * second, "plausible" is not good enough, and 9.447's own rule is that an
 * unverified mechanism must not ship as if it were verified.
 *
 * So `full` covers the instruction-boundary bits (which have a defined handler)
 * and the event bits move to a SEPARATE preset that has to be asked for by name.
 * A caller that wants them is asking for an experiment, and the command should
 * say so rather than fold them into "full".
 */
/*
 * 9.448f: `full` IS NOW THE SET THAT HAS EARNED ITS PLACE.
 *
 * Three reboots, three different defects, each hidden behind the previous one:
 *
 *   04:14  the ring race                -> fixed (9.448c)
 *   04:25  VmcbClean not cleared        -> fixed (9.448e)
 *   04:32  side-effecting instructions  -> this step
 *
 * The third is the one that matters most, because it was not a bug in my code -
 * it was a bug in my idea of what an intercept means. INVLPG/INVLPGA/INVD/WBINVD
 * are not observations, they are REQUESTS: the guest asks the CPU to invalidate
 * something so its future accesses are correct. Counting the exit and moving on
 * grants the count and denies the request.
 *
 * `full` now contains only bits for which either a correct handler exists or the
 * instruction is harmless to skip:
 *
 *   HLT / PAUSE          true no-ops for a stub guest; proven by `quiet`
 *   INVD / WBINVD        flushed on the guest's behalf (TlbControl, see the case)
 *   INVLPG / INVLPGA     same
 *   RDPMC                a read the guest performs; nothing to undo
 *   MONITOR / MWAIT      hints, not commands; the stub never waits on them
 *   XSETBV / ICEBP       state the host does not depend on
 *
 * WHAT IS STILL NOT IN `full`, AND WHY (all reachable by name):
 *   INTR/NMI/SMI/INIT/VINTR/RSM   event re-delivery, unproven  -> `events`
 *   VMLOAD/VMSAVE/STGI/CLGI/SKINIT the HOST executes these     -> `host-insn`
 *   IDTR/GDTR/LDTR/TR read+write   VMSAVE moves these registers
 *   TASKSR / FERR_FREEZE           same, via the task register
 *   PUSHF/POPF/IRET/SWINT          intervening in control transfer is a real
 *                                  behaviour change, not an observation
 *
 * That last group is the honest boundary of this step: they are HANDLED (the cases
 * exist and are counted) but not ARMED, and "handled but not armed" is the exact
 * distinction 9.447 taught this tree to stop blurring.
 */
#define S206_MISC1_FULL \
    (SVM_INTERCEPT_MISC1_PAUSE | SVM_INTERCEPT_MISC1_HLT | \
     SVM_INTERCEPT_MISC1_INVD | \
     SVM_INTERCEPT_MISC1_INVLPG | SVM_INTERCEPT_MISC1_INVLPGA | \
     SVM_INTERCEPT_MISC1_RDPMC)

/* The experimental set: hardware events, re-delivered through EventInj.
 * Ask for this by name. Never reached by `full`. */
#define S206_MISC1_EVENTS \
    (SVM_INTERCEPT_MISC1_INTR | SVM_INTERCEPT_MISC1_NMI | SVM_INTERCEPT_MISC1_SMI | \
     SVM_INTERCEPT_MISC1_INIT | SVM_INTERCEPT_MISC1_VINTR | SVM_INTERCEPT_MISC1_RSM)
/*
 * 9.448d: MISC2'S `full` DROPS VMLOAD / VMSAVE / STGI / CLGI / SKINIT.
 *
 * These are not guest instructions here - THEY ARE OURS. SvPrepareForVirtualization
 * calls __svm_vmsave twice per core, and the exit path uses STGI/CLGI. Arming their
 * intercept bits makes the hypervisor trap ITSELF: the host-side VMSAVE inside
 * prepare raises a VMEXIT while the VMCB is being built, the handler runs against
 * a half-initialised VMCB, and the machine dies before any of it can be logged.
 *
 * That is the second cause of the 04:14 reboot (the first was the event bits, see
 * S206_MISC1_EVENTS). Both were invisible in source review because the case exists
 * and looks handled; what was wrong is that the instruction belongs to the host,
 * not the guest.
 *
 * The rule this step establishes: an intercept bit may only be armed for an
 * instruction the GUEST executes. VMLOAD/VMSAVE/STGI/CLGI/SKINIT are host
 * operations in this tree, so they stay off, and their cases remain in the handler
 * for the day a real guest runs them - the case is not proof, the counter is.
 */
#define S206_MISC2_FULL \
    (SVM_INTERCEPT_MISC2_ICEBP | \
     SVM_INTERCEPT_MISC2_WBINVD | SVM_INTERCEPT_MISC2_MONITOR | SVM_INTERCEPT_MISC2_MWAIT | \
     SVM_INTERCEPT_MISC2_MWAIT_COND | SVM_INTERCEPT_MISC2_XSETBV)

/* Host-owned instructions. Ask by name; never part of `full`. */
#define S206_MISC2_HOST_INSN \
    (SVM_INTERCEPT_MISC2_VMLOAD | SVM_INTERCEPT_MISC2_VMSAVE | SVM_INTERCEPT_MISC2_STGI | \
     SVM_INTERCEPT_MISC2_CLGI | SVM_INTERCEPT_MISC2_SKINIT)

extern "C" UINT32 yghv_s206_preset_quiet_m1(VOID) { return S206_MISC1_QUIET; }
extern "C" UINT32 yghv_s206_preset_quiet_m2(VOID) { return S206_MISC2_QUIET; }
extern "C" UINT32 yghv_s206_preset_full_m1(VOID) { return S206_MISC1_FULL; }
extern "C" UINT32 yghv_s206_preset_full_m2(VOID) { return S206_MISC2_FULL; }

/*
 * 9.448d: the two sets `full` deliberately excludes now have their own preset.
 *
 * `events` re-delivers hardware interrupts through EventInj - plausible, unproven,
 * and known to reboot the machine on the first attempt, so it must be asked for by
 * name rather than folded into a preset that sounds safe.
 * `host-insn` arms instructions THIS TREE ITSELF executes (VMSAVE inside prepare,
 * STGI/CLGI on the exit path); arming them traps the hypervisor and is a
 * self-inflicted crash, not a feature. Present so the exclusion is testable rather
 * than merely asserted.
 */
extern "C" UINT32 yghv_s206_preset_events_m1(VOID) { return S206_MISC1_EVENTS; }
extern "C" UINT32 yghv_s206_preset_events_m2(VOID) { return 0u; }
extern "C" UINT32 yghv_s206_preset_host_m1(VOID) { return 0u; }
extern "C" UINT32 yghv_s206_preset_host_m2(VOID) { return S206_MISC2_HOST_INSN; }

/*
 * 9.448: HOW MANY DISTINCT EXIT CODES HAVE ACTUALLY FIRED.
 *
 * This is the number that replaces the documentation claim. 35 codes are
 * HANDLED; how many have been OBSERVED is a different number, and it is the one
 * a reader should trust. Reported alongside the armed mask so "we cover 35"
 * always arrives with "and here is how many we saw".
 */
extern "C" LONG yghv_s206_exit_codes_observed(VOID)
{
    LONG n = 0;

    for (UINT32 i = 0; i <= (S206_EXITCODE_HI - S206_EXITCODE_LO); i++)
    {
        if (InterlockedCompareExchange64(&g_S206ExitByCode[i], 0, 0) != 0)
            n += 1;
    }
    return n;
}

/*
 * 9.441: AN UNCONDITIONAL VMEXIT COUNTER - THE ONE INSTRUMENT THIS TREE LACKED.
 *
 * 9.440 established that three of the four readings the 9.434 causal chain rests
 * on are not measurements at all: g_S206VrLog only records protected-page write
 * NPFs (it stays empty on a machine with nothing armed, no matter how many exits
 * happen), g_VmrunStopped has no caller left (9.419 deleted the ASM check and the
 * C counter stayed behind, so it is identically zero), and g_resident_thread_count
 * is never written in 206-B (residents are not started).
 *
 * So "is there a #VMEXIT at all" - the most basic question about a hypervisor -
 * had no instrument that could answer it. This is that instrument. It is
 * incremented as the FIRST statement of SvHandleVmExit, before any branch, so it
 * counts exits regardless of exit code, arming state, or which path the handler
 * takes afterwards.
 *
 * What each reading will mean:
 *   exits == 0                        -> the guest never traps. Look at the VMCB
 *                                        intercept bits actually reaching hardware,
 *                                        and at what the guest is executing.
 *   exits > 0 and inVmrun == 12       -> exits happen but cores never come back
 *                                        out of the VMRUN loop. That is a different
 *                                        bug, and it lives in the exit path.
 *   exits climbing at MHz rates       -> VMEXIT storm (self-trapping guest).
 */
extern "C" { volatile LONG64 g_S206Exits = 0; }

/*
 * 9.543: DOES ANYTHING REACH THE EARLY-RETURN PATH?
 *
 * SvVirtualizeProcessor already contains the reference implementation's pattern:
 * RtlCaptureContext, then "if not installed do the VMRUN", then return - so the
 * second pass through the function exits immediately. That second pass is what
 * makes the C frame survive; without it, iretq jumps out of a frame nobody
 * returns through, which is what 0x139 was.
 *
 * The question this counts answers is whether that path is REACHABLE in the mode
 * under test. Stub guests resume into SvVirtualizeProcessor on every VMRUN resume,
 * so g_S206EarlyReturns climbs. A real Windows guest never comes back through this
 * function at all, so it will stay flat - and that difference is the actual
 * difference between the two modes, not the iretq instruction.
 *
 * Read-only telemetry: two increments, no branch, no structure change.
 */
extern "C" { volatile LONG64 g_S206VirtualizeEntries = 0; }

/*
 * 9.544 STAGE A: THE EXPLICIT SECOND-PASS FLAG.
 *
 * The reference implementation decides "first pass or second pass" with a flag set
 * by the first pass itself:
 *     if (Vcpu->Virtualized) return;          // Type2-AMD-HV svm.c
 * Ours decides it by probing CPUID 40000000h for the hypervisor vendor string -
 * which answers a DIFFERENT question ("is a hypervisor present?") that happens to
 * agree only while the guest never returns here (stub mode: entries=6, early=0, so
 * the branch was never exercised in the agreeing case either).
 *
 * Stage A adds the flag but does NOT let it decide anything yet: the branch below
 * still requires the probe to say "not installed" AND the flag to say "first pass".
 * If the two ever disagree the OLD answer wins and g_S206FlagDisagrees records it,
 * so a silent divergence shows up as a counter instead of as a changed boot path.
 *
 * The flag is set at exactly the point the probe would first have returned TRUE -
 * right before VMRUN - which is where the reference implementation sets it too.
 */
/*
 * 9.545: PER-CORE, not global.
 *
 * 9.544 measured disagreements on 4 of 6 cores between this flag and the CPUID
 * probe, because one core setting a global flag made every later core look like a
 * second pass while its own CPUID still said "no hypervisor".
 *
 * The reference implementation keeps this per-VCPU (if (Vcpu->Virtualized) return);
 * this is the same idea without touching VIRTUAL_PROCESSOR_DATA, whose union rounds
 * up by a whole page when it grows (9.541 -> 0x1E), moving every hard-coded asm
 * offset with it.
 *
 * Indexed by KeGetCurrentProcessorNumber(), clamped, so an out-of-range core uses
 * the conservative answer (first pass) rather than indexing past the array.
 */
extern "C" { volatile LONG g_S206SecondPass[SVM206_MAX_VPD] = { 0 }; }

/*
 * 9.546: WHAT THE GUEST ACTUALLY STARTED AT.
 *
 * RealGuestMode sets the guest Rip from ContextRecord->Rip, which IS the output of
 * RtlCaptureContext inside SvVirtualizeProcessor. So the guest should resume just
 * after that call, fall through the CPUID probe and reach the early return. Measured
 * early-returns is 0, so one of the assumptions is wrong and this records which.
 *
 * First-core-only: six cores writing one record would describe whichever core won
 * the race, not the configuration. g_S206GuestRipSeen counts writers so the caller
 * knows whether the record is a single core's or a collision.
 */
extern "C" { volatile LONG64 g_S206GuestRipSet = 0; }
extern "C" { volatile LONG64 g_S206GuestRspSet = 0; }
extern "C" { volatile LONG64 g_S206GuestFirstExitRip = 0; }
extern "C" { volatile LONG g_S206RealModeAtSetup = 0; }   /* 9.546: LONG, used with 32-bit interlocked */
extern "C" { volatile LONG g_S206GuestRipWriters = 0; }
extern "C" { volatile LONG g_S206GuestFirstExitTaken = 0; }

extern "C" void yghv_s206_guest_start_record(UINT64 *ripSet, UINT64 *rspSet,
                                             UINT64 *firstExitRip, UINT64 *realMode,
                                             UINT64 *writers, UINT64 *exitTaken)
{
    *ripSet        = (UINT64)InterlockedCompareExchange64(&g_S206GuestRipSet, 0, 0);
    *rspSet        = (UINT64)InterlockedCompareExchange64(&g_S206GuestRspSet, 0, 0);
    *firstExitRip  = (UINT64)InterlockedCompareExchange64(&g_S206GuestFirstExitRip, 0, 0);
    *realMode      = (UINT64)(UINT32)InterlockedCompareExchange(&g_S206RealModeAtSetup, 0, 0);
    *writers       = (UINT64)(UINT32)InterlockedCompareExchange(&g_S206GuestRipWriters, 0, 0);
    *exitTaken     = (UINT64)(UINT32)InterlockedCompareExchange(&g_S206GuestFirstExitTaken, 0, 0);
}

extern "C" void yghv_s206_guest_start_reset(VOID)
{
    InterlockedExchange64(&g_S206GuestRipSet, 0);
    InterlockedExchange64(&g_S206GuestRspSet, 0);
    InterlockedExchange64(&g_S206GuestFirstExitRip, 0);
    InterlockedExchange(&g_S206GuestRipWriters, 0);
    InterlockedExchange(&g_S206GuestFirstExitTaken, 0);
}


extern "C" { volatile LONG64 g_S206SecondPassReads = 0; }

extern "C" { volatile LONG64 g_S206FlagDisagrees = 0; }


extern "C" { volatile LONG64 g_S206EarlyReturns = 0; }

extern "C" void yghv_s206_virtualize_counts(UINT64 *entries, UINT64 *early,
                                             UINT64 *secondPass, UINT64 *disagrees)
{
    UINT64 marked = 0;
    UINT32 i;

    *entries    = (UINT64)InterlockedCompareExchange64(&g_S206VirtualizeEntries, 0, 0);
    *early      = (UINT64)InterlockedCompareExchange64(&g_S206EarlyReturns, 0, 0);
    *disagrees  = (UINT64)InterlockedCompareExchange64(&g_S206FlagDisagrees, 0, 0);

    /*
     * 9.545: report HOW MANY CORES are marked, not the value of one global. With a
     * single flag "1" was ambiguous - it could mean one core or all of them. The
     * count is what distinguishes "only core 0 got virtualized" from "all six did".
     */
    for (i = 0; i < SVM206_MAX_VPD; i++)
    {
        if (InterlockedCompareExchange(&g_S206SecondPass[i], 0, 0) != 0) marked += 1;
    }
    *secondPass = marked;
}



/*
 * 9.525: HOW MANY CORES TO VIRTUALISE, read from the registry at prepare.
 *
 * This exists because the cost curve is superlinear (44.3x for 12 cores, not
 * 11x - 9.524) and a two-point measurement cannot say where the knee is. This
 * machine has 6 physical and 12 logical cores, so the question "what happens
 * at 6" is the one that decides whether the fix is core pairing or contention,
 * and changing a compile-time constant per sample would mean one rebuild per
 * point.
 *
 * Absent, zero, or unreadable -> all cores: the default behaviour is
 * UNCHANGED, which is the only acceptable default for a parameter a boot
 * depends on.
 */
/*
 * 9.526: COUNT PHYSICAL CORES, NOT LOGICAL PROCESSORS.
 *
 * Measured (12-thread benchmark, every logical processor loaded):
 *   6 cores -> 1.7x     <- this machine, and the best point by far
 *   12 cores -> 11.6x   <- what "all processors" gives you
 *
 * RelationProcessorCore returns ONE record per physical core, and each record
 * carries a Mask listing all the SMT siblings that belong to it. Counting the
 * records is therefore the physical core count, with no CPUID decoding and no
 * assumption that SMT is 2-way.
 *
 * Returns 0 on failure, and every caller treats 0 as "use the supplied
 * available count instead" - a topology query that fails must not silently
 * reduce how much of the machine is virtualised.
 */
extern "C" ULONG yghv_s206_physical_cores(VOID)
{
    ULONG  len    = 0;
    ULONG  count  = 0;
    PUCHAR buf    = nullptr;
    NTSTATUS st;

    /* First call learns the required size and is expected to fail. */
    st = KeQueryLogicalProcessorRelationship(nullptr, RelationProcessorCore,
                                             nullptr, &len);
    if (st != STATUS_BUFFER_TOO_SMALL && st != STATUS_INFO_LENGTH_MISMATCH)
    {
        return 0;
    }

    buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, len, 'pCvS');
    if (buf == nullptr) return 0;

    st = KeQueryLogicalProcessorRelationship(nullptr, RelationProcessorCore,
                                             (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf,
                                             &len);
    if (NT_SUCCESS(st))
    {
        /*
         * The array is a sequence of VARIABLE-LENGTH records - the EX type
         * carries a Size field for exactly this reason, so advancing by
         * sizeof() would walk off the last record and count garbage.
         */
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX it =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf;
        PUCHAR end = buf + len;
        while ((PUCHAR)it < end && it->Size != 0)
        {
            if (it->Relationship == RelationProcessorCore) count += 1;
            it = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)
                 ((PUCHAR)it + it->Size);
        }
    }

    ExFreePoolWithTag(buf, 'pCvS');
    return count;
}

/*
 * 9.539: OPT-IN "real Windows context as guest" MODE. DEFAULT OFF.
 *
 * The 2026-08 OS-as-guest conclusion ("platform limit, errata 1363") was drawn
 * from experiments that all ran AFTER 9.443 replaced the guest with a perpetual
 * stub. The reference implementations (Type2-AMD-HV, miniSVM) put the RUNNING
 * Windows thread in the guest instead, and report it working. The intercept-bit
 * comparison shows we already match them there - so the untried variable is the
 * guest CONTEXT, not the VMCB configuration.
 *
 * This flag exists so that trying it cannot touch the shipping path: with it
 * off, Sv206SetupGuest sets RIP to the stub exactly as before. With it on, RIP
 * and RSP come from the captured Windows context.
 *
 * EXPECTED TO FAIL. 9.442 already established that a guest which is a real
 * thread has no safe exit: the stop path would resume it mid-function with
 * RCX/RBX destroyed and SVM already off. That is why this is a flag and not a
 * new default.
 */
extern "C" int yghv_s206_real_guest_mode(VOID)
{
    static volatile LONG cached = -1;
    UNICODE_STRING  path;
    OBJECT_ATTRIBUTES attrs;
    HANDLE          key = nullptr;
    ULONG           value = 0;
    ULONG           size  = sizeof(value);
    NTSTATUS        st;
    LONG            hit;

    hit = InterlockedCompareExchange(&cached, -1, -1);
    if (hit != -1) return (int)hit;

    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet"
                                L"\\Services\\yghvpa\\Parameters");
    InitializeObjectAttributes(&attrs, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);

    if (NT_SUCCESS(ZwOpenKey(&key, KEY_QUERY_VALUE, &attrs)))
    {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, L"RealGuestMode");
        st = ZwQueryValueKey(key, &name, KeyValuePartialInformation, nullptr, 0, &size);
        if (st == STATUS_BUFFER_OVERFLOW || st == STATUS_BUFFER_TOO_SMALL)
        {
            void *buf = ExAllocatePoolWithTag(NonPagedPool, size, 'gEvR');
            if (buf != nullptr)
            {
                st = ZwQueryValueKey(key, &name, KeyValuePartialInformation, buf, size, &size);
                if (NT_SUCCESS(st))
                {
                    KEY_VALUE_PARTIAL_INFORMATION *kv =
                        static_cast<KEY_VALUE_PARTIAL_INFORMATION *>(buf);
                    if (kv->Type == REG_DWORD && kv->DataLength >= sizeof(ULONG))
                    {
                        value = *reinterpret_cast<ULONG *>(kv->Data);
                    }
                }
                ExFreePoolWithTag(buf, 'gEvR');
            }
        }
        ZwClose(key);
    }

    hit = (value != 0) ? 1 : 0;
    InterlockedExchange(&cached, hit);
    return (int)hit;
}

extern "C" ULONG yghv_s206_core_limit(ULONG available)
{
    UNICODE_STRING  path;
    OBJECT_ATTRIBUTES attrs;
    HANDLE          key = nullptr;
    ULONG           value = 0;
    ULONG           size  = sizeof(value);
    NTSTATUS        st;

    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet"
                                L"\\Services\\yghvpa\\Parameters");
    InitializeObjectAttributes(&attrs, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);

    st = ZwOpenKey(&key, KEY_QUERY_VALUE, &attrs);
    if (!NT_SUCCESS(st))
    {
        /* 9.526: no Parameters key -> PHYSICAL cores, the measured optimum.
         * 12-thread load: 6 cores 1.7x vs 12 cores 11.6x. Falling back to
         * `available` here would keep the 11.6x behaviour as the default. */
        ULONG phys = yghv_s206_physical_cores();
        return (phys != 0 && phys < available) ? phys : available;
    }

    {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, L"CoreCount");
        st = ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                             nullptr, 0, &size);
        /* The first call is expected to fail: it is how the size is learned. */
        if (st == STATUS_BUFFER_OVERFLOW || st == STATUS_BUFFER_TOO_SMALL)
        {
            void *buf = ExAllocatePoolWithTag(NonPagedPool, size, 'mCvS');
            if (buf != nullptr)
            {
                st = ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                                     buf, size, &size);
                if (NT_SUCCESS(st))
                {
                    KEY_VALUE_PARTIAL_INFORMATION *kv =
                        static_cast<KEY_VALUE_PARTIAL_INFORMATION *>(buf);
                    if (kv->Type == REG_DWORD && kv->DataLength >= sizeof(ULONG))
                    {
                        value = *reinterpret_cast<ULONG *>(kv->Data);
                    }
                }
                ExFreePoolWithTag(buf, 'mCvS');
            }
        }
    }

    ZwClose(key);

    if (value == 0)
    {
        /* 9.526: 0 also means "no explicit override" -> physical cores. */
        ULONG phys = yghv_s206_physical_cores();
        return (phys != 0 && phys < available) ? phys : available;
    }
    if (value > available) value = available;
    return value;
}


/*
 * 9.521: HANDLER-COST INSTRUMENTATION.
 *
 * Two shared accumulators updated once per exit. The contention is real (~22,700
 * InterlockedAdd64 per second) but small next to what it measures, and the
 * measurement it produced (0.31us/exit = 0.7% of the machine) is what moved this
 * tree from "optimise the handler" to "optimise the core count".
 */
extern "C" { volatile LONG64 g_S206HandlerTscTotal = 0; }
extern "C" { volatile LONG64 g_S206HandlerTscCount = 0; }

extern "C" void yghv_s206_handler_tsc_read(UINT64 *total, UINT64 *count)
{
    *total = (UINT64)InterlockedCompareExchange64(&g_S206HandlerTscTotal, 0, 0);
    *count = (UINT64)InterlockedCompareExchange64(&g_S206HandlerTscCount, 0, 0);
}

extern "C" void yghv_s206_handler_tsc_reset(VOID)
{
    InterlockedExchange64(&g_S206HandlerTscTotal, 0);
    InterlockedExchange64(&g_S206HandlerTscCount, 0);
}


extern "C" LONG yghv_s206_cores_started(VOID)
{
    return InterlockedCompareExchange(&g_S206CoresStarted, 0, 0);
}

extern "C" LONG yghv_s206_cores_exited(VOID)
{
    return InterlockedCompareExchange(&g_S206CoresExited, 0, 0);
}

extern "C" { volatile LONG g_VmrunStopRequested = 0; }
/* How many VMRUN loops have taken the exit. The waiter needs a NUMBER, not a
 * flag: "I asked them to stop" and "they stopped" are different facts, and this
 * session lost 15 machines to confusing the two. */
/* 9.416: non-static and extern "C" for the same reason as g_VmrunStopRequested -
 * the ASM loop increments it directly, without a call. */
extern "C" { volatile LONG g_VmrunStopped = 0; }

/*
 * 9.414: called once per VMRUN-loop iteration, from SvLV10 in
 * svm_simplevm206_x64.asm. Only interlocked reads - no kernel API - so it is
 * callable with GIF clear inside the VM-exit frame. That is the requirement that
 * forced this into the ASM loop: the previous location (SvHandleVmExit) only runs
 * on a #VMEXIT, and the guest restarts from the same RIP, so it never ran.
 *
 * Returns 1 when the loop should leave; the ASM then takes the normal SvLV20
 * exit, the same one the handler uses, so there is still exactly one exit path.
 *
 * The request is NOT cleared here (9.412). Consuming it would mean the first core
 * to arrive stops and the rest keep running - the "stopped=1 / active=12" shape.
 */
extern "C" int Sv206VmrunShouldExit(VOID)
{
    /*
     * 9.443: THIS NO LONGER INCREMENTS THE COUNTER, AND THAT FIXES A REAL BUG.
     *
     * It used to do `InterlockedIncrement(&g_VmrunStopped)` on every call. Now that
     * the stub ticks on every iteration, that would increment once per tick - so
     * `g_VmrunStopped` would race past the core count in microseconds and the unload
     * waiter would conclude "all cores stopped" while eleven of them were still
     * running. A counter that measures ticks is not a counter of cores.
     *
     * The core count is incremented exactly once per core, in Sv206CoreThread, which
     * a core reaches only after SvLV20 has taken it out of the guest permanently and
     * control has returned through Sv206LeaveToHost and the entry trampoline.
     * That is the number DriverUnload actually needs.
     */
    return (InterlockedCompareExchange(&g_VmrunStopRequested, 0, 0) != 0) ? 1 : 0;
}

/*
 * 9.443: THE ABOVE IS NOW THE DESIGN'S CENTREPIECE, NOT DEAD CODE.
 *
 * History, because it matters: 9.419 deleted the ASM loop's check and left this
 * function with no callers, so g_VmrunStopped was identically zero and the unload
 * wait could only time out (9.440). 9.442 then established why the loop cannot
 * simply be asked to stop from inside SvHandleVmExit: SvLV20's `jmp rbx` RESUMES
 * THE GUEST at an arbitrary instruction boundary with SVM already off, which is
 * silent corruption for any thread that was not sitting inside __cpuidex.
 *
 * The way out is the one this function always described: the guest must be code
 * that WE wrote and that WE can stop at a point of our choosing. 9.443 makes that
 * true - the 206 guest becomes a dedicated stub whose loop reads this flag and
 * ends in a vmmcall when it is set. Then:
 *
 *   - the stop point is inside our own stub, where no Windows register is live;
 *   - the exit is a vmmcall, an intercepted instruction, so SvLV20's resume lands
 *     on OUR next instruction rather than someone's mid-function PC;
 *   - each core runs its own thread, so the VMRUN frame that never returns
 *     belongs to that thread and is never unmapped out from under a live core.
 *
 * This predicate is what those loops consult. It is reached through a vmmcall the
 * handler answers, NOT from the ASM frame - which is why the call-site constraint
 * that killed 9.413/9.414/9.416 no longer applies.
 */

/*
 * 9.422: A GLOBAL COUNT OF CORES THAT ARE INSIDE vmrun RIGHT NOW.
 *
 * 9.421 established the real crash instruction: `vmsave rax` at Sv206LaunchVm+0xD,
 * the first instruction after vmrun returns. It writes the guest state back into
 * the VMCB whose physical address is in rax - and if svm_core_cleanup has already
 * freed that VMCB, the write faults at DISPATCH (0xD1, 8 bytes).
 *
 * The race is therefore not "the loop would not stop". It is:
 *
 *     core A: inside vmrun, guest running
 *     core B: DriverUnload -> svm_core_cleanup frees A's VMCB
 *     core A: vmrun returns, executes vmsave -> writes freed memory
 *
 * No check placed on the loop's back-edge can help, because vmsave runs BEFORE the
 * back-edge. What is needed is the other side of the constraint: the teardown must
 * know whether anyone is still inside vmrun.
 *
 * Not per-core: VIRTUAL_PROCESSOR_DATA has no core index (HostStackLayout carries
 * GuestVmcbPa/HostVmcbPa/Self/Padding1/Reserved1 only), so the ASM cannot index a
 * per-core array. A global count needs no index and answers the only question
 * DriverUnload has: "is anybody inside".
 *
 * The ASM does `lock inc` before vmrun and `lock dec` after it - two stores with
 * no branch, no call, touching neither rsp nor rcx, so they are safe in this frame
 * in a way the 9.416 conditional check was not.
 */
extern "C" { volatile LONG g_inVmrunCount = 0; }

/*
 * 9.462: how many parked cores the last stop request actually woke.
 *
 * This is the replacement for "the stub ticks, so the flag is checked constantly".
 * A zero here after a `sc stop` would mean no core was ever woken, i.e. the unload
 * would hang waiting for threads that are asleep in hlt - a silent, permanent
 * STOP_PENDING. Reporting it turns that into a number a caller can read.
 */
static volatile LONG64 g_S206KickedCores = 0;

extern "C" LONG64 yghv_s206_kicked_cores(VOID)
{
    return InterlockedCompareExchange64(&g_S206KickedCores, 0, 0);
}

extern "C" int yghv_s206_invmrun_count(VOID)
{
    return (int)InterlockedCompareExchange(&g_inVmrunCount, 0, 0);
}

extern "C" void yghv_s206_vmrun_stop_request(VOID)
{
    /*
     * 9.462: SET THE FLAG, THEN WAKE THE PARKED CORES.
     *
     * Order matters: a core that wakes must find the flag already set, or it takes
     * the injected event, runs the handler, sees "not stopping", and parks again -
     * and the wake is wasted. Flag first, kick second.
     *
     * The kick is what replaces the old `vmmcall` tick. Without it a parked core
     * would never take a VMEXIT and Sv206VmrunShouldExit would never run, which is
     * why 9.432 could not simply delete vmmcall.
     */
    extern LONG yghv_s206_kick_parked_cores(VOID);
    LONG kicked;

    InterlockedExchange(&g_VmrunStopRequested, 1);
    kicked = yghv_s206_kick_parked_cores();
    InterlockedExchange64(&g_S206KickedCores, (LONG64)kicked);
}

extern "C" int yghv_s206_vmrun_stop_pending(VOID)
{
    return (int)InterlockedCompareExchange(&g_VmrunStopRequested, 0, 0);
}

/* Cleared once a core has taken the exit, so a later load starts clean. */
extern "C" void yghv_s206_to_stop_clear(VOID);

extern "C" void yghv_s206_vmrun_stop_clear(VOID)
{
    InterlockedExchange(&g_VmrunStopRequested, 0);
    InterlockedExchange(&g_VmrunStopped, 0);
}

/* How many loops have left. Compared against the core count by the waiter. */
/* 9.404: read-only accessors for the pre-teardown snapshot. */
extern "C" ULONG_PTR yghv_s206_vmrun_stopped_for_trace(VOID)
{
    return (ULONG_PTR)InterlockedCompareExchange(&g_VmrunStopped, 0, 0);
}

extern "C" LONG yghv_s206_cpuid_view_for_trace(VOID)
{
    return InterlockedCompareExchange(&g_CpuidView, 0, 0);
}

extern "C" int yghv_s206_vmrun_stopped_count(VOID)
{
    /*
     * 9.443: THIS NOW REPORTS CORES THAT ACTUALLY LEFT, NOT TICKS.
     *
     * It used to read g_VmrunStopped, which Sv206VmrunShouldExit incremented on
     * every call. With the stub ticking per iteration that would have counted
     * heartbeats, so the unload waiter would see "12 cores stopped" after twelve
     * ticks - microseconds - while every core was still in its VMRUN loop. The
     * waiter would then free the VMCB and NPT out from under twelve live cores.
     *
     * g_S206CoresExited is incremented once per core, in Sv206CoreThread, after
     * SvLV20 has taken that core out of the guest for good and control has unwound
     * back through the entry trampoline. That is the only number that means what the
     * caller thinks it means.
     */
    return (int)InterlockedCompareExchange(&g_S206CoresExited, 0, 0);
}

/* 9.441: the exit counter's read-only door, for the pre-teardown snapshot and
 * for any control-plane readout. Returns the raw value. */
extern "C" LONG64 yghv_s206_exits_count(VOID)
{
    return InterlockedCompareExchange64(&g_S206Exits, 0, 0);
}

extern "C" LONG64 yghv_s206_vmmcalls_count(VOID)
{
    return InterlockedCompareExchange64(&g_S206Vmmcalls, 0, 0);
}

extern "C" int yghv_s206_bp_int3_intercept(UINT32 on)
{
    InterlockedExchange(&g_BpInt3Intercept, on ? 1 : 0);
    return 1;
}

/*
 * 9.464: READ ONE LIVE CORE'S GUEST STATE, because "hlt does nothing" is a
 * symptom with three different causes and I could not tell them apart.
 *
 * Measured shape of the problem: the stub runs `hlt / mov rax,1 / vmmcall / jmp`
 * and vmmcall still fires ~12.6 million times per second, i.e. hlt returns
 * immediately every time. Three candidates, and each needs a different fix:
 *
 *   RFLAGS.IF == 0    -> hlt cannot be woken by maskable interrupts, so it would
 *                        WAIT, not return. (Rules this out if IF is 1.)
 *   VIntr pending     -> the virtual interrupt is already asserted, so the core
 *                        services "a pending interrupt" and falls straight through
 *                        hlt. This is the most likely one: nothing in this tree
 *                        ever clears VIntr, and 9.448's `events` experiments wrote
 *                        EventInj freely.
 *   InterruptShadow   -> a stale shadow makes the next sti-window instruction
 *                        behave differently, but by itself does not un-park hlt.
 *
 * Returns 0 if no core is registered. Otherwise fills the caller's 5 qwords:
 *   [0] Rflags   [1] VIntr   [2] InterruptShadow   [3] EventInj   [4] core index
 */
extern "C" LONG yghv_s206_guest_state(UINT64 *out)
{
    if (!out)
        return 0;

    for (UINT32 i = 0; i < SVM206_MAX_VPD; i++)
    {
        PVIRTUAL_PROCESSOR_DATA vp = g_S206LiveVpd[i];

        if (vp == NULL)
            continue;

        out[0] = vp->GuestVmcb.StateSaveArea.Rflags;
        out[1] = vp->GuestVmcb.ControlArea.VIntr;
        out[2] = vp->GuestVmcb.ControlArea.InterruptShadow;
        out[3] = vp->GuestVmcb.ControlArea.EventInj;
        out[4] = (UINT64)i;
        return 1;
    }
    return 0;
}

extern "C" int yghv_s206_bp_int3_intercept_get(VOID)
{
    return (int)InterlockedCompareExchange(&g_BpInt3Intercept, 0, 0);
}

/* Real-TSC ticks per millisecond, measured once. Used only for the freeze
   deadline. A loose estimate is fine: the deadline exists to bound a hang, not to
   be exact. */
static LONG64 yghv_s206_tsc_per_ms(void)
{
    static volatile LONG64 cached = 0;
    LONG64 v = InterlockedCompareExchange64(&cached, 0, 0);
    if (v)
        return v;
    {
        UINT64 t0 = __rdtsc();
        LARGE_INTEGER li;
        li.QuadPart = -10000;                    /* 1 ms in 100ns units */
        KeDelayExecutionThread(KernelMode, FALSE, &li);
        v = (LONG64)(__rdtsc() - t0);
        if (v <= 0)
            v = 1000000;                         /* absurd fallback rather than 0 */
        InterlockedExchange64(&cached, v);
    }
    return v;
}

extern "C" int yghv_s206_tsc_apply_all(UINT32 arm);

/*!
    @brief      Set the constant offset. PASSIVE (control plane).
    @result     always 1 — any offset is a legal offset.
*/
extern "C" int yghv_s206_tsc_offset_set(UINT64 offset)
{
    InterlockedExchange64(&g_TscOffset, (LONG64)offset);
    return 1;
}

extern "C" UINT64 yghv_s206_tsc_offset_get(VOID)
{
    return (UINT64)InterlockedCompareExchange64(&g_TscOffset, 0, 0);
}

/*!
    @brief      Freeze or thaw the clock FOR ONE CR3, with a hard deadline.

    @params     on        1 = freeze, 0 = thaw
    @params     cr3       the address space to freeze (0 refuses: see below)
    @params     max_ms    the deadline in real milliseconds; clamped to [1, 10000]

    @result     1 on success, 0 if refused.

    @details    A freeze with cr3 == 0 is REFUSED. A global freeze is what hung the
                machine on the first attempt (9.369.2), and "refuse the shape that
                already cost a reboot" is cheaper than remembering not to use it.
                Callers that want to think they froze everything can pass a CR3
                that matches everything - but they have to say so explicitly.

                The deadline is not optional and not advisory: the RDTSC handler
                compares it against the REAL TSC on every intercepted read and
                thaws itself the moment it passes. So the worst case for a freeze
                is `max_ms` of a stopped clock in one address space, and never a
                machine that needs a power cycle.

                Thawing is idempotent: Mode=0 is the only state that matters.
*/
extern "C" int yghv_s206_tsc_freeze(UINT32 on, UINT64 cr3, UINT32 max_ms)
{
    if (!on)
    {
        InterlockedExchange64(&g_TscMode, 0);
        InterlockedExchange64(&g_TscScopeCr3, 0);
        InterlockedExchange64(&g_TscFreezeExpiresAt, 0);
        /*
         * 9.370: THE RELEASE IS A PER-CORE WRITE, NOT A HOPE.
         *
         * 9.369b/c/d all assumed each core would clear its own bit the next time
         * it took an exit. Measured on G36: it did not, exits kept climbing, and
         * one run died of 0x101. So the control plane walks every core here and
         * clears the bit itself, with VmcbClean=0 (the same mechanism the
         * fake-write NCr3 switch has used in production).
         *
         * The walk is attempted EVEN IF the policy flags were already zero: an
         * armed bit can survive from an earlier load (VMCB memory outlives the
         * driver), so thaw is also the "make sure nobody is armed" operation.
         */
        {
            int ok = yghv_s206_tsc_apply_all(0);
            InterlockedExchange(&g_TscInterceptArmed, 0);
            if (!ok)
                InterlockedIncrement64(&g_TscApplyFailures);
        }
        return 1;
    }

    /* A global freeze is refused on purpose. See the note on the state above. */
    if (!cr3)
        return 0;

    if (max_ms == 0)
        max_ms = 1;
    if (max_ms > 10000)
        max_ms = 10000;

    {
        UINT64 off = yghv_s206_tsc_offset_get();
        LONG64 per_ms = yghv_s206_tsc_per_ms();
        UINT64 now = __rdtsc();

        /* Set the deadline FIRST: if a core reads between these stores it must
         * see a valid deadline, not the previous one. */
        InterlockedExchange64(&g_TscFreezeExpiresAt,
                              (LONG64)(now + (UINT64)per_ms * (UINT64)max_ms));
        InterlockedExchange64(&g_TscScopeCr3, (LONG64)(cr3 & ~(UINT64)0xFFF));
        InterlockedExchange64(&g_TscFrozenAt, (LONG64)(now + off));
        InterlockedExchange64(&g_TscBudgetLeft, YGHV_TSC_EXIT_BUDGET);
        InterlockedExchange64(&g_TscMode, 1);
        /*
         * 9.370: arm every core from here. If the walk fails, the freeze is
         * ROLLED BACK rather than left half-armed - a half-armed walk means some
         * cores exit on every RDTSC and others do not, which is both a detection
         * signal and a starvation risk.
         */
        {
            int ok = yghv_s206_tsc_apply_all(1);
            if (!ok)
            {
                InterlockedExchange64(&g_TscMode, 0);
                InterlockedExchange64(&g_TscScopeCr3, 0);
                InterlockedExchange64(&g_TscFreezeExpiresAt, 0);
                (void)yghv_s206_tsc_apply_all(0);
                InterlockedExchange(&g_TscInterceptArmed, 0);
                InterlockedIncrement64(&g_TscApplyFailures);
                return 0;                /* refused: could not arm every core */
            }
            InterlockedExchange(&g_TscInterceptArmed, 1);
        }
    }
    return 1;
}

/*! out[0]=offset out[1]=mode out[2]=frozen_at out[3]=intercepts out[4]=suppressed
    out[5]=intercept bit armed (0/1) out[6]=real rdtsc now, for a caller that wants
    to compute the guest-visible value itself. */
extern "C" void yghv_s206_tsc_stats(UINT64 *out)
{
    if (!out)
        return;
    out[0] = (UINT64)InterlockedCompareExchange64(&g_TscOffset, 0, 0);
    out[1] = (UINT64)InterlockedCompareExchange64(&g_TscMode, 0, 0);
    out[2] = (UINT64)InterlockedCompareExchange64(&g_TscFrozenAt, 0, 0);
    out[3] = (UINT64)InterlockedCompareExchange64(&g_TscIntercepts, 0, 0);
    out[4] = (UINT64)InterlockedCompareExchange64(&g_TscSuppressed, 0, 0);
    out[5] = (UINT64)InterlockedCompareExchange(&g_TscInterceptArmed, 0, 0);
    out[6] = (UINT64)__rdtsc();
    /* 9.369: the scoping fields, so a caller can tell "frozen" from "frozen for
     * the address space I care about" and can see the deadline it was given. */
    out[7] = (UINT64)InterlockedCompareExchange64(&g_TscScopeCr3, 0, 0);
    out[8] = (UINT64)InterlockedCompareExchange64(&g_TscFreezeExpiresAt, 0, 0);
    out[9] = (UINT64)InterlockedCompareExchange64(&g_TscExpired, 0, 0);
    out[10] = (UINT64)InterlockedCompareExchange64(&g_TscScopedOut, 0, 0);
    out[11] = (UINT64)InterlockedCompareExchange64(&g_TscBudgetLeft, 0, 0);
    out[12] = (UINT64)InterlockedCompareExchange64(&g_TscBudgetHits, 0, 0);
    out[13] = (UINT64)InterlockedCompareExchange64(&g_TscSelfDisarmed, 0, 0);
    out[14] = (UINT64)InterlockedCompareExchange64(&g_TscApplyFailures, 0, 0);
    out[15] = (UINT64)InterlockedCompareExchange64(&g_TscCoreHits, 0, 0);
    out[16] = (UINT64)InterlockedCompareExchange64(&g_TscCoreMisses, 0, 0);
}

extern "C" int yghv_s206_tsc_armed(VOID)
{
    return (int)InterlockedCompareExchange(&g_TscInterceptArmed, 0, 0);
}

extern "C" void yghv_s206_tsc_mark_armed(VOID)
{
    InterlockedExchange(&g_TscInterceptArmed, 1);
}

/*!
    @brief      Is an RDTSC intercept worth paying for right now?

    @details    RDTSC is one of the hottest instructions a guest executes — timing
                loops, KASLR-seed checks and every profiler hit it thousands of
                times a second. Arming the intercept turns each of those into a
                #VMEXIT, which is both a large slowdown and a detection signal in
                its own right (9.343's exit-latency counters measure exactly that).

                A CONSTANT OFFSET NEEDS NO INTERCEPT: hardware applies TscOffset
                itself. So the intercept is armed only when the policy asks for
                something the hardware cannot do — i.e. frozen time.

                This predicate is what keeps "Tier 5.1 is available" from becoming
                "Tier 5.1 is always on". A test that wants to observe RDTSC exits
                must first ask for a freeze; one that only sets an offset should
                see no exits at all, and that difference is itself checkable.
*/
extern "C" int yghv_s206_tsc_wanted(VOID)
{
    return (InterlockedCompareExchange64(&g_TscMode, 0, 0) != 0) ? 1 : 0;
}

/*
 * SimpleSVM specific constants.
 */

/*!
    @brief      Breaks into a kernel debugger when it is present.

    @details    This macro is emits software breakpoint that only hits when a
                kernel debugger is present. This macro is useful because it does
                not change the current frame unlike the DbgBreakPoint function,
                and breakpoint by this macro can be overwritten with NOP without
                impacting other breakpoints.
 */
#define SV_DEBUG_BREAK() \
    if (KD_DEBUGGER_NOT_PRESENT) \
    { \
        NOTHING; \
    } \
    else \
    { \
        __debugbreak(); \
    } \
    reinterpret_cast<void*>(0)

//
// A power state callback handle.
//
static PVOID g_PowerCallbackRegistration;

/* 9.404: the registration is static, so the pre-teardown snapshot cannot reach it
 * by link. This is the read-only door. */
extern "C" ULONG_PTR yghv_s206_power_cb_for_trace(VOID)
{
    return (ULONG_PTR)(g_PowerCallbackRegistration ? 1 : 0);
}

/*!
    @brief      Sends a message to the kernel debugger.

    @param[in]  Format - The format string to print.
 */
#pragma prefast(push)
#pragma prefast(disable : 26826, "C-style variable arguments needed for DbgPrint.")
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
static
VOID
SvDebugPrint (
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    va_list argList;

    va_start(argList, Format);
    vDbgPrintExWithPrefix("[SimpleSvm] ",
                          DPFLTR_IHVDRIVER_ID,
                          DPFLTR_ERROR_LEVEL,
                          Format,
                          argList);
    va_end(argList);
}
#pragma prefast(pop)

/*!
    @brief      Allocates page aligned, zero filled physical memory.

    @details    This function allocates page aligned nonpaged pool. The
                allocated memory is zero filled and must be freed with
                SvFreePageAlingedPhysicalMemory. On Windows 8 and later versions
                of Windows, the allocated memory is non executable.

    @param[in]  NumberOfBytes - A size of memory to allocate in byte. This must
                be equal or greater than PAGE_SIZE.

    @result     A pointer to the allocated memory filled with zero; or NULL when
                there is insufficient memory to allocate requested size.
 */
__drv_allocatesMem(Mem)
_Post_writable_byte_size_(NumberOfBytes)
_Post_maybenull_
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
_Must_inspect_result_
static
PVOID
SvAllocatePageAlingedPhysicalMemory (
    _In_ SIZE_T NumberOfBytes
    )
{
    PVOID memory;

    //
    // The size must be equal or greater than PAGE_SIZE in order to allocate
    // page aligned memory.
    //
    NT_ASSERT(NumberOfBytes >= PAGE_SIZE);

    /*
     * 9.386: THE PAGE ALIGNMENT WAS ONLY ASSERTED, AND THE ASSERTION IS EMPTY.
     *
     * The function is named PageAlinged and the only thing enforcing that was:
     *
     *     NT_ASSERT(PAGE_ALIGN(memory) == memory);
     *
     * NT_ASSERT compiles to nothing in a free build. So in the driver that
     * actually ships, this returned whatever ExAllocatePoolWithTag gave - which is
     * 8- or 16-byte aligned, NOT page aligned.
     *
     * Why that is fatal rather than cosmetic: the VPD's own address is used to
     * compute GuestVmcbPa and HostVmcbPa, and both are treated as PAGE BASES by
     * vmload/vmrun/vmsave. An offset VPD means those instructions read and write
     * the wrong page - and the faulting addresses in the last three bugchecks were
     * two ADJACENT pages (0x194bfd000/0x194bfe000, 0x2187f6000/0x2187f7000,
     * 0x3d0b19000/0x3d0b1a000), which is the shape of a calculation whose base was
     * not page aligned.
     *
     * Loading survived it because a fresh allocation has clean neighbouring pages;
     * unloading did not, because vmload/vmsave write whole-page state back.
     *
     * HOW THE ALIGNMENT IS NOW GUARANTEED
     *
     * Not by MmAllocateContiguousMemory: the VPD is ~32 KB (a 24 KB HostStackLimit
     * plus two 4 KB VMCBs), and asking for that much PHYSICALLY CONTIGUOUS memory
     * on a machine that has been up for a while fails often. A control path that
     * cannot allocate its own state is not more correct than a misaligned one.
     *
     * So: over-allocate by one page and carve an aligned span out of it. The
     * original pointer is kept immediately BELOW the aligned span, so the free
     * path can recover it without a side table (which would itself have to be
     * size-checked against every allocation).
     */
    {
        SIZE_T total = NumberOfBytes + PAGE_SIZE;
        PVOID raw;

        raw = ExAllocatePoolWithTag(NonPagedPool, total, 'MVSS');
        if (raw == nullptr)
            return nullptr;

        memory = (PVOID)PAGE_ALIGN((ULONG_PTR)raw);

        /*
         * Store the raw pointer just below the aligned base - BUT ONLY IF THAT
         * SLOT IS INSIDE OUR OWN ALLOCATION.
         *
         * When `raw` happens to be page aligned already (which is not rare: pool
         * blocks for large allocations are often page aligned), memory == raw and
         * memory[-1] would write 8 bytes BEFORE the block - a heap underrun that
         * corrupts the pool header. That is the same class of bug as the one this
         * function is being fixed for: a guarantee assumed rather than checked.
         *
         * So the aligned span is never the first 8 bytes of the block: if raw is
         * already aligned, the span is moved one page in, which always leaves room.
         */
        if ((ULONG_PTR)memory == (ULONG_PTR)raw)
            memory = (PVOID)((ULONG_PTR)raw + PAGE_SIZE);

        /* Now raw <= memory - PAGE_SIZE, so the slot below the span is inside the
         * allocation and outside the span itself. */
        ((PVOID *)memory)[-1] = raw;

        RtlZeroMemory(memory, NumberOfBytes);
    }
    return memory;
}

/*!
    @brief      Frees memory allocated by SvAllocatePageAlingedPhysicalMemory.

    @param[in]  BaseAddress - The address returned by
                SvAllocatePageAlingedPhysicalMemory.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
static
VOID
SvFreePageAlingedPhysicalMemory (
    _Pre_notnull_ __drv_freesMem(Mem) PVOID BaseAddress
    )
{
    /*
     * 9.386: the allocation over-allocates by a page and stores the raw pointer
     * immediately below the aligned span. Recover it from there - this is what
     * keeps the free path from needing a side table.
     */
    PVOID raw;

    if (BaseAddress == nullptr)
        return;

    raw = ((PVOID *)BaseAddress)[-1];
    if (raw == nullptr)
        return;                 /* not one of ours; refuse rather than corrupt */

    ExFreePoolWithTag(raw, 'MVSS');
}

/*!
    @brief      Allocates page aligned, zero filled contiguous physical memory.

    @details    This function allocates page aligned nonpaged pool where backed
                by contiguous physical pages. The allocated memory is zero
                filled and must be freed with SvFreeContiguousMemory. The
                allocated memory is executable.

    @param[in]  NumberOfBytes - A size of memory to allocate in byte.

    @result     A pointer to the allocated memory filled with zero; or NULL when
                there is insufficient memory to allocate requested size.
 */
_Post_writable_byte_size_(NumberOfBytes)
_Post_maybenull_
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
_Must_inspect_result_
static
PVOID
SvAllocateContiguousMemory (
    _In_ SIZE_T NumberOfBytes
    )
{
    PVOID memory;
    PHYSICAL_ADDRESS boundary, lowest, highest;

    boundary.QuadPart = lowest.QuadPart = 0;
    highest.QuadPart = -1;

    memory = MmAllocateContiguousNodeMemory(NumberOfBytes,
                                            lowest,
                                            highest,
                                            boundary,
                                            PAGE_READWRITE,
                                            MM_ANY_NODE_OK);
    if (memory != nullptr)
    {
        RtlZeroMemory(memory, NumberOfBytes);
    }
    return memory;
}

/*!
    @brief      Frees memory allocated by SvAllocateContiguousMemory.

    @param[in]  BaseAddress - The address returned by SvAllocateContiguousMemory.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
static
VOID
SvFreeContiguousMemory (
    _In_ PVOID BaseAddress
    )
{
    MmFreeContiguousMemory(BaseAddress);
}

/*!
    @brief          Injects #GP with 0 of error code.

    @param[in,out]  VpData - Per processor data.
 */
_IRQL_requires_same_
static
VOID
SvInjectGeneralProtectionException (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData
    )
{
    EVENTINJ event;

    //
    // Inject #GP(vector = 13, type = 3 = exception) with a valid error code.
    // An error code are always zero. See "#GP-General-Protection Exception
    // (Vector 13)" for details about the error code.
    //
    event.AsUInt64 = 0;
    event.Fields.Vector = 13;
    event.Fields.Type = 3;
    event.Fields.ErrorCodeValid = 1;
    event.Fields.Valid = 1;
    VpData->GuestVmcb.ControlArea.EventInj = event.AsUInt64;
}

/*!
    @brief          Handles #VMEXIT due to execution of the CPUID instructions.

    @details        This function returns unmodified results of the CPUID
                    instruction, except for few cases to indicate presence of
                    the hypervisor, and to process an unload request.

                    CPUID leaf 0x40000000 and 0x40000001 return modified values
                    to conform to the hypervisor interface to some extent. See
                    "Requirements for implementing the Microsoft Hypervisor interface"
                    https://msdn.microsoft.com/en-us/library/windows/hardware/Dn613994(v=vs.85).aspx
                    for details of the interface.

    @param[in,out]  VpData - Per processor data.
    @param[in,out]  GuestContext - Guest's GPRs.
 */
_IRQL_requires_same_
static
VOID
SvHandleCpuid (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    int leaf, subLeaf;
    SEGMENT_ATTRIBUTE attribute;

    //
    // Execute CPUID as requested.
    //
    leaf = static_cast<int>(GuestContext->VpRegs->Rax);
    subLeaf = static_cast<int>(GuestContext->VpRegs->Rcx);
    __cpuidex(registers, leaf, subLeaf);

    /*
     * 9.375 (Tier 9.1): the DEVICE-FREE control channel.
     *
     * Matched by MASK, not by an equality case, because the leaf carries the high
     * 16 bits of the request VA - see YGHV_CH_BASE in yghv_channel.h for why the
     * address has to be split across the only two registers CPUID reads.
     *
     * No object, no symbol link, no IOCTL: a caller with no handle at all puts a
     * request in its own memory and rings this leaf; we resolve the VA it named and
     * read the frame directly. That works only because the hypervisor can already
     * translate any guest VA - the same function the INT3 path uses.
     *
     * Register contract:
     *   in : EAX = YGHV_CH_BASE | va[47:32]   ECX = va[31:0]
     *   out: EAX = status   EBX:EDX = value   ECX = ACK (ALWAYS)
     *
     * The ack is written on every exit path including refusals; omitting it is what
     * made two callers dead code in 9.370.
     */
    if (((UINT32)leaf & YGHV_CH_BASE_MASK) == YGHV_CH_BASE)
    {
        uint64_t reqVa = ((uint64_t)((UINT32)leaf & 0xFFFFu) << 32)
                       | (uint64_t)(UINT32)subLeaf;
        uint32_t s_eax = 0, s_ebx = 0, s_ecx = 0, s_edx = 0;

        yghv_channel_doorbell(VpData->GuestVmcb.StateSaveArea.Cr3, reqVa,
                              0, 0, &s_eax, &s_ebx, &s_ecx, &s_edx);

        registers[0] = (int)s_eax;
        registers[1] = (int)s_ebx;
        registers[2] = (int)s_ecx;
        registers[3] = (int)s_edx;
        /* The GPR write-back below is shared by every answer, so jump to it rather
         * than duplicating it (and rather than a `break`, which has no target
         * outside a switch - the build caught that). */
        goto cpuid_writeback;
    }

    switch (leaf)
    {
    case CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS:
        /*
         * 9.368 (A4, Tier 5.4): THE HYPERVISOR BIT IS A POLICY, NOT A FACT.
         *
         * This used to OR the bit in unconditionally. "Indicate presence of a
         * hypervisor" is the honest answer, and it is also the one line a
         * detector checks first. Whether to admit it is a decision about what
         * the guest is allowed to know, so it belongs to the control plane.
         *
         * The count is incremented only when the answer actually DIFFERS from
         * the hardware's — a test that sets the level and sees `suppressed`
         * stay 0 would otherwise pass on a policy that never ran.
         */
        if (yghv_s206_cpuid_view_get() == YGHV_CPUID_VIEW_HONEST)
        {
            registers[2] |= CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT;
        }
        else
        {
            registers[2] &= ~((int)CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT);
            InterlockedIncrement64(&g_CpuidViewSuppressed);
        }
        break;
    case CPUID_HV_VENDOR_AND_MAX_FUNCTIONS:
        /*
         * 9.368/A4: level OFF says there is no hypervisor at all, so leaves
         * 0x40000000+ must answer as unsupported hardware does: all zero.
         *
         * NO_VENDOR_BIT deliberately still ANSWERS here. That is the level's
         * whole point — it clears bit 31 while leaving the vendor string
         * readable, which separates "detectors that check bit 31" from
         * "detectors that probe the leaves".
         */
        if (yghv_s206_cpuid_view_get() == YGHV_CPUID_VIEW_OFF)
        {
            registers[0] = registers[1] = registers[2] = registers[3] = 0;
            InterlockedIncrement64(&g_CpuidViewSuppressed);
            break;
        }
        //
        // Return a maximum supported hypervisor CPUID leaf range and a vendor
        // ID signature as required by the spec.
        //
        registers[0] = CPUID_HV_MAX;
        registers[1] = 'pmiS';  // "SimpleSvm   "
        registers[2] = 'vSel';
        registers[3] = '   m';
        break;
    case CPUID_HV_INTERFACE:
        /*
         * 9.368/A4: same rule as above. If the guest is being told there is no
         * hypervisor, the interface leaf cannot answer "Hv#0" — that alone
         * would confirm one is present.
         */
        if (yghv_s206_cpuid_view_get() == YGHV_CPUID_VIEW_OFF)
        {
            registers[0] = registers[1] = registers[2] = registers[3] = 0;
            InterlockedIncrement64(&g_CpuidViewSuppressed);
            break;
        }
        //
        // Return non Hv#1 value. This indicate that the SimpleSvm does NOT
        // conform to the Microsoft hypervisor interface.
        //
        registers[0] = '0#vH';  // Hv#0
        registers[1] = registers[2] = registers[3] = 0;
        break;
    case CPUID_FAKE_VPD:
        //
        // 9.288 (C6 v5): return EDX:EAX = this core's VPD PA without
        // de-virtualizing. Kernel mode only (the fake-write resync runs at
        // APC_LEVEL from yghv's control plane).
        //
        /*
         * 9.370: THE ACK NOW EXISTS. It did not before, and that is why two
         * callers were dead code.
         *
         * Both callers (SvFakeNcr3Resync and the TSC applier added with it)
         * checked `registers[2] != '1VSS'` to mean "this core answered". But this
         * branch never wrote ECX, so the check compared against whatever the
         * hardware's own CPUID had put in ECX for an unimplemented leaf - which
         * is not '1VSS'. Measured: '1VSS' appears only in those two comparisons
         * and in no assignment anywhere in the tree, so both callers took the
         * goto Exit path EVERY time.
         *
         * That is the project's recurring shape once more: an assertion about a
         * value nobody produces. The cost here was silent - SvFakeNcr3Resync has
         * been a no-op, and its "wire together" comment claimed otherwise.
         *
         * The ack is now written, so the check means what it says:
         *   ECX = 'SSV1' when the pointer is being returned
         *   ECX = 0      when refused (not kernel mode)
         */
        if (subLeaf == CPUID_FAKE_VPD)
        {
            attribute.AsUInt16 = VpData->GuestVmcb.StateSaveArea.SsAttrib;
            if (attribute.Fields.Dpl == DPL_SYSTEM)
            {
                /*
                 * 9.370b: RETURN THE VIRTUAL ADDRESS, NOT THE PHYSICAL ONE.
                 *
                 * This returned MmGetPhysicalAddress(VpData), and BOTH callers
                 * then dereferenced it as if it were a pointer:
                 *
                 *     vpData = (PVIRTUAL_PROCESSOR_DATA)(high << 32 | low);
                 *     vpData->GuestVmcb.ControlArea...
                 *
                 * That is the VA/PA confusion this project has recorded four
                 * times (KERNEL_API_CHECKLIST §21, §27) - and this instance had
                 * never fired because both callers were ALSO dead: they checked
                 * for an ack ('1VSS') that nothing wrote, so they always took the
                 * early exit. Fixing the ack (9.370) exposed the dereference, and
                 * it produced 0x3B at SvTscApplyOnCore+0x36 on the first run:
                 *
                 *     mov ecx, [rbx+600Ch]     ; vpData->...InterceptMisc1
                 *
                 * VpData IS a virtual address already - it is the argument the
                 * hypervisor was handed - so no translation is needed at all. The
                 * physical form bought nothing and cost a bugcheck.
                 *
                 * Honest note on why the physical one was chosen originally: the
                 * unload path (SvDevirtualizeProcessor) DOES need a PA, because it
                 * frees the page by physical address. That is a different caller
                 * reading a different leaf, and the two got conflated.
                 */
                UINT64 vpdVa = reinterpret_cast<UINT64>(VpData);
                registers[3] = static_cast<int>(vpdVa >> 32);
                registers[0] = static_cast<int>(vpdVa & MAXUINT32);
                registers[2] = 'SSV1';           /* ack: the pointer is valid */
                registers[1] = 0;
            }
            else
            {
                registers[0] = registers[1] = registers[2] = registers[3] = 0;
            }
        }
        break;
    case CPUID_UNLOAD_SIMPLE_SVM:
        if (subLeaf == CPUID_UNLOAD_SIMPLE_SVM)
        {
            /*
             * 9.378: THE ACK FOR THIS LEAF WAS NEVER WRITTEN, SO THE UNLOADER
             * HAS NEVER DE-VIRTUALIZED ANYTHING.
             *
             * SvDevirtualizeProcessor does:
             *
             *     __cpuidex(registers, CPUID_UNLOAD_SIMPLE_SVM, ...);
             *     if (registers[2] != 'SSVM')
             *         goto Exit;                  <-- always taken
             *     ... free the VPD ...
             *
             * registers[2] is ECX, which CPUID WRITES ON OUTPUT. This branch
             * never set it, so ECX held whatever the hardware returned for an
             * unimplemented leaf - never 'SSVM'. Every call therefore took the
             * goto, meaning SvDevirtualizeAllProcessors() walked every core and
             * de-virtualized none of them.
             *
             * Consequence, measured: eight machine restarts, one of which was
             * exactly this - 0x1E PRIVILEGED_INSTRUCTION at
             * svm_core_ipi_cleanup+0x5F, a `wrmsr` clearing EFER.SVME on a core
             * that was STILL IN GUEST MODE. The unloader exists precisely to
             * prevent that state, and it had never run.
             *
             * This is the SECOND time this project has been bitten by an ack
             * nobody wrote (the first was CPUID_FAKE_VPD's '1VSS', 9.370). The
             * shape is identical: an assertion about a value that only the
             * assertion mentions.
             */
            attribute.AsUInt16 = VpData->GuestVmcb.StateSaveArea.SsAttrib;
            if (attribute.Fields.Dpl == DPL_SYSTEM)
            {
                UINT64 vpdVa = reinterpret_cast<UINT64>(VpData);
                registers[2] = 'SSVM';          /* ack: the VPD pointer follows */
                registers[3] = static_cast<int>(vpdVa >> 32);
                registers[0] = static_cast<int>(vpdVa & MAXUINT32);
                registers[1] = 0;
                GuestContext->ExitVm = TRUE;
            }
            else
            {
                /* Kernel-mode only, as before: a ring-3 caller learns nothing. */
                registers[0] = registers[1] = registers[2] = registers[3] = 0;
            }
        }
        break;
    default:
        break;
    }

    //
    // Update guest's GPRs with results.
    //
cpuid_writeback:
    GuestContext->VpRegs->Rax = registers[0];
    GuestContext->VpRegs->Rbx = registers[1];
    GuestContext->VpRegs->Rcx = registers[2];
    GuestContext->VpRegs->Rdx = registers[3];

    //
    // Debug prints results. Very important to note that any use of API from
    // the host context is unsafe and absolutely avoided, unless the API is
    // documented to be accessible on IRQL IPI_LEVEL+. This is because
    // interrupts are disabled when host code is running, and IPI is not going
    // to be delivered when it is issued.
    //
    // This code is not exception and violating this rule. The reasons for this
    // code are to demonstrate a bad example, and simply show that the SimpleSvm
    // is functioning for a test purpose.
    //
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL)
    {
        SvDebugPrint("CPUID: %08x-%08x : %08x %08x %08x %08x\n",
                     leaf,
                     subLeaf,
                     registers[0],
                     registers[1],
                     registers[2],
                     registers[3]);
    }

    //
    // Then, advance RIP to "complete" the instruction.
    //
    VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
}

/*!
    @brief          Handles #VMEXIT due to execution of the WRMSR and RDMSR
                    instructions.

    @details        This protects EFER.SVME from being cleared by the guest by
                    injecting #GP when it is about to be cleared. For other MSR
                    access, it passes-through.

    @param[in,out]  VpData - Per processor data.
    @param[in,out]  GuestContext - Guest's GPRs.
 */
_IRQL_requires_same_
static
VOID
SvHandleMsrAccess (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
    ULARGE_INTEGER value;
    UINT32 msr;
    BOOLEAN writeAccess;

    msr = GuestContext->VpRegs->Rcx & MAXUINT32;
    writeAccess = (VpData->GuestVmcb.ControlArea.ExitInfo1 != 0);

    //
    // If IA32_MSR_EFER is accessed for write, we must protect the EFER_SVME bit
    // from being cleared.
    //
    if (msr == IA32_MSR_EFER)
    {
        //
        // #VMEXIT on IA32_MSR_EFER access should only occur on write access.
        //
        NT_ASSERT(writeAccess != FALSE);

        value.LowPart = GuestContext->VpRegs->Rax & MAXUINT32;
        value.HighPart = GuestContext->VpRegs->Rdx & MAXUINT32;
        if ((value.QuadPart & EFER_SVME) == 0)
        {
            /*
             * 9.366 (Tier 7.5, wiring): THIS is a rejected guest input — the
             * guest tried to clear EFER_SVME and we are about to inject #GP and
             * discard the write. Before this line the counter existed and was
             * never incremented, so `sys-sentry` reported escapes=0 and that
             * zero was indistinguishable from "nobody tried".
             *
             * detail = the offending value; a = 1 (write) b = CR/EFER marker.
             * Island-safe: counters only.
             */
            yghv_sentry_escape(YGHV_ESC_MSR_INDEX, IA32_MSR_EFER,
                               value.QuadPart, 1);
            //
            // Inject #GP if the guest attempts to clear the SVME bit. Protection of
            // this bit is required because clearing the bit while guest is running
            // leads to undefined behavior.
            //
            SvInjectGeneralProtectionException(VpData);
            return;
        }

        //
        // Otherwise, update the MSR as requested. Important to note that the value
        // should be checked not to allow any illegal values, and inject #GP as
        // needed. Otherwise, the hypervisor attempts to resume the guest with an
        // illegal EFER and immediately receives #VMEXIT due to VMEXIT_INVALID,
        // which in our case, results in a bug check. See "Extended Feature Enable
        // Register (EFER)" for what values are allowed.
        //
        // This code does not implement the check intentionally, for simplicity.
        //
        VpData->GuestVmcb.StateSaveArea.Efer = value.QuadPart;
    }
    else
    {
        //
        // If the MSR being accessed is not IA32_MSR_EFER, assert that #VMEXIT
        // can only occur on access to MSR outside the ranges controlled with
        // the MSR permissions map. This is true because the map is configured
        // not to intercept any MSR access but IA32_MSR_EFER. See
        // "MSR Ranges Covered by MSRPM" in "MSR Intercepts" for the MSR ranges
        // controlled by the map.
        //
        // Note that VMware Workstation has a bug that access to unimplemented
        // MSRs unconditionally causes #VMEXIT ignoring bits in the MSR
        // permissions map. This can be tested by reading MSR zero, for example.
        //
        NT_ASSERT(((msr > 0x00001fff) && (msr < 0xc0000000)) ||
                  ((msr > 0xc0001fff) && (msr < 0xc0010000)) ||
                   (msr > 0xc0011fff));

        //
        // Execute WRMSR or RDMSR on behalf of the guest. Important that this
        // can cause bug check when the guest tries to access unimplemented MSR
        // *even within the SEH block* because the below WRMSR or RDMSR raises
        // #GP and are not protected by the SEH block (or cannot be protected
        // either as this code run outside the thread stack region Windows
        // requires to proceed SEH). Hypervisors typically handle this by noop-ing
        // WRMSR and returning zero for RDMSR with non-architecturally defined
        // MSRs. Alternatively, one can probe which MSRs should cause #GP prior
        // to installation of a hypervisor and the hypervisor can emulate the
        // results.
        //
        if (writeAccess != FALSE)
        {
            value.LowPart = GuestContext->VpRegs->Rax & MAXUINT32;
            value.HighPart = GuestContext->VpRegs->Rdx & MAXUINT32;
            __writemsr(msr, value.QuadPart);
        }
        else
        {
            value.QuadPart = __readmsr(msr);
            GuestContext->VpRegs->Rax = value.LowPart;
            GuestContext->VpRegs->Rdx = value.HighPart;
        }
    }

    //
    // Then, advance RIP to "complete" the instruction.
    //
    VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
}

/*!
    @brief          Handles #VMEXIT due to execution of the VMRUN instruction.

    @details        This function always injects #GP to the guest.

    @param[in,out]  VpData - Per processor data.
    @param[in,out]  GuestContext - Guest's GPRs.
 */
_IRQL_requires_same_
static
VOID
SvHandleVmrun (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
    UNREFERENCED_PARAMETER(GuestContext);

    /*
     * 9.366 (Tier 7.5): a nested VMRUN is a refusal, and refusals are recorded.
     * detail = the guest's RIP that tried it (nested-VM attempt, not a fault).
     */
    yghv_sentry_escape(YGHV_ESC_VMCB_FIELD, VpData->GuestVmcb.StateSaveArea.Rip,
                       0, 0);

    SvInjectGeneralProtectionException(VpData);
}

/* 9.263 (206-C5b): reconstruct the faulting guest VA for the DENY #PF so the
 * delivered AV reports the TRUE store address instead of VA 0. Self-fetch the
 * instruction bytes (guest PT walk via curCr3/Padding1 + identity-NPT direct
 * map -- the proven CR-write-path pattern), decode the effective address with
 * a TIGHT store-opcode allowlist, and read the GPRs from the PUSHAQ image
 * (VMEXIT does not save R8-R15 into the SSA; GuestRegisters is the asm-saved
 * set, indexed (&R15)[15 - reg]).
 *
 * Any parse doubt -> return 0 and the caller keeps the 9.261 fallback
 * (CR2=0 + ec P=0), hardware-proven deterministic. A WRONG GVA here would be
 * worse than a fallback: it decides WHICH VA Windows resolves the fault at.
 * Island-safe: pure memory arithmetic + the non-paging PT walk. */
/* single guest page-table slot read via the direct map (same as
 * yghv_protect_guest_va_to_pa's yghv_pt_read; duplicated here so the
 * decoder can instrument each level without touching shared code). */
static UINT64 S206ReadGuestPt(UINT64 table_pa, UINT64 index)
{
    UINT64 *va;
    PHYSICAL_ADDRESS pa;
    if (!table_pa)
        return 0;
    pa.QuadPart = (LONGLONG)table_pa;
    va = (UINT64 *)MmGetVirtualForPhysical(pa);
    if (!va)
        return 0;
    return va[index];
}

static UINT32 S206DecodeStoreGva(
    _In_ PVIRTUAL_PROCESSOR_DATA VpData,
    _In_ PGUEST_REGISTERS Regs,
    _Out_ UINT64 *gva,
    _Out_ UINT64 *aux)
{
    /* returns 0 on success, else a reason code (recorded in the deny ring's
     * gva slot as 0xDEAD0000|reason so one run localizes the failure). */
    UINT8 ib[15];
    UINT64 rip = VpData->GuestVmcb.StateSaveArea.Rip;
    UINT64 curCr3 = VpData->HostStackLayout.Padding1;
    UINT64 ea;
    UINT32 pos = 0, rex = 0, i;
    UINT32 segFsGs = 0;   /* 4 = FS base, 5 = GS base, 0 = DS/ES/SS (base 0) */
    UINT8 op, op2 = 0, modrm, mod, rm, sib, scale, idx, base;
    INT64 disp = 0;
    UINT64 baseV = 0, idxV = 0;

    *gva = 0;
    if (curCr3 == 0)
        return 1;                  /* DG_NO_CR3 */
    if ((rip & 0xFFFULL) > 0xFF0ULL)
        return 2;                  /* DG_PAGE_BOUNDARY: would cross page end */

    {
        UINT64 gpa = yghv_protect_guest_va_to_pa(curCr3, rip);
        PHYSICAL_ADDRESS pa;
        PVOID va;
        if (!gpa) {
            /* 9.269: walk failed — classify level (11-14), then run the
             * CONTROL walk: the target's own (cr3, va) pair whose gpa we
             * already know. Control matches the known gpa => the walk
             * machinery and the direct map are fine and curCr3 (Padding1)
             * is the wrong address space (CR3-write emulation drift);
             * control fails => the machinery itself is broken. aux carries
             * the control-walk gpa (0 = control walk failed too). */
            UINT64 *auxOut = aux;
            static const UINT64 PT_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
            UINT64 pml4e, pdpte, pde, pte;
            {
                /* decisive probe: the PML4 page is mapped RAM by definition;
                 * if the PA->VA helper returns NULL for it, MmGetVirtualFor
                 * Physical itself is the broken link (9.250c's unexplained
                 * self-fetch failure, same shape). aux = the PML4 PA. */
                PHYSICAL_ADDRESS pml4pa;
                pml4pa.QuadPart = (LONGLONG)(curCr3 & PT_ADDR_MASK);
                if (!MmGetVirtualForPhysical(pml4pa)) {
                    if (auxOut) *auxOut = curCr3 & PT_ADDR_MASK;
                    return 16;
                }
            }
            pml4e = S206ReadGuestPt(curCr3 & PT_ADDR_MASK, (rip >> 39) & 0x1FF);
            if (!(pml4e & 1)) {
                if (auxOut) *auxOut = pml4e;
                return 11;   /* PML4E not present / read as zero */
            }
            pdpte = S206ReadGuestPt(pml4e & PT_ADDR_MASK, (rip >> 30) & 0x1FF);
            if (!(pdpte & 1)) {
                if (auxOut) *auxOut = pdpte;
                return 12;   /* PDPTE not present / read as zero */
            }
            if (pdpte & (1ULL << 7)) {
                *gva = 0;
                if (auxOut) *auxOut = 0x1B;   /* 1GB page: fetch below */
                (void)0;
            } else {
                pde = S206ReadGuestPt(pdpte & PT_ADDR_MASK, (rip >> 21) & 0x1FF);
                if (!(pde & 1)) {
                    if (auxOut) *auxOut = pde;
                    return 13;   /* PDE not present / read as zero */
                }
                if (pde & (1ULL << 7)) {
                    *gva = 0;
                    if (auxOut) *auxOut = 0x2B;   /* 2MB page: fetch below */
                } else {
                    pte = S206ReadGuestPt(pde & PT_ADDR_MASK, (rip >> 12) & 0x1FF);
                    if (!(pte & 1)) {
                        if (auxOut) *auxOut = pte;
                        return 14;   /* PTE not present / read as zero */
                    }
                }
            }
            /* entry says present but the shared helper returned 0 — run the
             * CONTROL walk against the target's known-good (cr3, va) pair;
             * its result decides machinery-vs-state. */
            {
                UINT64 ctrl = yghv_protect_control_walk_gpa();
                if (auxOut) *auxOut = ctrl;   /* == known gpa => curCr3 wrong */
                UINT64 pte2 = yghv_protect_guest_va_to_pa(curCr3, rip);
                if (!pte2)
                    return 3;      /* DG_WALK_FAIL with control evidence */
                pa.QuadPart = (LONGLONG)pte2;
            }
            va = MmGetVirtualForPhysical(pa);
            if (!va)
                return 4;          /* DG_NO_DIRECTMAP */
            for (i = 0; i < 15; i++)
                ib[i] = ((volatile UINT8 *)va)[i];
        } else {
            pa.QuadPart = (LONGLONG)gpa;
            va = MmGetVirtualForPhysical(pa);
            if (!va)
                return 4;          /* DG_NO_DIRECTMAP */
            for (i = 0; i < 15; i++)
                ib[i] = ((volatile UINT8 *)va)[i];
        }
    }

    /* legacy prefixes; FS/GS overrides add the segment base from the SSA */
    for (;;) {
        UINT8 b = ib[pos];
        if (b == 0x66 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) {
            if (++pos >= 15) return 8;   /* DG_TRUNCATED */
            continue;
        }
        if (b == 0x67)
            return 5;              /* DG_ADDR16: not worth decoding */
        if (b == 0x64 || b == 0x65) {
            segFsGs = (b == 0x64) ? 4 : 5;
            if (++pos >= 15) return 8;   /* DG_TRUNCATED */
            continue;
        }
        break;
    }
    if (ib[pos] >= 0x40 && ib[pos] <= 0x4F) {
        rex = ib[pos];
        if (++pos >= 15) return 8; /* DG_TRUNCATED */
    }
    op = ib[pos++];
    if (pos >= 15) return 8;       /* DG_TRUNCATED */

    if (op == 0x0F) {
        op2 = ib[pos++];
        if (pos >= 15) return 8;   /* DG_TRUNCATED */
        /* TIGHT allowlist of store-capable 0F forms; everything else bails.
           (0F 38/3A take a third opcode byte before modrm -- both kept.) */
        if (op2 == 0x38 || op2 == 0x3A)
            pos++;                 /* movdiri etc: modrm follows */
        else if (op2 != 0xB0 && op2 != 0xB1 && op2 != 0xC0 && op2 != 0xC1 &&
                 op2 != 0xAB && op2 != 0xB3 && op2 != 0xBB && op2 != 0xBA &&
                 op2 != 0xA4 && op2 != 0xA5 && op2 != 0xAC && op2 != 0xAD &&
                 op2 != 0x11 && op2 != 0x7F && op2 != 0x2B && op2 != 0xE7)
            return 6;              /* DG_OPCODE */
    } else if (op != 0x88 && op != 0x89 && op != 0x86 && op != 0x87 &&
               op != 0xC6 && op != 0xC7 && op != 0x80 && op != 0x81 &&
               op != 0x83 && op != 0xF6 && op != 0xF7) {
        return 6;                  /* DG_OPCODE */
    }

    modrm = ib[pos++];
    mod = (modrm >> 6) & 3;
    rm = modrm & 7;
    if (mod == 3)
        return 7;                  /* DG_REG_OPERAND: no store to decode */

    if (rm == 4) {
        /* SIB byte */
        if (pos >= 15) return 8;   /* DG_TRUNCATED */
        sib = ib[pos++];
        scale = (UINT8)(1 << ((sib >> 6) & 3));
        idx = (sib >> 3) & 7;
        base = sib & 7;
        if (idx != 4)
            idxV = (&Regs->R15)[15 - (idx | ((rex & 0x2) ? 8 : 0))] * scale;
        if (base == 5 && mod == 0) {
            if (pos + 4 > 15) return 8;   /* DG_TRUNCATED */
            disp = (INT64)(INT32)(ib[pos] | (ib[pos + 1] << 8) |
                                  (ib[pos + 2] << 16) |
                                  ((UINT32)ib[pos + 3] << 24));
            pos += 4;
        } else {
            baseV = (&Regs->R15)[15 - (base | ((rex & 0x1) ? 8 : 0))];
            if (mod == 1) {
                if (pos >= 15) return 8;   /* DG_TRUNCATED */
                disp = (INT64)(INT8)ib[pos++];
            } else if (mod == 2) {
                if (pos + 4 > 15) return 8;   /* DG_TRUNCATED */
                disp = (INT64)(INT32)(ib[pos] | (ib[pos + 1] << 8) |
                                      (ib[pos + 2] << 16) |
                                      ((UINT32)ib[pos + 3] << 24));
                pos += 4;
            }
        }
        ea = baseV + idxV + (UINT64)disp;
    } else if (mod == 0 && rm == 5) {
        /* 64-bit RIP-relative: EA = next-RIP + disp32. next-RIP needs the
           trailing immediate size, so only known forms pass. */
        UINT32 imm;
        if (pos + 4 > 15) return 8;   /* DG_TRUNCATED */
        disp = (INT64)(INT32)(ib[pos] | (ib[pos + 1] << 8) |
                              (ib[pos + 2] << 16) |
                              ((UINT32)ib[pos + 3] << 24));
        pos += 4;
        if (op == 0xC6) imm = 1;
        else if (op == 0xC7) imm = (rex & 0x8) ? 8 : 4;
        else if (op == 0x80) imm = 1;
        else if (op == 0x81) imm = 4;
        else if (op == 0x83) imm = 1;
        else if (op == 0xF6) imm = 1;
        else if (op == 0xF7) imm = (rex & 0x8) ? 4 : 0;
        else if (op == 0x0F && op2 == 0xBA) imm = 1;
        else return 9;             /* DG_RIPREL_IMM: unknown imm shape */
        ea = rip + pos + imm + (UINT64)disp;
    } else {
        UINT64 breg = rm | ((rex & 0x1) ? 8 : 0);
        baseV = (&Regs->R15)[15 - breg];
        if (mod == 1) {
            if (pos >= 15) return 8;   /* DG_TRUNCATED */
            disp = (INT64)(INT8)ib[pos++];
        } else if (mod == 2) {
            if (pos + 4 > 15) return 8;   /* DG_TRUNCATED */
            disp = (INT64)(INT32)(ib[pos] | (ib[pos + 1] << 8) |
                                  (ib[pos + 2] << 16) |
                                  ((UINT32)ib[pos + 3] << 24));
            pos += 4;
        }
        ea = baseV + (UINT64)disp;
    }

    if (segFsGs)
        ea += (segFsGs == 4) ? VpData->GuestVmcb.StateSaveArea.FsBase
                             : VpData->GuestVmcb.StateSaveArea.GsBase;

    /* canonical sanity: bits 63:48 must sign-extend bit 47 */
    {
        UINT64 top = ea >> 48;
        if (top != 0 && top != 0xFFFF)
            return 10;             /* DG_NONCANONICAL */
    }
    *gva = ea;
    return 0;
}

/*!
    @brief          C-level entry point of the host code called from Sv206LaunchVm.

    @details        This function loads save host state first, and then, handles
                    #VMEXIT which may or may not change guest's state via VpData
                    or GuestRegisters.

                    Interrupts are disabled when this function is called due to
                    the cleared GIF. Not all host state are loaded yet, so do it
                    with the VMLOAD instruction.

                    If the #VMEXIT handler detects a request to unload the
                    hypervisor, this function loads guest state, disables SVM
                    and returns to execution flow where the #VMEXIT triggered.

    @param[in,out]  VpData - Per processor data.
    @param[in,out]  GuestRegisters - Guest's GPRs.

    @result         TRUE when virtualization is terminated; otherwise FALSE.
 */
_IRQL_requires_same_
EXTERN_C
BOOLEAN
NTAPI
SvHandleVmExit (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_REGISTERS GuestRegisters
    )
{
    GUEST_CONTEXT guestContext;
    KIRQL oldIrql;
    UINT64 s521_tsc0 = 0;   /* 9.521: LOCAL, so concurrent cores cannot collide */

    /*
     * 9.441: THE EXIT COUNTER, FIRST STATEMENT, NO BRANCH BEFORE IT.
     *
     * This is the only place in the tree that observes "a #VMEXIT happened".
     * Everything below it is conditional - exit-code switches, policy checks,
     * the ExitVm path - so a counter placed anywhere later would be measuring
     * one of those instead. See the note at g_S206Exits.
     */
    InterlockedIncrement64(&g_S206Exits);

    /*
     * 9.448: AND THE PER-CODE COUNT, HERE, FOR THE SAME REASON.
     *
     * Counting inside each case would have been 35 separate call sites, each
     * able to be forgotten, and a case that falls through would be counted
     * twice. Counting once at the entry - before any switch - makes the table a
     * census of exits, not a tally of which branches happened to be instrumented.
     *
     * Measured reason this matters: the first version instrumented only the 25
     * NEW cases, so the codes the default build actually produces (CPUID, #DB)
     * would have read zero while the total counter read 800 million.
     */
    S206ExitCount(VpData->GuestVmcb.ControlArea.ExitCode);

    /*
     * 9.546: the Rip the guest was FOUND at on its first exit.
     *
     * This is the direct evidence for "where did it actually resume": StateSaveArea
     * .Rip at #VMEXIT is the instruction that caused the exit. If that address is not
     * inside SvVirtualizeProcessor, the guest never resumed there and no amount of
     * early-return wiring could have helped.
     */
    if (InterlockedIncrement(&g_S206GuestFirstExitTaken) == 1)
    {
        InterlockedExchange64(&g_S206GuestFirstExitRip,
                              (LONG64)VpData->GuestVmcb.StateSaveArea.Rip);
    }

    /*
     * 9.521: HANDLER COST, START.
     *
     * MEASURED RESULT: 0.31us per exit (1,119-1,138 TSC ticks over two windows).
     * With 22,700 exits/s across twelve cores that is 0.7% of the machine - so the
     * cost being chased is NOT in here. The pair stays because that number is the
     * thing that redirects every future optimisation effort.
     *
     * The start value is a LOCAL because two cores can run this function
     * concurrently for different VpData. Only the two accumulators are shared.
     */
    s521_tsc0 = __rdtsc();

    guestContext.VpRegs = GuestRegisters;
    guestContext.ExitVm = FALSE;

    /*
     * 9.396: THE STOP FLAG, CHECKED ON EVERY EXIT.
     *
     * This is the only point that reaches the VMRUN loop: the loop calls this
     * handler for every interception, so a flag read here is a flag the loop
     * cannot miss. The control thread cannot reach it any other way (see the note
     * at g_VmrunStopRequested).
     *
     * ExitVm is set rather than a separate "break" flag so the loop unwinds
     * through the EXISTING exit path - the one that does __svm_vmload, __svm_stgi
     * and clears EFER.SVME. A second exit path would be a second thing to get
     * right, and this tree has already paid for that kind of duplication.
     */
    /*
     * 9.414: THE STOP CHECK MOVED INTO THE ASM LOOP (SvLV10).
     *
     * It used to live here, inside the #VMEXIT handler. Measured on 9.410: after
     * all 14 DriverUnload steps reached DONE, a core was still looping, and
     * g_VmrunStopped was 0 - this code had never run once, because the guest is
     * restarted from the same RIP on every iteration and never reaches a CPUID.
     *
     * The check now happens once per loop iteration, before vmload, where the
     * stack invariants allow a call. See the note at SvLV10 in
     * svm_simplevm206_x64.asm - including why the r11/call version (9.413) died
     * with 0x139.
     *
     * Nothing is left here on purpose: a second check would double-count
     * g_VmrunStopped and make the readout mean two different things again.
     */
    (void)0;

    /*
     * 9.442: THE STOP FLAG CANNOT BE TAKEN HERE. ExitVm DOES NOT MEAN "RETURN TO
     * THE HOST" - IT MEANS "HAND CONTROL BACK TO THE GUEST AND TURN SVM OFF".
     *
     * This is the fact that condemns the whole stop-flag family, and it took the
     * exit counter to see it. Read SvLV20 for what it is:
     *
     *     mov rsp, rcx        ; RCX = StateSaveArea.Rsp, i.e. the GUEST's stack
     *     mov ecx, 'SSVM'
     *     jmp rbx             ; RBX = ControlArea.NRip, i.e. the GUEST's next insn
     *
     * So the ExitVm path does not unwind to the caller of Sv206LaunchVm - it
     * RESumes the guest, having cleared EFER.SVME first. Sv206LaunchVm is marked
     * "should never return to here", and it doesn't.
     *
     * For CPUID_UNLOAD that is exactly right: the guest is sitting inside
     * __cpuidex, NRip is the instruction after it, and jumping there completes the
     * instruction so the caller of __cpuidex continues normally - now on bare
     * metal. The register clobbering (RAX/RBX/RCX/RDX) is legal because CPUID
     * declares those clobbered.
     *
     * For a stop flag it is WRONG, and no amount of care in this handler fixes it:
     * the guest is at an ARBITRARY instruction boundary with live registers, and
     * jmp rbx would resume it mid-function with RCX/RBX destroyed and SVM already
     * off. That is silent corruption of whatever Windows thread was running.
     *
     * Measured state that closes the question (9.441's first run):
     *     exits = 213,601   the loops cycle normally; nothing is stuck
     *     vmmcall = 0       the guest is real kernel code, not a trampoline
     *     invmrun = 12      a live snapshot of a healthy 12-core loop
     *
     * So the loops are healthy and the flag has nowhere safe to act. What must
     * change is not this handler but WHO OWNS THE STACK: the only way to leave the
     * loop safely is for the loop to be abandoned deliberately, on a core that is
     * about to execute nothing but our own code. That is a design change, not a
     * patch, and it is recorded in 9.442 of the handoff doc rather than guessed at
     * here.
     */
    (void)0;

    //
    // Load some host state that are not loaded on #VMEXIT.
    //
    __svm_vmload(VpData->HostStackLayout.HostVmcbPa);

    /*
     * 9.368 (B1/B2, Tier 5.1): APPLY THE CLOCK POLICY ON EVERY EXIT.
     *
     * MEASURED FAILURE OF THE FIRST DESIGN: VpData->GuestVmcb.ControlArea is
     * written only in SvPrepareForVirtualization, which runs ONCE per core at
     * load time. So a policy set from the control plane afterwards was never
     * read: G35 showed mode=FROZEN with intercept bit armed=0 and rdtsc exits=0
     * - the guest kept reading hardware and the freeze did nothing while the
     * readout said it was on.
     *
     * Applying it here is where the core OWNS the VMCB: this handler runs ON that
     * core, between its VMRUNs, so no other code can be reading these fields at
     * the same time. An IOCTL writing them from another core would race a live
     * VMCB; this does not.
     *
     * The intercept bits are only SET here, never cleared, and only when the
     * policy asks for something hardware cannot do (frozen time). Clearing them
     * would cost a #VMEXIT to discover there is nothing to do - precisely the
     * cost this level avoids by staying at the default.
     *
     * At the default (offset 0, mode 0) this is two loads and two stores with no
     * branch taken.
     */
    {
        UINT64 off = (UINT64)InterlockedCompareExchange64(&g_TscOffset, 0, 0);
        int   want_armed = (InterlockedCompareExchange64(&g_TscMode, 0, 0) != 0) ? 1 : 0;

        /*
         * 9.369c: THE ARMED STATE IS PER-CORE, SO ASK THE PER-CORE FIELD.
         *
         * 9.369b tracked arming in one global LONG and used it for both edges:
         * "if want and not armed -> set; if not want and armed -> clear". The
         * set is idempotent, but the CLEAR never ran on the cores that had not
         * done the setting: the first core flipped the global to 1, every later
         * core saw `is_armed == 1` and therefore took the "already armed" branch
         * WITHOUT having set its own bits — and after the freeze it skipped the
         * clear for the same reason.
         *
         * Measured on G36 with the global version: mode=normal, exit budget spent,
         * and `intercept bit armed=1` with the exit counter still climbing
         * (608480 -> 718518 in 0.9s). That is the 0x101 condition, reproduced by
         * the very fix meant to prevent it.
         *
         * The intercept bits live in VpData->GuestVmcb.ControlArea, which is THIS
         * core's state, so this core's copy is what decides the edge. The global
         * flag is now only a read-only report to the control plane ("any core has
         * it"), set here and cleared when this core clears its own bits.
         */
        {
            int this_core_armed =
                (VpData->GuestVmcb.ControlArea.InterceptMisc1 & SVM_INTERCEPT_MISC1_RDTSC) ? 1 : 0;

            VpData->GuestVmcb.ControlArea.TscOffset = off;

            /*
             * 9.369e: THE FREEZE IS DISABLED AT THE ARMING SITE.
             *
             * What is proven: TscOffset works, costs no exits and does not change
             * measured intervals (G35 B1 / 9.369.3).
             *
             * What is NOT proven: that the RDTSC intercept can be RELEASED. Three
             * attempts to release it failed in the same way - exits kept climbing
             * after the policy said "not wanted" (1758757 -> 1990504, then
             * 289392 -> 536980, then 132478 -> ... with `armed=1` throughout).
             * Two of those runs cost the machine: one hang, one
             * 0x101 CLOCK_WATCHDOG_TIMEOUT.
             *
             * The measurement that settles it: `cores that disarmed themselves=0`
             * while the handler was demonstrably running (exits climbing). So the
             * clear branch never fired even though its condition read true from
             * here - i.e. what this handler writes into
             * VpData->GuestVmcb.ControlArea is not what the processor consults for
             * the next VMRUN. Until that is understood, arming is refused, because
             * an intercept that cannot be provably released starves a core and
             * takes the machine down silently.
             *
             * A freeze request is therefore accepted but inert; yghv_s206_tsc_freeze
             * reports it, and `intercept bit armed` stays 0 to say so.
             */
            if (0 /* arming disabled: see 9.369e and 9.369h */)
            {
                VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_RDTSC;
                VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_RDTSCP;
                VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                InterlockedExchange(&g_TscInterceptArmed, 1);
            }
            else if (!want_armed)
            {
                /* 9.369f: clear unconditionally when the policy wants none. The
                 * previous version cleared only if `this_core_armed`, and the
                 * GLOBAL report flag stayed 1 forever once any core had set it -
                 * so the readout claimed "armed" on a machine where nothing was.
                 * A sticky report is worse than no report: it hides the next real
                 * arming behind a stale 1. */
                if (this_core_armed)
                {
                    VpData->GuestVmcb.ControlArea.InterceptMisc1 &= ~((UINT32)SVM_INTERCEPT_MISC1_RDTSC);
                    VpData->GuestVmcb.ControlArea.InterceptMisc2 &= ~((UINT32)SVM_INTERCEPT_MISC2_RDTSCP);
                    VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                }
                InterlockedExchange(&g_TscInterceptArmed, 0);
            }
        }
        goto tsc_policy_done;
    }
tsc_policy_done:;


    NT_ASSERT(VpData->HostStackLayout.Reserved1 == MAXUINT64);

    //
    // Raise the IRQL to the DISPATCH_LEVEL level. This has no actual effect since
    // interrupts are disabled at #VMEXI but warrants bug check when some of
    // kernel API that are not usable on this context is called with Driver
    // Verifier. This protects developers from accidentally writing such #VMEXIT
    // handling code. This should actually raise IRQL to HIGH_LEVEL to represent
    // this running context better, but our Logger code is not designed to run at
    // that level unfortunately. Finally, note that this API is a thin wrapper
    // of mov-to-CR8 on x64 and safe to call on this context.
    //
    oldIrql = KeGetCurrentIrql();
    if (oldIrql < DISPATCH_LEVEL)
    {
        KeRaiseIrqlToDpcLevel();
    }

    //
    // Guest's RAX is overwritten by the host's value on #VMEXIT and saved in
    // the VMCB instead. Reflect the guest RAX to the context.
    //
    GuestRegisters->Rax = VpData->GuestVmcb.StateSaveArea.Rax;

    //
    // Update the _KTRAP_FRAME structure values in hypervisor stack, so that
    // Windbg can reconstruct call stack of the guest during debug session.
    // This is optional but very useful thing to do for debugging.
    //
    VpData->HostStackLayout.TrapFrame.Rsp = VpData->GuestVmcb.StateSaveArea.Rsp;
    VpData->HostStackLayout.TrapFrame.Rip = VpData->GuestVmcb.ControlArea.NRip;

    //
    // Handle #VMEXIT according with its reason.
    //
    /*
     * 7.4 (9.343): measure our own exit latency.
     *
     * rdtsc here is the same instruction a guest uses to time us, so the number
     * these counters keep is exactly what a detection loop would read. Worst
     * case is what matters: a single slow path in the handler is a signal, and
     * sys-sentry reports it before anyone else notices.
     */
    yghv_sentry_exit_begin(__rdtsc());

    switch (VpData->GuestVmcb.ControlArea.ExitCode)
    {
    case VMEXIT_CPUID:
        SvHandleCpuid(VpData, &guestContext);
        break;
    case VMEXIT_MSR:
        SvHandleMsrAccess(VpData, &guestContext);
        break;
    case VMEXIT_VMRUN:
        SvHandleVmrun(VpData, &guestContext);
        break;
    case VMEXIT_NPF:
        //
        // 9.249 step206-C2: nested page fault. With NCr3 = YuanGuardHV's
        // g_npt, a protected page (armed: W cleared) write lands here. The
        // verdict MUST be the lock-free bare path — we are at DISPATCH with
        // GIF=0; ExAcquireFastMutex would bugcheck/deadlock (9.229). Verdicts:
        //   ALLOW (target/ring0 write, auto-disarm) -> reopen page W,
        //     set TF so the #DB after the retried write re-arms (NX-back),
        //     advance RIP to NRip (the write retries with W now open);
        //   DENY  (foreign write) -> inject #PF with the NPT error code;
        //   NONE  (protection off / unknown GPA) -> inject #PF too (the page
        //     really is not writable in the NPT; the guest must see the fault).
        // TLB: after flipping a NPT PTE, set TlbControl=1 so the next VMRUN
        // flushes (the vendored VMCB has no per-ASID INVLPGA path of ours).
        //
        {
            UINT64 gpa = VpData->GuestVmcb.ControlArea.ExitInfo2;
            UINT64 rearmGpa = 0;
            int flip = 0;
            BOOLEAN writeFault =
                (VpData->GuestVmcb.ControlArea.ExitInfo1 & 0x2ULL) != 0;
            // 9.250: current guest CR3 = per-VCPU slot maintained by the
            // CR3-write exit (Padding1). StateSaveArea.Cr3 is the STALE
            // prepare-time value (VMEXIT does not save CR3) — using it made
            // every target match fail -> DENY.
            UINT64 curCr3 = VpData->HostStackLayout.Padding1;

            /* 9.252: NPF-entry telemetry — record EVERY write-fault NPF that
             * reaches this handler (cr3/gpa/cpl), island-memory only. The AV
             * mystery needs this: DENY telemetry was empty, so either the NPF
             * never reached the handler (NPT TLB staleness?) or it exited
             * through the NONE path (find_page miss). Both now leave evidence. */
            /*
             * 9.448c: ATOMIC RESERVATION, same shape as the deny / vr rings.
             * `g_S206NpfIdx < 32` then write then `++` lets two cores share a
             * slot and advance the index twice; the write is idempotent but the
             * index is not.
             */
            if (writeFault) {
                ULONG slot = (ULONG)InterlockedIncrement(
                    (volatile LONG *)&g_S206NpfIdx) - 1;

                if (slot < 32) {
                    g_S206NpfLog[slot * 3] = curCr3;
                    g_S206NpfLog[slot * 3 + 1] = gpa;
                    g_S206NpfLog[slot * 3 + 2] =
                        VpData->GuestVmcb.StateSaveArea.Cpl;
                }
            }

            //
            // Tier 4.6 (2026-10-17): watchpoint telemetry.
            //
            // A watched page raises #NPF exactly like an armed one, so the
            // same exit serves both. Record (CR3, RIP, GPA, kind, CPL) into a
            // preallocated ring — island-safe: stores only, no calls, no
            // allocation, no locks. The ring is drained later from PASSIVE by
            // the IOCTL that reads it.
            //
            // This is the debug-register superset: unlimited watchpoints, no
            // code bytes modified (so no int3 to detect), and DR0-DR3 stay
            // untouched (a target reading them sees nothing).
            //
            {
                UINT32 watchKind = writeFault ? 0x2u : 0x1u;
                if (yghv_watch_match(gpa, watchKind)) {
                    yghv_watch_record(curCr3,
                                      VpData->GuestVmcb.StateSaveArea.Rip,
                                      gpa,
                                      watchKind,
                                      (UINT32)VpData->GuestVmcb.StateSaveArea.Cpl);
                }
            }

            /* 9.282 (C6 v3): cpl=0 NPFs on ARMED pages must NEVER inject #PF
             * (kernel-mode injection = 0x50 bugcheck — the 206c6f crash was
             * the WPM readback's NtReadVirtualMemory memcpy hitting the fake
             * NCr3 state and taking the reflect branch: READ faults on armed
             * pages reach here too and the old code reflected them). Route
             * cpl=0 read faults through the fake path: NCr3=alt makes the
             * read return the scratch content (the writer's own bytes — a
             * perfect fake-success illusion for readbacks), TF fires #DB and
             * the handler switches NCr3 back to main. cpl=3 read faults and
             * unknown pages keep the reflect path (guest handles user #PFs). */
            if (!writeFault &&
                VpData->GuestVmcb.StateSaveArea.Cpl == 0 &&
                yghv_protect_find_page_bare(gpa) != NULL &&
                yghv_protect_fake_bare(gpa, &rearmGpa)) {
                UINT64 altPa = yghv_protect_fake_alt_pa();
                /* 9.297 (A'): same runtime selector as the write path. */
                {
                    int use_invlpga = 1;
                    int tlb_ctl = yghv_protect_fake_tlb_plan(&use_invlpga);
                    VpData->GuestVmcb.ControlArea.TlbControl = (UINT8)tlb_ctl;
                }
                if (altPa) {
                    VpData->GuestVmcb.ControlArea.NCr3 = altPa;
                    /* 9.297 (A'): the write path got VmcbClean=0 in 9.285
                     * ("the bare write was cache-swallowed") but the read
                     * path never did. Without it the NCr3 switch can be
                     * swallowed here, so the readback never sees the
                     * scratch copy. Mirrored from the write path. */
                    VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                }
                VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = gpa | 1ULL;
                VpData->GuestVmcb.StateSaveArea.Rflags |= 0x100ULL; /* TF */
                break;
            }

            /*
             * 9.336 (Tier 7.1): a guest access to a page we HID must not be
             * restored.
             *
             * Hiding works by removing the leaf entry from the guest identity
             * map (npt_exclude_pa). The page then raises #NPF on any access —
             * which is exactly the breakpoint shape, so the ALLOW loop below
             * would happily restore it and let the guest read our tables. That
             * was the hole: 7.1's hiding depended on the fault handler agreeing
             * to keep it hidden.
             *
             * So this check runs FIRST and answers with an access violation at
             * the null page (deterministic, unresolvable — the same shape the
             * DENY path uses). The guest sees a hole in physical memory, which
             * from its side is what a reserved range looks like anyway.
             */
            if (yghv_guard_is_hidden(gpa)) {
                /*
                 * 9.366 (Tier 7.5): a GUEST reaching for a page we hid is the
                 * single most interesting escape attempt this hypervisor can
                 * observe — it is either an inventory comparison looking for
                 * holes or a detector that already suspects us.
                 *
                 * It is recorded on the guest branch only (not on our own
                 * served access below): our own reads of a hidden table happen
                 * on every fault the ALLOW loop serves, and counting those as
                 * "someone tried to escape us" would make the number mean
                 * nothing. `detail = GPA, a = guest RIP, b = CPL`.
                 */
                /*
                 * 9.344: OUR OWN access must be served.
                 *
                 * 9.320.1 established that hiding a page the driver reads with
                 * plain C removes it from the driver too — the 13th crash. The
                 * answer is 4.4's ALLOW loop with the match condition changed
                 * from "registered RIP" to "RIP inside our image": restore the
                 * entry, let the instruction run under TF, and let #DB re-hide.
                 *
                 * The first version of this used yghv_protect_npt_remap_flags(),
                 * which starts with `if (KeAreAllApcsDisabled()) return 0;`. An
                 * NPF handler runs at DISPATCH with GIF=0, which IS "APCs
                 * disabled", so every call returned 0: the page was never
                 * restored, the decoy was never installed, and the fall-through
                 * access violation was unresolvable -> 0x3B (the 15th crash).
                 * The *_bare helpers below are the island-safe form, the same
                 * discipline as yghv_protect_arm_page_bare().
                 */
                if (yghv_guard_hiding_armed() &&
                    yghv_guard_fault_is_ours(
                        VpData->GuestVmcb.StateSaveArea.Rip,
                        (UINT32)VpData->GuestVmcb.StateSaveArea.Cpl)) {
                    if (yghv_guard_remap_bare(gpa)) {
                        VpData->GuestVmcb.ControlArea.GuestPaOfGhcb =
                            (gpa & ~(UINT64)0xFFFULL) | 4ULL;  /* bit2 = re-hide */
                        VpData->GuestVmcb.StateSaveArea.Rflags |= 0x100ULL; /* TF */
                        VpData->GuestVmcb.ControlArea.TlbControl = 1;
                        break;
                    }
                }
                /*
                 * 9.366 (Tier 7.5): WE GOT HERE, SO THE ACCESS IS NOT OURS.
                 *
                 * This is the one escape that matters most: a guest reaching for
                 * a page we hid is either an inventory comparison looking for
                 * holes or a detector that already suspects us. Recorded before
                 * the two guest-serving branches so a successful decoy read is
                 * counted as the attempt it is — a decoy that answers cleanly is
                 * exactly what makes the attempt worth knowing about.
                 *
                 * `detail = GPA, a = guest RIP, b = CPL`.
                 */
                yghv_sentry_escape(YGHV_ESC_GPA_GUARD, gpa,
                                   VpData->GuestVmcb.StateSaveArea.Rip,
                                   (UINT64)VpData->GuestVmcb.StateSaveArea.Cpl);
                /*
                 * Not ours: serve the DECOY rather than a hole.
                 *
                 * 9.320.5 noted that clearing the entry makes the address space
                 * discontinuous, which an inventory comparison can see. Pointing
                 * the leaf at a zero frame keeps the physical range present: the
                 * guest reads zeros (what reserved memory looks like) and its
                 * writes land in the decoy and are discarded.
                 */
                if (yghv_guard_hiding_armed() && yghv_guard_decoy_bare(gpa)) {
                    VpData->GuestVmcb.ControlArea.TlbControl = 1;
                    break;
                }
                VpData->GuestVmcb.StateSaveArea.Cr2 = 0;
                VpData->GuestVmcb.ControlArea.EventInj =
                    (1ULL << 31) | (3ULL << 8) | (1ULL << 11) |
                    0x0EULL | (0x6ULL << 32);   /* P=0,W=1,U=1 */
                break;
            }

            /*
             * 9.330: breakpoint/watch check for BOTH fault kinds.
             *
             * This used to live inside `if (writeFault)`. A read-watch removes
             * PRESENT, so writes fault too — and because the check was skipped
             * for non-write faults, those went to the "reflect as #PF" path and
             * the guest retried forever (measured: 30-second hang on the first
             * mmf-write after `mem-watch add ... r`).
             *
             * Handled here for read and write alike: record, restore the page
             * so the access can proceed, single-step via TF, and let the #DB
             * path re-arm.
             */
            {
                UINT32 bpKind = writeFault ? 0x2u : 0x1u;
                UINT64 bpRipAny = VpData->GuestVmcb.StateSaveArea.Rip;

                if (yghv_bp_on_npf(gpa, bpRipAny, bpKind,
                                   (UINT32)VpData->GuestVmcb.StateSaveArea.Cpl,
                                   curCr3)) {
                    if (yghv_guard_reopen_bare(gpa & ~(UINT64)0xFFFULL)) {
                        VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = gpa;
                        VpData->GuestVmcb.StateSaveArea.Rflags |= 0x100ULL; /* TF */
                        VpData->GuestVmcb.ControlArea.TlbControl = 1;
                        break;      /* out of the NPF case */
                    }
                    /* Could not reopen: deliver an access violation rather
                     * than spin. */
                    VpData->GuestVmcb.StateSaveArea.Cr2 = 0;
                    VpData->GuestVmcb.ControlArea.EventInj =
                        (1ULL << 31) | (3ULL << 8) | (1ULL << 11) |
                        0x0EULL | (0x6ULL << 32);
                    break;
                }
            }

            if (writeFault)
            {
                int vr;

                /*
                 * Tier 4.4 (2026-10-17): instruction-level breakpoints.
                 *
                 * A breakpoint page has its write permission revoked, so a
                 * guest write raises #NPF exactly like an armed page. The
                 * breakpoint is distinguished by (gpa, rip) rather than by
                 * membership in g_protect — which is why this check must come
                 * BEFORE yghv_protect_on_npf_write_bare(): that function only
                 * knows armed pages and would return YGHV_NPF_NONE, sending
                 * us down the inject-#PF path that froze the box in 9.313.
                 *
                 * On a match we take the ALLOW path ourselves, complete with
                 * the permission restore and single-step that 4.6 was missing:
                 *
                 *   1. record the hit (island-safe)
                 *   2. RESTORE the write permission, so the retried store lands
                 *   3. set TF, so #DB fires after the store
                 *   4. the #DB path re-arms (revokes again)
                 *
                 * Without step 2 the guest can never write the page and the
                 * machine spins in #NPF. That is the whole lesson of 9.313.
                 */
                /* 9.329: same reason as the watch path — NRIP is RIP+1 for an
                 * exception VMEXIT, so it can never be the instruction address
                 * a breakpoint was armed on. */
                UINT64 bpRip = VpData->GuestVmcb.StateSaveArea.Rip;
                if (yghv_bp_on_npf(gpa, bpRip, 0x2,
                                   (UINT32)VpData->GuestVmcb.StateSaveArea.Cpl,
                                   curCr3)) {
                    if (!yghv_guard_reopen_bare(gpa & ~(UINT64)0xFFFULL)) {
                        /* Could not reopen: refuse rather than spin. */
                                                 /*
                          * Could not reopen the page. Refusing to spin matters
                          * more than the breakpoint: deliver an access
                          * violation at the null page (ec P=0,W=1,U=1), the
                          * same deterministic shape the DENY path uses, so the
                          * guest catches it instead of retrying forever.
                          */
                         VpData->GuestVmcb.StateSaveArea.Cr2 = 0;
                         VpData->GuestVmcb.ControlArea.EventInj =
                             (1ULL << 31) | (3ULL << 8) | (1ULL << 11) |
                             0x0EULL | (0x6ULL << 32);
                        break;
                    }
                    rearmGpa = gpa;
                     /*
                      * 9.317 FIX: the #DB handler reads its re-arm target from
                      * GuestPaOfGhcb, NOT from the local rearmGpa. Without this
                      * the slot is 0 (or stale), the re-arm path is handed a null
                      * gpa, and the box bugchecks 0x3B walking a zero page.
                      * Measured: 0x3B / 0xC0000005 at 21:59:21.
                      */
                     VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = gpa;
                    flip = 1;
                    VpData->GuestVmcb.StateSaveArea.Rflags |=
                        0x100ULL; /* TF — the #DB re-arms */
                    break;
                }

                vr = yghv_protect_on_npf_write_bare(
                    curCr3,
                    VpData->GuestVmcb.StateSaveArea.Cpl,
                    gpa, &rearmGpa, &flip);

                /* 9.255: verdict telemetry — (gpa, vr | TF<<3). Island-safe
                 * memory only; flushed at unload. */
                /*
                 * 9.448c: ATOMIC RESERVATION, same defect as the deny ring.
                 *
                 * Check-then-write-then-increment is not atomic: two cores can
                 * reserve the same slot pair, and both then increment, so the
                 * index advances by 4 for 2 slots written - and g_S206VrIdx is
                 * exactly the value the crash dump showed being used as an array
                 * index (yghv_pa+0x2501b, reading g_S206DenyLog).
                 */
                {
                    ULONG slot = (ULONG)InterlockedAdd(
                        (volatile LONG *)&g_S206VrIdx, 2) - 2;

                    if (slot + 2 <= 64) {
                        g_S206VrLog[slot] = gpa;
                        g_S206VrLog[slot + 1] =
                            (UINT64)vr |
                            ((VpData->GuestVmcb.StateSaveArea.Rflags &
                              0x100ULL) << 3);
                    }
                }

                if (vr == 1 /* YGHV_NPF_ALLOW */ ||
                    vr == 3 /* YGHV_NPF_FAKE (C6 v4): the cpp switches this
                    core's NCr3 to the alt NPT below and the WHOLE copy runs
                    shadowed until the next CR3 write (context switch) */)
                {
                    /* 9.297 (A'): pick the TLB strategy at runtime. plan()
                     * returns the TlbControl value and whether to INVLPGA.
                     * Default (mode 0) reproduces the pre-9.297 behaviour
                     * exactly: TlbControl=1 + INVLPGA. */
                    int use_invlpga = 1;
                    int tlb_ctl = yghv_protect_fake_tlb_plan(&use_invlpga);
                    VpData->GuestVmcb.ControlArea.TlbControl = (UINT8)tlb_ctl;
                    /* 9.297 (A'): mode 4 — reopen the armed page in the MAIN
                     * npt and rely purely on TlbControl=2 (FLUSH ALL). If the
                     * store lands, FLUSH_ALL does reach the NPT TLB and the
                     * alt-NPT design is unnecessary; if it still spins, the
                     * NPT TLB ignores FLUSH_ALL and the limit is architectural. */
                    if (yghv_protect_fake_tlb_get() == 4) {
                        yghv_protect_fake_reopen_main(gpa);
                        VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = 0;
                        g_S206LastProtectHit = gpa;
                        break;
                    }
                    if (vr == 3 /* YGHV_NPF_FAKE */) {
                        /* C6 v4: switch this core's NCr3 to the alt NPT — a
                         * DIFFERENT nCr3 value architecturally flushes the
                         * NPT TLB, so the armed page's scratch mapping is
                         * visible and the copy's stores land in the shadow.
                         * 9.285: VmcbClean=0 forces the CPU to reload the
                         * field (the bare write was cache-swallowed). */
                        /* 9.289 (C6.1): INVLPGA the armed pages' known
                         * GVAs first -- 9.290 concluded Zen3 does not flush
                         * NPT translations on TlbControl or nCr3 change.
                         * 9.297 (A'): that conclusion is under review — INVLPGA
                         * invalidates GVA->GPA (guest page tables), NOT the
                         * nested GPA->HPA layer that caches this page. The
                         * selector lets us test TlbControl=2 (FLUSH ALL) and
                         * the no-INVLPGA variants without a rebuild. */
                        if (use_invlpga)
                            yghv_protect_fake_invpga_armeds(
                                VpData->GuestVmcb.ControlArea.GuestAsid);
                        UINT64 altPa = yghv_protect_fake_alt_pa();
                        if (altPa) {
                            VpData->GuestVmcb.ControlArea.NCr3 = altPa;
                            VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                        }
                        /* 9.285 (C6 v4): NO TF — the window closes at the
                         * next CR3 write (context switch), not per store;
                         * the whole copy runs shadowed on alt. */
                        VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = 0;
                        g_S206LastProtectHit = gpa;
                        break;
                    }
                    VpData->GuestVmcb.StateSaveArea.Rflags |= 0x100ULL; /* TF */
                    // 9.254: KEEP the saved RIP. For an NPF exit the trapped
                    // write did NOT execute and saved RIP points AT it; the
                    // page is reopened writable so the write re-executes on
                    // resume. The old RIP=NRip SKIPPED the protected write
                    // entirely (silent value loss).
                    // per-VCPU re-arm slot (repurposed GHCB GPA field):
                    VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = rearmGpa;
                    g_S206LastProtectHit = gpa;
                    break;
                }
                if (vr == 2 /* YGHV_NPF_DENY */)
                {
                    UINT64 ec;
                    // 9.261 (206-C4a run): an NPF is a VMEXIT, not an
                    // exception — the CPU never writes CR2 on the way out, so
                    // the #PF injected below used to carry whatever stale CR2
                    // this core's last REAL #PF left (per-core, drifting).
                    // Windows resolves the fault at the CR2 VA, not at the
                    // store: a resolvable stale VA = soft-fault + IRET =
                    // RETRY of the foreign store (the c12 spurious-retry
                    // mechanism, DENY branch this time). c16 measured it: one
                    // attacker store -> 8+ DENYs (ring saturated) -> the retry
                    // slipped through an ALLOW reopen window and LANDED
                    // instead of taking the AV. c10/c11's clean AVs were luck
                    // (stale CR2 happened to point at an unresolvable VA).
                    // Force a deterministic user AV: CR2 = the never-mapped
                    // null page with a self-consistent ec (P=0, W=1, U=1) —
                    // no resolve path can succeed there, so Windows delivers
                    // STATUS_ACCESS_VIOLATION at the faulting RIP every time.
                    // (Accurate GVA reconstruction via DecodeAssist is a
                    // later refinement; the catch semantics do not need it.)
                    /* 9.263 (206-C5b): decode the true faulting GVA and
                     * deliver the #PF at it (ec P=1, the standard
                     * EPT-protector shape: a user write to a page the guest
                     * PTE believes is writable is an access violation, and
                     * c4a proved only the WRONG-CR2 case retries). Decode
                     * failure keeps the 9.261 hardware-proven fallback
                     * (CR2=0, ec P=0: demand-fault on the never-mapped null
                     * page). The deny ring's 4th slot records the GVA
                     * (0 = decode fell back) so one run validates both the
                     * decoder and the delivery pairing. */
                    UINT64 gva = 0;
                    UINT64 dAux = 0;
                    UINT32 dReason =
                        S206DecodeStoreGva(VpData, GuestRegisters, &gva,
                                           &dAux);
                    BOOLEAN haveGva = (dReason == 0);
                    UINT64 e1 = VpData->GuestVmcb.ControlArea.ExitInfo1;
                    VpData->GuestVmcb.StateSaveArea.Cr2 = haveGva ? gva : 0;
                    ec = 0;
                    if (!haveGva) {
                        ec = 0x2 | 0x4;                    /* P=0, W=1, U=1 */
                    } else {
                        if (e1 & 0x1) ec |= 1;             /* P=1 (present) */
                        if (e1 & 0x2) ec |= 2;             /* W */
                        if (e1 & 0x4) ec |= 4;             /* U */
                    }
                    VpData->GuestVmcb.ControlArea.EventInj =
                        (1ULL << 31) | (3ULL << 8) | (1ULL << 11) |
                        0x0EULL | (ec << 32);
                    g_S206LastProtectHit = gpa;
                    /* 9.251: verdict-input telemetry (pure memory, island-safe;
                     * yghv DriverUnload flushes it to progress.log after
                     * Sv206CoopUnload brings the cores back to bare metal).
                     * 9.255: stride fix, no-wrap records. 9.263: 4 slots per
                     * entry, +GVA (0 = decode fell back). */
                    /* 9.267: 6 slots/entry. gva = decoded GVA, or
                     * 0xDEAD0000|reason on fallback (11-14 = walk failed at
                     * PML4E/PDPTE/PDE/PTE); rip = faulting instruction;
                     * aux = raw failing PT entry (or large-page tag). */
                    /*
                     * 9.448c: RESERVE THE SLOT ATOMICALLY, THEN FILL IT.
                     *
                     * The old code checked `g_S206DenyIdx + 6 <= 192`, wrote six
                     * slots, and only THEN did `g_S206DenyIdx += 6`. Two cores
                     * taking an NPF at the same time both passed the check with
                     * the same index, both wrote the same six slots (last writer
                     * wins), and both incremented - so the index advanced by 12
                     * for six slots of data, and the ring silently overflowed its
                     * own arithmetic.
                     *
                     * Measured consequence (the crash this fix is for): 0x1E
                     * C0000005 at yghv_pa+0x2501b, reading g_S206DenyLog with an
                     * index taken from g_S206VrIdx. Diagnostic habits blamed the
                     * reader; the writer was the one that could corrupt the index
                     * for a store that never happened.
                     *
                     * Now the RESERVATION is the atomic step: one core gets a
                     * private, already-bounded range or none at all. Fill happens
                     * inside that range, so no two cores can share slots.
                     */
                    {
                        ULONG slot = (ULONG)InterlockedAdd(
                            (volatile LONG *)&g_S206DenyIdx, 6) - 6;

                        if (slot + 6 <= 192) {
                            g_S206DenyLog[slot] = curCr3;
                            g_S206DenyLog[slot + 1] = gpa;
                            g_S206DenyLog[slot + 2] =
                                VpData->GuestVmcb.StateSaveArea.Cpl;
                            g_S206DenyLog[slot + 3] =
                                haveGva ? gva : (0xDEAD0000ULL | dReason);
                            g_S206DenyLog[slot + 4] =
                                VpData->GuestVmcb.StateSaveArea.Rip;
                            g_S206DenyLog[slot + 5] = dAux;
                        }
                        /*
                         * Over the bound, the reservation is still consumed: the
                         * index must not be rolled back, or two cores that both
                         * overflowed would fight over the same tail slot forever.
                         * An over-counted index is a readable, bounded fact; a
                         * torn one is a crash.
                         */
                    }
                    break;
                }
                /* 9.258 (206-C3): NONE + write = unknown/removed armed page
                 * (stale TLB after watchdog disarm, or a benign race). Do NOT
                 * inject #PF here — the guest PTE is writable, so Windows sees
                 * a spurious fault, retries, and loops forever (the c12 victim
                 * storm: DENY->#PF->retry->NPF->...). Silently reopen the page
                 * writable and let the write re-execute (RIP kept); TlbControl
                 * heals this core immediately and every CR3-write exit (which
                 * already sets TlbControl=1) propagates to the others. */
                yghv_protect_reopen_page_bare(gpa);
                VpData->GuestVmcb.ControlArea.TlbControl = 1;
                break;
            }
            // protection off / read fault / unknown page: reflect as #PF
            {
                UINT64 ec = 0;
                if (VpData->GuestVmcb.ControlArea.ExitInfo1 & 0x1ULL) ec |= 1;
                if (VpData->GuestVmcb.ControlArea.ExitInfo1 & 0x2ULL) ec |= 2;
                if (VpData->GuestVmcb.ControlArea.ExitInfo1 & 0x4ULL) ec |= 4;
                VpData->GuestVmcb.ControlArea.EventInj =
                    (1ULL << 31) | (3ULL << 8) | (1ULL << 11) |
                    0x0EULL | (ec << 32);
            }
        }
        break;
    case VMEXIT_CR3_WRITE /* 0x0013 — APM: CR writes are 0x10+index */:
    case VMEXIT_CR4_WRITE /* 0x0014 — intercepted alongside CR3 (see prepare):
                              KiSwapContext's mov cr4/cr3/cr4 PCIDE dance must
                              run entirely in-emulation */:
        //
        // 9.250b: intercept CR3 writes so the protection verdict knows the
        // CURRENT guest address space. The write did not execute (intercepted):
        // emulate it — StateSaveArea.Cr3 = new value (VMRUN loads it on resume),
        // per-VCPU current-cr3 slot, TLB flush (we break the native CR3-write
        // flush by intercepting; ASID-flush is the conservative equivalent),
        // RIP = NRip (valid for CR-access exits).
        //
        // The new CR3 value is NOT in ExitInfo2 (the frozen dump shows 0) and
        // DecodeAssist does NOT fill GuestInstructionBytes for CR exits either
        // (frozen dump: NumOfBytesFetched=0). Self-fetch the instruction: walk
        // the guest page tables (current CR3 = Padding1, set by earlier CR3
        // writes... chicken-and-egg for the FIRST one — prepare seeds Padding1
        // with the prepare-time CR3, which IS correct for the first window)
        // via yghv_protect_guest_va_to_pa, then read through the identity NPT.
        // Decode [REX] 0F 22 /r. Any failure = bugcheck with the state, NEVER a
        // garbage CR3 (that triple-faults instantly, the 12:34 failure mode).
        //
        {
            /* Which control register is being written: ExitCode 0x10+idx.
               (APM: CR-write exits are 0x10+CR-index; ExitInfo1's
               "general1" bits 3:0 echo the CR index too.) */
            UINT32 crIdx = (UINT32)(VpData->GuestVmcb.ControlArea.ExitCode & 0xF);
            UINT64 newCr = 0;
            UINT32 rex = 0, rm = 0, ok = 0;
            UINT64 rip = VpData->GuestVmcb.StateSaveArea.Rip;
            UINT64 curCr3 = VpData->HostStackLayout.Padding1;
            UINT8  ib[8] = { 0 };
            UINT8 *inst = NULL;

            /* self-fetch: guest VA -> GPA (guest PT walk) -> SPA (identity NPT)
               -> VA (direct map). Falls back to DecodeAssist bytes if present. */
            UINT8 n = VpData->GuestVmcb.ControlArea.NumOfBytesFetched;
            if (n > 0) {
                for (UINT8 q = 0; q < 8 && q < n; q++)
                    ib[q] = VpData->GuestVmcb.ControlArea.GuestInstructionBytes[q];
                inst = ib;
            } else if (curCr3 != 0) {
                UINT64 gpa = yghv_protect_guest_va_to_pa(curCr3, rip);
                if (gpa) {
                    PHYSICAL_ADDRESS pa;
                    pa.QuadPart = (LONGLONG)gpa;
                    PVOID va = MmGetVirtualForPhysical(pa);
                    if (va) {
                        for (UINT8 q = 0; q < 8; q++)
                            ib[q] = ((volatile UINT8 *)va)[q];
                        inst = ib;
                    }
                }
            }

            if (inst && inst[0] == 0x0F && inst[1] == 0x22) {
                rm = inst[2] & 7;                        /* MOV cr3, rm */
                ok = 1;
            } else if (inst && (inst[0] & 0xF0) == 0x40 &&
                       inst[1] == 0x0F && inst[2] == 0x22) {
                rex = inst[0] & 0xF;
                rm = (inst[3] & 7) | ((rex & 1) << 3);   /* REX.B extends rm */
                ok = 1;
            }
            if (ok && rm == 4) ok = 0;      /* RSP never sources a CR3 write */
            if (ok) {
                newCr = (&guestContext.VpRegs->R15)[15 - rm];
            }
            if (!ok) {
                /* PRIMARY path (13:28 crash fix): EXITINFO1 bits[7:6] ALWAYS
                 * carry the source GPR number for a CR write (hardware-filled,
                 * APM "LMSW and MOV to/from CR"). The 0x20631 bugcheck proved
                 * DecodeAssist/self-fetch can both miss (bit63=1: the task-gate
                 * / extended form zeroes bit4 and keeps bits[7:6]=0 = RAX), so
                 * trust the hardware field. bit63 additionally means "GPR is
                 * one of R8-R15" — adjust the PUSHAQ index (R8..R15 sit
                 * FIRST in GUEST_REGS). */
                UINT64 e1 = VpData->GuestVmcb.ControlArea.ExitInfo1;
                UINT32 gpr = (UINT32)((e1 >> 6) & 3ULL);
                if (e1 & 0x8000000000000000ULL) {
                    /* extended register: bits[9:8] give R8..R15 */
                    gpr = 8 + (UINT32)((e1 >> 8) & 3ULL);
                    newCr = (&guestContext.VpRegs->R15)[15 - gpr];
                } else {
                    /* gpr: 0=RAX 1=RCX 2=RDX 3=RBX (RSP/RBP are illegal here) */
                    static UINT8 const regIdx[4] = { 15, 14, 13, 12 };
                    newCr = (&guestContext.VpRegs->R15)[regIdx[gpr]];
                }
            }
            if (crIdx == 3) {
                /* CR3 write: update VMCB state (VMRUN loads it) + the
                   protection-verdict's current-CR3 slot */
                VpData->GuestVmcb.StateSaveArea.Cr3 = newCr;
                VpData->HostStackLayout.Padding1 = newCr;
                /* 9.289 (C6 v4): a context switch ends the fake window --
                 * if this core is on the alt NPT, switch back to main. The
                 * switched-out thread's shadow writes are simply discarded,
                 * and the incoming thread sees the real mappings. */
                if (yghv_protect_fake_alt_active() &&
                    VpData->GuestVmcb.ControlArea.NCr3 ==
                        yghv_protect_fake_alt_pa()) {
                    /* 9.289 (C6.1): drop the alt's stale scratch
                     * translations for the armed pages before switching. */
                    yghv_protect_fake_invpga_armeds(
                        VpData->GuestVmcb.ControlArea.GuestAsid);
                    VpData->GuestVmcb.ControlArea.NCr3 =
                        yghv_protect_fake_main_pa();
                    VpData->GuestVmcb.ControlArea.TlbControl = 1;
                }
            } else if (crIdx == 4) {
                /* CR4 write: update VMCB state (VMRUN loads it). Native TLB
                   flush semantics are replaced by TlbControl=1 below. */
                VpData->GuestVmcb.StateSaveArea.Cr4 = newCr;
            } else {
                /*
                 * 9.366 (Tier 7.5): an unexpected CR index is a VMCB field the
                 * guest reached that we do not emulate. Recorded BEFORE the
                 * bugcheck decision, because the degradation path may choose to
                 * survive rather than die and the attempt is evidence either
                 * way. `detail = CR index, a = RIP, b = the value attempted`.
                 */
                yghv_sentry_escape(YGHV_ESC_CR_VALUE, (UINT64)crIdx, rip, newCr);
                /* unexpected CR index (we only intercept 3 and 4) */
                S206RecordFatal(VpData->GuestVmcb.ControlArea.ExitCode,
                                rip, newCr);
                extern int yghv_fail_degrade(int reason, uint64_t a,
                                             uint64_t b, uint64_t c);
                if (!yghv_fail_degrade(1 /* BAD_CR */,
                                       VpData->GuestVmcb.ControlArea.ExitCode,
                                       rip, newCr)) {
                    S206RecordFatal(VpData->GuestVmcb.ControlArea.ExitCode,
                                    rip, newCr);
                    KeBugCheckEx(0xE2, 0x20602,
                                 VpData->GuestVmcb.ControlArea.ExitCode,
                                 rip, newCr);
                }
            }
            VpData->GuestVmcb.ControlArea.TlbControl = 1;
            VpData->GuestVmcb.StateSaveArea.Rip =
                VpData->GuestVmcb.ControlArea.NRip;
        }
        break;
    case VMEXIT_EXCEPTION_BP:
        //
        // 9.327 (Tier 4.4, INT3 mode): the guest executed a 0xCC we placed.
        //
        // The saved RIP points AT the 0xCC (a fault, not a trap), so the
        // original instruction is the one about to run. The loop mirrors the
        // NPF ALLOW path:
        //   1. record (CR3, RIP, VA, CPL) - island-safe
        //   2. restore the original byte we displaced
        //   3. set TF so #DB fires after the real instruction executes
        //   4. remember the VA in GuestPaOfGhcb with bit1 set, so the #DB
        //      handler re-patches instead of re-arming an NPT page
        // bit0 of that slot already means "fake-write shadow" (9.275), so
        // INT3 uses bit1 and the two cannot collide.
        //
        {
            UINT64 bpVa = VpData->GuestVmcb.StateSaveArea.Rip;
            UINT8 origByte = 0;

            if (yghv_bp_on_int3(bpVa,
                                (UINT32)VpData->GuestVmcb.StateSaveArea.Cpl,
                                VpData->HostStackLayout.Padding1) &&
                yghv_bp_int3_orig(bpVa, &origByte)) {
                UINT64 bpGpa = yghv_protect_guest_va_to_pa(
                                   VpData->HostStackLayout.Padding1, bpVa);
                /*
                 * 9.373: NO MEMORY WRITE HERE. THE LEAF IS FLIPPED INSTEAD.
                 *
                 * The old code called yghv_pa_write(bpGpa, &origByte, 1), which
                 * builds an MDL and maps it - a call that may block, executed
                 * inside a #BP exit at DISPATCH with GIF=0. One firing was
                 * survivable; the same 0xCC at 20 Hz produced 0x139
                 * MISSING_GSFRAME_STACKPTR and a reboot.
                 *
                 * Now the page's NPT leaf is pointed at the shadow frame, which
                 * already carries the ORIGINAL byte at this offset. The
                 * instruction fetches real bytes and executes, so the breakpoint
                 * keeps its semantics; #DB points the leaf back. Both are 8-byte
                 * table writes.
                 *
                 * If there is no shadow for this page (arm refused, or the copy
                 * failed), the switch fails and the #BP is REFLECTED to the guest
                 * rather than silently pretending the instruction ran.
                 */
                if (bpGpa && yghv_bp_shadow_switch(bpGpa, 1)) {
                    VpData->GuestVmcb.StateSaveArea.Rflags |= 0x100ULL; /* TF */
                    VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = bpVa | 2ULL;
                    VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                    VpData->GuestVmcb.ControlArea.TlbControl = 1;
                    break;
                }
                /* No shadow: fall through to the reflect branch below, so the
                 * guest still sees its own #BP semantics instead of a silently
                 * skipped instruction. */
            }

            /* No INT3 breakpoint here: this #BP is the guest's own.
             * Reflect it as a real #BP so Windows raises the usual exception. */
            VpData->GuestVmcb.ControlArea.EventInj =
                (1ULL << 31) | (3ULL << 8) | (1ULL << 11) | 0x03ULL;
        }
        break;
    case VMEXIT_EXCEPTION_DB:
        //
        // 9.249: after an ALLOWed protected write, we set TF; the #DB fires on
        // the instruction AFTER the write — re-arm the page (W->NX) here, clear
        // TF, and resume at NRip (the #DB is a trap: it already advanced).
        // Lock-free: same bare discipline as the NPF path.
        //
        {
            /* 9.255: #DB telemetry — (rip, rearmSlot, rflags, DR6) BEFORE
             * anything consumes the slot; proves whether the ALLOW aftermath
             * reached here and with which RIP/TF state. 9.258: DR6 added
             * (BS vs B0-B3 bits identify the #DB source). */
            /* 9.448c: atomic reservation — see the deny ring note. */
            {
                ULONG slot = (ULONG)InterlockedAdd(
                    (volatile LONG *)&g_S206DbgIdx, 4) - 4;

                if (slot + 4 <= 128) {
                    g_S206DbgLog[slot] =
                        VpData->GuestVmcb.StateSaveArea.Rip;
                    g_S206DbgLog[slot + 1] =
                        VpData->GuestVmcb.ControlArea.GuestPaOfGhcb;
                    g_S206DbgLog[slot + 2] =
                        VpData->GuestVmcb.StateSaveArea.Rflags;
                    g_S206DbgLog[slot + 3] =
                        VpData->GuestVmcb.StateSaveArea.Dr6;
                }
            }
            VpData->GuestVmcb.ControlArea.TlbControl = 1;
            if (VpData->GuestVmcb.ControlArea.GuestPaOfGhcb != 0) {
                UINT64 slot = VpData->GuestVmcb.ControlArea.GuestPaOfGhcb;
                /* 9.275: bit0 set = fake-write shadow (C6) — restore the
                 * saved real NPT entry instead of plain re-arm. */
                if (slot & 1ULL) {
                    /* C6 v2: switch NCr3 back to main (different value =
                     * architectural NPT TLB flush) before resuming.
                     * 9.285: VmcbClean=0 (see the FAKE branch). */
                    VpData->GuestVmcb.ControlArea.NCr3 =
                        yghv_protect_fake_main_pa();
                    VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                    VpData->GuestVmcb.ControlArea.TlbControl = 1;
                    yghv_protect_fake_restore(slot & ~1ULL);
                } else if (slot & 4ULL) {
                    /*
                    /*
                     * 9.350 FIX: re-hide with the SAME mechanism that hid it.
                     *
                     * 9.348 used yghv_guard_decoy_bare() unconditionally. But
                     * hidecore hides with UNMAP, so re-hiding with DECOY left
                     * the page present-but-zeroed: every read of g_bp saw an
                     * empty table, mem-bp add() wrote into a discarded decoy and
                     * reported OK, no breakpoint ever matched, and the guest
                     * store spun forever inside this handler. mmf-write never
                     * returned while the driver kept answering — a guest-side
                     * spin, not a deadlock.
                     *
                     * yghv_guard_rehide_bare() reads the entry's recorded
                     * hide_mode and applies the matching mechanism.
                     */
                    if (yghv_guard_hiding_armed()) {
                        (void)yghv_guard_rehide_bare(slot & ~7ULL);
                    }
                } else if (slot & 2ULL) {
                    /*
                     * 9.327 (INT3 mode): the #BP handler restored the real
                     * instruction and set TF; the instruction has now run, so
                     * put the 0xCC back for the next visit. bit1 marks this
                     * slot, so it cannot be confused with an NPT page (whose
                     * slot is a plain GPA with bit0 clear).
                     */
                    UINT64 int3Va = slot & ~3ULL;
                    UINT64 int3Gpa = yghv_protect_guest_va_to_pa(
                                         VpData->HostStackLayout.Padding1,
                                         int3Va);
                    /*
                     * 9.373: PUT THE LEAF BACK, DO NOT REWRITE THE BYTE.
                     *
                     * This is the second of the two MDL writes on the INT3 hot
                     * path (the first is in the #BP branch). Both had to go: they
                     * map a page to write one byte, and mapping may block, from
                     * inside an exit at DISPATCH with GIF=0.
                     *
                     * Pointing the leaf back at the real frame restores the page
                     * exactly - the 0xCC is still there, because the shadow copy
                     * was the thing that had it removed, not the guest page. So
                     * the breakpoint is armed again for the next visit with no
                     * byte written anywhere.
                     */
                    if (int3Gpa) {
                        (void)yghv_bp_shadow_switch(int3Gpa, 0);
                    }
                    VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                    VpData->GuestVmcb.ControlArea.TlbControl = 1;
                } else {
                    yghv_protect_arm_page_bare(slot);
                    /*
                     * Tier 4.4: if the page is not in g_protect it may be an
                     * instruction-level breakpoint - those live in mem_bp's own
                     * table and need their own re-arm, or the page stays
                     * writable after the first hit.
                     */
                    if (yghv_protect_find_page_bare(slot & ~1ULL) == NULL)
                        yghv_bp_rearm_bare(slot & ~1ULL);
                }
                VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = 0;
            }
            VpData->GuestVmcb.StateSaveArea.Rflags &= ~0x100ULL; /* clear TF */
            // 9.254: do NOT touch RIP. A TF-induced #DB is a TRAP — the saved
            // RIP already points at the next instruction. APM: NRIP for
            // exception VMEXITs = RIP+1 (ONE BYTE), so the old RIP=NRip
            // resumed mid-instruction -> corrupted guest stream -> the stable
            // AccessViolationException both selftest rounds died on.
        }
        break;
    case VMEXIT_RDTSC:
    case VMEXIT_RDTSCP:
        /*
         * 9.368 (B2, Tier 5.1): the guest asked what time it is.
         *
         * With TscOffset alone the hardware answers and this case never runs.
         * It runs only when the policy wanted something hardware cannot do —
         * a frozen clock — so the exit is the PRICE of that capability, and the
         * counter below is what tells a test whether the price was paid.
         *
         * HALF ISLAND-SAFE, and the unsafe half is a deliberate exception:
         * __rdtsc() is the `rdtsc` instruction itself (not a call), so reading
         * the hardware here is legal at DISPATCH with GIF=0. What is NOT legal is
         * anything that can raise IRQL or take a lock; there is none.
         *
         * RDTSC returns EDX:EAX on the SAME core, so the value handed back is a
         * real TSC reading plus whatever the policy wants added. Returning a
         * fabricated number without a hardware read would make two cores
         * disagree — a disagreement is exactly what a multi-core TSC comparison
         * looks for, so it would defeat the purpose.
         */
        {
            UINT64 real = __rdtsc();
            UINT64 visible;

            /*
             * 9.369 (B2 RE-DO): THE READER IS CLASSIFIED BEFORE ANY HOLDING HAPPENS.
             *
             * The first version froze every intercepted read. The guest here is
             * Windows, whose scheduler and timers run on TSC, so stopping the
             * clock stopped the OS and the machine hung with no dump at all.
             *
             * The verdict is therefore: freeze ONLY when this read comes from the
             * CR3 we were asked to watch. Everything else - including the kernel's
             * own timekeeping - gets the real value plus the offset, which is what
             * hardware would have produced.
             *
             * The deadline is checked on every read against the REAL TSC, so a
             * freeze cannot outlive max_ms no matter who forgets to thaw it.
             */
            if (InterlockedCompareExchange64(&g_TscMode, 0, 0) != 0)
            {
                LONG64 expires = InterlockedCompareExchange64(&g_TscFreezeExpiresAt, 0, 0);
                UINT64 scope = (UINT64)InterlockedCompareExchange64(&g_TscScopeCr3, 0, 0);

                /* Expired? Thaw from here on. Measured on the REAL clock: a
                 * deadline compared against the frozen value never advances. */
                if (expires && (LONG64)real >= expires)
                {
                    InterlockedExchange64(&g_TscMode, 0);
                    InterlockedExchange64(&g_TscScopeCr3, 0);
                    InterlockedExchange64(&g_TscFreezeExpiresAt, 0);
                    InterlockedIncrement64(&g_TscExpired);
                    visible = real + (UINT64)InterlockedCompareExchange64(&g_TscOffset, 0, 0);
                }
                else if (scope &&
                         (VpData->HostStackLayout.Padding1 & ~0xFFFULL) == scope)
                {
                    /*
                     * The watched address space: hold the clock - unless the
                     * budget is spent. A detector may legally sit in a tight
                     * rdtsc loop; charging it an exit each iteration would starve
                     * this core and produce 0x101 instead of stealth.
                     */
                    if (InterlockedDecrement64(&g_TscBudgetLeft) > 0)
                    {
                        visible = (UINT64)InterlockedCompareExchange64(&g_TscFrozenAt, 0, 0);
                        InterlockedIncrement64(&g_TscSuppressed);
                    }
                    else
                    {
                        /* Out of budget: end the freeze here, exactly as the
                         * deadline would have. */
                        InterlockedExchange64(&g_TscMode, 0);
                        InterlockedExchange64(&g_TscScopeCr3, 0);
                        InterlockedExchange64(&g_TscFreezeExpiresAt, 0);
                        InterlockedIncrement64(&g_TscBudgetHits);
                        visible = real + (UINT64)InterlockedCompareExchange64(&g_TscOffset, 0, 0);
                    }
                }
                else
                {
                    /* Everyone else - the OS included - keeps its own clock. */
                    InterlockedIncrement64(&g_TscScopedOut);
                    visible = real + (UINT64)InterlockedCompareExchange64(&g_TscOffset, 0, 0);
                }
            }
            else
            {
                /* Not frozen: return hardware + offset, which is exactly what the
                 * hardware would have produced on its own. */
                visible = real + (UINT64)InterlockedCompareExchange64(&g_TscOffset, 0, 0);
            }

            InterlockedIncrement64(&g_TscIntercepts);

            /*
             * 9.369d: A CORE THAT IS STILL ARMING ITSELF MUST DISARM ITSELF.
             *
             * The per-exit block above clears this core's bits when the policy
             * says "not wanted" - but it only runs on a core that reaches
             * SvHandleVmExit, and a core whose RDTSC intercept is armed will keep
             * reaching it, so in principle it converges. Measured: it did not.
             * `armed=1` persisted and exits went 1758757 -> 1990504 in 0.9s.
             *
             * Reason: the policy check happens at the TOP of the handler, before
             * this case runs, and the VMCB control area it edits is only re-read
             * by the processor at the next VMRUN. A core that armed itself and
             * then experiences only RDTSC exits never takes the "clear" edge,
             * because by the time it runs, `want_armed` is already 0 and its own
             * bits are still set - which IS the clear edge. That should work...
             * so the remaining possibility is that writing the control area here
             * does not survive the VMRUN without VmcbClean.
             *
             * Rather than reason further about cache semantics, this belt-and-
             * braces path does the one thing that is definitely safe: when a core
             * handles an RDTSC exit while the policy wants none, it disarms itself
             * HERE, in the same handler, with VmcbClean=0. The worst case for the
             * exit storm is then bounded by how long it takes the policy to read
             * 0 - i.e. by the freeze deadline or the exit budget, both of which
             * already exist.
             */
            if (InterlockedCompareExchange64(&g_TscMode, 0, 0) == 0)
            {
                if (VpData->GuestVmcb.ControlArea.InterceptMisc1 & SVM_INTERCEPT_MISC1_RDTSC)
                {
                    VpData->GuestVmcb.ControlArea.InterceptMisc1 &= ~((UINT32)SVM_INTERCEPT_MISC1_RDTSC);
                    VpData->GuestVmcb.ControlArea.InterceptMisc2 &= ~((UINT32)SVM_INTERCEPT_MISC2_RDTSCP);
                    VpData->GuestVmcb.ControlArea.VmcbClean = 0;
                    VpData->GuestVmcb.ControlArea.TlbControl = 1;
                    InterlockedExchange(&g_TscInterceptArmed, 0);
                    InterlockedIncrement64(&g_TscSelfDisarmed);
                }
            }

            GuestRegisters->Rax = visible & 0xFFFFFFFFULL;
            GuestRegisters->Rdx = (visible >> 32) & 0xFFFFFFFFULL;

            /* RDTSCP additionally writes TSC_AUX to ECX. Passing the real value
             * through keeps the AUX contract (it identifies the core) intact —
             * fabricating it is a separate decision and not part of B2. */
            if (VpData->GuestVmcb.ControlArea.ExitCode == VMEXIT_RDTSCP)
                GuestRegisters->Rcx = 0;

            VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
        }
        break;
    /*
     * 9.443: THE STOP PROTOCOL, AT THE ONE PLACE IT IS SAFE.
     *
     * The guest is Sv206GuestStub (see the ASM), a loop of `pause x4 / mov rax,1 /
     * vmmcall`. Every iteration therefore lands here, and this case is where the
     * decision is made:
     *
     *   stop not requested  ->  count the tick, return FALSE, SvLV10 runs the
     *                           guest again. The stub loops.
     *   stop requested      ->  set ExitVm. SvLV20 then does
     *                           `mov rsp,rcx; mov ecx,'SSVM'; jmp rbx` with
     *                           RBX = NRip, which is INSIDE OUR STUB. The guest
     *                           continues at Sv206GuestTick with EFER.SVME already
     *                           cleared, parks on the halt loop, and this core is
     *                           done being a hypervisor.
     *
     * WHY THIS IS SAFE HERE AND WAS NOT SAFE FROM AN ARBITRARY EXIT (9.442):
     * SvLV20 consumes RCX and RBX to resume the guest. When the guest is our stub,
     * the registers it destroys are the stub's own - nothing of Windows is live,
     * because the stub never called into anything. 9.442's objection applied to
     * resuming an arbitrary kernel thread mid-function, which is exactly what the
     * old caller-continuation guest was.
     *
     * g_VmrunStopped is incremented here so the unload waiter has a number that
     * actually moves. It counts CORES THAT HAVE LEFT, not ticks.
     */
    case VMEXIT_VMMCALL /* 0x81 */:
        InterlockedIncrement64(&g_S206Vmmcalls);

        /*
         * 9.465: ADVANCE RIP - and this is the bug that made every "slow the stub
         * down" attempt look like it did nothing.
         *
         * Measured shape: the stub was `mov ecx,1000000 / pause... / vmmcall /
         * jmp`, vmmcall stayed at ~12.6 MILLION per second no matter how large
         * the counter was, and 100000 vs 1000000 pauses changed the rate by
         * nothing at all. The reason is this case: it counted the exit and broke,
         * WITHOUT writing Rip = NRip. So VMRUN re-entered the guest at the SAME
         * `vmmcall` instruction every time. The counter and the whole pause loop
         * ahead of it were never executed - not once, after the first pass.
         *
         * That also explains why 9.462/9.463/9.464 all measured ~12-14 million
         * exits regardless of what the stub did: they were measuring a loop that
         * ran a single instruction, so every change to the surrounding code was
         * invisible.
         *
         * The pattern is the same one this tree already documented for every
         * other boundary exit (the 9.448 block above advances RIP for exactly
         * this reason): an intercept that does not advance RIP turns the guest
         * into an infinite repetition of the intercepted instruction.
         */
        VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;

        if (Sv206VmrunShouldExit() != 0)
        {
            guestContext.ExitVm = TRUE;
        }
        break;
    case VMEXIT_SHUTDOWN /* 0x7f */:
        //
        // 9.250c: the guest triple-faulted (SHUTDOWN state). With SHUTDOWN
        // intercepted this becomes an OBSERVABLE bug check carrying the last
        // guest RIP — never the silent instant reset.
        //
        S206RecordFatal(VMEXIT_SHUTDOWN,
                        VpData->GuestVmcb.StateSaveArea.Rip,
                        VpData->HostStackLayout.Padding1);
        extern int yghv_fail_degrade(int reason, uint64_t a,
                                     uint64_t b, uint64_t c);
        (void)yghv_fail_degrade(0 /* informational */, VMEXIT_SHUTDOWN,
                                VpData->GuestVmcb.StateSaveArea.Rip,
                                VpData->HostStackLayout.Padding1);
        break;

    /*
     * 9.448: THE OTHER 23 EXIT CODES vmexit.c HANDLES, SO THE 206 MAIN LINE
     * COVERS THE SAME SET.
     *
     * Before this block the 206 handler had 12 cases and vmexit.c had 35; the
     * planning docs said "35 exit codes implemented" and that was true only of
     * the old resident path. This block closes the gap.
     *
     * TWO KINDS OF CASE, AND ONLY TWO
     *
     * 1. INSTRUCTION-BOUNDARY EXITS. These are "the guest executed an instruction
     *    we chose to watch, nothing about it needs interpreting": HLT, PAUSE,
     *    INVD, INVLPG, INVLPGA, WBINVD, MONITOR, MWAIT, MWAIT_COND, XSETBV,
     *    IRET, SWINT, RSM, PUSHF, POPF, TASKSR, FERR_FREEZE, RDPMC, VMLOAD,
     *    VMSAVE, STGI, CLGI, SKINIT, ICEBP. The handling is identical: count it,
     *    advance RIP past the instruction (otherwise the guest re-executes it
     *    forever - the exact 9.416 failure shape), continue the loop.
     *
     * 2. STATE-CHANGING EXITS. INTR and NMI are not instruction boundaries: the
     *    processor has already stored the interrupt in ExitIntInfo and expects a
     *    re-injection or an STGI. Concluding "boundary, advance RIP" for them
     *    would silently DROP the interrupt - Windows would stop seeing timer and
     *    device interrupts. They are counted and re-injected as-is.
     *
     * WHAT IS NOT HERE, AND WHY IT IS HONEST TO SAY SO
     *
     * IOIO (0x7B) needs an IOPM (I/O permission bitmap) that this tree never
     * builds, so arming it would fault every port access with no emulation
     * behind it. CR0/CR3/CR4/CR8 READ and WRITE are already handled
     * (VMEXIT_CR3_WRITE / VMEXIT_CR4_WRITE above). EXCEPTION_DF/GP/NP/SS/UD
     * require exception interception, which 9.244 deliberately keeps OFF in the
     * 206 baseline (guests' benign #GP probes would become 0xE2 within minutes).
     * Those are documented as not-armed rather than stubbed into "worked".
     */
    /*
     * 9.448f: THE FOUR INSTRUCTIONS THAT ARE NOT "NOTHING HAPPENED".
     *
     * Third reboot, and the first two diagnoses were both incomplete. VmcbClean
     * (9.448e) was a real bug and is fixed. But the preset still rebooted, because
     * these four are SIDE-EFFECTING:
     *
     *     INVLPG   0x79   invalidate one TLB entry
     *     INVLPGA  0x7a   invalidate one TLB entry by ASID
     *     INVD     0x76   invalidate the whole cache (no writeback)
     *     WBINVD   0x89   write back and invalidate the whole cache
     *
     * The generic "boundary" handler advanced RIP and continued - which DROPS the
     * invalidation. The guest then believes its TLB is fresh and keeps using a
     * stale translation; a stale translation in a hypervisor that also rewrites
     * NPT entries is not a corner case, it is memory corruption with a delay fuse.
     * That is how a machine dies with 0x1E and no dump: the fault is somewhere
     * else entirely, seconds later.
     *
     * The correct handling is not "skip" - it is to actually perform the flush on
     * the guest's behalf. TlbControl is the architectural way to do that from the
     * VMCB: writing 1 makes the next VMRUN flush this core's guest TLB, which is
     * exactly what INVLPG/INVLPGA asked for. INVD/WBINVD have no VMCB equivalent;
     * what they mean for us is "the guest will stop trusting its caches", and the
     * honest response is to flush our TLB too and let the hardware caches be -
     * this tree never relies on cache coherence for correctness, only on TLB
     * state, so flushing the TLB is the part that matters.
     */
    case VMEXIT_INVLPG /* 0x79 */:
    case VMEXIT_INVLPGA /* 0x7a */:
    case VMEXIT_INVD /* 0x76 */:
    case VMEXIT_WBINVD /* 0x89 */:
        VpData->GuestVmcb.ControlArea.TlbControl = 1;
        VpData->GuestVmcb.ControlArea.VmcbClean = 0;
        VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
        break;

    case VMEXIT_HLT /* 0x78 */:
    case VMEXIT_PAUSE /* 0x77 */:
    case VMEXIT_MONITOR /* 0x8a */:
    case VMEXIT_MWAIT /* 0x8b */:
    case VMEXIT_MWAIT_CONDITIONAL /* 0x8c */:
    case VMEXIT_XSETBV /* 0x8d */:
    case VMEXIT_IRET /* 0x74 */:
    case VMEXIT_SWINT /* 0x75 */:
    case VMEXIT_RSM /* 0x73 */:
    case VMEXIT_PUSHF /* 0x70 */:
    case VMEXIT_POPF /* 0x71 */:
    case VMEXIT_TASK_SWITCH /* 0x7d */:
    case VMEXIT_FERR_FREEZE /* 0x7e */:
    case VMEXIT_RDPMC /* 0x6f */:
    case VMEXIT_VMLOAD /* 0x82 */:
    case VMEXIT_VMSAVE /* 0x83 */:
    case VMEXIT_STGI /* 0x84 */:
    case VMEXIT_CLGI /* 0x85 */:
    case VMEXIT_SKINIT /* 0x86 */:
    case VMEXIT_ICEBP /* 0x88 */:
        /*
         * Advance RIP past the instruction. Without it the guest re-executes the
         * same instruction immediately and the loop becomes an exit storm -
         * measured once already as the 9.437 START_PENDING hang.
         *
         * The exit is already counted at the entry to this handler (9.448), so
         * there is no counter here: one call site, not thirty-five.
         */
        VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
        break;

    case VMEXIT_INTR /* 0x60 */:
    case VMEXIT_NMI /* 0x61 */:
        /*
         * NOT an instruction boundary. The event is pending in ExitIntInfo and
         * the guest must receive it; advancing RIP here would drop it.
         *
         * 9.448 re-injects it verbatim: same vector, same type, same error code.
         * That is the pass-through a hypervisor owes a guest it is not trying to
         * modify. Dropping these would freeze Windows - timer and device
         * interrupts would simply stop arriving.
         */
        {
            UINT64 info = VpData->GuestVmcb.ControlArea.ExitIntInfo;

            /*
             * Re-inject only if the hardware says an event is valid, and only
             * when the VMCB's injection slot is free. Writing EventInj while a
             * previous event is still marked valid would corrupt it.
             */
            if ((info & (1ULL << 31)) != 0 &&
                VpData->GuestVmcb.ControlArea.EventInj == 0)
            {
                VpData->GuestVmcb.ControlArea.EventInj = info;
            }
        }
        break;
    default:
        // 9.250c: an unhandled exit code must be OBSERVABLE (bugcheck with the
        // exit info) — a plain KeBugCheck here is indistinguishable from the
        // silent-reset failure mode. The 12:12 crash (0xE2 zero-params) was
        // exactly this path with ExitCode=0x13.
        S206RecordFatal(VpData->GuestVmcb.ControlArea.ExitCode,
                        VpData->GuestVmcb.StateSaveArea.Rip,
                        VpData->GuestVmcb.ControlArea.ExitInfo1);
            extern int yghv_fail_degrade(int reason, uint64_t a,
                                         uint64_t b, uint64_t c);
            if (!yghv_fail_degrade(0 /* UNKNOWN_EXIT */,
                                   VpData->GuestVmcb.ControlArea.ExitCode,
                                   VpData->GuestVmcb.StateSaveArea.Rip,
                                   VpData->GuestVmcb.ControlArea.ExitInfo1)) {
                KeBugCheckEx(0xE2, 0x20600,
                             VpData->GuestVmcb.ControlArea.ExitCode,
                             VpData->GuestVmcb.StateSaveArea.Rip,
                             VpData->GuestVmcb.ControlArea.ExitInfo1);
            }
    }

    //
    // Again, no effect to change IRQL but restoring it here since a #VMEXIT
    // handler where the developers most likely call the kernel API inadvertently
    // is already executed.
    //
    yghv_sentry_exit_end(__rdtsc());

    if (oldIrql < DISPATCH_LEVEL)
    {
        KeLowerIrql(oldIrql);
    }

    //
    // Terminate the SimpleSvm hypervisor if requested.
    //
    if (guestContext.ExitVm != FALSE)
    {
        NT_ASSERT(VpData->GuestVmcb.ControlArea.ExitCode == VMEXIT_CPUID);

        //
        // Set return values of CPUID instruction as follows:
        //  RBX     = An address to return
        //  RCX     = A stack pointer to restore
        //  EDX:EAX = An address of per processor data to be freed by the caller
        //
        guestContext.VpRegs->Rax = reinterpret_cast<UINT64>(VpData) & MAXUINT32;
        guestContext.VpRegs->Rbx = VpData->GuestVmcb.ControlArea.NRip;
        /*
         * 9.383: THE ACK GOES HERE, AND THIS IS THE UNIFIED ROOT CAUSE.
         *
         * SvDevirtualizeProcessor does:
         *     __cpuidex(registers, CPUID_UNLOAD_SIMPLE_SVM, ...);
         *     if (registers[2] != 'SSVM') goto Exit;
         * and registers[2] is ECX. The value it will actually see is THIS line -
         * RCX is overwritten right here with the guest's RSP. So the check failed
         * every single time and SvDevirtualizeAllProcessors de-virtualized NOTHING.
         *
         * THAT ONE FACT EXPLAINS BOTH CRASH FAMILIES:
         *
         *   0x1E PRIVILEGED_INSTRUCTION at svm_core_ipi_cleanup+0x5F
         *     cores were still in guest mode, and the unload IPI cleared
         *     EFER.SVME there - undefined per the AMD APM. Fixed by 9.380, which
         *     removed that write because the ExitVm path below already clears
         *     SVME in the one legal window.
         *
         *   0xD1 at IRQL 14 on the two adjacent NPT pages 0x194bfd000/0x194bfe000
         *     with the cores STILL VIRTUALIZED, DriverUnload went on to
         *     npt_cleanup() and freed the very tables those cores were running
         *     with as NCr3. The next guest instruction caused an #NPF, the walk
         *     read freed memory, and it faulted inside the exit handler.
         *
         * So the two fixes are one fix plus its precondition: 9.380 removed a write
         * that should not happen; this one makes the de-virtualizer actually run so
         * that npt_cleanup is no longer reached with live cores.
         *
         * RSP is not lost: SvDevirtualizeProcessor only reads EAX:EDX for the VPD
         * address. The resume state that the ORIGINAL caller (the guest itself,
         * continuing after CPUID) would need is kept in RBX (NRip) and is not read
         * by the unloader.
         */
        /*
         * 9.441: 9.383's `Rcx = 'SSVM'` WAS A STACK-POINTER BUG. REVERTED.
         *
         * 9.383 replaced `Rcx = StateSaveArea.Rsp` with the literal 'SSVM' so that
         * SvDevirtualizeProcessor's `registers[2] != 'SSVM'` check would pass. The
         * check does need ECX, and 9.383 correctly noticed that this line was
         * overwriting it - but it "fixed" that by putting a magic number into the
         * register that SvLV20 uses as a STACK POINTER.
         *
         * The ASM, in order (svm_simplevm206_x64.asm):
         *
         *     test al, al
         *     POPAQ                  ; <- pop rcx restores Rcx from GuestRegisters
         *     jnz SvLV20
         *   SvLV20:
         *     mov rsp, rcx           ; <- RSP = 0x4D565353
         *     mov ecx, 'SSVM'        ; <- and THEN ECX gets the magic value
         *     jmp rbx
         *
         * POPAQ runs BEFORE the branch, so by the time `mov rsp, rcx` executes,
         * RCX is exactly what this line wrote. 0x4D565353 is not a canonical
         * address on x64 (bit 47 disagrees with bits 48-63), so the very next
         * stack operation - and `jmp rbx` into kernel code that will touch the
         * stack immediately - raises #GP.
         *
         * AND THE LINE WAS REDUNDANT: `mov ecx, 'SSVM'` in SvLV20 already writes
         * the ack after restoring RSP. The CPUID leaf itself also sets
         * registers[2] = 'SSVM' (9.378, see the note above), so
         * SvDevirtualizeProcessor's check is satisfied twice over without this.
         *
         * So the original assignment is restored: the guest's own RSP, which is
         * what SvLV20 has always documented it expects ("RCX = An original stack
         * pointer to restore"). 9.383's comment above ("RSP is not lost") argued
         * about the wrong function: it is true of SvDevirtualizeProcessor and
         * irrelevant to SvLV20, which runs first and uses the same register.
         */
        guestContext.VpRegs->Rcx = VpData->GuestVmcb.StateSaveArea.Rsp;
        guestContext.VpRegs->Rdx = reinterpret_cast<UINT64>(VpData) >> 32;

        /*
         * 9.443: FOR THE 206 STUB GUEST, THE EXIT IS TO THE HOST, NOT TO THE GUEST.
         *
         * SvLV20 does exactly two things with what we leave in the register block:
         *
         *     mov rsp, rcx
         *     jmp rbx
         *
         * For CPUID_UNLOAD the correct values are "the guest's stack" and "the
         * guest's next instruction", because the guest is inside __cpuidex and must
         * finish that instruction on bare metal. That is the path above.
         *
         * Our 206 guest is different in kind: it is a stub WE own, it was never
         * doing anything on behalf of a Windows thread, and the only sane thing to
         * do with it after the stop flag is to LEAVE - not to resume it. So this
         * branch hands SvLV20 the thread's own stack and a one-instruction thunk
         * that returns through the entry trampoline:
         *
         *     rcx = g_S206ThreadRsp[core]  (the address where the trampoline's
         *          call pushed a return address - this thread's real stack)
         *     rbx = Sv206LeaveToHost       (a single `ret`)
         *
         * The consequence is the one 9.442 said was missing: the core stops
         * executing driver code entirely before DriverUnload returns, so the image
         * can be unmapped without anything of ours left running on it. A stub that
         * merely parked (cli/hlt) would only have moved the PAGED_IP crash from
         * `vmsave` to `hlt`.
         *
         * Two earlier attempts got the stack wrong and each cost a machine:
         *   9.443   used &VpData->HostStackLayout (driver memory) -> the scheduler
         *           faulted inside KiSwapContext, 0x1E C0000005 / STACKPTR_ERROR.
         *   9.444a  captured a function's own return-address slot instead of the
         *           call site -> the `ret` popped a saved register, 0x0E at
         *           nt!KiStartSystemThread+0x34.
         *
         * Which path is taken is decided by the exit code: only a VMMCALL from our
         * stub reaches here with the stop flag set, and the CPUID_UNLOAD leaf is a
         * separate case that keeps the guest-resume semantics.
         */
        if (VpData->GuestVmcb.ControlArea.ExitCode == VMEXIT_VMMCALL)
        {
            /*
             * 9.445: THE STACK POINTER COMES FROM Sv206LaunchVm ITSELF.
             *
             * Sv206LaunchVm captured RSP before its `mov rsp, rcx` switched to the
             * per-core host stack, and stored it in ControlArea.HostExitRsp. That
             * register value IS the thread's real stack at the call - no
             * reconstruction, no offset arithmetic, nothing to get wrong.
             *
             * Four earlier attempts tried to DERIVE this value instead, and each
             * broke a different ABI invariant:
             *   9.443   a driver-allocated stack          -> 0x1E, #GP in KiSwapContext
             *   9.444a  a function's own cookie slot      -> 0x0E at KiStartSystemThread
             *   9.444b  a global indexed by the wrong id  -> 0x50 at MiSystemFault
             *   9.444c  hand-unwound past a /GS frame     -> 0xF7 _report_gsfailure
             *
             * The value was always available; nobody had simply saved it.
             *
             * Sv206LeaveToHost then ends the thread from ASM on that stack, WITHOUT
             * unwinding any C frame - because a C frame may only be left by its own
             * epilogue (it owns the /GS cookie and the SEH chain).
             *
             * RAX carries the terminate routine, not RDX: the thunk ends with
             * `call rax`. The first version put it in RDX and the thunk called
             * whatever the guest had left in RAX - measured as 0x1E C0000005
             * faulting at yghv_pa+0x2501b, the instruction right after that call.
             *
             * These writes land in the PUSHAQ block, and SvLV20's POPAQ restores from
             * it before `mov rsp,rcx; jmp rbx`, so this is exactly what the thunk
             * sees:
             *   Rcx (slot +0x70) = the thread's real stack
             *   Rax (slot +0x78) = PsTerminateSystemThread
             *   Rbx (slot +0x60) = Sv206LeaveToHost
             */
            UINT64 threadRsp = VpData->GuestVmcb.ControlArea.HostExitRsp;

            if (threadRsp != 0)
            {
                guestContext.VpRegs->Rcx = threadRsp;
                guestContext.VpRegs->Rax =
                    reinterpret_cast<UINT64>(&PsTerminateSystemThread);
                guestContext.VpRegs->Rbx =
                    reinterpret_cast<UINT64>(&Sv206LeaveToHost);
            }
        }

        //
        // Load guest state (currently host state is loaded).
        //
        __svm_vmload(MmGetPhysicalAddress(&VpData->GuestVmcb).QuadPart);

        //
        // Set the global interrupt flag (GIF) but still disable interrupts by
        // clearing IF. GIF must be set to return to the normal execution, but
        // interruptions are not desirable until SVM is disabled as it would
        // execute random kernel-code in the host context.
        //
        _disable();
        __svm_stgi();

        //
        // Disable SVM, and restore the guest RFLAGS. This may enable interrupts.
        // Some of arithmetic flags are destroyed by the subsequent code.
        //
        __writemsr(IA32_MSR_EFER, __readmsr(IA32_MSR_EFER) & ~EFER_SVME);
        __writeeflags(VpData->GuestVmcb.StateSaveArea.Rflags);
        goto Exit;
    }

    //
    // Reflect potentially updated guest's RAX to VMCB. Again, unlike other GPRs,
    // RAX is loaded from VMCB on VMRUN.
    //
    VpData->GuestVmcb.StateSaveArea.Rax = guestContext.VpRegs->Rax;

    //
    // Tier 4.1 (2026-10-11): split-view selection.
    //
    // This is the last point before the guest resumes, so it is where the
    // nested-page-table base for the NEXT run gets chosen. The choice is
    // per-CORE (the VMCB is per-VCPU), not per-access: if the target and an
    // observer share a core within one time slice, the last VMRUN wins. That
    // is the documented limit of this approach (see common/mem_view.h).
    //
    // Lock-free and call-free — yghv_view_pick_ncr3 only scans a nonpaged
    // array, so it is legal at DISPATCH with GIF=0.
    //
    {
        UINT64 viewNcr3 = yghv_view_pick_ncr3(
            VpData->GuestVmcb.StateSaveArea.Cr3);
        if (viewNcr3 != 0 &&
            VpData->GuestVmcb.ControlArea.NCr3 != viewNcr3)
        {
            VpData->GuestVmcb.ControlArea.NCr3 = viewNcr3;
            VpData->GuestVmcb.ControlArea.VmcbClean = 0;
        }
    }

Exit:
    /*
     * 9.521: HANDLER COST, END.
     *
     * `Exit:` is the ONLY way out of this function - every case breaks or falls
     * through to it - so no path escapes uncounted. A second return anywhere would
     * silently skew every reading from that day on.
     *
     * The value INCLUDES this instrumentation (~2 TSC reads + 1 contended
     * InterlockedAdd64). It is NOT subtracted: that correction was never measured
     * either, and an invented correction reads as more precision than exists.
     */
    {
        UINT64 tsc1 = __rdtsc();
        UINT64 d    = tsc1 - s521_tsc0;
        if (s521_tsc0 != 0 && d != 0 && d < 100000000ULL)
        {
            InterlockedExchangeAdd64(&g_S206HandlerTscTotal, (LONG64)d);
            InterlockedIncrement64(&g_S206HandlerTscCount);
        }
    }

    NT_ASSERT(VpData->HostStackLayout.Reserved1 == MAXUINT64);
    return guestContext.ExitVm;
}

/*!
    @brief      Returns attributes of a segment specified by the segment selector.

    @details    This function locates a segment descriptor from the segment
                selector and the GDT base, extracts attributes of the segment,
                and returns it. The returned value is the same as what the "dg"
                command of Windbg shows as "Flags". Here is an example output
                with 0x18 of the selector:
                ----
                0: kd> dg 18
                P Si Gr Pr Lo
                Sel        Base              Limit          Type    l ze an es ng Flags
                ---- ----------------- ----------------- ---------- - -- -- -- -- --------
                0018 00000000`00000000 00000000`00000000 Data RW Ac 0 Bg By P  Nl 00000493
                ----

    @param[in]  SegmentSelector - A segment selector to get attributes of a
                corresponding descriptor.
    @param[in]  GdtBase - A base address of GDT.

    @result     Attributes of the segment.
 */
_IRQL_requires_same_
_Check_return_
static
UINT16
SvGetSegmentAccessRight (
    _In_ UINT16 SegmentSelector,
    _In_ ULONG_PTR GdtBase
    )
{
    PSEGMENT_DESCRIPTOR descriptor;
    SEGMENT_ATTRIBUTE attribute;

    //
    // Get a segment descriptor corresponds to the specified segment selector.
    //
    descriptor = reinterpret_cast<PSEGMENT_DESCRIPTOR>(
                                        GdtBase + (SegmentSelector & ~RPL_MASK));

    //
    // Extract all attribute fields in the segment descriptor to a structure
    // that describes only attributes (as opposed to the segment descriptor
    // consists of multiple other fields).
    //
    attribute.Fields.Type = descriptor->Fields.Type;
    attribute.Fields.System = descriptor->Fields.System;
    attribute.Fields.Dpl = descriptor->Fields.Dpl;
    attribute.Fields.Present = descriptor->Fields.Present;
    attribute.Fields.Avl = descriptor->Fields.Avl;
    attribute.Fields.LongMode = descriptor->Fields.LongMode;
    attribute.Fields.DefaultBit = descriptor->Fields.DefaultBit;
    attribute.Fields.Granularity = descriptor->Fields.Granularity;
    attribute.Fields.Reserved1 = 0;

    return attribute.AsUInt16;
}

/*!
    @brief      Tests whether the SimpleSvm hypervisor is installed.

    @details    This function checks a result of CPUID leaf 40000000h, which
                should return a vendor name of the hypervisor if any of those
                who implement the Microsoft Hypervisor interface is installed.
                If the SimpleSvm hypervisor is installed, this should return
                "SimpleSvm", and if no hypervisor is installed, it the result of
                CPUID is undefined. For more details of the interface, see
                "Requirements for implementing the Microsoft Hypervisor interface"
                https://msdn.microsoft.com/en-us/library/windows/hardware/Dn613994(v=vs.85).aspx

    @result     TRUE when the SimpleSvm is installed; otherwise, FALSE.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
BOOLEAN
SvIsSimpleSvmHypervisorInstalled (
    VOID
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    char vendorId[13];

    //
    // When the SimpleSvm hypervisor is installed, CPUID leaf 40000000h will
    // return "SimpleSvm   " as the vendor name.
    //
    __cpuid(registers, CPUID_HV_VENDOR_AND_MAX_FUNCTIONS);
    RtlCopyMemory(vendorId + 0, &registers[1], sizeof(registers[1]));
    RtlCopyMemory(vendorId + 4, &registers[2], sizeof(registers[2]));
    RtlCopyMemory(vendorId + 8, &registers[3], sizeof(registers[3]));
    vendorId[12] = ANSI_NULL;

    return (strcmp(vendorId, "SimpleSvm   ") == 0);
}

/*!
    @brief      Virtualizes the current processor.

    @details    This function enables SVM, initialize VMCB with the current
                processor state, and enters the guest mode on the current
                processor.

    @param[in,out]  VpData - The address of per processor data.
    @param[in]      SharedVpData - The address of share data.
    @param[in]      ContextRecord - The address of CONETEXT to use as an initial
                    context of the processor after it is virtualized.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
static
VOID
SvPrepareForVirtualization (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ const CONTEXT* ContextRecord
    )
{
    /*
     * 9.448b: REGISTER THIS CORE'S VPD so a mask change can reach its live VMCB.
     *
     * Done here rather than in Sv206CoreThread because this is the one point that
     * already holds VpData for this core, and it runs before any guest code does.
     * Registration is idempotent (yghv_s206_register_vpd replaces an equal entry),
     * so a repeated prepare cannot fill the table with duplicates.
     */
    {
        extern void yghv_s206_register_vpd(PVOID vpData);
        yghv_s206_register_vpd(VpData);
    }

    DESCRIPTOR_TABLE_REGISTER gdtr, idtr;
    PHYSICAL_ADDRESS guestVmcbPa, hostVmcbPa, hostStateAreaPa, pml4BasePa, msrpmPa;

    //
    // Capture the current GDTR and IDTR to use as initial values of the guest
    // mode.
    //
    _sgdt(&gdtr);
    __sidt(&idtr);

    guestVmcbPa = MmGetPhysicalAddress(&VpData->GuestVmcb);
    hostVmcbPa = MmGetPhysicalAddress(&VpData->HostVmcb);
    hostStateAreaPa = MmGetPhysicalAddress(&VpData->HostStateArea);
#if defined(YGHV_206B_GNPT)
    // 9.246: nested paging root = YuanGuardHV's g_npt (mapped to max-physical,
    // identity). SharedVpData->Pml4Entries stays allocated but unused.
    pml4BasePa.QuadPart = g_npt.pml4_pa;
#else
    pml4BasePa = MmGetPhysicalAddress(&SharedVpData->Pml4Entries);
#endif
    msrpmPa = MmGetPhysicalAddress(SharedVpData->MsrPermissionsMap);

    //
    // Configure to trigger #VMEXIT with CPUID and VMRUN instructions. CPUID is
    // intercepted to present existence of the SimpleSvm hypervisor and provide
    // an interface to ask it to unload itself.
    //
    // VMRUN is intercepted because it is required by the processor to enter the
    // guest mode; otherwise, #VMEXIT occurs due to VMEXIT_INVALID when a
    // processor attempts to enter the guest mode. See "Canonicalization and
    // Consistency Checks" on "VMRUN Instruction".
    //
    /*
     * 9.369h: FORCE THE RDTSC BITS CLEAR AT PREPARE, don't merely not set them.
     *
     * Measured after disabling BOTH arming sites (9.369e, 9.369g): exits still
     * climbed 495084 -> 653673 and `intercept bit armed` still read 1. Refraining
     * from writing a bit cannot clear a bit that is already there, and the VMCB
     * control area is memory that outlives a driver reload - an armed bit from an
     * earlier load survives into the next one.
     *
     * Tier 5.1's frozen-time half is disabled (9.369e says why: the release could
     * not be proven, and an unproven release produced a hang and a 0x101). What
     * remains is TscOffset, which needs no intercept at all. So the correct state
     * for these two bits at load time is CLEAR, asserted rather than assumed.
     *
     * This is the only place that must know: every other site's arming is
     * disabled, so prepare owns the bits.
     */
    VpData->GuestVmcb.ControlArea.InterceptMisc1 &= ~((UINT32)SVM_INTERCEPT_MISC1_RDTSC);
    VpData->GuestVmcb.ControlArea.InterceptMisc2 &= ~((UINT32)SVM_INTERCEPT_MISC2_RDTSCP);
    VpData->GuestVmcb.ControlArea.VmcbClean = 0;
    InterlockedExchange(&g_TscInterceptArmed, 0);

    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_CPUID;
#if defined(YGHV_206B_GNPT)
    // 9.250: intercept CR3 writes (UINT16 bit 3 of the write half at +0x002).
    // Two reasons: (a) the protection verdict needs the CURRENT guest CR3 —
    // VMEXIT does not save CR3 into the VMCB state area (it is stale from
    // prepare), and the HSAVE area holds the host CR3, so interception is the
    // only clean source; (b) the emulated write (RIP=NRip, StateSaveArea.Cr3
    // = new value) must be paired with TlbControl=1 since intercepting
    // removes the native CR3-write TLB flush. Cost: one VMEXIT per context
    // switch. Only in the g_npt mode; 206-A baseline stays verbatim.
    // 9.250: intercept CR3 AND CR4 writes (UINT16 write half at +0x002:
    // bit3 = CR3, bit4 = CR4). 9.250e (13:43 SHUTDOWN bugcheck): Windows'
    // KiSwapContext does  mov cr4,~PGE; mov cr3,new; mov cr4,old  — an
    // atomicity trick around PCIDE. Intercepting ONLY the CR3 write and
    // emulating it with TlbControl=1 left the surrounding native CR4 writes
    // running with a TLB state the native sequence doesn't expect -> the
    // final mov cr4,rcx faulted -> triple fault -> SHUTDOWN (our 0xE2/0x20601).
    // Intercepting BOTH CR3 and CR4 writes moves the whole sequence into the
    // island: each write is emulated + TlbControl=1, which is the exact
    // semantics a native write would have (full flush), and no intermediate
    // native CR4 write executes with a half-updated PCID/TLB state.
    // ExitInfo1 encoding is the same for all CR writes (bits[7:6] source GPR,
    // bit63 extended GPR), so one emulator serves both. Cost: +2 VMEXITs per
    // KiSwapContext. 206-A baseline stays verbatim.
    VpData->GuestVmcb.ControlArea.InterceptCrWrite |= 0x0018;
    // 9.256: intercept #DB (InterceptException UINT16 bit 1). The VMCB's
    // InterceptException is never initialized elsewhere (9.244 reverted the
    // 205k block), so it stayed 0 — the ALLOW->TF->#DB->re-arm cycle was DEAD
    // CODE: TF was set in the guest but the single-step #DB went NATIVE to
    // KiTrap01 -> STATUS_SINGLE_STEP to the user thread -> unhandled -> WER
    // crash (the c12/c13 fast dialog deaths; dbg ring empty proved zero #DB
    // VMEXITs). Only bit 1 is set: Windows' benign #GP/#PF probes stay native
    // (the 9.244 concern targets #GP, vector 13 — untouched).
    /*
     * 9.373: FORCE bit 3 (#BP) CLEAR AT PREPARE.
     *
     * Measured across two runs: the SAME 0x139 MISSING_GSFRAME_STACKPTR appeared
     * even when the policy flag was 0 and no breakpoint had been armed in that
     * boot at all. The cause is the one 9.369h already recorded for the RDTSC
     * bits: a VMCB intercept bit lives in memory that OUTLIVES a driver reload,
     * so a bit set by an earlier load stays armed for the next one. Refraining
     * from setting it cannot clear it.
     *
     * So prepare owns bit 3 the same way it owns the TSC bits: cleared here, and
     * re-armed only by yghv_s206_bp_int3_apply_all() when the policy asks.
     */
    VpData->GuestVmcb.ControlArea.InterceptException &= ~((UINT32)(1u << 3));

    VpData->GuestVmcb.ControlArea.InterceptException |= (1u << 1);
    /*
     * 9.372 (C1): INTERCEPT #BP -- OPT-IN, AND ONLY BECAUSE IT WAS NEVER ON.
     *
     * MEASURED FACT (9.367.3): this build only ever set bit 1 (#DB). #BP is
     * vector 3, i.e. bit 3, and it was NEVER set. So `case VMEXIT_EXCEPTION_BP`
     * -- the whole chain "record -> restore the byte we displaced -> set TF ->
     * #DB re-patches" -- could only be reached when the guest executed an INT3
     * on its own, and in that situation yghv_bp_on_int3() finds no slot and the
     * handler takes the reflect branch.
     *
     * The INT3-mode test that has been passing since Tier 4.4 (g18 B1-B3) checks
     * that 0xCC gets WRITTEN and RESTORED. Its target page is a data page whose
     * own comment says "never executed". So the hot path has zero evidence: the
     * 0xCC was never fetched, never faulted, and never handled.
     *
     * WHY OPT-IN RATHER THAN ALWAYS ON. Turning on an exception intercept has
     * already cost this project a bugcheck once: the 205k block enabled #GP and
     * Windows' benign PatchGuard/SEH probes became an observable 0xE2 within
     * minutes (9.244). #BP is much narrower than #GP - it fires only on a 0xCC
     * the guest executes - but "much narrower" is an argument, not a measurement,
     * and this is a machine that has already been taken down three times in this
     * session by argued-not-measured changes.
     *
     * So the mask is armed only when a policy flag says so. A test asks for it,
     * measures the chain, and the default boot path stays exactly as it was.
     * `g_BpInt3Intercept` is checked here and nowhere else; the reflect branch in
     * the handler is what makes it safe to leave on - a #BP that is NOT ours is
     * re-injected to the guest, which is the guest's own semantics.
     */
    if (InterlockedCompareExchange(&g_BpInt3Intercept, 0, 0))
        VpData->GuestVmcb.ControlArea.InterceptException |= (1u << 3);
    // 9.250c: intercept SHUTDOWN (bit 31) — a guest triple fault then becomes
    // an observable VMEXIT (handler bugchecks with the state) instead of the
    // CPU dying silently = the instant-reset failure mode.
    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= (1u << 31);
#endif
    /*
     * 9.432: ARM VMMCALL, WHICH THE TEST GUEST NEEDS TO EVER RETURN.
     *
     * 9.430/9.431 traced the whole session's failures to one missing bit:
     * svm_trampoline.asm is `mov rax,1 / vmmcall / jmp` - vmmcall is the ONLY
     * instruction that produces a #VMEXIT for that guest. Without its intercept
     * the call is not trapped, no exit happens, vmrun never returns, and every
     * downstream mechanism that assumes "there will be exits" quietly stops
     * working:
     *
     *   SvHandleVmExit        never runs  (g_S206VrLog stayed empty all session)
     *   the stop-flag check   never runs  (it sits on the loop's back-edge)
     *   lock dec after vmsave never runs  (g_inVmrunCount = 12, forever)
     *
     * VMRUN and VMMCALL are adjacent bits of the same field, which is how a tree
     * ends up with one and not the other: upstream intercepts both, because its
     * design is "the guest hands control back with vmmcall".
     */
    VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_VMRUN;
    VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_VMMCALL;

    //
    // NOTE (9.244): the "205k discriminator" exception-interception block that
    // briefly lived in the vendored copy was REVERTED for the 206-A baseline.
    // 205k already served its purpose (proved the control area innocent by
    // entering Windows with our deltas). Keeping InterceptException!=0 in the
    // survival vehicle would turn Windows' benign #GP probes (PatchGuard
    // canaries / SEH probes) into an observable 0xE2 within minutes — the guest
    // IDT must swallow them, exactly as C0 (upstream unmodified) does.
    //

    //
    // Also, configure to trigger #VMEXIT on MSR access as configured by the
    // MSRPM. In our case, write to IA32_MSR_EFER is intercepted.
    //
    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_MSR_PROT;
    VpData->GuestVmcb.ControlArea.MsrpmBasePa = msrpmPa.QuadPart;

    /*
     * 9.448: fold in the caller-selected extra intercept bits.
     *
     * The 25 cases added in this step cover exit codes whose bits are NOT in the
     * default set (HLT, PAUSE, MONITOR/MWAIT, WBINVD, VMLOAD/VMSAVE, STGI/CLGI,
     * INTR/NMI...). A case with no armed bit can never fire, so this is where
     * "we implement 35 exit codes" becomes testable: arm a bit, run traffic, and
     * read yghv_s206_exit_count_of() to see whether it fired.
     *
     * 9.448b: CLEAR-THEN-SET, not OR. The VMCB control area outlives a driver
     * reload (9.369h), so a bit armed by an earlier load survives into this one.
     * OR-ing a new mask can therefore only ever add - `exit-mask clear` would
     * silently do nothing. The extra-bit range is owned exclusively by this
     * setting, so replacing it outright cannot disarm a default bit.
     */
    VpData->GuestVmcb.ControlArea.InterceptMisc1 =
        (UINT32)((VpData->GuestVmcb.ControlArea.InterceptMisc1 & ~S206_MISC1_EXTRA_MASK) |
                 (UINT32)g_S206ExtraMisc1);
    VpData->GuestVmcb.ControlArea.InterceptMisc2 =
        (UINT32)((VpData->GuestVmcb.ControlArea.InterceptMisc2 & ~S206_MISC2_EXTRA_MASK) |
                 (UINT32)g_S206ExtraMisc2);

    /*
     * 9.366 (Tier 7.6): fill the attack-surface inventory HERE, where the mask
     * above is actually set.
     *
     * This is the whole point of 7.6: "what can be attacked" has to be read off
     * the thing that arms it, not asserted in a comment. Before this call
     * nothing ever populated the list, so `sys-sentry surface` said
     * "0 entry(s)" — a true statement that read as "we checked and found
     * nothing", when in fact nobody had looked.
     *
     * PASSIVE at prepare time, so the plain (non-interlocked-only) surface
     * helpers are legal here; on the island the counters are what must stay
     * island-safe, and they are.
     *
     * 9.368 (A2): AND IT IS DERIVED FROM THE MASK, NOT FROM A HAND-WRITTEN LIST.
     *
     * 9.366 added nine names in yghv_sentry.c and asserted they matched what the
     * mask arms. They did — that day. But the list and the mask were two
     * statements in two files, so adding an intercept meant remembering to add a
     * name, and forgetting produced an inventory that silently under-reported
     * the surface. That is the same "two places must agree" shape as 9.366.5.
     *
     * The mask is the truth, so the mask is what gets read. This file is the one
     * that owns the VMCB, so this file knows the bit layout; yghv_sentry.c keeps
     * only storage and no longer has any opinion about what a VMCB is.
     */

    //
    // Specify guest's address space ID (ASID). TLB is maintained by the ID for
    // guests. Use the same value for all processors since all of them run a
    // single guest in our case. Use 1 as the most likely supported ASID by the
    // processor. The actual the supported number of ASID can be obtained with
    // CPUID. See "CPUID Fn8000_000A_EBX SVM Revision and Feature
    // Identification". Zero of ASID is reserved and illegal.
    //
    VpData->GuestVmcb.ControlArea.GuestAsid = 1;

    //
    // Enable Nested Page Tables. By enabling this, the processor performs the
    // nested page walk, that involves with an additional page walk to translate
    // a guest physical address to a system physical address. An address of
    // nested page tables is specified by the NCr3 field of VMCB.
    //
    // We have already build the nested page tables with SvBuildNestedPageTables.
    //
    // Note that our hypervisor does not trigger any additional #VMEXIT due to
    // the use of Nested Page Tables since all physical addresses from 0-512 GB
    // are configured to be accessible from the guest.
    //
    VpData->GuestVmcb.ControlArea.NpEnable |= SVM_NP_ENABLE_NP_ENABLE;
    VpData->GuestVmcb.ControlArea.NCr3 = pml4BasePa.QuadPart;

    /*
     * 9.368 (A2): THE INVENTORY IS FILLED **AFTER** THE LAST MASK WRITE.
     *
     * Measured on the first derived run: the list came back with 7 entries and
     * no "npf" — because the scan ran before NpEnable was set, so NPT looked
     * unarmed. The hand-written list it replaced had "npf" in it, which is
     * exactly the difference between a list that is true because someone typed
     * it and one that is true because it was read.
     *
     * Keeping the call here, after the last field that can arm an intercept, is
     * therefore not cosmetic: moving it up silently drops entries again.
     */
    /*
     * 9.368 (B1, Tier 5.1): TSC OFFSET, WRITTEN WHERE THE GUEST STATE IS BUILT.
     *
     * VMCB +0x050 is added by hardware to every RDTSC/RDTSCP the guest runs, so
     * a shift costs nothing per read — no exit, no handler, no timing tell of its
     * own. This is the cheap half of Tier 5.1 and the half that works with no
     * intercept at all.
     *
     * It is written once here per core, so a change made from the control plane
     * takes effect on the NEXT VMRUN of that core (a CPUID handshake re-enters
     * this function). That is deliberate: rewriting the field on a live VMCB from
     * an IOCTL would race the core running out of it, and the value is per-core
     * state.
     */
    VpData->GuestVmcb.ControlArea.TscOffset = (UINT64)yghv_s206_tsc_offset_get();

    /*
     * 9.368 (B2, Tier 5.1): RDTSC / RDTSCP INTERCEPT.
     *
     * MEASURED FACT that contradicts the plan: before this line, neither bit was
     * ever set for this build. MEM_ENGINE_PLAN_TIER5.md asserted that
     * "INTERCEPT_RDTSC 已在 206 主线启用" and listed RDTSC among the 35
     * "already implemented" exits — the same catalogue-versus-implementation error
     * 9.367.1 recorded.
     *
     * Arming them is what makes a NON-CONSTANT view possible: an offset shifts the
     * clock, only an intercept can freeze it. The cost is one exit per read, which
     * is why the policy is checked before arming rather than always on — at the
     * default (no freeze, zero offset) there is nothing to do and no reason to pay
     * a VMEXIT on the hottest instruction a guest can execute.
     */
    /*
     * 9.369g: DISABLED HERE TOO - and this line was the actual culprit.
     *
     * 9.369e disabled the arming in the per-exit path but this second arming site
     * in prepare was left in place. That is where the exits came from: measured
     * on 206zL, a run with arming "disabled", the counter still climbed
     * 47569 -> 76549 and exits began at 47569 rather than 0.
     *
     * A VMCB intercept bit is written once here and the processor honours it for
     * every VMRUN until something clears it. Disabling one of two write sites
     * therefore disabled nothing: an armed bit from this branch survives every
     * policy check downstream.
     *
     * Both sites are off now. TscOffset (B1) does not need either of them - the
     * hardware applies an offset itself - so the working half of Tier 5.1 is
     * unaffected by this.
     */
    if (0 /* disabled, see 9.369e / 9.369g */ && yghv_s206_tsc_wanted())
    {
        VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_RDTSC;
        VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_RDTSCP;
        yghv_s206_tsc_mark_armed();
    }

    if (!yghv_sentry_surface_armed()) {
        SvArmSurfaceFromMask(VpData);
    }

    //
    // Set up the initial guest state based on the current system state. Those
    // values are loaded into the processor as guest state when the VMRUN
    // instruction is executed.
    //
    VpData->GuestVmcb.StateSaveArea.GdtrBase = gdtr.Base;
    VpData->GuestVmcb.StateSaveArea.GdtrLimit = gdtr.Limit;
    VpData->GuestVmcb.StateSaveArea.IdtrBase = idtr.Base;
    VpData->GuestVmcb.StateSaveArea.IdtrLimit = idtr.Limit;

    VpData->GuestVmcb.StateSaveArea.CsLimit = GetSegmentLimit(ContextRecord->SegCs);
    VpData->GuestVmcb.StateSaveArea.DsLimit = GetSegmentLimit(ContextRecord->SegDs);
    VpData->GuestVmcb.StateSaveArea.EsLimit = GetSegmentLimit(ContextRecord->SegEs);
    VpData->GuestVmcb.StateSaveArea.SsLimit = GetSegmentLimit(ContextRecord->SegSs);
    VpData->GuestVmcb.StateSaveArea.CsSelector = ContextRecord->SegCs;
    VpData->GuestVmcb.StateSaveArea.DsSelector = ContextRecord->SegDs;
    VpData->GuestVmcb.StateSaveArea.EsSelector = ContextRecord->SegEs;
    VpData->GuestVmcb.StateSaveArea.SsSelector = ContextRecord->SegSs;
    VpData->GuestVmcb.StateSaveArea.CsAttrib = SvGetSegmentAccessRight(ContextRecord->SegCs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.DsAttrib = SvGetSegmentAccessRight(ContextRecord->SegDs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.EsAttrib = SvGetSegmentAccessRight(ContextRecord->SegEs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.SsAttrib = SvGetSegmentAccessRight(ContextRecord->SegSs, gdtr.Base);

    VpData->GuestVmcb.StateSaveArea.Efer = __readmsr(IA32_MSR_EFER);
    VpData->GuestVmcb.StateSaveArea.Cr0 = __readcr0();
    VpData->GuestVmcb.StateSaveArea.Cr2 = __readcr2();
    VpData->GuestVmcb.StateSaveArea.Cr3 = __readcr3();
    VpData->GuestVmcb.StateSaveArea.Cr4 = __readcr4();
    /*
     * 9.463: RFLAGS IS INHERITED AGAIN, WITHOUT THE IF FORCING.
     *
     * 9.462 forced IF=1 because the stub parked in hlt, and hlt with IF=0 cannot
     * be woken by maskable interrupts. hlt is gone (see the stub) so the forcing
     * protects nothing - and every bit written into the guest's saved state is
     * one more thing that can disagree with the thread it belongs to. The guest
     * here is a bounded spin, which has no interest in IF either way.
     */
    VpData->GuestVmcb.StateSaveArea.Rflags = ContextRecord->EFlags | 0x200ULL; /* 9.516: IF=1, hlt must be wakeable */
    VpData->GuestVmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
    /*
     * 9.443: THE GUEST IS OUR OWN STUB - NOT THE CALLER'S CONTINUATION.
     *
     * 9.434 correctly identified this line as the crux: it used to be
     * `ContextRecord->Rip`, which is SvVirtualizeProcessor's own continuation.
     * 9.436 then tried to fix that by pointing it at the old test trampoline, and
     * the machine stopped booting (9.437: START_PENDING forever) because that
     * trampoline was a pure `mov rax,1 / vmmcall / jmp` heartbeat and 9.435's
     * handler answered it with `break` - an infinite VMEXIT storm inside a
     * synchronous SvVirtualizeAllProcessors.
     *
     * 9.439 reverted to the caller's continuation, which made loading work again
     * (58 ms, measured) but left the real defect in place, and 9.442 finally named
     * it: a caller-continuation guest EXECUTES THE FUNCTION'S `return`. Control
     * flows back to SvExecuteOnEachProcessor, the thread moves to the next core,
     * and another vmrun starts - leaving the previous frame suspended forever with
     * its `lock dec` unexecuted. Twelve cores, twelve suspended frames, and after
     * DriverUnload unmaps the image their `vmsave` runs on dead code: the 0xD1 /
     * PAGED_IP at Sv206LaunchVm+0x14.
     *
     * The guest is therefore a stub we own (Sv206GuestStub). Its loop is
     * `pause x4 / mov rax,1 / vmmcall`, so:
     *   - every iteration is an intercepted instruction, giving the handler a vote;
     *   - the handler ends the guest by setting ExitVm, and SvLV20 resumes us at
     *     OUR next instruction - not at some Windows thread's mid-function PC;
     *   - the stub never touches the stack, so RSP below is never dereferenced and
     *     a host stack pointer is safe to hand it.
     *
     * This is what makes "stop on purpose" possible at all, and it is why the
     * stop flag no longer has to be set from an arbitrary exit (which 9.442 showed
     * was corruption, not a fix).
     */
    /*
     * 9.539: OPT-IN REAL-CONTEXT GUEST. Default OFF, and with it off this line
     * is the same assignment as before - that is deliberate, because a mode that
     * may crash must not be reachable by accident.
     *
     * Turned on, the guest is the captured Windows thread context instead of the
     * stub. That is the configuration Type2-AMD-HV / miniSVM use and the one the
     * 2026-08 experiments never tested (they all ran with the stub as guest).
     *
     * RSP comes along with it: a real thread dereferences its own stack, so the
     * stub-era reasoning ("the stub never touches the stack, so handing it a host
     * pointer is safe") does not hold here - the real RSP must be the real RSP.
     */
    if (yghv_s206_real_guest_mode())
    {
        VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;
        VpData->GuestVmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
    }
    else
    {
        VpData->GuestVmcb.StateSaveArea.Rip =
            reinterpret_cast<UINT64>(&Sv206GuestStub);
    }

    /*
     * 9.546: record what the guest was actually handed (first writer wins).
     *
     * Without this, "the guest resumed at ContextRecord->Rip" is an assumption, and
     * the whole early-return model rests on it. Recording the value makes it a
     * reading - and if the real value is not what the code says it should be, that
     * is the answer to why early-returns never climbs.
     */
    {
        if (InterlockedIncrement(&g_S206GuestRipWriters) == 1)
        {
            InterlockedExchange64(&g_S206GuestRipSet,
                                  (LONG64)VpData->GuestVmcb.StateSaveArea.Rip);
            InterlockedExchange64(&g_S206GuestRspSet,
                                  (LONG64)VpData->GuestVmcb.StateSaveArea.Rsp);
            InterlockedExchange(&g_S206RealModeAtSetup,
                                yghv_s206_real_guest_mode() ? 1 : 0);
        }
    }
    VpData->GuestVmcb.StateSaveArea.GPat = __readmsr(IA32_MSR_PAT);

    /*
     * 9.445: HostExitRsp IS WRITTEN BY Sv206LaunchVm, NOT HERE.
     *
     * It used to be adopted here from a value the entry trampoline had left in a
     * global. That whole chain is gone: Sv206LaunchVm captures the thread's RSP with
     * one instruction immediately before its `mov rsp, rcx`, and stores it into this
     * same field. Nothing between the two needs to carry it.
     *
     * Clear it here so a stale value from a previous load cannot be mistaken for a
     * valid exit stack: the field lives in memory the VMCB occupies, and that memory
     * outlives a driver reload (the same trap 9.369h recorded for intercept bits).
     */
    VpData->GuestVmcb.ControlArea.HostExitRsp = 0;

    //
    // Save some of the current state on VMCB. Some of those states are:
    // - FS, GS, TR, LDTR (including all hidden state)
    // - KernelGsBase
    // - STAR, LSTAR, CSTAR, SFMASK
    // - SYSENTER_CS, SYSENTER_ESP, SYSENTER_EIP
    // See "VMSAVE and VMLOAD Instructions" for mode details.
    //
    // Those are restored to the processor right before #VMEXIT with the VMLOAD
    // instruction so that the guest can start its execution with saved state,
    // and also, re-saved to the VMCS with right after #VMEXIT with the VMSAVE
    // instruction so that the host (hypervisor) do not destroy guest's state.
    //
    __svm_vmsave(guestVmcbPa.QuadPart);

    //
    // Store data to stack so that the host (hypervisor) can use those values.
    //
    VpData->HostStackLayout.Reserved1 = MAXUINT64;
    VpData->HostStackLayout.SharedVpData = SharedVpData;
    VpData->HostStackLayout.Self = VpData;
    VpData->HostStackLayout.HostVmcbPa = hostVmcbPa.QuadPart;
    VpData->HostStackLayout.GuestVmcbPa = guestVmcbPa.QuadPart;
#if defined(YGHV_206B_GNPT)
    // 9.250c: seed the per-VCPU current-guest-CR3 slot with the CR3 at
    // prepare time (correct for the first CR3 self-fetch; the CR3-write
    // exit keeps it current from then on). Padding1 is an otherwise-unused
    // alignment field in the HostStackLayout we own.
    VpData->HostStackLayout.Padding1 = __readcr3();
#endif

    //
    // Set an address of the host state area to VM_HSAVE_PA MSR. The processor
    // saves some of the current state on VMRUN and loads them on #VMEXIT. See
    // "VM_HSAVE_PA MSR (C001_0117h)".
    //
    __writemsr(SVM_MSR_VM_HSAVE_PA, hostStateAreaPa.QuadPart);

    //
    // Also, save some of the current state to VMCB for the host. This is loaded
    // after #VMEXIT to reproduce the current state for the host (hypervisor).
    //
    __svm_vmsave(hostVmcbPa.QuadPart);
}

/*!
    @brief      Virtualize the current processor.

    @details    This function enables SVM, initialize VMCB with the current
                processor state, and enters the guest mode on the current
                processor.

    @param[in]  Context - A pointer of share data.

    @result     STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
NTSTATUS
SvVirtualizeProcessor (
    _In_opt_ PVOID Context
    )
{
    NTSTATUS status;
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;
    PVIRTUAL_PROCESSOR_DATA vpData;
    PCONTEXT contextRecord;

    SV_DEBUG_BREAK();

    vpData = nullptr;

    NT_ASSERT(ARGUMENT_PRESENT(Context));
    _Analysis_assume_(ARGUMENT_PRESENT(Context));

    /* 9.543: how many times this function is entered at all. */
    InterlockedIncrement64(&g_S206VirtualizeEntries);

    // YGHV C0 local patch (2026-09-18): Win10 19045 has no ExAllocatePool2
    // (Win11+); zeroing reproduces Pool2's guaranteed-zero return.
    contextRecord = static_cast<PCONTEXT>(ExAllocatePoolWithTag(
                                                        NonPagedPool,
                                                        sizeof(*contextRecord),
                                                        'MVSS'));
    if (contextRecord == nullptr)
    {
        SvDebugPrint("Insufficient memory.\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    RtlZeroMemory(contextRecord, sizeof(*contextRecord));

    //
    // Allocate per processor data.
    //
#pragma prefast(suppress : __WARNING_MEMORY_LEAK, "Ownership is taken on success.")
    vpData = static_cast<PVIRTUAL_PROCESSOR_DATA>(
            SvAllocatePageAlingedPhysicalMemory(sizeof(VIRTUAL_PROCESSOR_DATA)));
    if (vpData == nullptr)
    {
        SvDebugPrint("Insufficient memory.\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Capture the current RIP, RSP, RFLAGS, and segment selectors. This
    // captured state is used as an initial state of the guest mode; therefore
    // when virtualization starts by the later call of Sv206LaunchVm, a processor
    // resume its execution at this location and state.
    //
    RtlCaptureContext(contextRecord);

    //
    // First time of this execution, the SimpleSvm hypervisor is not installed
    // yet. Therefore, the branch is taken, and virtualization is attempted.
    //
    // At the second execution of here, after Sv206LaunchVm virtualized the
    // processor, SvIsSimpleSvmHypervisorInstalled returns TRUE, and this
    // function exits with STATUS_SUCCESS.
    //
    /*
     * 9.543: THE EARLY RETURN IS ALREADY HERE - counting it, not adding it.
     *
     * This is the reference implementation's exact shape: SvIsSimpleSvmHypervisor
     * Installed() reads CPUID 40000000h, which our handler answers with the
     * "SimpleSvm   " vendor string, so the second pass falls straight through to
     * `return STATUS_SUCCESS` at the bottom. When that happens the C frame is
     * resumed INTO and unwound normally - which is why iretq does not fault there
     * and does fault in a mode where this never fires.
     */
    /*
     * 9.544 STAGE A: consult the explicit flag AS WELL AS the probe, and let the
     * PROBE decide. Behaviour is therefore identical to before this change; the
     * flag is only being brought up and watched.
     *
     * Why both: if CPUID and the flag ever disagree, taking the flag would change
     * the boot path in a way nothing had measured. Taking the probe keeps the
     * shipped behaviour and turns the disagreement into g_S206FlagDisagrees, which
     * virt-counts exposes. A divergence that is visible is a fact; one that silently
     * changes behaviour is a crash waiting to be blamed on something else.
     */
    {
        BOOLEAN installed = SvIsSimpleSvmHypervisorInstalled();
        /*
         * 9.545: read THIS core's flag. Out-of-range clamps to the conservative
         * answer ("first pass") - an unmapped core must not be treated as already
         * virtualized, or it would be skipped without ever running setup.
         */
        ULONG   thisCore = KeGetCurrentProcessorNumber();
        LONG    secondPass;
        if (thisCore >= SVM206_MAX_VPD) thisCore = SVM206_MAX_VPD - 1;
        secondPass = InterlockedCompareExchange(&g_S206SecondPass[thisCore], 0, 0);
        InterlockedIncrement64(&g_S206SecondPassReads);

        /* They should agree: installed <=> second pass. Record it when they do not. */
        if ((installed != FALSE) != (secondPass != 0))
        {
            InterlockedIncrement64(&g_S206FlagDisagrees);
        }
        else if (secondPass != 0)
        {
            /* Agreed, and it IS the second pass: this is the reference shape. */
            InterlockedIncrement64(&g_S206EarlyReturns);
            status = STATUS_SUCCESS;
            goto Exit;
        }
    }

    if (SvIsSimpleSvmHypervisorInstalled() == FALSE)
    {
        SvDebugPrint("Attempting to virtualize the processor.\n");
        sharedVpData = static_cast<PSHARED_VIRTUAL_PROCESSOR_DATA>(Context);

        //
        // Enable SVM by setting EFER.SVME. It has already been verified that this
        // bit was writable with SvIsSvmSupported.
        //
        __writemsr(IA32_MSR_EFER, __readmsr(IA32_MSR_EFER) | EFER_SVME);

        //
        // Set up VMCB, the structure describes the guest state and what events
        // within the guest should be intercepted, ie, triggers #VMEXIT.
        //
        SvPrepareForVirtualization(vpData, sharedVpData, contextRecord);

        /*
         * 9.544: SET THE FLAG BEFORE VMRUN - the same place the reference
         * implementation sets Virtualized.
         *
         * If the guest ever resumes into this function, it now sees "second pass" and
         * returns through the normal epilogue instead of running setup again. In stub
         * mode that resume never happens, so setting it changes nothing - which is
         * exactly what the stage-A measurement is meant to confirm.
         */
        /*
         * 9.545: set THIS core's flag only. A global write here is what made the
         * other five cores look like second passes before they had run setup.
         */
        {
            ULONG coreForFlag = KeGetCurrentProcessorNumber();
            if (coreForFlag >= SVM206_MAX_VPD) coreForFlag = SVM206_MAX_VPD - 1;
            InterlockedExchange(&g_S206SecondPass[coreForFlag], 1);
        }

        //
        // Switch to the host RSP to run as the host (hypervisor), and then
        // enters loop that executes code as a guest until #VMEXIT happens and
        // handles #VMEXIT as the host.
        //
        // This function should never return to here.
        //
        Sv206LaunchVm(&vpData->HostStackLayout.GuestVmcbPa);
        SV_DEBUG_BREAK();
#pragma prefast(suppress : __WARNING_USE_OTHER_FUNCTION, "Unrecoverble path.")
        KeBugCheck(MANUALLY_INITIATED_CRASH);
    }

    SvDebugPrint("The processor has been virtualized.\n");
    status = STATUS_SUCCESS;

Exit:
    if (contextRecord != nullptr)
    {
        ExFreePoolWithTag(contextRecord, 'MVSS');
    }
    if ((!NT_SUCCESS(status)) && (vpData != nullptr))
    {
        //
        // Frees per processor data if allocated and this function is
        // unsuccessful.
        //
        SvFreePageAlingedPhysicalMemory(vpData);
    }
    /*
     * 9.544: the unconditional increment is GONE. 9.543 put it here and it counted
     * every successful return - including the first-pass setup path - which would
     * have made "early returns" look healthy in exactly the mode where none happen.
     * The count now increments only on the branch that actually is the second pass.
     */
    return status;
}

/*!
    @brief      Execute a callback on all processors one-by-one.

    @details    This function execute Callback with Context as a parameter for
                each processor on the current IRQL. If the callback returned
                non-STATUS_SUCCESS value or any error occurred, this function
                stops execution of the callback and returns the error code.

                When NumOfProcessorCompleted is not NULL, this function always
                set a number of processors that successfully executed the
                callback.

    @param[in]  Callback - A function to execute on all processors.
    @param[in]  Context - A parameter to pass to the callback.
    @param[out] NumOfProcessorCompleted - A pointer to receive a number of
                processors executed the callback successfully.

    @result     STATUS_SUCCESS when Callback executed and returned STATUS_SUCCESS
                on all processors; otherwise, an appropriate error code.
 */
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
NTSTATUS
SvExecuteOnEachProcessor (
    _In_ NTSTATUS (*Callback)(PVOID),
    _In_opt_ PVOID Context,
    _Out_opt_ PULONG NumOfProcessorCompleted
    )
{
    NTSTATUS status;
    ULONG i, numOfProcessors;
    PROCESSOR_NUMBER processorNumber;
    GROUP_AFFINITY affinity, oldAffinity;

    status = STATUS_SUCCESS;

    //
    // Get a number of processors on this system.
    //
    numOfProcessors = yghv_s206_core_limit(
        KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS));   /* 9.525 */

    for (i = 0; i < numOfProcessors; i++)
    {
        //
        // Convert from an index to a processor number.
        //
        status = KeGetProcessorNumberFromIndex(i, &processorNumber);
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }

        //
        // Switch execution of this code to a processor #i.
        //
        affinity.Group = processorNumber.Group;
        affinity.Mask = 1ULL << processorNumber.Number;
        affinity.Reserved[0] = affinity.Reserved[1] = affinity.Reserved[2] = 0;
        KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);

        //
        // Execute the callback.
        //
        status = Callback(Context);

        //
        // Revert the previously executed processor.
        //
        KeRevertToUserGroupAffinityThread(&oldAffinity);

        //
        // Exit if the callback returned error.
        //
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }
    }

Exit:
    //
    // i must be the same as the number of processors on the system when this
    // function returns STATUS_SUCCESS;
    //
    NT_ASSERT(!NT_SUCCESS(status) || (i == numOfProcessors));

    //
    // Set a number of processors that successfully executed callback if the
    // out parameter is present.
    //
    if (ARGUMENT_PRESENT(NumOfProcessorCompleted))
    {
        *NumOfProcessorCompleted = i;
    }
    return status;
}

/*!
    @brief      De-virtualize the current processor if virtualized.

    @details    This function asks SimpleSVM hypervisor to deactivate itself
                through CPUID with a back-door function id and frees per
                processor data if it is returned. If the SimpleSvm is not
                installed, this function does nothing.

    @param[in]  Context - An out pointer to receive an address of shared data.

    @result     Always STATUS_SUCCESS.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
/* 9.288 (C6 v5): per-core NCr3 residue clear — runs on each core via
 * SvExecuteOnEachProcessor at the config-fake disable time. The core's VPD
 * is obtained via the CPUID_FAKE_VPD leaf (no de-virtualization). */
static
NTSTATUS
SvFakeNcr3Resync (
    _In_opt_ PVOID Context
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    UINT64 high, low;
    PVIRTUAL_PROCESSOR_DATA vpData;

    UNREFERENCED_PARAMETER(Context);

    __cpuidex(registers, CPUID_FAKE_VPD, CPUID_FAKE_VPD);
    if (registers[2] != 'SSV1')
    {
        goto Exit;
    }
    high = registers[3];
    low = registers[0] & MAXUINT32;
    vpData = reinterpret_cast<PVIRTUAL_PROCESSOR_DATA>(high << 32 | low);
    if (vpData->GuestVmcb.ControlArea.NCr3 ==
        yghv_protect_fake_alt_pa())
    {
        vpData->GuestVmcb.ControlArea.NCr3 =
            yghv_protect_fake_main_pa();
        vpData->GuestVmcb.ControlArea.VmcbClean = 0;
    }
Exit:
    return STATUS_SUCCESS;
}

extern "C"
VOID
yghv_protect_fake_ncr3_resync (
    VOID
    )
{
    NT_VERIFY(NT_SUCCESS(SvExecuteOnEachProcessor(SvFakeNcr3Resync,
                                                  nullptr,
                                                  nullptr)));
}

/*
 * 9.370 (B2 re-do): APPLY THE CLOCK POLICY ON EVERY CORE, FROM THE CONTROL PLANE.
 *
 * THE TWO QUESTIONS THAT BLOCKED B2, ANSWERED WITHOUT GUESSING
 *
 *   1 "how do we reach another core's VMCB?"
 *     We do not need to. SvExecuteOnEachProcessor already exists and is used by
 *     SvFakeNcr3Resync below it - it pins this thread to each core in turn and
 *     runs a callback THERE. SvFakeNcr3Resync then obtains that core's VPD with
 *     the CPUID_FAKE_VPD leaf, because the VPD address lives in the guest's
 *     registers on that core. Same trick, no stored handles, no per-core table.
 *
 *   2 "when does a VMCB write become visible to the processor?"
 *     VmcbClean = 0 before the next VMRUN. That is exactly what
 *     SvFakeNcr3Resync does with NCr3, and what the fake-write path does with
 *     its NCr3 switch - both are in production and were verified on hardware.
 *
 * So the arming and the release both become a per-core write from PASSIVE,
 * instead of a hope that each core converges by itself. That is the difference
 * between the version that produced 0x101 and this one: the exit storm existed
 * because the release depended on the core happening to re-enter the handler
 * with the right policy; here the control plane walks every core and writes the
 * bit itself, with VmcbClean=0, while that core is pinned.
 *
 * The per-exit and per-prepare sites stay disabled (9.369e/9.369g): a third
 * writer would reintroduce the "which write won" ambiguity that cost two
 * machines.
 */
static
NTSTATUS
SvTscApplyOnCore (
    _In_opt_ PVOID Context
    )
{
    int registers[4];
    UINT64 high, low;
    PVIRTUAL_PROCESSOR_DATA vpData;
    UINT32 want = (UINT32)(ULONG_PTR)Context;

    __cpuidex(registers, CPUID_FAKE_VPD, CPUID_FAKE_VPD);
    if (registers[2] != 'SSV1')
    {
        InterlockedIncrement64(&g_TscCoreMisses);
        goto Exit;                       /* this core is not virtualized */
    }

    high = registers[3];
    low = registers[0] & MAXUINT32;
    vpData = reinterpret_cast<PVIRTUAL_PROCESSOR_DATA>(high << 32 | low);
    if (!vpData)
    {
        InterlockedIncrement64(&g_TscCoreMisses);
        goto Exit;
    }
    InterlockedIncrement64(&g_TscCoreHits);

    /* 0xFFFFFFFF means "probe": touch nothing after validating the pointer. */
    if (want == 0xFFFFFFFFu)
        goto Exit;

    /* 0xFFFFFFFE means "apply the #BP exception bit only" (9.372/C1). Kept in the
     * same walk as the TSC policy because the same two facts apply: a VMCB bit is
     * per-core state, and only a walk from PASSIVE can write it on every core. */
    if (want == 0xFFFFFFFEu)
    {
        if (InterlockedCompareExchange(&g_BpInt3Intercept, 0, 0))
            vpData->GuestVmcb.ControlArea.InterceptException |= (1u << 3);
        else
            vpData->GuestVmcb.ControlArea.InterceptException &= ~((UINT32)(1u << 3));
        vpData->GuestVmcb.ControlArea.VmcbClean = 0;
        goto Exit;
    }

    vpData->GuestVmcb.ControlArea.TscOffset = yghv_s206_tsc_offset_get();

    if (want)
    {
        vpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_RDTSC;
        vpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_RDTSCP;
    }
    else
    {
        vpData->GuestVmcb.ControlArea.InterceptMisc1 &= ~((UINT32)SVM_INTERCEPT_MISC1_RDTSC);
        vpData->GuestVmcb.ControlArea.InterceptMisc2 &= ~((UINT32)SVM_INTERCEPT_MISC2_RDTSCP);
    }
    vpData->GuestVmcb.ControlArea.VmcbClean = 0;

Exit:
    return STATUS_SUCCESS;
}

/*
 * Walk every core and write the clock policy. PASSIVE (the control plane calls
 * this; SvExecuteOnEachProcessor pins and migrates, which needs PASSIVE).
 *
 * Returns 1 when the walk completed, 0 when it did not. A failed walk leaves
 * SOME cores armed - which is precisely the condition that produced 0x101 - so
 * the caller must treat 0 as "the machine may be about to starve", not as a
 * cosmetic error.
 */
/*
 * 9.370c: A PROBE THAT WALKS BUT DOES NOT ARM.
 *
 * 0x3B at SvTscApplyOnCore+0x36 proved the walk reached a bogus pointer. The fix
 * (returning the VA) is a two-line change whose failure mode is another bugcheck,
 * so it gets verified WITHOUT arming the intercept: this walks every core, reads
 * the VPD pointer, and touches nothing else. If the pointer is wrong it still
 * faults - but the machine has no exit storm to compound it.
 *
 * out: [0]=cores reached  [1]=cores missed  [2]=walk status
 */
extern "C" void yghv_s206_tsc_probe(UINT64 *out)
{
    int ok;
    if (!out)
        return;
    InterlockedExchange64(&g_TscCoreHits, 0);
    InterlockedExchange64(&g_TscCoreMisses, 0);
    ok = yghv_s206_tsc_apply_all(0xFFFFFFFFu);   /* 0xFFFFFFFF = probe only */
    out[0] = (UINT64)InterlockedCompareExchange64(&g_TscCoreHits, 0, 0);
    out[1] = (UINT64)InterlockedCompareExchange64(&g_TscCoreMisses, 0, 0);
    out[2] = (UINT64)(ok ? 1 : 0);
}

extern "C" int yghv_s206_tsc_apply_all(UINT32 arm)
{
    NTSTATUS st = SvExecuteOnEachProcessor(SvTscApplyOnCore,
                                           (PVOID)(ULONG_PTR)(arm ? 1 : 0),
                                           nullptr);
    return NT_SUCCESS(st) ? 1 : 0;
}

/*
 * 9.372 (C1): apply the #BP intercept bit on EVERY core.
 *
 * The prepare-time arming at SvPrepareForVirtualization runs once per core at
 * load, so setting the flag afterwards changed nothing - the same failure B2
 * measured and the same fix: walk the cores from PASSIVE and write the VMCB
 * while pinned to each one.
 *
 * Returns 1 when the walk completed. A partial walk means some cores intercept
 * #BP and others do not, which is both a detection signal and a correctness
 * problem, so the caller must report it rather than assume.
 */
extern "C" int yghv_s206_bp_int3_apply_all(VOID)
{
    /*
     * 9.374: DISABLED. Two restarts (0x139 both times) traced to this walk.
     *
     * 9.370 proved that walking the cores from PASSIVE is the only way to write a
     * per-core VMCB bit, and that it works for the TSC intercept. The difference
     * here is the #BP window: while this walk runs, another process is executing
     * the very page the bit is about to govern. Measured on G42: the holder was
     * spinning on its stub page and `sys-bp-int3 on` was the last line before the
     * machine died - the same 0x139 MISSING_GSFRAME_STACKPTR as the run where the
     * bit had merely survived from an earlier boot.
     *
     * So "walk the cores" is not a general-purpose primitive. It is safe when
     * nothing is executing the state being changed and unsafe otherwise, and
     * nothing in this tree can guarantee the first condition for an arbitrary
     * guest page.
     *
     * The bit therefore stays exactly where the 13/13 suite verified it: set at
     * prepare, from the policy flag, once per core at load. That is enough for the
     * OPT-IN contract - a caller who wants #BP intercepted sets the flag before
     * loading. It is not enough to toggle at runtime, and pretending otherwise
     * has now cost two machines.
     */
    return 0;
}



static
NTSTATUS
SvDevirtualizeProcessor (
    _In_opt_ PVOID Context
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    UINT64 high, low;
    PVIRTUAL_PROCESSOR_DATA vpData;
    PSHARED_VIRTUAL_PROCESSOR_DATA* sharedVpDataPtr;

    if (!ARGUMENT_PRESENT(Context))
    {
        goto Exit;
    }

    //
    // Ask SimpleSVM hypervisor to deactivate itself. If the hypervisor is
    // installed, this ECX is set to 'SSVM', and EDX:EAX indicates an address
    // of per processor data to be freed.
    //
    __cpuidex(registers, CPUID_UNLOAD_SIMPLE_SVM, CPUID_UNLOAD_SIMPLE_SVM);
    /*
     * 9.378: this check now means what it says. It compared against 'SSVM', which
     * the leaf never produced, so this function always returned without doing
     * anything - and SvDevirtualizeAllProcessors, whose whole job is to bring the
     * cores back to bare metal before teardown, was a no-op. See the note at the
     * leaf for the crash that proved it (0x1E at svm_core_ipi_cleanup+0x5F).
     */
    if (registers[2] != 'SSVM')
    {
        SvDebugPrint("SvDevirtualizeProcessor: not virtualized, nothing to do\n");
        goto Exit;
    }

    SvDebugPrint("The processor has been de-virtualized.\n");

    //
    // Get an address of per processor data indicated by EDX:EAX.
    //
    high = registers[3];
    low = registers[0] & MAXUINT32;
    vpData = reinterpret_cast<PVIRTUAL_PROCESSOR_DATA>(high << 32 | low);
    NT_ASSERT(vpData->HostStackLayout.Reserved1 == MAXUINT64);

    //
    // Save an address of shared data, then free per processor data.
    //
    sharedVpDataPtr = static_cast<PSHARED_VIRTUAL_PROCESSOR_DATA*>(Context);
    *sharedVpDataPtr = vpData->HostStackLayout.SharedVpData;
    SvFreePageAlingedPhysicalMemory(vpData);

Exit:
    return STATUS_SUCCESS;
}

/*!
    @brief      De-virtualize all virtualized processors.

    @details    This function execute a callback to de-virtualize a processor on
                all processors, and frees shared data when the callback returned
                its pointer from a hypervisor.
 */
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
static
VOID
SvDevirtualizeAllProcessors (
    VOID
    )
{
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;

    sharedVpData = nullptr;

    //
    // De-virtualize all processors and free shared data when returned.
    //
    NT_VERIFY(NT_SUCCESS(SvExecuteOnEachProcessor(SvDevirtualizeProcessor,
                                                  &sharedVpData,
                                                  nullptr)));
    if (sharedVpData != nullptr)
    {
        SvFreeContiguousMemory(sharedVpData->MsrPermissionsMap);
        SvFreePageAlingedPhysicalMemory(sharedVpData);
    }
}

/*!
    @brief          Build the MSR permissions map (MSRPM).

    @details        This function sets up MSRPM to intercept to IA32_MSR_EFER,
                    as suggested in "Extended Feature Enable Register (EFER)"
                    ----
                    Secure Virtual Machine Enable (SVME) Bit
                    Bit 12, read/write. Enables the SVM extensions. (...) The
                    effect of turning off EFER.SVME while a guest is running is
                    undefined; therefore, the VMM should always prevent guests
                    from writing EFER.
                    ----

                    Each MSR is controlled by two bits in the MSRPM. The LSB of
                    the two bits controls read access to the MSR and the MSB
                    controls write access. A value of 1 indicates that the
                    operation is intercepted. This function locates an offset for
                    IA32_MSR_EFER and sets the MSB bit. For details of logic, see
                    "MSR Intercepts".

    @param[in,out]  MsrPermissionsMap - The MSRPM to set up.
 */
_IRQL_requires_same_
static
VOID
SvBuildMsrPermissionsMap (
    _Inout_ PVOID MsrPermissionsMap
    )
{
    constexpr UINT32 BITS_PER_MSR = 2;
    constexpr UINT32 SECOND_MSR_RANGE_BASE = 0xc0000000;
    constexpr UINT32 SECOND_MSRPM_OFFSET = 0x800 * CHAR_BIT;
    RTL_BITMAP bitmapHeader;
    ULONG offsetFrom2ndBase, offset;

    //
    // Setup and clear all bits, indicating no MSR access should be intercepted.
    //
    RtlInitializeBitMap(&bitmapHeader,
                        static_cast<PULONG>(MsrPermissionsMap),
                        SVM_MSR_PERMISSIONS_MAP_SIZE * CHAR_BIT
                        );
    RtlClearAllBits(&bitmapHeader);

    //
    // Compute an offset from the second MSR permissions map offset (0x800) for
    // IA32_MSR_EFER in bits. Then, add an offset until the second MSR
    // permissions map.
    //
    offsetFrom2ndBase = (IA32_MSR_EFER - SECOND_MSR_RANGE_BASE) * BITS_PER_MSR;
    offset = SECOND_MSRPM_OFFSET + offsetFrom2ndBase;

    //
    // Set the MSB bit indicating write accesses to the MSR should be intercepted.
    //
    RtlSetBits(&bitmapHeader, offset + 1, 1);
}

/*!
    @brief      Build pass-through style page tables used in nested paging.

    @details    This function build page tables used in Nested Page Tables. The
                page tables are used to translate from a guest physical address
                to a system physical address and pointed by the NCr3 field of
                VMCB, like the traditional page tables are pointed by CR3.

                The nested page tables built in this function are set to
                translate a guest physical address to the same system physical
                address. For example, guest physical address 0x1000 is
                translated into system physical address 0x1000.

                In order to save memory to build nested page tables, 2MB large
                pages are used (as opposed to the standard pages that describe
                translation only for 4K granularity. Also, only up to 1 TB of
                translation is built. 1GB huge pages are not used due to VMware
                not supporting this feature.

    @param[out] SharedVpData - Out buffer to build nested page tables.
 */
_IRQL_requires_same_
static
VOID
SvBuildNestedPageTables (
    _Out_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData
    )
{
    ULONG64 pdptBasePa, pdBasePa, translationPa;

    //
    // Build only two PML4 entries. Those entries have subtables that control up to
    // 1 TB physical memory. PFN points to a base physical address of the page
    // directory pointer table.
    //
    for (ULONG64 pml4Index = 0; pml4Index < 2; pml4Index++) {
        PPML4_ENTRY_2MB pml4e = &SharedVpData->Pml4Entries[pml4Index];
        PPML4E_TREE pml4eTree = &SharedVpData->Pml4eTrees[pml4Index];

        //
        // Set the US (User) bit of all nested page table entries to be translated
        // without #VMEXIT, as all guest accesses are treated as user accesses at
        // the nested level. Also, the RW (Write) bit of nested page table entries
        // that corresponds to guest page tables must be 1 since all guest page
        // table accesses are threated as write access. See "Nested versus Guest
        // Page Faults, Fault Ordering" for more details.
        //
        // Those settings do not lower security since permission checks are done
        // twice independently: based on guest page tables, and nested page tables.
        // See "Nested versus Guest Page Faults, Fault Ordering" for more details.
        //
        pdptBasePa = MmGetPhysicalAddress(&pml4eTree->PdptEntries).QuadPart;
        pml4e->Fields.PageFrameNumber = pdptBasePa >> PAGE_SHIFT;
        pml4e->Fields.Valid = 1;
        pml4e->Fields.Write = 1;
        pml4e->Fields.User = 1;

        //
        // One PML4 entry controls 512 page directory pointer entires.
        //
        for (ULONG64 pdptIndex = 0; pdptIndex < 512; pdptIndex++)
        {
            //
            // PFN points to a base physical address of the page directory table.
            //
            pdBasePa = MmGetPhysicalAddress(&pml4eTree->PdEntries[pdptIndex][0]).QuadPart;
            pml4eTree->PdptEntries[pdptIndex].Fields.PageFrameNumber = pdBasePa >> PAGE_SHIFT;
            pml4eTree->PdptEntries[pdptIndex].Fields.Valid = 1;
            pml4eTree->PdptEntries[pdptIndex].Fields.Write = 1;
            pml4eTree->PdptEntries[pdptIndex].Fields.User = 1;

            //
            // One page directory entry controls 512 page directory entries.
            //
            // We do not explicitly configure PAT in the NPT entry. The consequences
            // of this are: 1) pages whose PAT (Page Attribute Table) type is the
            // Write-Combining (WC) memory type could be treated as the
            // Write-Combining Plus (WC+) while it should be WC when the MTRR type is
            // either Write Protect (WP), Writethrough (WT) or Writeback (WB), and
            // 2) pages whose PAT type is Uncacheable Minus (UC-) could be treated
            // as Cache Disabled (CD) while it should be WC, when MTRR type is WC.
            //
            // While those are not desirable, this is acceptable given that 1) only
            // introduces additional cache snooping and associated performance
            // penalty, which would not be significant since WC+ still lets
            // processors combine multiple writes into one and avoid large
            // performance penalty due to frequent writes to memory without caching.
            // 2) might be worse but I have not seen MTRR ranges configured as WC
            // on testing, hence the unintentional UC- will just results in the same
            // effective memory type as what would be with UC.
            //
            // See "Memory Types" (7.4), for details of memory types,
            // "PAT-Register PA-Field Indexing", "Combining Guest and Host PAT Types",
            // and "Combining PAT and MTRR Types" for how the effective memory type
            // is determined based on Guest PAT type, Host PAT type, and the MTRR
            // type.
            //
            // The correct approach may be to look up the guest PTE and copy the
            // caching related bits (PAT, PCD, and PWT) when constructing NTP
            // entries for non RAM regions, so the combined PAT will always be the
            // same as the guest PAT type. This may be done when any issue manifests
            // with the current implementation.
            //
            for (ULONG64 pdIndex = 0; pdIndex < 512; pdIndex++)
            {
                //
                // PFN points to a base physical address of system physical address
                // to be translated from a guest physical address. Set the PS
                // (LargePage) bit to indicate that this is a large page and no
                // subtable exists.
                //
                translationPa = (pml4Index * 512 * 512) + (pdptIndex * 512) + pdIndex;
                pml4eTree->PdEntries[pdptIndex][pdIndex].Fields.PageFrameNumber = translationPa;
                pml4eTree->PdEntries[pdptIndex][pdIndex].Fields.Valid = 1;
                pml4eTree->PdEntries[pdptIndex][pdIndex].Fields.Write = 1;
                pml4eTree->PdEntries[pdptIndex][pdIndex].Fields.User = 1;
                pml4eTree->PdEntries[pdptIndex][pdIndex].Fields.LargePage = 1;
            }
        }
    }
}

/*!
    @brief      Test whether the current processor support the SVM feature.

    @details    This function tests whether the current processor has enough
                features to run SimpleSvm, especially about SVM features.

    @result     TRUE if the processor supports the SVM feature; otherwise, FALSE.
 */
_IRQL_requires_same_
_Check_return_
static
BOOLEAN
SvIsSvmSupported (
    VOID
    )
{
    BOOLEAN svmSupported;
    int registers[4];   // EAX, EBX, ECX, and EDX
    ULONG64 vmcr;

    svmSupported = FALSE;

    //
    // Test if the current processor is AMD one. An AMD processor should return
    // "AuthenticAMD" from CPUID function 0. See "Function 0h-Maximum Standard
    // Function Number and Vendor String".
    //
    __cpuid(registers, CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING);
    if ((registers[1] != 'htuA') ||
        (registers[3] != 'itne') ||
        (registers[2] != 'DMAc'))
    {
        goto Exit;
    }

    //
    // Test if the SVM feature is supported by the current processor. See
    // "Enabling SVM" and "CPUID Fn8000_0001_ECX Feature Identifiers".
    //
    __cpuid(registers, CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX);
    if ((registers[2] & CPUID_FN8000_0001_ECX_SVM) == 0)
    {
        goto Exit;
    }

    //
    // Test if the Nested Page Tables feature is supported by the current
    // processor. See "Enabling Nested Paging" and "CPUID Fn8000_000A_EDX SVM
    // Feature Identification".
    //
    __cpuid(registers, CPUID_SVM_FEATURES);
    if ((registers[3] & CPUID_FN8000_000A_EDX_NP) == 0)
    {
        goto Exit;
    }

    //
    // Test if the SVM feature can be enabled. When VM_CR.SVMDIS is set,
    // EFER.SVME cannot be 1; therefore, SVM cannot be enabled. When
    // VM_CR.SVMDIS is clear, EFER.SVME can be written normally and SVM can be
    // enabled. See "Enabling SVM".
    //
    vmcr = __readmsr(SVM_MSR_VM_CR);
    if ((vmcr & SVM_VM_CR_SVMDIS) != 0)
    {
        goto Exit;
    }

    svmSupported = TRUE;

Exit:
    return svmSupported;
}

/* ==========================================================================
 * 9.443: ONE SYSTEM THREAD PER CORE, BECAUSE THE VMRUN FRAME MUST OWN ITS STACK.
 * ==========================================================================
 *
 * WHY THE OLD MODEL CANNOT WORK ANY MORE
 *
 * 9.442 established that a guest which is the caller's continuation EXECUTES THE
 * FUNCTION'S `return`, so control flows back out of SvVirtualizeProcessor and
 * SvExecuteOnEachProcessor advances to the next core - while the vmrun frame stays
 * suspended forever. With the guest replaced by our own stub (9.443) that failure
 * mode is gone, but a second one appears immediately: Sv206LaunchVm no longer
 * returns AT ALL. So SvExecuteOnEachProcessor, a plain for-loop on ONE thread,
 * would block on the first core and never virtualise the other eleven.
 *
 * The fix is the model the rest of this tree already uses for residents
 * (yghv_os_guest_thread in main.c): one system thread per core, each pinned with
 * KeSetSystemAffinityThread, each calling SvVirtualizeProcessor on its own. Then:
 *
 *   - every core's VMRUN frame is suspended on ITS OWN thread stack;
 *   - no thread is shared between cores, so advancing to "the next core" cannot
 *     strand a frame belonging to a thread that has moved on;
 *   - when the stop flag is seen, that core's SvHandleVmExit sets ExitVm, SvLV20
 *     resumes the stub, the stub parks, and the thread is free to terminate;
 *   - Sv206Entry returns as soon as all threads have been CREATED, which is what
 *     "the hypervisor is up" means for this design.
 *
 * DriverUnload sets the stop flag and waits for g_VmrunStopped to reach the core
 * count - a number that now actually moves (see the VMMCALL case). If the wait
 * times out the VMCB/NPT guard still refuses to free, so a stuck core leaks rather
 * than being handed freed pages.
 */
typedef struct _SV206_CORE_THREAD_CONTEXT
{
    PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData;
    ULONG CoreIndex;
    volatile LONG Status;
} SV206_CORE_THREAD_CONTEXT, *PSV206_CORE_THREAD_CONTEXT;

/*
 * 9.445: A NOINLINE WRAPPER, SO THE CALL INTO THE LOOP IS A REAL CALL.
 *
 * SvVirtualizeProcessor is a large static function and the optimiser would otherwise
 * be free to inline it into Sv206CoreThread. It does not matter for the exit path any
 * more (Sv206LaunchVm records the stack itself), but keeping this as a distinct frame
 * makes the call chain in a crash dump readable: Sv206CoreThread -> Sv206VirtualizeC
 * -> SvVirtualizeProcessor -> Sv206LaunchVm.
 */
__declspec(noinline)
static LONG Sv206VirtualizeC(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData)
{
    return (LONG)SvVirtualizeProcessor(SharedVpData);
}

static VOID Sv206CoreThread(PVOID Context)
{
    PSV206_CORE_THREAD_CONTEXT ctx =
        static_cast<PSV206_CORE_THREAD_CONTEXT>(Context);

    KeSetSystemAffinityThread((KAFFINITY)(1ULL << ctx->CoreIndex));

    /*
     * 9.445: ENTER THE LOOP DIRECTLY. NO TRAMPOLINE, NO CAPTURED STACK.
     *
     * Sv206LaunchVm now records this thread's real RSP itself, before it switches to
     * the per-core host stack - that is the one place where both facts are in hand at
     * once. Nothing here has to compute or pass a stack pointer, which is what four
     * earlier attempts got wrong in four different ways (0x1E / 0x0E / 0x50 / 0xF7).
     *
     * This call does not return on the normal path: Sv206LaunchVm is "should never
     * return to here". After a stop, Sv206LeaveToHost ends the thread from ASM, on
     * this thread's own stack, without unwinding this frame.
     *
     * It DOES return on the early-out path (hypervisor already installed) and on
     * allocation failure, so the status is recorded and the thread ends normally.
     */
    ctx->Status = Sv206VirtualizeC(ctx->SharedVpData);

    InterlockedIncrement(&g_S206CoresExited);

    /*
     * Reached only on the early-out / error paths above. Terminate through a volatile
     * pointer so the compiler keeps this frame: written directly, the optimiser emits
     * a tail call (`add rsp,20h ; pop rbx ; jmp PsTerminateSystemThread`), and if the
     * callee ever returns there is no frame left and the thread dies inside
     * KiStartSystemThread.
     */
    {
        typedef NTSTATUS (*PFN_TERMINATE_THREAD)(NTSTATUS);
        static PFN_TERMINATE_THREAD volatile terminateFn = &PsTerminateSystemThread;

        terminateFn(STATUS_SUCCESS);
    }

    /* Only reached if the call above returned, which it should not. */
    for (;;)
    {
        LARGE_INTEGER delay;

        delay.QuadPart = -10000;    /* 1 ms, relative */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
}

/*!
    @brief      Virtualizes all processors on the system.

    @details    This function attempts to virtualize all processors on the
                system, and returns STATUS_SUCCESS if all processors are
                successfully virtualized. If any processor is not virtualized,
                this function de-virtualizes all processors and returns an error
                code.

    @result     STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
NTSTATUS
SvVirtualizeAllProcessors (
    VOID
    )
{
    NTSTATUS status;
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;
    ULONG numOfProcessorsCompleted;

    sharedVpData = nullptr;
    numOfProcessorsCompleted = 0;

    //
    // Test whether the current processor supports all required SVM features. If
    // not, exit as error.
    //
    if (SvIsSvmSupported() == FALSE)
    {
        SvDebugPrint("SVM is not fully supported on this processor.\n");
        status = STATUS_HV_FEATURE_UNAVAILABLE;
        goto Exit;
    }

    //
    // Allocate a data structure shared across all processors. This data is
    // page tables used for Nested Page Tables.
    //
#pragma prefast(suppress : __WARNING_MEMORY_LEAK, "Ownership is taken on success.")
    sharedVpData = static_cast<PSHARED_VIRTUAL_PROCESSOR_DATA>(
        SvAllocatePageAlingedPhysicalMemory(sizeof(SHARED_VIRTUAL_PROCESSOR_DATA)));
    if (sharedVpData == nullptr)
    {
        SvDebugPrint("Insufficient memory.\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Allocate MSR permissions map (MSRPM) onto contiguous physical memory.
    //
    sharedVpData->MsrPermissionsMap = SvAllocateContiguousMemory(
                                                    SVM_MSR_PERMISSIONS_MAP_SIZE);
    if (sharedVpData->MsrPermissionsMap == nullptr)
    {
        SvDebugPrint("Insufficient memory.\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Build nested page table and MSRPM.
    //
#if !defined(YGHV_206B_GNPT)
    SvBuildNestedPageTables(sharedVpData);
#endif
    SvBuildMsrPermissionsMap(sharedVpData->MsrPermissionsMap);

    //
    // 9.443: ONE THREAD PER CORE. See the note at Sv206CoreThread for why the old
    // SvExecuteOnEachProcessor(SvVirtualizeProcessor, ...) cannot work once the
    // guest is a stub: that guest never returns, so a single-threaded for-loop
    // would stop at core 0 and leave the rest unvirtualised.
    //
    // Threads are CREATED here and not joined: each one lives inside its own VMRUN
    // loop for as long as the hypervisor is up. DriverUnload stops them by setting
    // g_VmrunStopRequested and waiting on g_VmrunStopped.
    //
    {
        ULONG numOfProcessors = yghv_s206_core_limit(
            KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS));   /* 9.525 */
        ULONG created = 0;

        if (numOfProcessors > SVM206_MAX_THREADS)
        {
            numOfProcessors = SVM206_MAX_THREADS;
        }

        for (ULONG i = 0; i < numOfProcessors; i++)
        {
            PROCESSOR_NUMBER processorNumber;
            HANDLE threadHandle = nullptr;
            NTSTATUS st;

            st = KeGetProcessorNumberFromIndex(i, &processorNumber);
            if (!NT_SUCCESS(st))
            {
                status = st;
                goto Exit;
            }

            /*
             * The context has to outlive the thread's use of it, and the thread
             * never returns in the normal case - so it is allocated, not stacked.
             * One small nonpaged block per core, leaked only if the thread never
             * exits, which is the same lifetime as the VMRUN loop itself.
             */
            PSV206_CORE_THREAD_CONTEXT ctx =
                static_cast<PSV206_CORE_THREAD_CONTEXT>(
                    ExAllocatePoolWithTag(NonPagedPool,
                                          sizeof(SV206_CORE_THREAD_CONTEXT),
                                          '2CvS'));
            if (ctx == nullptr)
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
                goto Exit;
            }
            RtlZeroMemory(ctx, sizeof(*ctx));
            ctx->SharedVpData = sharedVpData;
            ctx->CoreIndex = i;
            ctx->Status = (LONG)STATUS_PENDING;

            st = PsCreateSystemThread(&threadHandle,
                                      THREAD_ALL_ACCESS,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      Sv206CoreThread,
                                      ctx);
            if (!NT_SUCCESS(st))
            {
                ExFreePoolWithTag(ctx, '2CvS');
                status = st;
                goto Exit;
            }

            /*
             * The handle is kept so DriverUnload can wait on the thread object if
             * it ever needs to; the counter g_VmrunStopped is the primary signal.
             */
            g_S206CoreThreads[i] = threadHandle;

            created += 1;
            InterlockedIncrement(&g_S206CoresStarted);
        }

        numOfProcessorsCompleted = created;
        if (created == 0)
        {
            status = STATUS_UNSUCCESSFUL;
            goto Exit;
        }
        status = STATUS_SUCCESS;
    }

Exit:
    if (!NT_SUCCESS(status))
    {
        //
        // On failure, after successful allocation of shared data.
        //
        if (numOfProcessorsCompleted != 0)
        {
            //
            // If one or more processors have already been virtualized,
            // de-virtualize any of those processors, and free shared data.
            //
            NT_ASSERT(sharedVpData != nullptr);
            SvDevirtualizeAllProcessors();
        }
        else
        {
            //
            // If none of processors has not been virtualized, simply free
            // shared data.
            //
            if (sharedVpData != nullptr)
            {
                if (sharedVpData->MsrPermissionsMap != nullptr)
                {
                    SvFreeContiguousMemory(sharedVpData->MsrPermissionsMap);
                }
                SvFreePageAlingedPhysicalMemory(sharedVpData);
            }
        }
    }
    return status;
}

/*!
    @brief      An entry point of this driver.

    @param[in]  DriverObject - A driver object.
    @param[in]  RegistryPath - Unused.

    @result     STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_Use_decl_annotations_
EXTERN_C
NTSTATUS
Sv206Entry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    UNICODE_STRING objectName;
    OBJECT_ATTRIBUTES objectAttributes;
    PCALLBACK_OBJECT callbackObject;
    PVOID callbackRegistration;

    UNREFERENCED_PARAMETER(RegistryPath);

    SV_DEBUG_BREAK();

    callbackRegistration = nullptr;
    DriverObject->DriverUnload = SvDriverUnload;

    //
    // Opts-in no-execute (NX) nonpaged pool when available for security. By
    // defining POOL_NX_OPTIN as 1 and calling this function, nonpaged pool
    // allocation by the ExAllocatePool family with the NonPagedPool flag
    // automatically allocates NX nonpaged pool on Windows 8 and later versions
    // of Windows, while on Windows 7 where NX nonpaged pool is unsupported,
    // executable nonpaged pool is returned as usual.
    //
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    //
    // Registers a power state callback (SvPowerCallbackRoutine) to handle
    // system sleep and resume to manage virtualization state.
    //
    // First, opens the \Callback\PowerState callback object provides
    // notification regarding power state changes. This is a system defined
    // callback object that was already created by Windows. To open a system
    // defined callback object, the Create parameter of ExCreateCallback must be
    // FALSE (and AllowMultipleCallbacks is ignore when the Create parameter is
    // FALSE).
    //
    objectName = RTL_CONSTANT_STRING(L"\\Callback\\PowerState");
    objectAttributes = RTL_CONSTANT_OBJECT_ATTRIBUTES(&objectName,
                                                      OBJ_CASE_INSENSITIVE);
    status = ExCreateCallback(&callbackObject, &objectAttributes, FALSE, TRUE);
    if (!NT_SUCCESS(status))
    {
        SvDebugPrint("Failed to open the power state callback object.\n");
        goto Exit;
    }

    //
    // Then, registers our callback. The open callback object must be
    // dereferenced.
    //
    callbackRegistration = ExRegisterCallback(callbackObject,
                                              SvPowerCallbackRoutine,
                                              nullptr);
    ObDereferenceObject(callbackObject);
    if (callbackRegistration == nullptr)
    {
        SvDebugPrint("Failed to register a power state callback.\n");
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    //
    // Virtualize all processors on the system.
    //
    status = SvVirtualizeAllProcessors();

Exit:
    if (NT_SUCCESS(status))
    {
        //
        // On success, save the registration handle for un-registration.
        //
        NT_ASSERT(callbackRegistration);
        g_PowerCallbackRegistration = callbackRegistration;
    }
    else
    {
        //
        // On any failure, clean up stuff as needed.
        //
        if (callbackRegistration != nullptr)
        {
            ExUnregisterCallback(callbackRegistration);
        }
    }
    return status;
}

/*!
    @brief      Driver unload callback.

    @details    This function de-virtualize all processors on the system.

    @param[in]  DriverObject - Unused.
 */
_Use_decl_annotations_
static
VOID
SvDriverUnload (
    PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    SV_DEBUG_BREAK();

    //
    // Unregister the power state callback.
    //
    NT_ASSERT(g_PowerCallbackRegistration);
    ExUnregisterCallback(g_PowerCallbackRegistration);

    //
    // De-virtualize all processors on the system.
    //
    SvDevirtualizeAllProcessors();
}

/*!
    @brief      9.246 step206-B coexist unload hook.

    @details    Called from YuanGuardHV's own DriverUnload when
                YGHV_206B_COEXIST is built (main.c re-registers ITS DriverUnload
                after Sv206Entry, so SvDriverUnload above is never reached by
                the SCM). Performs upstream's unload sequence — unregister the
                power callback, then devirtualize every processor via the
                ring-0 CPUID backdoor — after which yghv's DriverUnload
                continues with its own teardown (control device, NPT, SVM).
 */
/*
 * 9.399: release the power callback WITHOUT walking the cores.
 *
 * DriverUnload needs this half even when the stop flag already stopped every
 * VMRUN loop, because ExUnregisterCallback has nothing to do with the cores. The
 * walk is the other half, and it is only safe when a core failed to stop.
 */
extern "C" int yghv_s206_power_callback_release(VOID)
{
    if (g_PowerCallbackRegistration != nullptr) {
        ExUnregisterCallback(g_PowerCallbackRegistration);
        g_PowerCallbackRegistration = nullptr;
    }
    return 1;
}

extern "C"
VOID
Sv206CoopUnload (
    VOID
    )
{
    if (g_PowerCallbackRegistration != nullptr) {
        ExUnregisterCallback(g_PowerCallbackRegistration);
        g_PowerCallbackRegistration = nullptr;
    }
    SvDevirtualizeAllProcessors();
}

/*!
    @brief      PowerState callback routine.

    @details    This function de-virtualize all processors when the system is
                exiting system power state S0 (ie, the system is about to sleep
                etc), and virtualize all processors when the system has just
                reentered S0 (ie, the system has resume from sleep etc).

                Those operations are required because virtualization is cleared
                during sleep.

                For the meanings of parameters, see ExRegisterCallback in MSDN.

    @param[in]  CallbackContext - Unused.
    @param[in]  Argument1 - A PO_CB_XXX constant value.
    @param[in]  Argument2 - A value of TRUE or FALSE.
 */
_Use_decl_annotations_
static
VOID
SvPowerCallbackRoutine (
    PVOID CallbackContext,
    PVOID Argument1,
    PVOID Argument2
    )
{
    UNREFERENCED_PARAMETER(CallbackContext);

    //
    // PO_CB_SYSTEM_STATE_LOCK of Argument1 indicates that a system power state
    // change is imminent.
    //
    if (Argument1 != reinterpret_cast<PVOID>(PO_CB_SYSTEM_STATE_LOCK))
    {
        goto Exit;
    }

    if (Argument2 != FALSE)
    {
        //
        // The system has just reentered S0. Re-virtualize all processors.
        //
        NT_VERIFY(NT_SUCCESS(SvVirtualizeAllProcessors()));
    }
    else
    {
        //
        // The system is about to exit system power state S0. De-virtualize all
        // processors.
        //
        SvDevirtualizeAllProcessors();
    }

Exit:
    return;
}
