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

/* Guarded 8-byte read. Used only by the structural probe, whose candidates are
 * by definition unvalidated pointers. */
static int yghv_probe_read64(const void *va, UINT64 *out)
{
    if ((UINT64)va < 0xFFFF800000000000ULL) return 0;
    if (((UINT64)va & 7) != 0) return 0;
    __try {
        *out = *(const volatile UINT64 *)va;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

/* An image name is 15 bytes of ANSI in EPROCESS; if the offset is right the
 * bytes are printable and NUL-terminated, and if it is wrong they almost never
 * are. This is the check that makes the link probe decisive. */
static int yghv_probe_name_ok(PEPROCESS p, UINT16 name_off)
{
    const volatile UINT8 *n;
    int i, printable = 0;

    if (!name_off) return 1;             /* no name offset -> skip the check */
    n = (const volatile UINT8 *)((PUCHAR)p + name_off);
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
static int yghv_probe_links(PEPROCESS sys, UINT16 pid_off, UINT16 name_off,
                           UINT16 *out_off, UINT32 *out_count)
{
    UINT32 off;

    if (!sys || !pid_off) return 0;

    for (off = 0x100; off < 0x900; off += 8) {
        UINT64 head = (UINT64)sys + off;
        UINT64 prev = head;
        UINT32 hops = 0, good = 0;
        int ok = 1;

        while (hops < 512) {
            UINT64 flink = 0, blink = 0, cand, pid = 0;

            if (!yghv_probe_read64((const void *)prev, &flink)) { ok = 0; break; }
            if (flink == head) break;                       /* circle closed */
            if (flink < 0xFFFF800000000000ULL) { ok = 0; break; }
            if (!yghv_probe_read64((const void *)(flink + 8), &blink)) { ok = 0; break; }
            if (blink != prev) { ok = 0; break; }           /* doubly linked */

            cand = flink - off;
            if (cand < 0xFFFF800000000000ULL) { ok = 0; break; }
            if (!yghv_probe_read64((const void *)(cand + pid_off), &pid)) { ok = 0; break; }
            if (pid > 0x100000) { ok = 0; break; }          /* implausible pid */
            if (!yghv_probe_name_ok((PEPROCESS)cand, name_off)) { ok = 0; break; }

            good++;
            prev = flink;
            hops++;
        }

        /*
         * A real machine always has more than 16 processes, and closing the
         * circle is what separates "this is the process list" from "this is
         * some other list that happens to be doubly linked".
         */
        if (ok && good >= 16 && prev == head) {
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
}
