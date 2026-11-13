/*!
    @file       SimpleSvm.hpp

    @brief      SVM specific definitions.

    @author     Satoshi Tanda

    @copyright  Copyright (c) 2017-2019, Satoshi Tanda. All rights reserved.
 */
#pragma once

#include <basetsd.h>

//
// A size of two the MSR permissions map.
//
#define SVM_MSR_PERMISSIONS_MAP_SIZE    (PAGE_SIZE * 2)

//
// See "SVM Related MSRs"
//
#define SVM_MSR_VM_CR                   0xc0010114
#define SVM_MSR_VM_HSAVE_PA             0xc0010117

#define SVM_VM_CR_SVMDIS                (1UL << 4)

//
// See "VMCB Layout, Control Area"
//
#define SVM_INTERCEPT_MISC1_CPUID       (1UL << 18)
#define SVM_INTERCEPT_MISC1_MSR_PROT    (1UL << 28)
#define SVM_INTERCEPT_MISC2_VMRUN       (1UL << 0)

/*
 * 9.448: the REST of the Misc1 / Misc2 intercept bits, so the 206 main line can
 * actually cover the same exit set vmexit.c covers (35 codes) instead of 12.
 *
 * WHY THEY ARE OPT-IN, NOT DEFAULT
 *
 * Arming HLT or PAUSE on a running Windows guest is not a neutral change: every
 * idle core executes HLT, so each idle transition becomes an exit - and PAUSE
 * fires on every spinlock contention path. This tree is already ~12x slower than
 * the pre-9.445 build because 12 cores sit in guest VMRUN loops (measured: 2.88e8
 * exits, 2.88e8 vmmcalls on one unload). Adding HLT/PAUSE to the armed set would
 * multiply that again.
 *
 * So these bits are armed only when a caller asks (YghvSetExitMask, IOCTL 0x82A),
 * and the default build keeps exactly the set it had before. The claim "35 exit
 * codes implemented" then becomes MEASURED and SELECTABLE: arm the mask, run
 * traffic, read the per-code counters, see which fired. A case whose bit is not
 * armed can never fire, so the counters are what separate "the code exists" from
 * "the code works".
 *
 * Bit numbering follows the AMD VMCB control area layout (APM vol 2, "VMCB
 * Layout, Control Area"). Misc1 covers exit codes 0x60..0x7F, Misc2 0x80..0x8D.
 * CR0/CR3/CR4/CR8 read+write are separate 16-bit fields (InterceptCrRead /
 * InterceptCrWrite), not bits inside Misc1/2.
 */
#define SVM_INTERCEPT_MISC1_INTR        (1UL << 0)   /* 0x60 */
#define SVM_INTERCEPT_MISC1_NMI         (1UL << 1)   /* 0x61 */
#define SVM_INTERCEPT_MISC1_SMI         (1UL << 2)   /* 0x62 */
#define SVM_INTERCEPT_MISC1_INIT        (1UL << 3)   /* 0x63 */
#define SVM_INTERCEPT_MISC1_VINTR       (1UL << 4)   /* 0x64 */
#define SVM_INTERCEPT_MISC1_CR0_SEL     (1UL << 5)   /* 0x65 */
#define SVM_INTERCEPT_MISC1_IDTR_READ   (1UL << 6)   /* 0x66 */
#define SVM_INTERCEPT_MISC1_GDTR_READ   (1UL << 7)   /* 0x67 */
#define SVM_INTERCEPT_MISC1_LDTR_READ   (1UL << 8)   /* 0x68 */
#define SVM_INTERCEPT_MISC1_TR_READ     (1UL << 9)   /* 0x69 */
#define SVM_INTERCEPT_MISC1_IDTR_WRITE  (1UL << 10)  /* 0x6A */
#define SVM_INTERCEPT_MISC1_GDTR_WRITE  (1UL << 11)  /* 0x6B */
#define SVM_INTERCEPT_MISC1_LDTR_WRITE  (1UL << 12)  /* 0x6C */
#define SVM_INTERCEPT_MISC1_TR_WRITE    (1UL << 13)  /* 0x6D */
#define SVM_INTERCEPT_MISC1_RDPMC       (1UL << 15)  /* 0x6F */
#define SVM_INTERCEPT_MISC1_PUSHF       (1UL << 16)  /* 0x70 */
#define SVM_INTERCEPT_MISC1_POPF        (1UL << 17)  /* 0x71 */
#define SVM_INTERCEPT_MISC1_RSM         (1UL << 19)  /* 0x73 */
#define SVM_INTERCEPT_MISC1_IRET        (1UL << 20)  /* 0x74 */
#define SVM_INTERCEPT_MISC1_SWINT       (1UL << 21)  /* 0x75 */
#define SVM_INTERCEPT_MISC1_INVD        (1UL << 22)  /* 0x76 */
#define SVM_INTERCEPT_MISC1_PAUSE       (1UL << 23)  /* 0x77 */
#define SVM_INTERCEPT_MISC1_HLT         (1UL << 24)  /* 0x78 */
#define SVM_INTERCEPT_MISC1_INVLPG      (1UL << 25)  /* 0x79 */
#define SVM_INTERCEPT_MISC1_INVLPGA     (1UL << 26)  /* 0x7A */
#define SVM_INTERCEPT_MISC1_IOIO        (1UL << 27)  /* 0x7B */
#define SVM_INTERCEPT_MISC1_TASKSR      (1UL << 29)  /* 0x7D */
#define SVM_INTERCEPT_MISC1_FERR        (1UL << 30)  /* 0x7E */
#define SVM_INTERCEPT_MISC1_SHUTDOWN    (1UL << 31)  /* 0x7F */

