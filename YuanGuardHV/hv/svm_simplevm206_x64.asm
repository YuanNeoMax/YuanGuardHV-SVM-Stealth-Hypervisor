;
; @file       x64.asm
;
; @brief      All assembly code.
;
; @author     Satoshi Tanda
;
; @copyright  Copyright (c) 2017-2019, Satoshi Tanda. All rights reserved.
;
.const

KTRAP_FRAME_SIZE            equ     190h
MACHINE_FRAME_SIZE          equ     28h

.code

extern SvHandleVmExit : proc

; 9.422: the in-vmrun counter. Bumped before vmrun, dropped immediately after
; vmsave. The stop-flag externs of 9.416 are gone: those variables are static
; again (9.419) because the inline check they served was proven useless (9.417).
extern g_inVmrunCount : dword
;
; 9.555: incremented immediately before the thread is terminated below, so
; the unload waiter in main.c has a condition that can actually be met.
; It is a LONG (dword).
;
extern g_S206CoresExitedAsm : dword

;
;   @brief      Saves all general purpose registers to the stack.
;
;   @details    This macro does not alter the flag register.
;
PUSHAQ macro
        push    rax
        push    rcx
        push    rdx
        push    rbx
        push    -1      ; Dummy for rsp.
        push    rbp
        push    rsi
        push    rdi
        push    r8
        push    r9
        push    r10
        push    r11
        push    r12
        push    r13
        push    r14
        push    r15
        endm

;
;   @brief      Loads all general purpose registers from the stack.
;
;   @details    This macro does not alter the flag register.
;
POPAQ macro
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     r11
        pop     r10
        pop     r9
        pop     r8
        pop     rdi
        pop     rsi
        pop     rbp
        pop     rbx    ; Dummy for rsp (this value is destroyed by the next pop).
        pop     rbx
        pop     rdx
        pop     rcx
        pop     rax
        endm

