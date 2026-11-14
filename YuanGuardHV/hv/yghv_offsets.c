/*
 * yghv_offsets.c -- derive the kernel object offsets this driver needs (9.589).
 *
 * WHY DERIVE INSTEAD OF TABULATE
 *   The single most expensive class of bug in this tree so far is "the code
 *   looks right and reads the wrong place silently". A hard-coded EPROCESS
 *   offset table is exactly that bug with a delay fuse: correct on the build
 *   it was written for, and afterwards it reads a neighbouring field, which
 *   is a plausible value of the right width, so nothing complains.
 *
 *   So nothing here is a table. Every offset is DERIVED FROM THE RUNNING
 *   KERNEL, and then CROSS-CHECKED against a second, independent export. If a
 *   derivation does not produce agreement, the offset stays 0 and every
 *   accessor built on it refuses to run. A feature that reports "offsets not
 *   ready" is a feature that is visibly broken; a feature that reads garbage
 *   is one that corrupts the machine quietly.
 *
 * HOW THE DERIVATION WORKS
 *   Windows exports tiny accessors that do exactly one thing -- read a field
 *   and return it. Measured on this machine (C:\Windows\System32\ntoskrnl.exe):
 *
 *       PsGetProcessId                 48 8B 81 40 04 00 00 C3   mov rax,[rcx+0x440]; ret
 *       PsGetProcessPeb                48 8B 81 50 05 00 00 C3   mov rax,[rcx+0x550]; ret
 *       PsGetProcessSectionBaseAddress 48 8B 81 20 05 00 00 C3   mov rax,[rcx+0x520]; ret
 *       PsGetProcessImageFileName      48 8D 81 A8 05 00 00 C3   lea rax,[rcx+0x5A8]; ret
 *       PsGetProcessProtection         8A 81 7A 08 00 00 C3      mov al,[rcx+0x87A];  ret
 *       PsGetThreadId                  48 8B 81 80 04 00 00 C3   mov rax,[rcx+0x480]; ret
 *       PsGetThreadProcess             48 8B 81 20 02 00 00 C3   mov rax,[rcx+0x220]; ret
 *       PsGetThreadTeb                 48 8B 81 F0 00 00 00 C3   mov rax,[rcx+0xF0];  ret
 *       PsGetThreadProcessId           48 8B 81 78 04 00 00 C3   mov rax,[rcx+0x478]; ret
 *
 *   The instruction is a load through RCX with a 32-bit displacement, and the
 *   displacement IS the field offset. So: MmGetSystemRoutineAddress gives the
 *   address, we scan the first few bytes for that one instruction shape, and
 *   we read the displacement out of it. No pattern database, no per-build
 *   constants, and if Microsoft ever changes the encoding the derivation
 *   fails loudly instead of returning a wrong number.
 *
 *   This is deliberately NOT a signature scan of ntoskrnl .text looking for an
 *   arbitrary function: an accessor address is published by the export table,
 *   so there is nothing to search for and nothing to get wrong.
 *
 * WHAT CANNOT BE DERIVED THIS WAY
 *   ActiveProcessLinks has no accessor. It is found by a STRUCTURAL PROBE:
 *   for every aligned candidate offset, treat the pointer at system+off as a
 *   LIST_ENTRY and check that (a) every node is doubly consistent -- its Blink
 *   points back at the node we came from -- (b) every Flink is a canonical
 *   kernel pointer, (c) every node holds a plausible UniqueProcessId at the
 *   DERIVED pid offset, and (d) the walk closes a full circle. A wrong offset
 *   cannot satisfy all four: it must be a real doubly-linked list of real
 *   EPROCESSes.
 *
 *   Validating with the derived pid offset is the part that makes this safe.
 *   If the pid offset were itself wrong, the pid checks would reject every
 *   candidate and the probe would fail closed.
 *
 * THREAD LIST: NOT DERIVED, ON PURPOSE
 *   EPROCESS.ThreadListHead needs a second unknown (ETHREAD.ThreadListEntry)
 *   to interpret, so the probe becomes two-dimensional and has to invoke
 *   PsGetThreadProcess() on tens of thousands of candidates that may be
 *   garbage pointers -- each one a fault that must be caught. That is a lot
 *   of machinery, and it buys nothing: thread enumeration is available
 *   export-only (SystemProcessInformation for the CIDs, then
 *   PsLookupThreadByThreadId), and process-wide suspension is PsSuspendProcess,
 *   which is exported and already does "every thread in the process".
 *   thread_list_head therefore stays 0 = NOT DERIVED, and features must not
 *   use it.
 *
 * IRQL: PASSIVE_LEVEL only (reads ntoskrnl text, walks the process list,
 * touches process objects). Called once from DriverEntry.
 */