#define SVM_INTERCEPT_MISC2_VMLOAD      (1UL << 2)   /* 0x82 */
#define SVM_INTERCEPT_MISC2_VMSAVE      (1UL << 3)   /* 0x83 */
#define SVM_INTERCEPT_MISC2_STGI        (1UL << 4)   /* 0x84 */
#define SVM_INTERCEPT_MISC2_CLGI        (1UL << 5)   /* 0x85 */
#define SVM_INTERCEPT_MISC2_SKINIT      (1UL << 6)   /* 0x86 */
#define SVM_INTERCEPT_MISC2_ICEBP       (1UL << 8)   /* 0x88 */
#define SVM_INTERCEPT_MISC2_WBINVD      (1UL << 9)   /* 0x89 */
#define SVM_INTERCEPT_MISC2_MONITOR     (1UL << 10)  /* 0x8A */
#define SVM_INTERCEPT_MISC2_MWAIT       (1UL << 11)  /* 0x8B */
#define SVM_INTERCEPT_MISC2_MWAIT_COND  (1UL << 12)  /* 0x8C */
#define SVM_INTERCEPT_MISC2_XSETBV      (1UL << 13)  /* 0x8D */

/*
 * 9.433: THE BIT NUMBER WAS WRONG. VMMCALL IS BIT 1, NOT BIT 2.
 *
 * Checked against the authoritative table (FreeBSD bhyve's amd/svm.h, VMCB
 * control offset 0x10):
 *
 *     VMCB_INTCPT_VMRUN   BIT(0)
 *     VMCB_INTCPT_VMMCALL BIT(1)      <-- this one
 *     VMCB_INTCPT_VMLOAD  BIT(2)
 *     VMCB_INTCPT_VMSAVE  BIT(3)
 *     VMCB_INTCPT_STGI    BIT(4)
 *     VMCB_INTCPT_CLGI    BIT(5)
 *
 * 9.432 armed bit 2, which is VMLOAD - so the guest's vmmcall was still not
 * trapped and vmrun still never returned. The measurement that caught it: the
 * crash offset stayed at Sv206LaunchVm+0x14 (inside vmrun) instead of moving,
 * and g_inVmrunCount would still be 12.
 *
 * 9.432: VMMCALL, WHICH THE TEST GUEST USES AS ITS ONLY EXIT.
 *
 * svm_trampoline.asm is three instructions:
 *
 *     svm_trampoline_test_guest:
 *         mov  rax, 1        ; YGHV_CMD_HEARTBEAT
 *         vmmcall            ; <-- the only thing that produces a #VMEXIT
 *         jmp  svm_trampoline_test_guest
 *
 * Upstream SimpleSvm intercepts VMMCALL, because that is the whole mechanism its
 * test guest uses to hand control back. This tree armed VMRUN (bit 0) and left
 * VMMCALL (bit 2) out - and the two are adjacent in the same field, which is
 * exactly how a copy of "one and a half of two related facts" looks.
 *
 * Measured consequence of the gap (9.430): the guest's vmmcall is not intercepted,
 * so no #VMEXIT happens, vmrun never returns, the in-vmrun counter never drops,
 * SvHandleVmExit never runs (g_S206VrLog was empty for a whole session), and
 * DriverUnload hung on a counter that could not reach zero.
 *
 * Total bit 34 = Misc2 bit 2.
 */