;
;   @brief      Enters the loop that executes the guest and handles #VMEXIT.
;
;   @details    This function switches to the host stack pointer, runs the guest
;               and handles #VMEXIT until SvHandleVmExit returns non-zero value.
;               When SvHandleVmExit returned non-zero value, this function
;               returns execution flow to the next instruction of the
;               instruction triggered #VMEXIT after terminating virtualization.
;
;   @param[in]  HostRsp - A stack pointer for the hypervisor.
;
Sv206LaunchVm proc frame
        ;
        ; 9.445: SAVE THE THREAD'S REAL RSP BEFORE SWITCHING AWAY FROM IT.
        ;
        ; This is the only place that has both facts at once: RSP is still the
        ; thread's own stack, and RCX is the HostStackLayout pointer (which sits at
        ; offset 0 of the VPD, so the VPD address is also the VMCB address).
        ;
        ; The saved value is what a stopped core returns to. Everything 9.443-9.444
        ; tried to reconstruct by hand - the address where a `call` pushed its return
        ; address, one or two frames up - is simply this register, captured before
        ; anything touches it.
        ;
        ; Stored at VPD+0x6110 = GuestVmcb.ControlArea.HostExitRsp (the yghv-private
        ; slot carved out of the VMCB's architectural hole). Note RCX points at
        ; HostStackLayout, i.e. VPD+0x5FD0, so the VPD base is RCX-5FD0.
        ;
        mov     rax, rsp
        mov     qword ptr [rcx - 5FD0h + 6110h], rax

        ;
        ; Update the current stack pointer with the host RSP. This protects
        ; values stored on stack for the hypervisor from being overwritten by
        ; the guest due to a use of the same stack memory.
        ;
        mov rsp, rcx    ; Rsp <= HostRsp

SvLV10: ;
        ; Run the loop to executed the guest and handle #VMEXIT. Below is the
        ; current stack leyout.
        ; ----
        ; Rsp          => 0x...fd0 GuestVmcbPa       ; HostStackLayout
        ;                 0x...fd8 HostVmcbPa        ;
        ;                 0x...fe0 Self              ;
        ;                 0x...fe8 SharedVpData      ;
        ;                 0x...ff0 Padding1          ;
        ;                 0x...ff8 Reserved1         ;
        ; ----
        ;
        ;
        ; 9.414: THE PER-ITERATION STOP CHECK, AT THE ONLY SAFE POINT IN THIS FRAME.
        ;
        ; 9.413 tried this after `jnz SvLV20` and produced 0x139 MISSING_GSFRAME:
        ; at that point rsp still pointed into the KTRAP_FRAME and a call wrote the
        ; return address into the PUSHAQ'd GuestRegisters block, and r11 (used to
        ; save rax) is volatile. The invariants were not checked before the change.
        ;
        ; HERE, and only here, the invariants hold:
        ;   rsp  = HostRsp, pointing at HostStackLayout (0xfd0..0xff8 of this
        ;          frame). The stack grows DOWN from there, so rsp-8 and below is
        ;          free stack - pushing here touches nothing the loop owns.
        ;   rcx  = the original guest stack pointer. SvLV20 does `mov rsp, rcx`,
        ;          so it MUST hold its entry value on every path out of here.
        ;          rcx is volatile across a call (first integer argument), so it
        ;          is pushed and restored around it.
        ;   rax  = not yet loaded (the very next instruction loads it), so the
        ;          call may clobber it.
        ;   rdx/r8-r11 = volatile and unused at this point.
        ;
        ; Sv206VmrunShouldExit performs only interlocked reads: lock cmpxchg /
        ; lock xadd intrinsics, no kernel API, so it is callable with GIF clear.
        ; This is why the check can live in the loop at all - SvHandleVmExit is
        ; C code that runs only on a #VMEXIT, and this guest is restarted from the
        ; same RIP each iteration, so that check never fired (9.410: stopped=0).
        ;
        ;
        ; 9.419: THE INLINE CHECK IS GONE - 9.417 PROVED IT DOES NOT HELP.
        ;
        ; Three versions were tried at this point:
        ;   9.413  call into C, rsp in the wrong place       -> 0x139
        ;   9.414  call into C, rsp correct                  -> 0x7F/8 double fault
        ;   9.416  four inline instructions, no call at all  -> 0x7F/8, identical
        ;
        ; The third is what settles it: if a C function's prolog were the cause,
        ; removing the call would have fixed it. It did not. So the check itself
        ; is not what breaks the machine, and adding it here buys nothing while
        ; costing four instructions on the hottest path in the driver.
        ;
        ; What the stop request actually needs is a decision about WHEN the loop
        ; is allowed to be stopped at all - and that is not answerable from inside
        ; the loop. Left out deliberately until the trace (now 120 slots) says
        ; where DriverUnload really stops.

        mov rax, [rsp]  ; RAX <= VpData->HostStackLayout.GuestVmcbPa
        vmload rax      ; load previously saved guest state from VMCB

        ;
        ; 9.422: MARK THIS CORE AS BEING INSIDE vmrun.
        ;
        ; 9.421 found the real crash instruction: `vmsave rax` at +0x0D, the first
        ; instruction after vmrun returns. It writes guest state back into the VMCB
        ; at that physical address, and svm_core_cleanup may already have freed it.
        ;
        ; No check on the loop's back-edge can prevent that, because vmsave runs
        ; BEFORE the back-edge. The constraint belongs on the teardown side, and
        ; the teardown needs to know whether anybody is inside vmrun - which is
        ; this counter.
        ;
        ; Two stores, no branch, no call, rsp/rcx untouched: safe here in a way the
        ; conditional check of 9.416 was not (that one produced 0x139 and 0x7F).
        ;
        lock inc dword ptr [g_inVmrunCount]

        ;
        ; Start the guest. The VMRUN instruction resumes execution of the guest
        ; with state described in VMCB (specified by RAX by its physical address)
        ; until #VMEXI is triggered. On #VMEXIT, the VMRUN instruction completes
        ; and resumes the next instruction (ie, vmsave in our case).
        ;
        ; The VMRUN instruction does the following things in this order:
        ; - saves some current state (ie. host state) into the host state-save
        ;   area specified in IA32_MSR_VM_HSAVE_PA
        ; - loads guest state from the VMCB state-save area
        ; - enables interrupts by setting the the global interrupt flag (GIF)
        ; - resumes execution of the guest until #VMEXIT occurs
        ; See "Basic Operation" for more details.
        ;
        ; On #VMEXIT:
        ; - disables interrupts by clearing the the global interrupt flag (GIF)
        ; - saves current guest state into and update VMCB to provide information
        ;   to handle #VMEXIT
        ; - loads the host state previously saved by the VMRUN instruction
        ; See "#VMEXIT" in the volume 2 and "VMRUN" in the volume 3 for more
        ; details.
        ;
        vmrun rax       ; Switch to the guest until #VMEXIT

        ;
        ; #VMEXIT occurred. Now, some of guest state has been saved to VMCB, but
        ; not all of it. Save some of unsaved state with the VMSAVE instruction.
        ;
        ; RAX (and some other state like RSP) has been restored from the host
        ; state-save, so it has the same value as before and not guest's one.
        ;
        vmsave rax      ; Save current guest state to VMCB

        ;
        ; 9.422: DROP THE MARK IMMEDIATELY AFTER vmsave - never before it.
        ;
        ; The counter answers "is anybody about to touch a VMCB". vmsave IS that
        ; touch. Clearing the mark before vmsave would tell the teardown "clear to
        ; free" while the write is still in flight, which is exactly the race this
        ; exists to prevent. So the decrement goes after vmsave, with no instruction
        ; in between.
        ;
        ; The increment above sits after vmload and before vmrun for the symmetric
        ; reason: if vmload faulted (VMCB already gone) the count is never incremented
        ; and therefore never needs decrementing. A counter that can leak to a
        ; permanent 1 would make the wait below time out every single unload.
        ;
        lock dec dword ptr [g_inVmrunCount]

        ;
        ; Optionally, allocate the trap frame so that Windbg can display stack
        ; trace of the guest while SvHandleVmExit is being executed. The trap
        ; frame fields necessary for this are initialized in SvHandleVmExit.
        ;
        .pushframe
        sub     rsp, KTRAP_FRAME_SIZE
        .allocstack KTRAP_FRAME_SIZE - MACHINE_FRAME_SIZE + 100h

        ;
        ; Also save guest's GPRs since those are not saved anywhere by the
        ; processor on #VMEXIT and will be destroyed by subsequent host code.
        ;
        PUSHAQ          ; Stack pointer decreased 8 * 16

        ;
        ; Set parameters for SvHandleVmExit. Below is the current stack layout.
        ; ----
        ; Rsp                             => 0x...dc0 R15               ; GUEST_REGISTERS
        ;                                    0x...dc8 R14               ;
        ;                                             ...               ;
        ;                                    0x...e38 RAX               ;
        ; Rsp + 8 * 16                    => 0x...e40 TrapFrame         ; HostStackLayout
        ;                                             ...               ;
        ; Rsp + 8 * 16 + KTRAP_FRAME_SIZE => 0x...fd0 GuestVmcbPa       ;
        ;                                    0x...fd8 HostVmcbPa        ;
        ; Rsp + 8 * 18 + KTRAP_FRAME_SIZE => 0x...fe0 Self              ;
        ;                                    0x...fe8 SharedVpData      ;
        ;                                    0x...ff0 Padding1          ;
        ;                                    0x...ff8 Reserved1         ;
        ; ----
        ;
        mov rdx, rsp                                ; Rdx <= GuestRegisters
        mov rcx, [rsp + 8 * 18 + KTRAP_FRAME_SIZE]  ; Rcx <= VpData

        ;
        ; Allocate stack for homing space (0x20) and volatile XMM registers
        ; (0x60). Save those registers because subsequent host code may destroy
        ; any of those registers. XMM6-15 are not saved because those should be
        ; preserved (those are non volatile registers). Finally, indicates the
        ; end of the function prolog as stack pointer changes are all done. This
        ; is for Windbg to reconstruct stack trace.
        ;
        sub rsp, 80h
        movaps xmmword ptr [rsp + 20h], xmm0
        movaps xmmword ptr [rsp + 30h], xmm1
        movaps xmmword ptr [rsp + 40h], xmm2
        movaps xmmword ptr [rsp + 50h], xmm3
        movaps xmmword ptr [rsp + 60h], xmm4
        movaps xmmword ptr [rsp + 70h], xmm5
        .endprolog

        ;
        ; Handle #VMEXIT.
        ;
        call SvHandleVmExit

        ;
        ; Restore XMM registers and roll back stack pointer.
        ;
        movaps xmm5, xmmword ptr [rsp + 70h]
        movaps xmm4, xmmword ptr [rsp + 60h]
        movaps xmm3, xmmword ptr [rsp + 50h]
        movaps xmm2, xmmword ptr [rsp + 40h]
        movaps xmm1, xmmword ptr [rsp + 30h]
        movaps xmm0, xmmword ptr [rsp + 20h]
        add rsp, 80h

        ;
        ; Test a return value of SvHandleVmExit (RAX), then POPAQ to restore the
        ; original guest's GPRs.
        ;
        test al, al
        POPAQ

        ;
        ; If non zero value is returned from SvHandleVmExit, this function exits
        ; the loop. Otherwise, continue the loop and resume the guest.
        ;
        jnz SvLV20                  ; if (ExitVm != 0) jmp SvLV20
        add rsp, KTRAP_FRAME_SIZE   ; else, restore RSP and
        jmp SvLV10                  ; jmp SvLV10