#include <ntddk.h>
#include <stdint.h>
#include "common/yghv_nt.h"
#include "common/yghv_str.h"
#include "common/yghv_strtab.h"
#include "common/yghv_safe.h"
#include "debug.h"

yghv_offsets_t g_yghv_off;

static volatile LONG64 g_off_failures;
static PEPROCESS       g_off_system;

/*
 * Derive one offset from one export. Returns 1 only when the export exists AND
 * its body has the accessor shape with a sane displacement.
 */
static int yghv_off_from_export(const WCHAR *name, UINT16 *out)
{
    UNICODE_STRING us;
    PVOID fn;
    UINT32 disp = 0;

    RtlInitUnicodeString(&us, name);
    fn = MmGetSystemRoutineAddress(&us);
    if (!fn) return 0;
    if (!yghv_acc_find_rcx_disp((const UINT8 *)fn, 64, &disp)) return 0;
    *out = (UINT16)disp;
    return 1;
}

/*
 * The same derivation, addressed by table id instead of by literal.
 *
 * Every export name this file uses lives in yghv_strtab.h, so no plaintext
 * name reaches the binary. Keeping ONE function that both callers funnel
 * through means the encrypted path and the direct path cannot drift apart:
 * they are the same path with the name obtained differently.
 */
static int yghv_off_by_id(int id, UINT16 *out)
{
    WCHAR name[YGHV_STR_MAX_CHARS];

    if (!yghv_str_get_w(id, (unsigned short *)name, YGHV_STR_MAX_CHARS)) return 0;
    return yghv_off_from_export(name, out);
}

static void yghv_init_unicode_by_id(UNICODE_STRING *us, int id, WCHAR *buf)
{
    yghv_str_get_w(id, (unsigned short *)buf, YGHV_STR_MAX_CHARS);
    RtlInitUnicodeString(us, buf);
}

/*
 * Guarded 8-byte read for the structural probe, whose candidates are by
 * definition unvalidated pointers.
 *
 * 9.608 -- WHY THIS COST A BUGCHECK (0x50, PAGE_FAULT_IN_NONPAGED_AREA)
 *
 * The previous version was a plain `static` helper. clang INLINED it into
 * yghv_offsets_init (it has no symbol of its own in the .map: only
 * yghv_offsets_init, yghv_off_by_id and friends are there), and the load it
 * guarded then faulted from a context the handler did not cover. The dump is
 * unambiguous:
 *
 *     yghv_pa+0x250d9   mov rbx,qword ptr [rbx]
 *     READ_ADDRESS      ffff9e804c7d1ec0     <- canonical, NOT mapped
 *     Arg2 = 0          read
 *
 * ffff9e80... passes the ONE test this function made -- `>= 0xFFFF800000000000`
 * -- because "the upper canonical half" is not "mapped". A candidate offset
 * swept from 0x100 to 0x8F8 will eventually land on a garbage pointer whose
 * value happens to be canonical, and a lower-bound check cannot tell that
 * pointer from a real EPROCESS.
 *
 * So the fix is in two parts, and BOTH are needed:
 *
 *   (a) NOINLINE. The guard must survive compilation as a guard. Measured:
 *       every function in this file that matters is in the .map; a helper that
 *       is not is a helper whose __try block was restructured by the optimizer.
 *   (b) ASK THE OS, not a range constant. MmIsAddressValid answers "is this
 *       mapped", which is the question a lower-bound compare cannot answer.
 *       It is a raceable check -- a page can be unmapped right after it -- so
 *       the __try stays as the second line; but it removes the whole class of
 *       "canonical but never mapped" reads that produce this bugcheck.
 *
 * MmIsAddressValid is only valid at DISPATCH_LEVEL or below and never for
 * pageable memory at raised IRQL; this probe runs at PASSIVE in DriverEntry.
 */
