/*!
    @file       SimpleSvm.cpp

    @brief      All C code.

    @author     Satoshi Tanda

    @copyright  Copyright (c) 2017-2020, Satoshi Tanda. All rights reserved.
 */
#define POOL_NX_OPTIN   1
#include "common/npt.h"   /* NPT_PERM_* / NPT_4K_PAGE_FLAGS */
#include "svm_simplevm206.hpp"

#include <intrin.h>
#include <ntifs.h>
#include <stdarg.h>

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
/* Tier 4.4: re-arm an instruction-level breakpoint page (PASSIVE). */
extern int yghv_bp_rearm_bare(UINT64 gpa);
/* Tier 4.4: change one page's NPT permission (PASSIVE). */
extern int yghv_protect_npt_set_perm(UINT64 gpa, UINT64 perm);
/* Tier 4.4 (9.327, INT3 mode): #BP handling + the byte we displaced. */
extern int yghv_bp_on_int3(UINT64 va, UINT32 cpl, UINT64 cr3);
extern int yghv_bp_int3_orig(UINT64 va, UINT8 *out);
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
        yghv_trace_u64("s206 vr count", (UINT64)g_S206VrIdx);
        for (ULONG q = 0; q * 2 < g_S206VrIdx; q++) {
            yghv_trace_u64("s206 vr gpa", g_S206VrLog[q * 2]);
            yghv_trace_u64("s206 vr verdict", g_S206VrLog[q * 2 + 1]);
        }
        g_S206VrIdx = 0;
    }
    /* 9.255: #DB handler hits; 9.258: +DR6 */
    if (g_S206DbgIdx == 0)
        yghv_trace("s206 dbg-log empty");
    else {
        yghv_trace_u64("s206 dbg count", (UINT64)g_S206DbgIdx);
        for (ULONG q = 0; q * 4 < g_S206DbgIdx; q++) {
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
    yghv_trace_u64("s206 deny count", (UINT64)g_S206DenyIdx);
    for (ULONG q = 0; q * 6 < g_S206DenyIdx && q < 32; q++) {
        yghv_trace_u64("s206 deny cr3", g_S206DenyLog[q * 6]);
        yghv_trace_u64("s206 deny gpa", g_S206DenyLog[q * 6 + 1]);
        yghv_trace_u64("s206 deny cpl", g_S206DenyLog[q * 6 + 2]);
        yghv_trace_u64("s206 deny gva", g_S206DenyLog[q * 6 + 3]);
        yghv_trace_u64("s206 deny rip", g_S206DenyLog[q * 6 + 4]);
        yghv_trace_u64("s206 deny aux", g_S206DenyLog[q * 6 + 5]);
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
static_assert(sizeof(VIRTUAL_PROCESSOR_DATA) == KERNEL_STACK_SIZE + PAGE_SIZE * 3,
              "VIRTUAL_PROCESSOR_DATA Size Mismatch");

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

#define EFER_SVME       (1UL << 12)

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

    // YGHV C0 local patch (2026-09-18): Win10 19045 ntoskrnl has no
    // ExAllocatePool2 export (Win11+). With-tag + zero preserves semantics.
    memory = ExAllocatePoolWithTag(NonPagedPool, NumberOfBytes, 'MVSS');
    if (memory != nullptr)
    {
        NT_ASSERT(PAGE_ALIGN(memory) == memory);
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
    ExFreePoolWithTag(BaseAddress, 'MVSS');
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

    switch (leaf)
    {
    case CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS:
        //
        // Indicate presence of a hypervisor by setting the bit that are
        // reserved for use by hypervisor to indicate guest status. See "CPUID
        // Fn0000_0001_ECX Feature Identifiers".
        //
        registers[2] |= CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT;
        break;
    case CPUID_HV_VENDOR_AND_MAX_FUNCTIONS:
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
        if (subLeaf == CPUID_FAKE_VPD)
        {
            attribute.AsUInt16 = VpData->GuestVmcb.StateSaveArea.SsAttrib;
            if (attribute.Fields.Dpl == DPL_SYSTEM)
            {
                PHYSICAL_ADDRESS vpdPa;
                vpdPa = MmGetPhysicalAddress(VpData);
                registers[3] = static_cast<int>(vpdPa.HighPart);
                registers[0] = static_cast<int>(vpdPa.LowPart);
            }
        }
        break;
    case CPUID_UNLOAD_SIMPLE_SVM:
        if (subLeaf == CPUID_UNLOAD_SIMPLE_SVM)
        {
            //
            // Unload itself if the request is from the kernel mode.
            //
            attribute.AsUInt16 = VpData->GuestVmcb.StateSaveArea.SsAttrib;
            if (attribute.Fields.Dpl == DPL_SYSTEM)
            {
                GuestContext->ExitVm = TRUE;
            }
        }
        break;
    default:
        break;
    }

    //
    // Update guest's GPRs with results.
    //
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

    guestContext.VpRegs = GuestRegisters;
    guestContext.ExitVm = FALSE;

    //
    // Load some host state that are not loaded on #VMEXIT.
    //
    __svm_vmload(VpData->HostStackLayout.HostVmcbPa);

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
            if (writeFault && g_S206NpfIdx < 32) {
                g_S206NpfLog[g_S206NpfIdx * 3] = curCr3;
                g_S206NpfLog[g_S206NpfIdx * 3 + 1] = gpa;
                g_S206NpfLog[g_S206NpfIdx * 3 + 2] =
                    VpData->GuestVmcb.StateSaveArea.Cpl;
                g_S206NpfIdx++;
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
                                      VpData->GuestVmcb.ControlArea.NRip,
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
                UINT64 bpRip = VpData->GuestVmcb.ControlArea.NRip;
                if (yghv_bp_on_npf(gpa, bpRip, 0x2,
                                   (UINT32)VpData->GuestVmcb.StateSaveArea.Cpl,
                                   curCr3)) {
                    if (!yghv_protect_npt_set_perm(
                            gpa & ~(UINT64)0xFFFULL,
                            NPT_PERM_PRESENT | NPT_PERM_WRITABLE)) {
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
                if (g_S206VrIdx + 2 <= 64) {
                    g_S206VrLog[g_S206VrIdx] = gpa;
                    g_S206VrLog[g_S206VrIdx + 1] =
                        (UINT64)vr |
                        ((VpData->GuestVmcb.StateSaveArea.Rflags &
                          0x100ULL) << 3);
                    g_S206VrIdx += 2;
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
                    if (g_S206DenyIdx + 6 <= 192) {
                        g_S206DenyLog[g_S206DenyIdx] = curCr3;
                        g_S206DenyLog[g_S206DenyIdx + 1] = gpa;
                        g_S206DenyLog[g_S206DenyIdx + 2] =
                            VpData->GuestVmcb.StateSaveArea.Cpl;
                        g_S206DenyLog[g_S206DenyIdx + 3] =
                            haveGva ? gva : (0xDEAD0000ULL | dReason);
                        g_S206DenyLog[g_S206DenyIdx + 4] =
                            VpData->GuestVmcb.StateSaveArea.Rip;
                        g_S206DenyLog[g_S206DenyIdx + 5] = dAux;
                        g_S206DenyIdx += 6;
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
                /* unexpected CR index (we only intercept 3 and 4) */
                S206RecordFatal(VpData->GuestVmcb.ControlArea.ExitCode,
                                rip, newCr);
                KeBugCheckEx(0xE2, 0x20602,
                             VpData->GuestVmcb.ControlArea.ExitCode,
                             rip, newCr);
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
                if (bpGpa)
                    (void)yghv_pa_write(bpGpa, &origByte, 1);
                VpData->GuestVmcb.StateSaveArea.Rflags |= 0x100ULL; /* TF */
                VpData->GuestVmcb.ControlArea.GuestPaOfGhcb = bpVa | 2ULL;
                VpData->GuestVmcb.ControlArea.TlbControl = 1;
                break;
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
            if (g_S206DbgIdx + 4 <= 128) {
                g_S206DbgLog[g_S206DbgIdx] =
                    VpData->GuestVmcb.StateSaveArea.Rip;
                g_S206DbgLog[g_S206DbgIdx + 1] =
                    VpData->GuestVmcb.ControlArea.GuestPaOfGhcb;
                g_S206DbgLog[g_S206DbgIdx + 2] =
                    VpData->GuestVmcb.StateSaveArea.Rflags;
                g_S206DbgLog[g_S206DbgIdx + 3] =
                    VpData->GuestVmcb.StateSaveArea.Dr6;
                g_S206DbgIdx += 4;
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
                    if (int3Gpa) {
                        UINT8 cc = 0xCC;
                        (void)yghv_pa_write(int3Gpa, &cc, 1);
                    }
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
    case VMEXIT_SHUTDOWN /* 0x7f */:
        //
        // 9.250c: the guest triple-faulted (SHUTDOWN state). With SHUTDOWN
        // intercepted this becomes an OBSERVABLE bug check carrying the last
        // guest RIP — never the silent instant reset.
        //
        S206RecordFatal(VMEXIT_SHUTDOWN,
                        VpData->GuestVmcb.StateSaveArea.Rip,
                        VpData->HostStackLayout.Padding1);
        KeBugCheckEx(0xE2, 0x20601, VMEXIT_SHUTDOWN,
                     VpData->GuestVmcb.StateSaveArea.Rip,
                     VpData->HostStackLayout.Padding1);
        break;
    default:
        // 9.250c: an unhandled exit code must be OBSERVABLE (bugcheck with the
        // exit info) — a plain KeBugCheck here is indistinguishable from the
        // silent-reset failure mode. The 12:12 crash (0xE2 zero-params) was
        // exactly this path with ExitCode=0x13.
        S206RecordFatal(VpData->GuestVmcb.ControlArea.ExitCode,
                        VpData->GuestVmcb.StateSaveArea.Rip,
                        VpData->GuestVmcb.ControlArea.ExitInfo1);
        KeBugCheckEx(0xE2, 0x20600,
                     VpData->GuestVmcb.ControlArea.ExitCode,
                     VpData->GuestVmcb.StateSaveArea.Rip,
                     VpData->GuestVmcb.ControlArea.ExitInfo1);
    }

    //
    // Again, no effect to change IRQL but restoring it here since a #VMEXIT
    // handler where the developers most likely call the kernel API inadvertently
    // is already executed.
    //
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
        guestContext.VpRegs->Rcx = VpData->GuestVmcb.StateSaveArea.Rsp;
        guestContext.VpRegs->Rdx = reinterpret_cast<UINT64>(VpData) >> 32;

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
    VpData->GuestVmcb.ControlArea.InterceptException |= (1u << 1);
    // 9.250c: intercept SHUTDOWN (bit 31) — a guest triple fault then becomes
    // an observable VMEXIT (handler bugchecks with the state) instead of the
    // CPU dying silently = the instant-reset failure mode.
    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= (1u << 31);
#endif
    VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_VMRUN;

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
    VpData->GuestVmcb.StateSaveArea.Rflags = ContextRecord->EFlags;
    VpData->GuestVmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
    VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;
    VpData->GuestVmcb.StateSaveArea.GPat = __readmsr(IA32_MSR_PAT);

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
    numOfProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

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
    if (registers[2] != '1VSS')
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
    if (registers[2] != 'SSVM')
    {
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
    // Execute SvVirtualizeProcessor on and virtualize each processor one-by-one.
    // How many processors were successfully virtualized is stored in the third
    // parameter.
    //
    // STATUS_SUCCESS is returned if all processor are successfully virtualized.
    // When any error occurs while virtualizing processors, this function does
    // not attempt to virtualize the rest of processor. Therefore, only part of
    // processors on the system may have been virtualized on error. In this case,
    // it is a caller's responsibility to clean-up (de-virtualize) such
    // processors.
    //
    status = SvExecuteOnEachProcessor(SvVirtualizeProcessor,
                                      sharedVpData,
                                      &numOfProcessorsCompleted);

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