#define SVM_INTERCEPT_MISC2_VMMCALL     (1UL << 1)
#define SVM_NP_ENABLE_NP_ENABLE         (1UL << 0)

/*
 * 9.368 (B1, Tier 5.1): RDTSC = InterceptMisc1 bit 14; RDTSCP = overall bit 39,
 * i.e. bit 7 of InterceptMisc2 (Misc2 covers 32..63).
 *
 * MEASURED BEFORE ADDING THESE: this build only ever set CPUID(18), MSR_PROT(28)
 * and SHUTDOWN(31) in InterceptMisc1. MEM_ENGINE_PLAN_TIER5.md states that
 * "现有 INTERCEPT_RDTSC 已在 206 主线启用" — that is FALSE here, and it is the
 * same error 9.367.1 recorded: the plan counted AMD's exit-code catalogue as if
 * it were this build's mask.
 *
 * Both bits sit in fields the VMCB struct already declares. That is why this pair
 * was chosen over the WBINVD/MWAIT group: those are in InterceptMisc3, which the
 * struct does not have (9.367.2).
 */
#define SVM_INTERCEPT_MISC1_RDTSC       (1UL << 14)
#define SVM_INTERCEPT_MISC2_RDTSCP      (1UL << (39 - 32))

typedef struct _VMCB_CONTROL_AREA
{
    UINT16 InterceptCrRead;             // +0x000
    UINT16 InterceptCrWrite;            // +0x002
    UINT16 InterceptDrRead;             // +0x004
    UINT16 InterceptDrWrite;            // +0x006
    UINT32 InterceptException;          // +0x008
    UINT32 InterceptMisc1;              // +0x00c
    UINT32 InterceptMisc2;              // +0x010
    UINT8 Reserved1[0x03c - 0x014];     // +0x014
    UINT16 PauseFilterThreshold;        // +0x03c
    UINT16 PauseFilterCount;            // +0x03e
    UINT64 IopmBasePa;                  // +0x040
    UINT64 MsrpmBasePa;                 // +0x048
    UINT64 TscOffset;                   // +0x050
    UINT32 GuestAsid;                   // +0x058
    UINT32 TlbControl;                  // +0x05c
    UINT64 VIntr;                       // +0x060
    UINT64 InterruptShadow;             // +0x068
    UINT64 ExitCode;                    // +0x070
    UINT64 ExitInfo1;                   // +0x078
    UINT64 ExitInfo2;                   // +0x080
    UINT64 ExitIntInfo;                 // +0x088
    UINT64 NpEnable;                    // +0x090
    UINT64 AvicApicBar;                 // +0x098
    UINT64 GuestPaOfGhcb;               // +0x0a0
    UINT64 EventInj;                    // +0x0a8
    UINT64 NCr3;                        // +0x0b0
    UINT64 LbrVirtualizationEnable;     // +0x0b8
    UINT64 VmcbClean;                   // +0x0c0
    UINT64 NRip;                        // +0x0c8
    UINT8 NumOfBytesFetched;            // +0x0d0
    UINT8 GuestInstructionBytes[15];    // +0x0d1
    UINT64 AvicApicBackingPagePointer;  // +0x0e0
    UINT64 Reserved2;                   // +0x0e8
    UINT64 AvicLogicalTablePointer;     // +0x0f0
    UINT64 AvicPhysicalTablePointer;    // +0x0f8
    UINT64 Reserved3;                   // +0x100
    UINT64 VmcbSaveStatePointer;        // +0x108
    /*
     * 9.444: host-private slot, NOT part of the AMD VMCB layout.
     *
     * The VMCB control area is 0x400 bytes and everything above +0x110 is reserved
     * by the architecture, so the tail of Reserved4 is ours to use. This field holds
     * the stack pointer SvLV20 must restore when a 206 guest is stopped - i.e. the
     * address where the entry trampoline's call pushed a return address.
     *
     * It lives here rather than in a global array because a global needs an index,
     * and the index was the bug: the threads are pinned with a processor MASK while
     * KeGetCurrentProcessorNumber() returns a processor INDEX, and on this machine
     * the two do not coincide. The VMCB is per-core and is already handed to
     * SvHandleVmExit, so the value is reachable without any index at all.
     *
     * Reserved4 below still reserves the rest of the architectural hole; this field
     * is carved out of its head and the array is shortened to match, so the struct
     * size and every other offset are unchanged.
     */
    UINT64 HostExitRsp;                 // +0x110  (yghv-private, not AMD-defined)
    UINT8 Reserved4[0x400 - 0x118];     // +0x118
} VMCB_CONTROL_AREA, *PVMCB_CONTROL_AREA;
static_assert(sizeof(VMCB_CONTROL_AREA) == 0x400,
              "VMCB_CONTROL_AREA Size Mismatch");