SvLV20: ;
        ; Virtualization has been terminated. Restore an original (guest's,
        ; although it is no longer the "guest") stack pointer and return to the
        ; next instruction of CPUID triggered this #VMEXIT.
        ;
        ; Here is contents of certain registers:
        ;   RBX     = An address to return
        ;   RCX     = An original stack pointer to restore
        ;   EDX:EAX = An address of per processor data for this processor
        ;
        mov rsp, rcx

        ;
        ; Update RCX with the magic value indicating that the SimpleSvm
        ; hypervisor has been unloaded.
        ;
        mov ecx, 'SSVM'

        ;
        ; Return to the next instruction of CPUID triggered this #VMEXIT. The
        ; registry values to be returned are:
        ;   EBX     = Undefined
        ;   ECX     = 'SSVM'
        ;   EDX:EAX = An address of per processor data for this processor
        ;
        jmp rbx
Sv206LaunchVm endp

; 9.289 (C6.1): INVLPGA helper — invalidates the nested-TLB translation for
; the guest virtual address in RCX, associated with the guest ASID in EDX,
; under this core's current nCR3. Used by the fake-write path to make the
; shadow mapping immediately visible (Zen3 does not flush NPT translations
; on TlbControl=1 or on nCr3 change).
SvInvlpgaByVa proc
    mov rax, rcx        ; RAX = GVA
    mov ecx, edx        ; ECX = guest ASID
    invlpga rax, ecx
    ret
