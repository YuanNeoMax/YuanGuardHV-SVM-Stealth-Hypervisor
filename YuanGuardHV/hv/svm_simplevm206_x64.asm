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

; 9.444: the C half of the 206 entry (see Sv206EnterHypervisor). Declared here so
; the entry trampoline can call it; the name is unmangled because it is extern "C".
extern Sv206VirtualizeC : proc

; 9.444: per-core exit stack pointers. The entry trampoline fills the slot for its
; own core before the loop starts; SvHandleVmExit reads it on every VMMCALL.
extern g_S206ThreadRsp : qword

; 9.422: the in-vmrun counter. Bumped before vmrun, dropped immediately after
; vmsave. The stop-flag externs of 9.416 are gone: those variables are static
; again (9.419) because the inline check they served was proven useless (9.417).
extern g_inVmrunCount : dword

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
    ; A short spin between ticks keeps this from being a pure VMEXIT storm. Each
    ; iteration still exits, so the stop flag is observed within one spin, but the
    ; exit rate is bounded rather than maximal.
Sv206GuestTick:
    pause
    pause
    pause
    pause
    mov     rax, 1              ; YGHV_CMD_HEARTBEAT - "I am alive, check the flag"
    vmmcall                     ; <-- the ONLY intercepted instruction in the loop
    jmp     Sv206GuestTick

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
Sv206LeaveToHost proc
    ;
    ; ARRIVE ON THE THREAD'S OWN STACK, THEN RETURN ONE FRAME.
    ;
    ; SvLV20 has already executed `mov rsp, rcx` with the value Sv206EnterHypervisor
    ; captured, so RSP now points exactly at the return-address slot that
    ; `call Sv206VirtualizeC` pushed. A single `ret` pops it and resumes
    ; Sv206EnterHypervisor right after that call - on the thread's real stack, which
    ; is the only stack a context switch can survive on.
    ;
    ; 9.443 and the first 9.444 attempt both failed here for different reasons:
    ;   9.443  handed SvLV20 a DRIVER-ALLOCATED stack; the scheduler faulted inside
    ;          KiSwapContext (0x1E C0000005 / STACKPTR_ERROR).
    ;   9.444a did a bare `ret` from the WRONG pointer (a function's own
    ;          return-address slot instead of the call site), so it popped a saved
    ;          register and jumped into it (0x0E at nt!KiStartSystemThread+0x34).
    ;
    ; This version is exact: the pointer was captured by the same function that made
    ; the call, eight bytes below its own entry RSP, so [rsp] here is guaranteed to
    ; be that call's return address.
    ;
    ; Nothing else is needed. SVM is already off (the ExitVm path cleared
    ; EFER.SVME) and interrupts are already enabled (it restored RFLAGS).
    ;
    ret
Sv206LeaveToHost endp

;
; ============================================================================
; 9.444: THE ENTRY TRAMPOLINE - CAPTURE THE EXIT POINT, THEN ENTER THE LOOP.
; ============================================================================
;
; Called as:  Sv206EnterHypervisor(SharedVpData, coreIndex)
;   rcx = SharedVpData   (forwarded to the C wrapper)
;   rdx = core index     (selects the g_S206ThreadRsp slot to fill)
;
; THE SLOT MUST BE FILLED BEFORE THE LOOP RUNS, NOT AFTER IT RETURNS.
;
; An earlier revision took an `&exitRsp` out-parameter and stored it in C after this
; function returned. That is too late: SvHandleVmExit reads g_S206ThreadRsp[core] on
; every VMMCALL, including the very first one - by which time the slot must already
; hold the exit stack. Writing it here, before the call into the loop, removes the
; ordering dependency entirely.
;
; WHY THIS IS ASM AND NOT C
;
; The value stored must be the address where `call Sv206VirtualizeC` pushes its
; return address - i.e. (RSP after alignment) - 8. Capturing that from C means
; asking the compiler where its own stack pointer is at an arbitrary point, and the
; optimiser is free to put anything in between. Measured cost of getting it wrong
; once already: 0x0E at nt!KiStartSystemThread+0x34.
;
; Here the alignment and the capture are three instructions apart, so they cannot
; drift.
;
; THE TWO-STEP RETURN, WHICH IS THE WHOLE TRICK
;
;   entry            RSP = S        [S]   = return address into Sv206CoreThread
;   sub rsp, 8       RSP = S-8      (aligns the call, see below)
;   exitRsp = S-16                  <-- where `call` will push
;   call wrapper     RSP = S-16     [S-16] = return address into THIS function
;   ... VMRUN loop runs, never returns normally ...
;   SvLV20: mov rsp, S-16 ; jmp thunk
;   thunk:  ret      pops [S-16] -> back into THIS function, RSP = S-8
;   add rsp, 8       RSP = S
;   ret              pops [S]    -> back into Sv206CoreThread, RSP = S+8
;
; Both returns land on the thread's own stack, and Sv206CoreThread's frame - its
; locals and saved registers - is exactly as it left it. That is what makes
; PsTerminateSystemThread legal at the end of it.
;
; THE ALIGNMENT
;
; On entry RSP is 8 mod 16 (the caller's `call` pushed 8). A callee expects the same
; at ITS entry, so the call site must be 0 mod 16. Hence `sub rsp, 8` before the
; call, and the matching `add rsp, 8` after it.
;
Sv206EnterHypervisor proc
    sub     rsp, 8
    ;
    ; Record where the CALL below will push its return address, into this core's
    ; slot, BEFORE entering the loop - SvHandleVmExit reads it on the very first
    ; VMMCALL and must find a valid stack there.
    ;
    lea     rax, [rsp - 8]
    lea     r8, g_S206ThreadRsp
    mov     qword ptr [r8 + rdx*8], rax
    call    Sv206VirtualizeC
    add     rsp, 8
    ret
Sv206EnterHypervisor endp

        end