//
// See "VMCB Layout, State Save Area"
//
typedef struct _VMCB_STATE_SAVE_AREA
{
    UINT16 EsSelector;                  // +0x000
    UINT16 EsAttrib;                    // +0x002
    UINT32 EsLimit;                     // +0x004
    UINT64 EsBase;                      // +0x008
    UINT16 CsSelector;                  // +0x010
    UINT16 CsAttrib;                    // +0x012
    UINT32 CsLimit;                     // +0x014
    UINT64 CsBase;                      // +0x018
    UINT16 SsSelector;                  // +0x020
    UINT16 SsAttrib;                    // +0x022
    UINT32 SsLimit;                     // +0x024
    UINT64 SsBase;                      // +0x028
    UINT16 DsSelector;                  // +0x030
    UINT16 DsAttrib;                    // +0x032
    UINT32 DsLimit;                     // +0x034
    UINT64 DsBase;                      // +0x038
    UINT16 FsSelector;                  // +0x040
    UINT16 FsAttrib;                    // +0x042
    UINT32 FsLimit;                     // +0x044
    UINT64 FsBase;                      // +0x048
    UINT16 GsSelector;                  // +0x050
    UINT16 GsAttrib;                    // +0x052
    UINT32 GsLimit;                     // +0x054
    UINT64 GsBase;                      // +0x058
    UINT16 GdtrSelector;                // +0x060
    UINT16 GdtrAttrib;                  // +0x062
    UINT32 GdtrLimit;                   // +0x064
    UINT64 GdtrBase;                    // +0x068
    UINT16 LdtrSelector;                // +0x070
    UINT16 LdtrAttrib;                  // +0x072
    UINT32 LdtrLimit;                   // +0x074
    UINT64 LdtrBase;                    // +0x078
    UINT16 IdtrSelector;                // +0x080
    UINT16 IdtrAttrib;                  // +0x082
    UINT32 IdtrLimit;                   // +0x084
    UINT64 IdtrBase;                    // +0x088
    UINT16 TrSelector;                  // +0x090
    UINT16 TrAttrib;                    // +0x092
    UINT32 TrLimit;                     // +0x094
    UINT64 TrBase;                      // +0x098
    UINT8 Reserved1[0x0cb - 0x0a0];     // +0x0a0
    UINT8 Cpl;                          // +0x0cb
    UINT32 Reserved2;                   // +0x0cc
    UINT64 Efer;                        // +0x0d0
    UINT8 Reserved3[0x148 - 0x0d8];     // +0x0d8
    UINT64 Cr4;                         // +0x148
    UINT64 Cr3;                         // +0x150
    UINT64 Cr0;                         // +0x158
    UINT64 Dr7;                         // +0x160
    UINT64 Dr6;                         // +0x168
    UINT64 Rflags;                      // +0x170
    UINT64 Rip;                         // +0x178
    UINT8 Reserved4[0x1d8 - 0x180];     // +0x180
    UINT64 Rsp;                         // +0x1d8
    UINT8 Reserved5[0x1f8 - 0x1e0];     // +0x1e0
    UINT64 Rax;                         // +0x1f8
    UINT64 Star;                        // +0x200
    UINT64 LStar;                       // +0x208
    UINT64 CStar;                       // +0x210
    UINT64 SfMask;                      // +0x218
    UINT64 KernelGsBase;                // +0x220
    UINT64 SysenterCs;                  // +0x228
    UINT64 SysenterEsp;                 // +0x230
    UINT64 SysenterEip;                 // +0x238
    UINT64 Cr2;                         // +0x240
    UINT8 Reserved6[0x268 - 0x248];     // +0x248
    UINT64 GPat;                        // +0x268
    UINT64 DbgCtl;                      // +0x270
    UINT64 BrFrom;                      // +0x278
    UINT64 BrTo;                        // +0x280
    UINT64 LastExcepFrom;               // +0x288
    UINT64 LastExcepTo;                 // +0x290
} VMCB_STATE_SAVE_AREA, *PVMCB_STATE_SAVE_AREA;
static_assert(sizeof(VMCB_STATE_SAVE_AREA) == 0x298,
              "VMCB_STATE_SAVE_AREA Size Mismatch");