SvInvlpgaByVa endp

;
; ============================================================================
; 9.443: THE 206 GUEST STUB — A GUEST WE OWN AND CAN STOP ON PURPOSE.
; ============================================================================
;
; WHY THIS EXISTS (9.442)
;
; The 206 guest used to be `StateSaveArea.Rip = ContextRecord->Rip`, i.e. the
; continuation of SvVirtualizeProcessor itself. That is the upstream "transparent
; virtualisation" semantic, and it has a consequence that took this session a very
; long time to see: the guest RUNS TO THE END OF THAT FUNCTION and executes its
; `return`. Control then flows back into SvExecuteOnEachProcessor's for-loop, the
; thread is pinned to the next core, and a NEW vmrun starts - while the previous
; vmrun frame is still on the stack and its `lock dec` never runs. Twelve cores
; means twelve permanently suspended vmrun frames. When DriverUnload returns and
; the OS unmaps the image, those frames execute `vmsave` on a dead code page:
; exactly the 0xD1 / PAGED_IP at Sv206LaunchVm+0x14 this machine kept producing.
;
; The measurement that proves the diagnosis (9.441's exit counter):
;     exits = 213,601      the loops cycle normally - nothing is stuck
;     vmmcall = 0          the guest is real kernel code, not a stub
;     invmrun = 12         a live snapshot of twelve healthy, nested loops
;
; WHY A STUB FIXES IT
;
; The loop can only be left safely at a point we control. SvLV20 resumes the guest
; with `mov rsp, rcx; mov ecx,'SSVM'; jmp rbx`, so whatever RIP the guest had is
; where execution continues, with RCX and RBX consumed. That is harmless if the
; RIP is inside code we wrote (it simply continues our loop) and catastrophic if it
; is mid-function in an arbitrary Windows thread. A stub makes it harmless.
;
; THE PROTOCOL
;
;   guest:  vmmcall                        ; "tick"
;   host :  SvHandleVmExit -> VMEXIT_VMMCALL
;             if (stop requested)  ExitVm = TRUE   -> SvLV20 -> resume at +next
;             else                 (continue)       -> SvLV10 -> vmrun again
;
; When ExitVm is set, SvLV20 resumes the guest at NRip with EFER.SVME already
; cleared. The guest then falls out of the loop and executes `ret`-like idle code
; that must never be reached by a live core - so the loop's exit path parks on a
; `hlt` guarded by `cli`, and the owning thread terminates from the host side.
;
; RSP: the stub never touches the stack. It is given the host stack pointer by
; SvPrepareForVirtualization (StateSaveArea.Rsp = ContextRecord->Rsp), and since
; it neither pushes nor calls, that value is never dereferenced. That is the one
; property that makes a stub safe to enter on a stack that belongs to the host.
;
Sv206GuestStub proc
    ; 9.464: HLT FOLLOWED BY VMMCALL - the root fix, and why neither alone worked.
    ;
    ; THE PROBLEM, STATED EXACTLY
    ; A core must satisfy two things at once while the machine is idle:
    ;   (1) NOT burn CPU, which requires hlt (a spin cannot stop consuming cycles)
    ;   (2) still reach Sv206VmrunShouldExit periodically, which requires a VMEXIT,
    ;       and a core that never executes a VMRUN never re-enters the exit path.
    ;
    ; 9.462 got (1) and lost (2): `hlt; jmp` parked forever, invmrun stayed at 0xc,
    ; and DriverUnload unmapped the image under twelve parked cores -> 0xD1
    ; PAGED_IP at Sv206LaunchVm+0x1e with rcx = 0xa.
    ;
    ; 9.463 got (2) and lost (1): a 100000-iteration pause loop still ran at 100%
    ; CPU (measured with Get-Counter: baseline 29%, loaded 100%), it only lowered
    ; the VMEXIT rate.
    ;
    ; WHY THE TWO TOGETHER WORK, AND WHY THE EARLIER HOST-SIDE WAKES COULD NOT
    ; The missing fact is what happens when hlt is interrupted. The core wakes,
    ; executes the NEXT INSTRUCTION, and continues. In 9.462 that next instruction
    ; was `jmp` - back to hlt - so a wake produced no exit and the stop flag was
    ; never read. That is why injecting EventInj from the host failed three times:
    ; the injection DID wake the core (that part was never broken), but waking it
    ; accomplished nothing because the instruction after hlt could not report it.
    ;
    ; Put `vmmcall` immediately after hlt and every wake becomes a VMEXIT:
    ;   - idle: the core sits in hlt, consuming no cycles          -> (1) satisfied
    ;   - interrupted: it wakes, hits vmmcall, exits, the handler runs
    ;     Sv206VmrunShouldExit and parks again or leaves            -> (2) satisfied
    ;
    ; The wake source is Windows itself. These cores still receive timer and IPI
    ; interrupts, and they are delivered to the guest because INTR is NOT intercepted
    ; here (checked the prepared set: CPUID, MSR_PROT, RDTSC/RDTSCP, VMRUN/VMMCALL,
    ; SHUTDOWN). So the exit rate is bounded by the interrupt rate, not by how fast
    ; the stub can spin - which is the property a spin can never have.
    ;
    ; IF MUST BE 1 for a maskable interrupt to wake hlt. The guest's RFLAGS comes
    ; from ContextRecord->EFlags on a kernel thread that is not at DISPATCH_LEVEL,
    ; and InterruptShadow is left 0, so there is no shadow window holding them off.
    ; If that ever becomes false the core would wake only on NMI/SMI - which is
    ; exactly the hang 9.462 produced, so this is the assumption to check first if
    ; a future change makes the stub stop leaving hlt.
Sv206GuestTick:
    ; 9.465: hlt stays (harmless, and instantly correct on a C-state-capable
    ; machine) but it does NOT park here, because this platform has no C-state:
    ; measured with powercfg, only S3 exists - no C1/C2/C3. With no C-state to
    ; enter, hlt returns immediately and the loop below is what actually keeps
    ; the core occupied. See 9.464 for the four-part proof that every hlt wake
    ; condition (IF=1, VIntr=0, shadow=0, EventInj=0) was already satisfied and
    ; hlt still returned on every pass.
    ;
    ; THE COUNT IS THE WHOLE TUNING KNOB. Measured relationship on this machine:
    ;   no pause loop        -> ~12.6 MILLION exits/s   (0.95us per iteration)
    ;   100000 pause         -> ~13.7 million exits/s   (did not help: see below)
    ;
    ; Why 100000 pause changed nothing: `pause` on this AMD core is ~1-3 cycles,
    ; not the ~30 that ended up in an earlier estimate, so 100000 of them is a
    ; quarter of a microsecond - invisible next to the VMRUN round trip it sits
    ; inside. The delay has to be two orders of magnitude larger to matter.
    ;
    ; 1000000 pause at ~2 cycles each is ~2M cycles at 3GHz ~= 0.7ms per
    ; iteration, so ~1400 exits/s per core, ~17000/s across twelve cores. That is
    ; below any rate a guest could notice, while a stop request is still seen
    ; within a millisecond - faster than the old 4-pause loop ever was.
    ;
    ; WHAT THIS DOES NOT FIX, STATED PLAINLY: the core still spins, so CPU stays
    ; at 100%. This changes how often the core reports in, not whether it works.
    ; Genuinely freeing the core needs C-states (absent) or not-virtualising it
    ; (a different architecture). Chosen deliberately per operator direction.
Sv206Spin:
    hlt                         ; 9.516 re-test: park in C1, no pause loop
    mov     rax, 1              ; YGHV_CMD_HEARTBEAT - report the wake
    vmmcall                     ; the ONE intercepted instruction
    jmp     Sv206Spin

    ; Reached only when the handler set ExitVm and SvLV20 resumed us here with
    ; SVM already disabled.
    ;
    ; THIS MUST NOT PARK, AND THE REASON IS THE WHOLE POINT OF 9.443.
    ;
    ; Parking (cli/hlt) would leave this core executing code from the driver image
    ; after DriverUnload returns - which is the PAGED_IP crash this design exists to
    ; remove, just moved from `vmsave` to `hlt`. A stub that parks has not solved
    ; anything; it has only renamed the fault.
    ;
    ; Instead the ExitVm path hands us a HOST stack pointer and a HOST continuation
    ; address (SvHandleVmExit sets VpRegs->Rcx and VpRegs->Rbx for that purpose).
    ; SvLV20 then does `mov rsp, rcx; jmp rbx`, so control leaves the guest for good
    ; and lands in C code running on this core's own host stack. That continuation
    ; terminates the thread, so nothing of ours is left executing when the image is
    ; unmapped.
    ;
    ; This label is therefore unreachable by design. It is kept as a `ud2` so that
    ; if the protocol is ever broken, the failure is an immediate, attributable
    ; exception at a known address rather than a silent fall through into whatever
    ; bytes happen to follow.
Sv206GuestParked:
    ud2
Sv206GuestStub endp

;
; ============================================================================
; 9.444: LEAVING THE GUEST ON THE THREAD'S OWN STACK.
; ============================================================================
;
; 9.443 pointed SvLV20 at a C function (Sv206GuestExited) and the machine died with
; 0x1E C0000005 / STACKPTR_ERROR. The stack showed the #GP inside KiSwapContext,
; reached from PsTerminateSystemThread. The cause is structural, not a bug in that
; function:
;
;   SvLV20 does `mov rsp, rcx`, and 9.443 gave it &VpData->HostStackLayout - a
;   stack THIS DRIVER allocated. A context switch saves RSP into the KTHREAD and
;   restores it later; from a foreign stack the scheduler's frame lands where it
;   cannot survive, and the switch faults.
;
; So the exit must land on the thread's REAL stack. That stack is still intact:
; Sv206CoreThread called SvVirtualizeProcessor, which called Sv206LaunchVm, and all
; those frames are sitting on it. The VMRUN loop never unwound them, but they are
; still there - one `ret` away.
;
; This thunk is the exit target. The ExitVm path hands SvLV20:
;   rcx = the thread RSP saved by Sv206CoreThread immediately before its call
;   rbx = Sv206LeaveToHost
;
; SvLV20 then does `mov rsp, rcx; mov ecx,'SSVM'; jmp rbx`, landing here on the real
; stack, with interrupts already enabled (the ExitVm path restored RFLAGS) and SVM
; already disabled. All that is left is to unwind the frames the VMRUN loop left
; behind and return to Sv206CoreThread, which terminates the thread from a context
; where every kernel API is legal.
;
; This thunk must not call anything: it only moves RSP and returns.
; ---------------------------------------------------------------------------
; 9.551: A CALLABLE VMMCALL.
;
; The only other `vmmcall` in this file is inside Sv206GuestStub - instructions the
; GUEST executes, not something C can call. The real-guest path needs to tick once
; from C so the stop handshake has an exit to answer, so it needs this.
;
; vmmcall is intercepted (it is in the prepared intercept set), so this raises
; #VMEXIT on the current core. The handler then runs Sv206VmrunShouldExit exactly
; as it does for a stub tick: if a stop was requested it sets ExitVm and the normal
; exit protocol unwinds the VMRUN frame; if not, execution resumes at the
; instruction after this one and this function returns.
;
; No stack use, no register saves: it is a leaf that does one instruction.
; ---------------------------------------------------------------------------
Sv206TickVmmcall proc
    vmmcall
    ret
Sv206TickVmmcall endp

Sv206LeaveToHost proc
    ;
    ; ARRIVE ON THE THREAD'S REAL STACK AND END THE THREAD FROM ASM.
    ;
    ; SvLV20 has already done `mov rsp, rcx` with the value Sv206LaunchVm captured
    ; before it switched stacks, so RSP is now the thread's own stack pointer as it
    ; was at the call into Sv206LaunchVm - i.e. [rsp] is that call's return address
    ; and everything above it is the live C frames (SvVirtualizeProcessor,
    ; Sv206VirtualizeC, Sv206CoreThread).
    ;
    ; WHAT NOT TO DO, AND WHY (four machines)
    ;
    ; 9.443-9.444 tried to `ret` or `jmp` into those C frames to unwind them by hand.
    ; Every attempt broke a different ABI invariant:
    ;   9.443   returned onto a driver-allocated stack  -> 0x1E, #GP in KiSwapContext
    ;   9.444a  returned to a function's own cookie slot -> 0x0E at KiStartSystemThread
    ;   9.444b  indexed the saved RSP wrongly           -> 0x50 at MiSystemFault
    ;   9.444c  hand-unwound past a /GS frame           -> 0xF7 _report_gsfailure
    ;
    ; The lesson is not "compute the offset better". It is that a C function's frame
    ; may only be left by ITS OWN epilogue: it owns the /GS cookie, the SEH chain and
    ; the alignment. So this thunk does not touch those frames at all - it ends the
    ; thread from here, on the correct stack, and lets the scheduler discard them.
    ;
    ; That is legal because we are on the thread's real stack with interrupts enabled
    ; and SVM already off (the ExitVm path cleared EFER.SVME and restored RFLAGS).
    ;
    ; 9.445's first version missed one detail and cost a fifth machine: 0x1E C0000005
    ; faulting at THIS thunk's own `hlt`. A bare `call rax` pushes its return address
    ; at [rsp-8], which is the slot just below the stack pointer Sv206LaunchVm's
    ; caller expects untouched - and if the terminate routine ever returns, execution
    ; walks into whatever that push displaced.
    ;
    ; So the call is made BELOW the saved pointer, on a frame with the shadow space
    ; the x64 ABI guarantees: `sub rsp,28h` = 0x20 shadow + 8 alignment. Nothing at or
    ; above the original RSP is written.
    ;
    ; Rax holds PsTerminateSystemThread, placed there by SvHandleVmExit, so this file
    ; needs no kernel import.
    ;
    ;
    ; 9.555: THIS CORE HAS LEFT FOR GOOD - SAY SO BEFORE TERMINATING.
    ;
    ; Sv206CoreThread increments this after Sv206VirtualizeC returns, but this
    ; path never returns: the call below ends the thread. So the unload waiter
    ; (main.c:5123) compared a counter that could never reach the core count and
    ; always ran its full budget - measured ~4 s of pure spin on every unload.
    ;
    ; lock inc with a memory operand: it touches NO register, so rax
    ; (PsTerminateSystemThread) and rcx (STATUS_SUCCESS) are still exactly what
    ; the call below needs. Placed before the stack adjustment so the sequence
    ; 9.445 proved correct is byte-for-byte unchanged.
    ;
    lock inc dword ptr [g_S206CoresExitedAsm]

    sub     rsp, 28h
    xor     ecx, ecx                ; STATUS_SUCCESS
    call    rax                     ; terminate the thread - does not return
    ;
    ; Not reached. If it ever is, restore and park without touching anything.
    ;
    add     rsp, 28h
Sv206LeavePark:
    hlt
    jmp     Sv206LeavePark
Sv206LeaveToHost endp

;

        end