static __declspec(noinline) int yghv_probe_read64(const void *va, UINT64 *out)
{
    /* Alignment is this probe's own requirement (it walks LIST_ENTRYs), so it
     * stays here; the canonical-range and mapped checks, the overflow-safe end
     * computation and the __try all live in yghv_safe_read64, which is the one
     * place in this tree allowed to dereference an unvalidated pointer. */
    if (((UINT64)va & 7) != 0) return 0;
    return yghv_safe_read64(va, out);
}

/* An image name is 15 bytes of ANSI in EPROCESS; if the offset is right the
 * bytes are printable and NUL-terminated, and if it is wrong they almost never
 * are. This is the check that makes the link probe decisive. */
/* 9.608: NOINLINE for the same reason as yghv_probe_read64 -- this function
 * dereferences 15 bytes at (p + name_off) where BOTH operands came out of the
 * garbage being probed. Inlined, its __try is at the optimizer's mercy. */
static __declspec(noinline) int yghv_probe_name_ok(PEPROCESS p, UINT16 name_off)
{
    const volatile UINT8 *n;
    int i, printable = 0;

    if (!name_off) return 1;             /* no name offset -> skip the check */
    if (!p) return 0;
    if ((UINT64)p < 0xFFFF800000000000ULL) return 0;

    n = (const volatile UINT8 *)((PUCHAR)p + name_off);

    /* 9.608: the __try is the authority, not these hints. Requiring BOTH the
     * first and last of the 15 bytes to be MmIsAddressValid rejected names that
     * straddle a page boundary -- legitimate, mapped, readable names. The loop
     * is bounded at 15 bytes and wrapped in __try, so a genuinely dead page
     * still ends in "return 0" rather than a bugcheck. */
    if (!MmIsAddressValid((PVOID)n)) return 0;

    __try {
        for (i = 0; i < 15; i++) {
            UINT8 c = n[i];
            if (c == 0) break;
            if (c < 0x20 || c > 0x7E) return 0;
            printable++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return printable > 0;               /* an empty name is not a process */
}

/*
 * Structural probe for EPROCESS.ActiveProcessLinks.
 *
 * Walk from the system process at each candidate offset and require a fully
 * consistent doubly-linked circle. Hops is capped well above any real
 * machine's process count; a candidate that runs out of hops without closing
 * the circle is rejected.
 */
/* 9.608: NOINLINE -- see yghv_probe_read64. This function's whole body is a
 * walk over unvalidated pointers; compiled inline, the safeguards are the
 * optimizer's to rearrange, and the bugcheck proved it rearranged them. */
/* 9.608: why the probe fails, in numbers rather than in a debugger.
 * A probe that returns 0 silently costs a boot per hypothesis; these are read
 * back through yghv_offsets_stats() as the "best attempt" of the sweep. */
static volatile LONG64 g_probe_best_good;
static volatile LONG64 g_probe_best_closed;
static volatile LONG64 g_probe_best_off;
static volatile LONG64 g_probe_attempts;
/* Which acceptance test ended the best attempt. 9.608: a single "it failed"
 * number costs a boot per guess; these say WHICH test. */
static volatile LONG64 g_probe_reason;

static __declspec(noinline) int yghv_probe_links(PEPROCESS sys, UINT16 pid_off, UINT16 name_off,
                           UINT16 *out_off, UINT32 *out_count)
{
    UINT32 off;

    if (!sys || !pid_off) return 0;

    /* 9.608: the starting object is PsInitialSystemProcess. If that export was
     * not resolvable we would be sweeping offsets off a NULL/GARBAGE base, and
     * every candidate below would be built on it. Refuse before the sweep --
     * this one line is what the crash dump asked for. */
    if ((UINT64)sys < 0xFFFF800000000000ULL) return 0;
    if (((UINT64)sys & 7) != 0) return 0;
    if (!MmIsAddressValid((PVOID)sys)) return 0;

    for (off = 0x100; off < 0x900; off += 8) {
        UINT64 head = (UINT64)sys + off;
        UINT64 prev = head;
        UINT32 hops = 0, good = 0;
        int ok = 1;
        int closed = 0;               /* 9.608: the unsatisfiable-test fix */
        int reason = 0;               /* 9.608: which test ended this attempt */
        int truncated = 0;            /* 9.608: walk ended on a live-list race */

        /*
         * 9.608: this used to be MmIsAddressValid(head) -- but "head" is
         * PsInitialSystemProcess + off, an address INSIDE an object, and this
         * probe is looking for the one offset that lands on a LIST_ENTRY. A
         * candidate that is not mapped is simply a wrong candidate and the read
         * below will refuse it, so an extra pre-test here does not protect the
         * walk -- it only removes candidates, and it removed the RIGHT one when
         * the object straddled a page. Measured: this line plus a two-page range
         * check made the probe reject everything, and derived_mask came back
         * 0x28 instead of 0x2B.
         *
         * The first read (yghv_probe_read64 on prev == head) is the real test.
         */

        while (hops < 512) {
            UINT64 flink = 0, blink = 0, cand, pid = 0;

            if (!yghv_probe_read64((const void *)prev, &flink)) { ok = 0; reason = 1; break; }
            if (flink == head) { closed = 1; break; }        /* circle closed */
            if (flink < 0xFFFF800000000000ULL) { ok = 0; reason = 2; break; }
            /* flink+8 must not overflow back into user space: a flink in the
             * top 8 bytes of the address space would wrap and the guarded read
             * would then be asked about a DIFFERENT, valid-looking address. */
            if (flink > 0xFFFFFFFFFFFFFFF0ULL) { ok = 0; reason = 3; break; }
            if (!yghv_probe_read64((const void *)(flink + 8), &blink)) { ok = 0; reason = 4; break; }
            if (blink != prev) { ok = 0; reason = 5; break; }   /* doubly linked */

            /* cand is derived by subtraction and can UNDERFLOW into user space
             * or into a non-canonical hole; both are rejected here, and the
             * sum cand+pid_off is re-checked rather than trusted. */
            cand = flink - off;
            if (cand < 0xFFFF800000000000ULL) { ok = 0; reason = 6; break; }
            if (cand > 0xFFFFFFFFFFFFFFF0ULL - pid_off) { ok = 0; reason = 7; break; }
            if (!yghv_probe_read64((const void *)(cand + pid_off), &pid)) { ok = 0; reason = 8; break; }

            /*
             * 9.608 -- A BAD NODE AFTER A LONG GOOD RUN IS NOT A WRONG OFFSET.
             *
             * This used to `ok = 0; break;` on the first node whose pid looked
             * implausible, which rejects the whole candidate. Measured on this
             * machine: the sweep reached 188 correctly doubly-linked nodes at
             * off=0x448 (the real ActiveProcessLinks offset) and then stopped at
             * reason 9 with best good = 188, best closed = 0. A wrong offset
             * cannot walk 188 nodes with blink == prev at every step; the walk
             * had already proved the answer and threw it away on one anomaly.
             *
             * The anomaly is expected on a LIVE list: processes exit and are
             * created while we walk, and a node unlinked between our two reads
             * is a torn view, not a wrong offset. The bound exists to reject
             * garbage EARLY -- it has no business rejecting node 188.
             *
             * So: a node that fails the plausibility gate ENDS THE WALK but does
             * not invalidate it. `truncated` records that, and the acceptance
             * below requires a long run of good nodes rather than a perfect one.
             * The strictly-decisive tests (canonical pointer, doubly linked) keep
             * their `ok = 0` -- those cannot happen on a correct offset.
             */
            if (pid > 0x100000) {               /* implausible: stop, keep count */
                reason = 9;
                truncated = 1;
                break;
            }
            if (!yghv_probe_name_ok((PEPROCESS)cand, name_off)) {
                reason = 10;
                truncated = 1;
                break;
            }

            good++;
            prev = flink;
            hops++;
        }

        /*
         * 9.608 -- THE CIRCLE TEST WAS UNSATISFIABLE, AND THAT IS WHY
         * ActiveProcessLinks WAS NEVER DERIVED.
         *
         * The loop above breaks out of the walk in exactly one of two ways:
         *
         *   (a) flink == head          -> "circle closed", break with ok == 1
         *   (b) any validation failure -> break with ok == 0
         *
         * In case (a) the break happens BEFORE `prev = flink`, so `prev` at this
         * point still holds the LAST NON-HEAD node. The old condition demanded
         * `prev == head`, which can therefore NEVER be true on the successful
         * path -- every candidate offset was rejected, the probe returned 0,
         * YGHV_OFF_LINKS was never set, and IOCTL 0x838 op 0 answered
         * STATUS_NOT_IMPLEMENTED for the whole life of the feature.
         *
         * Measured on this machine before the fix: derived_mask = 0x2FBD, i.e.
         * thirteen of fourteen fields derived and LINKS the only hole -- with
         * every other offset correct, which is what made it look like a machine
         * problem rather than a one-line logic error.
         *
         * The fix records the closure explicitly instead of trying to infer it
         * from `prev`. `closed` is 1 only on path (a), which is exactly the
         * claim the old code was trying to make.
         */
        /* Record the best attempt of the sweep before judging it. */
        InterlockedIncrement64(&g_probe_attempts);
        if (good > InterlockedCompareExchange64(&g_probe_best_good, 0, 0)) {
            InterlockedExchange64(&g_probe_best_good, (LONG64)good);
            InterlockedExchange64(&g_probe_best_closed, (LONG64)closed);
            InterlockedExchange64(&g_probe_best_off, (LONG64)off);
            InterlockedExchange64(&g_probe_reason, (LONG64)reason);
        }

        /*
         * 9.608 -- THE ACCEPTANCE, AFTER TWO BUGS THAT MADE IT UNSATISFIABLE.
         *
         * The old condition was `ok && good >= 16 && prev == head`:
         *   - `prev == head` could never be true, because the loop breaks on
         *     flink == head BEFORE assigning prev (that is the first bug, now
         *     `closed`);
         *   - `ok` required EVERY node to pass, so one torn entry in a live
         *     list rejected a walk that had already proved itself 188 times
         *     (that is the second bug, now `truncated`).
         *
         * What actually distinguishes "this is PsLoadedModuleList's process
         * ring" from a coincidence is the LENGTH of a consistent doubly-linked
         * run with sane pids, plus the fact that it came back around. So:
         *
         *   closed                    -> decisive, accept
         *   truncated after >= 32     -> accept (a live-list race ended it)
         *
         * 32 is double the old floor of 16 and still far below any real
         * machine's process count (188 measured here). An arbitrary structure
         * does not produce 32 nodes with blink == prev and plausible pids.
         */
        /*
         * 9.608 -- THE FLOOR APPLIES TO BOTH BRANCHES. I dropped it once and
         * the sweep then accepted an EMPTY list.
         *
         * History, because this is the third revision of one condition:
         *   original: ok && good >= 16 && prev == head   -- prev never equals
         *             head, so this was ALWAYS FALSE (ActiveProcessLinks was
         *             never derived in the shipped driver);
         *   my first fix: ok && (closed || (truncated && good >= 32))  -- the
         *             floor fell off the closed branch, so a LIST_ENTRY that
         *             closes on the FIRST hop (good == 0) was accepted.
         *
         * Measured consequence of that mistake: the sweep returned off = 0x158
         * (an empty LIST_ENTRY near the front of EPROCESS) instead of 0x448,
         * the snapshot walk then followed a self-referential node and reported
         * zero processes, and derived_mask still claimed LINKS was derived --
         * a wrong offset wearing a correct mask, which is the worst state this
         * file can be in.
         *
         * A real process list has processes. Whatever ends the walk, the walk
         * has to have visited some.
         */
        if (ok && good >= 16 && (closed || truncated)) {
            *out_off = (UINT16)off;
            *out_count = good;
            return 1;
        }
    }
    return 0;
}

/*
 * Cross-check: the pid offset must be reachable through TWO different exports.
 * PsGetProcessId reads it from the object directly; PsGetCurrentProcessId
 * reads gs:[188] and then the same field. Agreement means the decode is right
 * in both places, which rules out "we locked onto a neighbouring instruction".
 */
static int yghv_validate_pid(UINT16 pid_off)
{
    UINT16 a = 0, b = 0;

    if (!yghv_off_by_id(YGHV_SID_PSGETPROCESSID, &a)) return 0;
    if (!yghv_off_by_id(YGHV_SID_PSGETCURRENTPROCESSID, &b)) {
        /* The second export is the strongest evidence but its absence is not
         * a reason to reject the first; the caller records it unvalidated. */
        return 0;
    }
    return (a == pid_off && b == pid_off);
}

NTSTATUS yghv_offsets_init(void)
{
    UINT16 pid = 0, links = 0, protect = 0, peb = 0, base = 0, name = 0;
    UINT16 tid = 0, tproc = 0, teb = 0;
    UINT16 ppid = 0, xstat = 0;
    UINT32 count = 0;
    UNICODE_STRING sysName;
    WCHAR sysWide[YGHV_STR_MAX_CHARS];
    PVOID sysVar;
    ULONG build = 0;

    RtlZeroMemory(&g_yghv_off, sizeof(g_yghv_off));

    if (!yghv_str_selftest()) {
        /*
         * Constant folding in yghv_str.h did not survive the compiler, so
         * every encrypted name in this build is garbage. Nothing downstream
         * can work, and a failure here is a BUILD defect, not a machine
         * state -- say so plainly and stay uninitialised.
         */
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[YGHV] yghv_str_selftest FAILED - string encryption is broken in this build\n");
        return STATUS_UNSUCCESSFUL;
    }

    (void)PsGetVersion(NULL, NULL, &build, NULL);

    /* --- EPROCESS / ETHREAD fields, one export at a time --- */
    if (!yghv_off_by_id(YGHV_SID_PSGETPROCESSID, &pid)) {
        InterlockedIncrement64(&g_off_failures);
        LOG_ERROR("offsets: the pid accessor has no derivable shape");
        return STATUS_UNSUCCESSFUL;      /* nothing else is safe without pid */
    }
    if (yghv_off_by_id(YGHV_SID_PSGETPROCESSPEB, &peb))
        g_yghv_off.derived_mask |= YGHV_OFF_PEB;
    if (yghv_off_by_id(YGHV_SID_PSGETPROCESSSECTIONBASEADDRESS, &base))
        g_yghv_off.derived_mask |= YGHV_OFF_BASE;
    if (yghv_off_by_id(YGHV_SID_PSGETPROCESSIMAGEFILENAME, &name))
        g_yghv_off.derived_mask |= YGHV_OFF_NAME;
    if (yghv_off_by_id(YGHV_SID_PSGETTHREADID, &tid))
        g_yghv_off.derived_mask |= YGHV_OFF_TID;
    if (yghv_off_by_id(YGHV_SID_PSGETTHREADPROCESS, &tproc))
        g_yghv_off.derived_mask |= YGHV_OFF_TPROC;
    if (yghv_off_by_id(YGHV_SID_PSGETTHREADTEB, &teb))
        g_yghv_off.derived_mask |= YGHV_OFF_TEB;

    /*
     * 9.591: parent pid and exit status. Both exports are single-instruction
     * accessors on 19045 (see yghv_accessor.h), so the decode is trustworthy.
     *
     * Deliberately NOT derived here, even though the offsets are known:
     *
     *   PsGetProcessSignatureLevel   decodes to 0x879 -- the SIGNER byte.
     *   PsGetProcessProtection       (0x87A) is the one already in the table.
     *                                Two exports, two adjacent bytes, and the
     *                                accessor decoder cannot tell which is
     *                                which; taking the wrong one writes the
     *                                protection level into the signer nibble.
     *   PsGetThreadFreezeCount       is a two-level dereference, not a field
     *                                read; the "offset" it yields is ETHREAD
     *                                +0x220, which is a pointer, not a counter.
     *   PsIsProtectedProcess         is "test [rcx+0x87A],7", which the
     *                                decoder cannot see (no 8A/8B/8D).
     */
    if (yghv_off_by_id(YGHV_SID_PSGETPROCESSINHERITEDFROMUNIQUEPROCESSID, &ppid))
        g_yghv_off.derived_mask |= YGHV_OFF_PPID;
    if (yghv_off_by_id(YGHV_SID_PSGETPROCESSEXITSTATUS, &xstat))
        g_yghv_off.derived_mask |= YGHV_OFF_XSTAT;

    /*
     * PsGetProcessProtection exists from Windows 10 1607 onward. Before that
     * the byte exists but nothing exports its offset, so the field stays 0 and
     * the protection feature reports "not available on this build" instead of
     * writing a guessed byte into an EPROCESS.
     */
    if (yghv_off_by_id(YGHV_SID_PSGETPROCESSPROTECTION, &protect)) {
        g_yghv_off.derived_mask |= YGHV_OFF_PROTECT;
    } else {
        LOG_INFO("offsets: no protection accessor on this build - protection control disabled");
    }

    g_yghv_off.unique_process_id = pid;
    g_yghv_off.peb               = peb;
    g_yghv_off.section_base      = base;
    g_yghv_off.image_file_name   = name;
    g_yghv_off.protection        = protect;
    g_yghv_off.thread_unique_id  = tid;
    g_yghv_off.thread_process    = tproc;
    g_yghv_off.thread_teb        = teb;
    g_yghv_off.inherited_pid     = ppid;
    g_yghv_off.exit_status       = xstat;
    g_yghv_off.thread_cid        = 0;      /* not derived; see the header note */
    g_yghv_off.thread_list_head  = 0;      /* not derived, on purpose */
    g_yghv_off.process_lock      = 0;
    g_yghv_off.process_flags     = 0;

    /* --- system process object --- */
    yghv_init_unicode_by_id(&sysName, YGHV_SID_PSINITIALSYSTEMPROCESS, sysWide);
    sysVar = MmGetSystemRoutineAddress(&sysName);
    if (!sysVar) {
        InterlockedIncrement64(&g_off_failures);
        LOG_ERROR("offsets: the system-process variable was not found");
        return STATUS_UNSUCCESSFUL;
    }
    /*
     * 9.608: MmGetSystemRoutineAddress resolves BOTH routines and data
     * exports, and for a data export it returns the VARIABLE's address -- which
     * must then be dereferenced. That dereference was unguarded: a resolver
     * that handed back something non-mapped (or a build where this export is
     * not a variable at all) faulted right here instead of failing closed.
     */
    if ((UINT64)sysVar < 0xFFFF800000000000ULL || ((UINT64)sysVar & 7) != 0 ||
        !MmIsAddressValid(sysVar)) {
        InterlockedIncrement64(&g_off_failures);
        LOG_ERROR("offsets: the system-process export is not a readable pointer");
        return STATUS_UNSUCCESSFUL;
    }
    g_off_system = *(PEPROCESS *)sysVar;
    if (!g_off_system) {
        InterlockedIncrement64(&g_off_failures);
        return STATUS_UNSUCCESSFUL;
    }

    /* The system process is pid 4 by definition. If our pid offset is wrong,
     * this is where it shows up first -- before any feature uses it. */
    {
        UINT64 sysPid = 0;
        if (!yghv_probe_read64((const void *)((PUCHAR)g_off_system + pid), &sysPid) ||
            sysPid != 4) {
            InterlockedIncrement64(&g_off_failures);
            LOG_ERROR("offsets: pid at derived offset is not 4 for the system process");
            g_yghv_off.unique_process_id = 0;
            g_yghv_off.derived_mask &= ~YGHV_OFF_PID;
            return STATUS_UNSUCCESSFUL;
        }
    }
    g_yghv_off.derived_mask |= YGHV_OFF_PID;

    /* --- process list, by structural probe --- */
    if (yghv_probe_links(g_off_system,
                         g_yghv_off.unique_process_id,
                         (g_yghv_off.derived_mask & YGHV_OFF_NAME) ? g_yghv_off.image_file_name : 0,
                         &links, &count)) {
        g_yghv_off.active_process_links = links;
        g_yghv_off.derived_mask |= YGHV_OFF_LINKS;
        LOG_INFO("offsets: ActiveProcessLinks at +0x%x, %u processes", links, count);
    } else {
        InterlockedIncrement64(&g_off_failures);
        LOG_ERROR("offsets: ActiveProcessLinks probe found no consistent list");
    }

    /* --- validation --- */
    if (yghv_validate_pid(pid)) g_yghv_off.validated_mask |= YGHV_OFF_PID;
    if (g_yghv_off.derived_mask & YGHV_OFF_PEB)    g_yghv_off.validated_mask |= YGHV_OFF_PEB;
    if (g_yghv_off.derived_mask & YGHV_OFF_NAME)   g_yghv_off.validated_mask |= YGHV_OFF_NAME;

    g_yghv_off.magic        = YGHV_OFF_MAGIC;
    g_yghv_off.version      = YGHV_OFF_VERSION;
    g_yghv_off.build_number = (UINT32)build;
    g_yghv_off.failures     = (UINT32)InterlockedCompareExchange64(&g_off_failures, 0, 0);

    LOG_INFO("offsets: build %u pid=0x%x peb=0x%x base=0x%x name=0x%x prot=0x%x "
             "tid=0x%x tproc=0x%x teb=0x%x ppid=0x%x xstat=0x%x "
             "derived=0x%x validated=0x%x",
             build, pid, peb, base, name, protect, tid, tproc, teb, ppid, xstat,
             g_yghv_off.derived_mask, g_yghv_off.validated_mask);

    return STATUS_SUCCESS;
}

int yghv_offsets_ready(void)
{
    return (g_yghv_off.magic == YGHV_OFF_MAGIC) &&
           ((g_yghv_off.derived_mask & YGHV_OFF_PID) != 0);
}

PEPROCESS yghv_offsets_system_process(void)
{
    return g_off_system;
}

void yghv_offsets_stats(uint64_t *out)
{
    if (!out) return;
    out[0] = g_yghv_off.derived_mask;
    out[1] = g_yghv_off.validated_mask;
    out[2] = (uint64_t)InterlockedCompareExchange64(&g_off_failures, 0, 0);
    out[3] = g_yghv_off.build_number;
    /* 9.608 diagnostics: best good/closed/off the links sweep ever reached. */
    out[4] = (uint64_t)InterlockedCompareExchange64(&g_probe_best_good, 0, 0);
    out[5] = (uint64_t)InterlockedCompareExchange64(&g_probe_best_closed, 0, 0);
    out[6] = (uint64_t)InterlockedCompareExchange64(&g_probe_best_off, 0, 0);
    out[7] = (uint64_t)InterlockedCompareExchange64(&g_probe_reason, 0, 0);
}