//
// An entire VMCB (Virtual machine control block) layout.
//
typedef struct _VMCB
{
    VMCB_CONTROL_AREA ControlArea;
    VMCB_STATE_SAVE_AREA StateSaveArea;
    UINT8 Reserved1[0x1000 - sizeof(VMCB_CONTROL_AREA) - sizeof(VMCB_STATE_SAVE_AREA)];
} VMCB, *PVMCB;
static_assert(sizeof(VMCB) == 0x1000,
              "VMCB Size Mismatch");

//
// See "Event Injection"
//
typedef struct _EVENTINJ
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Vector : 8;          // [0:7]
            UINT64 Type : 3;            // [8:10]
            UINT64 ErrorCodeValid : 1;  // [11]
            UINT64 Reserved1 : 19;      // [12:30]
            UINT64 Valid : 1;           // [31]
            UINT64 ErrorCode : 32;      // [32:63]
        } Fields;
    };
} EVENTINJ, *PEVENTINJ;
static_assert(sizeof(EVENTINJ) == 8,
              "EVENTINJ Size Mismatch");

//
// See "SVM Intercept Codes"
//
#define VMEXIT_CR0_READ             0x0000
#define VMEXIT_CR1_READ             0x0001
#define VMEXIT_CR2_READ             0x0002
#define VMEXIT_CR3_READ             0x0003
#define VMEXIT_CR4_READ             0x0004
#define VMEXIT_CR5_READ             0x0005
#define VMEXIT_CR6_READ             0x0006
#define VMEXIT_CR7_READ             0x0007
#define VMEXIT_CR8_READ             0x0008
#define VMEXIT_CR9_READ             0x0009
#define VMEXIT_CR10_READ            0x000a
#define VMEXIT_CR11_READ            0x000b
#define VMEXIT_CR12_READ            0x000c
#define VMEXIT_CR13_READ            0x000d
#define VMEXIT_CR14_READ            0x000e
#define VMEXIT_CR15_READ            0x000f
#define VMEXIT_CR0_WRITE            0x0010
#define VMEXIT_CR1_WRITE            0x0011
#define VMEXIT_CR2_WRITE            0x0012
#define VMEXIT_CR3_WRITE            0x0013
#define VMEXIT_CR4_WRITE            0x0014
#define VMEXIT_CR5_WRITE            0x0015
#define VMEXIT_CR6_WRITE            0x0016
#define VMEXIT_CR7_WRITE            0x0017
#define VMEXIT_CR8_WRITE            0x0018
#define VMEXIT_CR9_WRITE            0x0019
#define VMEXIT_CR10_WRITE           0x001a
#define VMEXIT_CR11_WRITE           0x001b
#define VMEXIT_CR12_WRITE           0x001c
#define VMEXIT_CR13_WRITE           0x001d
#define VMEXIT_CR14_WRITE           0x001e
#define VMEXIT_CR15_WRITE           0x001f
#define VMEXIT_DR0_READ             0x0020
#define VMEXIT_DR1_READ             0x0021
#define VMEXIT_DR2_READ             0x0022
#define VMEXIT_DR3_READ             0x0023
#define VMEXIT_DR4_READ             0x0024
#define VMEXIT_DR5_READ             0x0025
#define VMEXIT_DR6_READ             0x0026
#define VMEXIT_DR7_READ             0x0027
#define VMEXIT_DR8_READ             0x0028
#define VMEXIT_DR9_READ             0x0029
#define VMEXIT_DR10_READ            0x002a
#define VMEXIT_DR11_READ            0x002b
#define VMEXIT_DR12_READ            0x002c
#define VMEXIT_DR13_READ            0x002d
#define VMEXIT_DR14_READ            0x002e
#define VMEXIT_DR15_READ            0x002f
#define VMEXIT_DR0_WRITE            0x0030
#define VMEXIT_DR1_WRITE            0x0031
#define VMEXIT_DR2_WRITE            0x0032
#define VMEXIT_DR3_WRITE            0x0033
#define VMEXIT_DR4_WRITE            0x0034
#define VMEXIT_DR5_WRITE            0x0035
#define VMEXIT_DR6_WRITE            0x0036
#define VMEXIT_DR7_WRITE            0x0037
#define VMEXIT_DR8_WRITE            0x0038
#define VMEXIT_DR9_WRITE            0x0039
#define VMEXIT_DR10_WRITE           0x003a
#define VMEXIT_DR11_WRITE           0x003b
#define VMEXIT_DR12_WRITE           0x003c
#define VMEXIT_DR13_WRITE           0x003d
#define VMEXIT_DR14_WRITE           0x003e
#define VMEXIT_DR15_WRITE           0x003f
#define VMEXIT_EXCEPTION_DE         0x0040
#define VMEXIT_EXCEPTION_DB         0x0041
#define VMEXIT_EXCEPTION_NMI        0x0042
#define VMEXIT_EXCEPTION_BP         0x0043
#define VMEXIT_EXCEPTION_OF         0x0044
#define VMEXIT_EXCEPTION_BR         0x0045
#define VMEXIT_EXCEPTION_UD         0x0046
#define VMEXIT_EXCEPTION_NM         0x0047
#define VMEXIT_EXCEPTION_DF         0x0048
#define VMEXIT_EXCEPTION_09         0x0049
#define VMEXIT_EXCEPTION_TS         0x004a
#define VMEXIT_EXCEPTION_NP         0x004b
#define VMEXIT_EXCEPTION_SS         0x004c
#define VMEXIT_EXCEPTION_GP         0x004d
#define VMEXIT_EXCEPTION_PF         0x004e
#define VMEXIT_EXCEPTION_15         0x004f
#define VMEXIT_EXCEPTION_MF         0x0050
#define VMEXIT_EXCEPTION_AC         0x0051
#define VMEXIT_EXCEPTION_MC         0x0052
#define VMEXIT_EXCEPTION_XF         0x0053
#define VMEXIT_EXCEPTION_20         0x0054
#define VMEXIT_EXCEPTION_21         0x0055
#define VMEXIT_EXCEPTION_22         0x0056
#define VMEXIT_EXCEPTION_23         0x0057
#define VMEXIT_EXCEPTION_24         0x0058
#define VMEXIT_EXCEPTION_25         0x0059
#define VMEXIT_EXCEPTION_26         0x005a
#define VMEXIT_EXCEPTION_27         0x005b
#define VMEXIT_EXCEPTION_28         0x005c
#define VMEXIT_EXCEPTION_VC         0x005d
#define VMEXIT_EXCEPTION_SX         0x005e
#define VMEXIT_EXCEPTION_31         0x005f
#define VMEXIT_INTR                 0x0060
#define VMEXIT_NMI                  0x0061
#define VMEXIT_SMI                  0x0062
#define VMEXIT_INIT                 0x0063
#define VMEXIT_VINTR                0x0064
#define VMEXIT_CR0_SEL_WRITE        0x0065
#define VMEXIT_IDTR_READ            0x0066
#define VMEXIT_GDTR_READ            0x0067
#define VMEXIT_LDTR_READ            0x0068
#define VMEXIT_TR_READ              0x0069
#define VMEXIT_IDTR_WRITE           0x006a
#define VMEXIT_GDTR_WRITE           0x006b
#define VMEXIT_LDTR_WRITE           0x006c
#define VMEXIT_TR_WRITE             0x006d
#define VMEXIT_RDTSC                0x006e
#define VMEXIT_RDPMC                0x006f
#define VMEXIT_PUSHF                0x0070
#define VMEXIT_POPF                 0x0071
#define VMEXIT_CPUID                0x0072
#define VMEXIT_RSM                  0x0073
#define VMEXIT_IRET                 0x0074
#define VMEXIT_SWINT                0x0075
#define VMEXIT_INVD                 0x0076
#define VMEXIT_PAUSE                0x0077
#define VMEXIT_HLT                  0x0078
#define VMEXIT_INVLPG               0x0079
#define VMEXIT_INVLPGA              0x007a
#define VMEXIT_IOIO                 0x007b
/*
 * 9.476: THIS MACRO HAS TWO SPELLINGS IN THE TREE, AND THE WARNING THAT GAVE IT
 * AWAY WAS A MACRO-REDEFINITION (C4005) NOBODY WAS READING.
 *
 * common/svm_defs.h defines VMEXIT_MSR/VMMCALL/NPF as aliases of SVM_EXIT_*;
 * this header defines them as literals. Measured, the values currently agree
 * (0x7C / 0x81 / 0x400), so the redefinition is harmless TODAY - but a macro
 * redefinition is the one kind of disagreement the compiler reports and the
 * build then ignores, so the next person to change one copy gets two values
 * silently, whichever header happened to be included first winning.
 *
 * The guard below turns that into a COMPILE ERROR: if svm_defs.h was already
 * included and its value differs, this #if fires instead of a warning scrolling
 * past. If it was not included, the literal stands as before.
 */
#if defined(SVM_EXIT_MSR) && (SVM_EXIT_MSR != 0x7C)
#error "svm_defs.h SVM_EXIT_MSR disagrees with svm_simplevm206.hpp VMEXIT_MSR"
#endif
#if defined(SVM_EXIT_VMMCALL) && (SVM_EXIT_VMMCALL != 0x81)
#error "svm_defs.h SVM_EXIT_VMMCALL disagrees with svm_simplevm206.hpp VMEXIT_VMMCALL"
#endif
#if defined(SVM_EXIT_NPF) && (SVM_EXIT_NPF != 0x400)
#error "svm_defs.h SVM_EXIT_NPF disagrees with svm_simplevm206.hpp VMEXIT_NPF"
#endif

#if defined(SVM_EXIT_CPUID) && (SVM_EXIT_CPUID != 0x72)
#error "svm_defs.h SVM_EXIT_CPUID disagrees with svm_simplevm206.hpp VMEXIT_CPUID"
#endif

#define VMEXIT_MSR                  0x007c
#define VMEXIT_TASK_SWITCH          0x007d
#define VMEXIT_FERR_FREEZE          0x007e
#define VMEXIT_SHUTDOWN             0x007f
#define VMEXIT_VMRUN                0x0080
#define VMEXIT_VMMCALL              0x0081
#define VMEXIT_VMLOAD               0x0082
#define VMEXIT_VMSAVE               0x0083
#define VMEXIT_STGI                 0x0084
#define VMEXIT_CLGI                 0x0085
#define VMEXIT_SKINIT               0x0086
#define VMEXIT_RDTSCP               0x0087
#define VMEXIT_ICEBP                0x0088
#define VMEXIT_WBINVD               0x0089
#define VMEXIT_MONITOR              0x008a
#define VMEXIT_MWAIT                0x008b
#define VMEXIT_MWAIT_CONDITIONAL    0x008c
#define VMEXIT_XSETBV               0x008d
#define VMEXIT_EFER_WRITE_TRAP      0x008f
#define VMEXIT_CR0_WRITE_TRAP       0x0090
#define VMEXIT_CR1_WRITE_TRAP       0x0091
#define VMEXIT_CR2_WRITE_TRAP       0x0092
#define VMEXIT_CR3_WRITE_TRAP       0x0093
#define VMEXIT_CR4_WRITE_TRAP       0x0094
#define VMEXIT_CR5_WRITE_TRAP       0x0095
#define VMEXIT_CR6_WRITE_TRAP       0x0096
#define VMEXIT_CR7_WRITE_TRAP       0x0097
#define VMEXIT_CR8_WRITE_TRAP       0x0098
#define VMEXIT_CR9_WRITE_TRAP       0x0099
#define VMEXIT_CR10_WRITE_TRAP      0x009a
#define VMEXIT_CR11_WRITE_TRAP      0x009b
#define VMEXIT_CR12_WRITE_TRAP      0x009c
#define VMEXIT_CR13_WRITE_TRAP      0x009d
#define VMEXIT_CR14_WRITE_TRAP      0x009e
#define VMEXIT_CR15_WRITE_TRAP      0x009f
#define VMEXIT_NPF                  0x0400
#define AVIC_INCOMPLETE_IPI         0x0401
#define AVIC_NOACCEL                0x0402
#define VMEXIT_VMGEXIT              0x0403
#define VMEXIT_INVALID              -1
