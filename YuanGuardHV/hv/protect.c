#include <ntifs.h>
#include "protect.h"
#include "svm_vcpu.h"
#include "debug.h"
#include "control_plane.h"

extern npt_mgr_t g_npt;

NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId, PEPROCESS *Process);
NTKERNELAPI NTSTATUS MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress,
    SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
    PSIZE_T NumberOfBytesCopied);
NTKERNELAPI NTSTATUS ZwFlushBuffersFile(HANDLE FileHandle,
                                        PIO_STATUS_BLOCK IoStatusBlock);

static const uint64_t YGHV_PT_ADDR_MASK = 0x000FFFFFFFFFF000ULL;

static void yghv_hook_diag(const char *stage, NTSTATUS st) {
    static const char hex[] = "0123456789abcdef";
    static ULONG diag_lines = 0;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    char buf[96];
    size_t n = 0;
    int i;

    /* REV-048: cap the diagnostic log so it cannot grow without bound. */
    if (++diag_lines > 4096)
        return;

    RtlInitUnicodeString(&name, L"\\SystemRoot\\yghv_hook.log");
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_APPEND_DATA, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0)))
        return;
    while (stage[n] && n < sizeof(buf) - 16) {
        buf[n] = stage[n];
        n++;
    }
    buf[n++] = ' '; buf[n++] = 's'; buf[n++] = 't'; buf[n++] = '=';
    buf[n++] = '0'; buf[n++] = 'x';
    for (i = 7; i >= 0; i--)
        buf[n++] = hex[((uint32_t)st >> (i * 4)) & 0xF];
    buf[n++] = '\r'; buf[n++] = '\n';
    ZwWriteFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)n, NULL, NULL);
    ZwFlushBuffersFile(h, &iosb);
    ZwClose(h);
}

/* REV-036: yghv_hook_diag performs Zw* file I/O, which requires PASSIVE_LEVEL.
   Every diag call site runs under g_protect_lock (FAST_MUTEX -> APC_LEVEL),
   an IRQL violation on exactly the error paths the diag serves.  Record the
   failure in memory (LOG_ERROR is IRQL-safe) and flush the log after the lock
   is released. */
static const char *g_hook_diag_stage = NULL;
static NTSTATUS g_hook_diag_status = 0;

static void yghv_hook_diag_mark(const char *stage, NTSTATUS st) {
    g_hook_diag_stage = stage;
    g_hook_diag_status = st;
    LOG_ERROR("hook diag: %s st=0x%x", stage, (unsigned)st);
}

static void yghv_hook_diag_flush(void) {
    if (g_hook_diag_stage) {
        yghv_hook_diag(g_hook_diag_stage, g_hook_diag_status);
        g_hook_diag_stage = NULL;
        g_hook_diag_status = 0;
    }
}

yghv_protect_state_t g_protect;
yghv_protect_hook_t g_protect_hooks[YGHV_PROTECT_MAX_HOOKS];
uint64_t g_protect_cr3_list[YGHV_PROTECT_MAX_TARGETS];
static FAST_MUTEX g_protect_lock;

static BOOLEAN yghv_protect_is_target_cr3_locked(uint64_t cr3) {
    uint32_t i;
    /* 9.249 (run_c10 evidence): with CR4.PCIDE=1 the CR3 in the VMCB carries
     * the PCID in bits 11:0 (and possibly bit 63 no-flush), while targets[]
     * stores KPROCESS.DirectoryTableBase = PFN only. A raw == compare always
     * fails -> every protected write was DENY'd. Compare PFN bits only
     * (51:12); PCID/no-flush must not affect target matching. */
    uint64_t cr3_pfn = cr3 & YGHV_PT_ADDR_MASK;
    for (i = 0; i < g_protect.target_count; i++) {
        if (g_protect.targets[i].cr3 != 0 &&
            (g_protect.targets[i].cr3 & YGHV_PT_ADDR_MASK) == cr3_pfn)
            return TRUE;
    }
    return FALSE;
}

static yghv_protect_page_t *yghv_protect_find_page_locked(uint64_t gpa) {
    uint32_t t, i;
    uint64_t page = gpa & ~0xFFFULL;
    for (t = 0; t < g_protect.target_count; t++) {
        for (i = 0; i < g_protect.targets[t].page_count; i++) {
            if (g_protect.targets[t].pages[i].gpa == page)
                return &g_protect.targets[t].pages[i];
        }
    }
    return NULL;
}

static yghv_protect_target_t *yghv_protect_find_target_by_cr3_locked(
    uint64_t cr3) {
    uint32_t i;
    uint64_t cr3_pfn = cr3 & YGHV_PT_ADDR_MASK;   /* 9.249: PCID-immune */
    for (i = 0; i < g_protect.target_count; i++) {
        if (g_protect.targets[i].cr3 != 0 &&
            (g_protect.targets[i].cr3 & YGHV_PT_ADDR_MASK) == cr3_pfn)
            return &g_protect.targets[i];
    }
    return NULL;
}

static yghv_protect_target_t *yghv_protect_find_target_by_pid_locked(
    uint32_t pid) {
    uint32_t i;
    for (i = 0; i < g_protect.target_count; i++) {
        if (g_protect.targets[i].pid == pid)
            return &g_protect.targets[i];
    }
    return NULL;
}

static void yghv_protect_refresh_cr3_list_locked(void) {
    uint32_t i;
    for (i = 0; i < YGHV_PROTECT_MAX_TARGETS; i++) {
        g_protect_cr3_list[i] = (i < g_protect.target_count)
            ? g_protect.targets[i].cr3 : 0;
    }
}

static NTSTATUS yghv_protect_add_page_for_locked(yghv_protect_target_t *t,
    uint64_t target_va);
static NTSTATUS yghv_protect_remove_page_for_locked(yghv_protect_target_t *t,
    uint64_t target_va);
static int yghv_protect_arm_page_locked(yghv_protect_page_t *p);
static int yghv_protect_disarm_page_locked(yghv_protect_page_t *p);
static NTSTATUS yghv_protect_start_locked(void);
static NTSTATUS yghv_protect_stop_locked(void);
static NTSTATUS yghv_protect_install_hook_locked(uint8_t hook_id, uint64_t func_va);
static NTSTATUS yghv_protect_remove_hook_locked(uint8_t hook_id);

/* 9.280 (C6 v2) fake machinery: declarations live here because the arm/
 * disarm paths mirror NPT ops into the alt NPT. Definitions further down. */
static npt_mgr_t g_npt_alt;              /* identity clone + scratch mappings */
static int g_npt_alt_ready;
static volatile LONG g_fake_mode;
static volatile LONG g_fake_attempts;
static volatile LONG g_fake_ok;
static volatile LONG g_fake_restores;
static volatile LONG g_fake_last_reject; /* 1=mode 2=alt 0=ok */
static volatile LONG g_fake_alt_active;  /* a core is running on the alt NPT */
static PVOID yghv_protect_fake_scratch_for(uint64_t gpa);
void SvInvlpgaByVa(UINT64 gva, UINT32 asid);   /* 9.289: asm, 206-only */

/* 9.299 (C line): the sync guard lives further down but arm_page_locked
 * (above it) needs to register pages with it. */
int  yghv_protect_sync_arm(uint64_t gpa);
void yghv_protect_sync_disarm(uint64_t gpa);
int  yghv_protect_sync_resnapshot(uint64_t gpa);   /* Phase 7 */
int  yghv_protect_sync_is_guarded(uint64_t gpa);   /* Phase 7 */
static int yghv_protect_sync_arm_locked(uint64_t gpa);   /* caller holds lock */

/* 9.297 (A'): runtime TLB-flush strategy for the fake-write path.
 * 9.290 concluded "Zen3 NPT translations do not follow TlbControl=1 / nCr3 /
 * INVLPGA" — but that evidence came from experiments that (a) used
 * TlbControl=1, which by APM flushes only the *guest* TLB (GVA->GPA), not the
 * NPT (GPA->HPA); (b) used INVLPGA, which also only touches GVA->GPA.
 * Neither addresses the nested-page-table layer that actually caches the
 * armed page's translation. TlbControl=2 (FLUSH entire TLB) was never tried
 * on this path. This selector lets one build sweep the strategies at runtime:
 *   0 = legacy   : TlbControl=1 + INVLPGA + nCR3=alt   (current behaviour)
 *   1 = flushall : TlbControl=2 + INVLPGA + nCR3=alt
 *   2 = noinvl   : TlbControl=2 + nCR3=alt  (no INVLPGA)
 *   3 = invlonly : TlbControl=1 + INVLPGA + nCR3=alt, no VmcbClean=0
 * Readable/writable via IOCTL_YGHV_SET_FAKE_TLB. */
static volatile LONG g_fake_tlb_mode;
static volatile LONG g_fake_tlb_seen;   /* times the selector was consulted */

int yghv_protect_fake_tlb_get(void) {
    return (int)InterlockedCompareExchange(&g_fake_tlb_mode, 0, 0);
}

void yghv_protect_fake_tlb_set(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 4) mode = 4;
    InterlockedExchange(&g_fake_tlb_mode, mode);
    InterlockedExchange(&g_fake_tlb_seen, 0);
}

LONG yghv_protect_fake_tlb_seen(void) {
    return InterlockedCompareExchange(&g_fake_tlb_seen, 0, 0);
}

/* Called from the island NPF path (cpp) — pure memory, no locks. Returns the
 * TlbControl value to write and whether INVLPGA should run. */
int yghv_protect_fake_tlb_plan(int *use_invlpga) {
    int m = (int)InterlockedCompareExchange(&g_fake_tlb_mode, 0, 0);
    InterlockedIncrement(&g_fake_tlb_seen);
    switch (m) {
    case 1: if (use_invlpga) *use_invlpga = 1; return 2;   /* FLUSH_ALL */
    case 2: if (use_invlpga) *use_invlpga = 0; return 2;   /* FLUSH_ALL, no INVLPGA */
    case 3: if (use_invlpga) *use_invlpga = 1; return 1;   /* legacy + INVLPGA */
    case 4: if (use_invlpga) *use_invlpga = 0; return 2;   /* 9.297: reopen main + FLUSH_ALL */
    default: if (use_invlpga) *use_invlpga = 1; return 1;  /* legacy */
    }
}

/* 9.297 (A'): mode 4 asks whether TlbControl=2 (FLUSH ALL) can make a *main*
 * NPT permission flip visible. Instead of switching nCR3 to the alt table
 * (which relies on the nCr3-change flush), this reopens the armed page in the
 * MAIN npt and relies purely on FLUSH_ALL. If the store then lands (spin
 * stops) the NPT TLB does honour FLUSH_ALL and the whole alt-NPT design is
 * unnecessary; if it still spins, the NPT TLB ignores FLUSH_ALL too and the
 * limitation is architectural. */
int yghv_protect_fake_reopen_main(uint64_t gpa) {
    int st;
    if (!g_protect.active)
        return 0;
    st = npt_set_page_perm(&g_npt, gpa & ~(uint64_t)0xFFFULL,
                           NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    return st == 0;
}

/* ==================================================================== */
/* 9.299 (C line): SYNC GUARD — snapshot + poll, no NPT permission flip. */
/*                                                                       */
/* Why: 9.297/9.298 established that Zen3's NPT (GPA->HPA) TLB does not  */
/* follow TlbControl=1, TlbControl=2, nCR3 changes or INVLPGA, and that   */
/* INVLPGB does not exist on Zen3. Any design that flips an NPT           */
/* permission and expects the change to be visible immediately is dead.   */
/*                                                                       */
/* The sync guard does not flip anything. The armed page stays            */
/* present+writable in the NPT for its entire armed life, so the hardware */
/* never needs to invalidate a translation and the limitation cannot      */
/* apply. A background thread polls: it snapshots the page's real content */
/* and, when the content changes, restores the snapshot. Detection        */
/* latency is the poll interval; the page is transiently modified, which  */
/* is the cost of not being able to block the store.                      */
/* ==================================================================== */

/* 9.302: raised from 16 to match YGHV_PROTECT_MAX_PAGES. With 16, arming more
 * than 16 pages silently left the rest armed-but-unguarded: their writes would
 * land permanently with no rollback and no error. Memory cost of the raise is
 * 64 slots x 32B = 2KB of statics plus at most 64 x 4KB = 256KB of snapshots,
 * allocated lazily only for pages actually armed. */
#define YGHV_SYNC_MAX 64          /* pages the guard tracks at once */
#define YGHV_SYNC_DEFAULT_MS 50

typedef struct {
    uint64_t gpa;
    uint8_t *snapshot;            /* HV_PAGE_SIZE nonpaged copy */
    volatile LONG active;
    volatile LONG64 restores;     /* times this page was rolled back */
} yghv_sync_slot_t;

static yghv_sync_slot_t g_sync[YGHV_SYNC_MAX];
static volatile LONG g_sync_mode;             /* 0 = off, 1 = on */
static volatile LONG g_sync_polls;
static volatile LONG g_sync_hits;
static volatile LONG g_sync_refused;   /* 9.302: arm refused (slot table full) */
static volatile LONG64 g_sync_resnaps; /* Phase 7: cooperative re-snapshots */
static volatile LONG64 g_sync_cpu_100ns; /* 9.304: cumulative poll CPU time */
static volatile LONG g_sync_ms = YGHV_SYNC_DEFAULT_MS;
static HANDLE g_sync_thread;
static KEVENT g_sync_stop;
static volatile LONG g_sync_stop_flag;
static volatile LONG g_sync_running;
static PVOID g_sync_scratch;          /* one 4KB read buffer, allocated lazily */

int yghv_protect_sync_mode_get(void) {
    return (int)InterlockedCompareExchange(&g_sync_mode, 0, 0);
}

void yghv_protect_sync_mode_set(int on) {
    uint64_t gpas[YGHV_SYNC_MAX];
    uint32_t n = 0, t;

    InterlockedExchange(&g_sync_mode, on ? 1 : 0);
    if (!on) {
        /* 9.301: turning the guard OFF must also drop every slot, not just
         * flip the flag. Otherwise the slots stay "active" (inflating the
         * reported page count), and a later re-enable would adopt a STALE
         * snapshot — one taken before the page was last written — as the
         * baseline, so the guard would immediately "restore" content the
         * target had legitimately changed in the meantime. */
        for (t = 0; t < YGHV_SYNC_MAX; t++)
            InterlockedExchange(&g_sync[t].active, 0);
        return;
    }

    /* Snapshot every page already armed, so the guard has a baseline the
     * moment it is switched on. Collect under the lock, then arm OUTSIDE it —
     * sync_arm takes the same FastMutex and re-acquiring it in one thread
     * would deadlock (that was the 0xE2 crash). */
    ExAcquireFastMutex(&g_protect_lock);
    for (t = 0; t < g_protect.target_count && n < YGHV_SYNC_MAX; t++) {
        uint32_t i;
        for (i = 0; i < g_protect.targets[t].page_count && n < YGHV_SYNC_MAX; i++) {
            if (g_protect.targets[t].pages[i].armed)
                gpas[n++] = g_protect.targets[t].pages[i].gpa;
        }
    }
    ExReleaseFastMutex(&g_protect_lock);

    for (t = 0; t < n; t++)
        (void)yghv_protect_sync_arm(gpas[t]);
}

/* 9.302: out[6] = arm attempts the guard had to REFUSE (slot table full).
 * Previously that failure was silent — a page could be armed for protection
 * yet carry no guard slot, so writes to it would land permanently. Surfacing
 * the count makes the condition observable instead of a silent hole. */
/* 9.304: out[7] = total CPU time spent inside the poll body, in 100ns units.
 *
 * Measuring the guard from user mode is hopeless here: Get-Process sums a
 * whole 12-core box's kernel activity (network, storage, DPCs), which dwarfs
 * the guard by two orders of magnitude — a 22-page run measured the same as a
 * 0-page run, and sometimes lower. Timing the poll from inside the driver
 * isolates the signal completely. Callers divide by polls to get per-poll cost
 * and by pages to get per-page cost. */
void yghv_protect_sync_diag(UINT64 out[8]) {
    out[0] = (UINT64)InterlockedCompareExchange(&g_sync_mode, 0, 0);
    out[1] = (UINT64)InterlockedCompareExchange(&g_sync_polls, 0, 0);
    out[2] = (UINT64)InterlockedCompareExchange(&g_sync_hits, 0, 0);
    out[3] = (UINT64)InterlockedCompareExchange(&g_sync_ms, 0, 0);
    out[4] = (UINT64)InterlockedCompareExchange(&g_sync_running, 0, 0);
    out[5] = (UINT64)InterlockedCompareExchange(&g_sync_refused, 0, 0);
    out[7] = (UINT64)InterlockedCompareExchange64(&g_sync_cpu_100ns, 0, 0);
    {
        int i, n = 0;
        for (i = 0; i < YGHV_SYNC_MAX; i++)
            if (InterlockedCompareExchange(&g_sync[i].active, 0, 0))
                n++;
        out[6] = (UINT64)n;
    }
}

void yghv_protect_sync_set_interval(int ms) {
    if (ms < 1) ms = 1;
    if (ms > 5000) ms = 5000;
    InterlockedExchange(&g_sync_ms, ms);
}

/* 9.303: read the live poll interval, so get_config can report the truth. */
int yghv_protect_sync_interval_get(void) {
    return (int)InterlockedCompareExchange(&g_sync_ms, 0, 0);
}

/* 9.299 FIX-2: MmGetVirtualForPhysical only maps physical pages the SYSTEM
 * already has a direct mapping for (its own page tables, pool, ...). A target
 * process's user page has no such mapping, so dereferencing the VA it returns
 * is an access violation — the 0x3B/c0000005 crash at yghv_c_line+0x12bd2 was
 * memcpy faulting on exactly that (and it only appeared once the FIX-1
 * deadlock was removed, because before that the poll never ran).
 * MmCopyMemory takes a physical address directly and needs no mapping. */
static int yghv_sync_read_page(uint64_t gpa, void *dst, SIZE_T len) {
    MM_COPY_ADDRESS src;
    SIZE_T done = 0;
    src.PhysicalAddress.QuadPart = (LONGLONG)(gpa & ~(uint64_t)0xFFFULL);
    return NT_SUCCESS(MmCopyMemory(dst, src, len,
                                   MM_COPY_MEMORY_PHYSICAL, &done)) &&
           done == len;
}

/* 9.299 FIX-4 (simplified): the slot stores NO process pointer.
 *
 * Holding an EPROCESS reference in the slot meant the guard, the watchdog and
 * cleanup could all free the same pointer — a use-after-free class I am not
 * going to hand-roll. Instead the poll looks the owner up fresh, under
 * g_protect_lock (the lock that already protects target->process lifetime),
 * takes its own reference, drops the lock, and only then attaches.
 *
 * Taking g_protect_lock HERE is safe: the guard thread is independent and
 * holds nothing. (The 0xE2 deadlock was a caller that ALREADY held the lock
 * re-acquiring it; that path uses the _locked variant.)
 *
 * Reading needs no process at all — MmCopyMemory takes a physical address.
 * Only the write-back needs a context, and KeStackAttachProcess requires
 * PASSIVE_LEVEL, so the attach must happen after the mutex is released.
 */
/* Write one page back into the owning process.
 *
 * 9.305 REWRITE. The previous version attached to the process and did a raw
 * RtlCopyMemory to the target VA. It crashed (0x7E, yghv_a6+0x12e13, memmove
 * with a 4KB length — the disassembly shows the attach/copy/detach sequence
 * and a movaps faulting on the destination). Three independent faults made
 * that approach unsafe:
 *
 *   1. A physical address is NOT unique across processes. Matching a slot's
 *      gpa against g_protect.targets to recover a target_va can return a VA
 *      belonging to a DIFFERENT process than the one we then attach to.
 *   2. The build uses /EHs-c-, so __try/__except generates NO handler at all
 *      (there is no __C_specific_handler in the binary). The guard was
 *      decorative: a fault inside it became a bugcheck instead of being caught.
 *   3. Attaching by hand means owning the EPROCESS lifetime, and the watchdog
 *      can free it concurrently.
 *
 * MmCopyVirtualMemory copies by (process, VA) and returns a status — no attach,
 * no alignment contract, no exception handling, no reliance on gpa uniqueness.
 * The project already uses it this way (yghv_protect_resolve_va_for). The
 * (process, VA) pair is read under g_protect_lock and the process referenced
 * before the lock is dropped, so the pointer cannot go stale. */
static int yghv_sync_write_page(uint64_t gpa, const void *src, SIZE_T len) {
    PEPROCESS owner = NULL;
    uint64_t target_va = 0;
    uint32_t t, p;
    SIZE_T copied = 0;
    NTSTATUS st;

    ExAcquireFastMutex(&g_protect_lock);
    for (t = 0; t < g_protect.target_count; t++) {
        yghv_protect_target_t *tg = &g_protect.targets[t];
        if (!tg->process || !tg->cr3)
            continue;
        for (p = 0; p < tg->page_count; p++) {
            if ((tg->pages[p].gpa & ~(uint64_t)0xFFFULL) ==
                (gpa & ~(uint64_t)0xFFFULL)) {
                owner = tg->process;
                target_va = tg->pages[p].target_va;
                break;
            }
        }
        if (owner)
            break;
    }
    /* Take the (process, VA) pair as ONE consistent unit, inside the lock that
     * already protects target->process. */
    if (owner && target_va) {
        ObReferenceObject(owner);
    } else {
        owner = NULL;
    }
    ExReleaseFastMutex(&g_protect_lock);

    if (!owner)
        return 0;

    st = MmCopyVirtualMemory(IoGetCurrentProcess(), (PVOID)src,
                             owner, (PVOID)(ULONG_PTR)target_va,
                             len, KernelMode, &copied);

    ObDereferenceObject(owner);

    return NT_SUCCESS(st) && copied == len;
}

/* Take (or retake) a snapshot of the real page behind a gpa. */
static void yghv_sync_snapshot_locked(yghv_sync_slot_t *s) {
    if (!s->snapshot)
        return;
    (void)yghv_sync_read_page(s->gpa, s->snapshot, HV_PAGE_SIZE);
}

/* 9.299 FIX: ExAcquireFastMutex is NOT recursive. arm_page_locked() already
 * holds g_protect_lock when it registers a page with the guard, so sync_arm
 * must have a variant that assumes the caller holds it. The public sync_arm
 * takes the lock and delegates; the arm path calls the _locked form directly.
 * (The original version acquired the mutex in both, which self-deadlocked on
 * the first protect-page-after-sync-enable and took the box down with a guest
 * triple fault — see the 0xE2/0x20601 dump.) */
static int yghv_protect_sync_arm_locked(uint64_t gpa) {
    int i, free_slot = -1;
    gpa &= ~(uint64_t)0xFFFULL;
    for (i = 0; i < YGHV_SYNC_MAX; i++) {
        if (InterlockedCompareExchange(&g_sync[i].active, 0, 0) &&
            g_sync[i].gpa == gpa) {
            yghv_sync_snapshot_locked(&g_sync[i]);   /* refresh */
            return 1;
        }
        if (free_slot < 0 && !InterlockedCompareExchange(&g_sync[i].active, 0, 0))
            free_slot = i;
    }
    if (free_slot < 0) {
        InterlockedIncrement(&g_sync_refused);   /* 9.302: make it observable */
        return 0;                                /* table full */
    }
    if (!g_sync[free_slot].snapshot) {
        g_sync[free_slot].snapshot =
            ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE, YGHV_TAG);
        if (!g_sync[free_slot].snapshot)
            return 0;
    }
    g_sync[free_slot].gpa = gpa;
    g_sync[free_slot].restores = 0;
    yghv_sync_snapshot_locked(&g_sync[free_slot]);
    InterlockedExchange(&g_sync[free_slot].active, 1);
    return 1;
}

int yghv_protect_sync_arm(uint64_t gpa) {
    int r;
    ExAcquireFastMutex(&g_protect_lock);
    r = yghv_protect_sync_arm_locked(gpa);
    ExReleaseFastMutex(&g_protect_lock);
    return r;
}

void yghv_protect_sync_disarm(uint64_t gpa) {
    int i;
    gpa &= ~(uint64_t)0xFFFULL;
    for (i = 0; i < YGHV_SYNC_MAX; i++) {
        if (InterlockedCompareExchange(&g_sync[i].active, 0, 0) &&
            g_sync[i].gpa == gpa) {
            InterlockedExchange(&g_sync[i].active, 0);
            
        }
    }
}

/*
 * Phase 7 (2026-10-11): cooperative write support.
 *
 * The guard restores a page whenever its content drifts from the snapshot.
 * A write from Engine B lands on the PHYSICAL FRAME, so the poll sees drift
 * and undoes it — meaning an ordinary write to a guarded page does not
 * stick. That is the intended behaviour against a host-side writer, but it
 * also blocks US.
 *
 * This entry point lets our own writer say "this change is deliberate": the
 * snapshot is retaken (making the new content the baseline) so the poll has
 * nothing to restore. Protection stays armed.
 *
 * Returns 1 if the gpa is guarded and the snapshot was refreshed, 0 if the
 * page is not guarded (the caller should just write normally).
 *
 * Call it BEFORE the write: retaking afterwards would read the page twice
 * and could race the poll.
 */
int yghv_protect_sync_resnapshot(uint64_t gpa) {
    int i;
    int hit = 0;
    gpa &= ~(uint64_t)0xFFFULL;

    /* FastMutex is NOT recursive (9.299), and this runs from the IOCTL path
     * where nothing else is held — the same discipline public sync_arm uses. */
    ExAcquireFastMutex(&g_protect_lock);
    for (i = 0; i < YGHV_SYNC_MAX; i++) {
        if (InterlockedCompareExchange(&g_sync[i].active, 0, 0) &&
            g_sync[i].gpa == gpa) {
            yghv_sync_snapshot_locked(&g_sync[i]);
            InterlockedIncrement64(&g_sync_resnaps);
            hit = 1;
            break;
        }
    }
    ExReleaseFastMutex(&g_protect_lock);
    return hit;
}

/*
 * Query only: is this gpa currently guarded?
 *
 * Deliberately does NOT take g_protect_lock — it is called from the write
 * path, and taking the mutex there would serialize every write behind the
 * guard. The slot table is a plain array of volatile fields, so a racy read
 * is safe: a false positive only means we re-snapshot a page that was about
 * to be disarmed anyway.
 */
int yghv_protect_sync_is_guarded(uint64_t gpa) {
    int i;
    gpa &= ~(uint64_t)0xFFFULL;
    for (i = 0; i < YGHV_SYNC_MAX; i++) {
        if (InterlockedCompareExchange(&g_sync[i].active, 0, 0) &&
            g_sync[i].gpa == gpa)
            return 1;
    }
    return 0;
}

/* Poll body: for each tracked page, read it through MmCopyMemory, compare
 * against the snapshot, and write the snapshot back on any difference.
 * Called from the guard thread at PASSIVE. */
static void yghv_sync_poll_once(void) {
    int i;
    UCHAR *cur = NULL;
    ULONGLONG t0, t1;          /* 9.304: TSC around the poll body */
    InterlockedIncrement(&g_sync_polls);

    /* One scratch buffer for all pages — no per-poll allocation. */
    if (!g_sync_scratch) {
        g_sync_scratch = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE, YGHV_TAG);
        if (!g_sync_scratch)
            return;
    }
    cur = (UCHAR *)g_sync_scratch;

    /* Wall time around the body, via __rdtsc — the project's existing timing
     * primitive (KeQueryPerformanceCounter would pull in hal.lib). On this
     * invariant-TSC CPU the delta is a faithful cycle count; callers convert
     * to time with the measured frequency. At 20 polls/s against a 50ms
     * interval the body is a small fraction of the period, so this is a close
     * proxy for the CPU the poll consumes. */
    t0 = __rdtsc();

    for (i = 0; i < YGHV_SYNC_MAX; i++) {
        if (!InterlockedCompareExchange(&g_sync[i].active, 0, 0))
            continue;
        if (!g_sync[i].snapshot)
            continue;
        if (!yghv_sync_read_page(g_sync[i].gpa, cur, HV_PAGE_SIZE))
            continue;                       /* unreadable this round; try later */
        if (RtlCompareMemory(cur, g_sync[i].snapshot, HV_PAGE_SIZE) != HV_PAGE_SIZE) {
            if (yghv_sync_write_page(g_sync[i].gpa, g_sync[i].snapshot, HV_PAGE_SIZE)) {
                InterlockedIncrement64(&g_sync[i].restores);
                InterlockedIncrement(&g_sync_hits);
            }
        }
    }

    /* Accumulate raw TSC cycles; the reader converts using the CPU's TSC
     * frequency, which it can obtain without another kernel round trip. */
    t1 = __rdtsc();
    InterlockedExchangeAdd64(&g_sync_cpu_100ns, (LONG64)(t1 - t0));
}

static VOID yghv_sync_guard_thread(PVOID ctx) {
    LARGE_INTEGER delay;
    UNREFERENCED_PARAMETER(ctx);
    InterlockedExchange(&g_sync_running, 1);
    LOG_ERROR("sync guard: thread up (interval=%d ms)",
              (int)InterlockedCompareExchange(&g_sync_ms, 0, 0));
    while (!InterlockedCompareExchange(&g_sync_stop_flag, 0, 0)) {
        LARGE_INTEGER t;
        int ms = (int)InterlockedCompareExchange(&g_sync_ms, 0, 0);
        if (InterlockedCompareExchange(&g_sync_mode, 0, 0))
            yghv_sync_poll_once();
        t.QuadPart = -(LONGLONG)ms * 10000LL;   /* ms -> 100ns units */
        KeWaitForSingleObject(&g_sync_stop, Executive, KernelMode, FALSE, &t);
    }
    InterlockedExchange(&g_sync_running, 0);
    LOG_ERROR("sync guard: thread exit (polls=%d hits=%d)",
              (int)InterlockedCompareExchange(&g_sync_polls, 0, 0),
              (int)InterlockedCompareExchange(&g_sync_hits, 0, 0));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void yghv_protect_sync_init(void) {
    NTSTATUS st;
    KeInitializeEvent(&g_sync_stop, NotificationEvent, FALSE);
    InterlockedExchange(&g_sync_stop_flag, 0);
    st = PsCreateSystemThread(&g_sync_thread, THREAD_ALL_ACCESS, NULL, NULL,
                              NULL, yghv_sync_guard_thread, NULL);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("sync guard: thread create failed 0x%x", st);
        g_sync_thread = NULL;
        return;
    }
    LOG_ERROR("sync guard: created");
}

void yghv_protect_sync_stop(void) {
    int i;
    /* 9.299 FIX-3: the cleanup below must run even if the thread was never
     * created (sync_init can fail) — otherwise any slot that took an EPROCESS
     * reference leaks it, and the snapshot pool leaks with it. So only the
     * thread join is conditional. */
    if (g_sync_thread) {
        InterlockedExchange(&g_sync_stop_flag, 1);
        KeSetEvent(&g_sync_stop, IO_NO_INCREMENT, FALSE);
        {
            PVOID obj = NULL;
            if (NT_SUCCESS(ObReferenceObjectByHandle(g_sync_thread, THREAD_ALL_ACCESS,
                                                     *PsThreadType, KernelMode,
                                                     &obj, NULL))) {
                KeWaitForSingleObject(obj, Executive, KernelMode, FALSE, NULL);
                ObDereferenceObject(obj);
            }
            ZwClose(g_sync_thread);
        }
        g_sync_thread = NULL;
    }
    InterlockedExchange(&g_sync_mode, 0);
    if (g_sync_scratch) {
        ExFreePoolWithTag(g_sync_scratch, YGHV_TAG);
        g_sync_scratch = NULL;
    }
    for (i = 0; i < YGHV_SYNC_MAX; i++) {
        InterlockedExchange(&g_sync[i].active, 0);
        if (g_sync[i].snapshot) {
            ExFreePoolWithTag(g_sync[i].snapshot, YGHV_TAG);
            g_sync[i].snapshot = NULL;
        }
    }
    LOG_ERROR("sync guard: stopped");
}

static uint64_t yghv_pt_read(uint64_t table_pa, uint64_t index) {
    uint64_t *va;
    if (!table_pa) return 0;
    va = MmGetVirtualForPhysical((PHYSICAL_ADDRESS){ .QuadPart = table_pa });
    if (!va) return 0;
    return va[index];
}

uint64_t yghv_protect_guest_va_to_pa(uint64_t cr3, uint64_t va) {
    uint64_t pml4e, pdpte, pde, pte;
    pml4e = yghv_pt_read(cr3 & YGHV_PT_ADDR_MASK, (va >> 39) & 0x1FF);
    if (!(pml4e & 1)) return 0;
    pdpte = yghv_pt_read(pml4e & YGHV_PT_ADDR_MASK, (va >> 30) & 0x1FF);
    if (!(pdpte & 1)) return 0;
    if (pdpte & (1ULL << 7))  /* 1GB page */
        return (pdpte & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);
    pde = yghv_pt_read(pdpte & YGHV_PT_ADDR_MASK, (va >> 21) & 0x1FF);
    if (!(pde & 1)) return 0;
    if (pde & (1ULL << 7))  /* 2MB page */
        return (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);
    pte = yghv_pt_read(pde & YGHV_PT_ADDR_MASK, (va >> 12) & 0x1FF);
    if (!(pte & 1)) return 0;
    return (pte & YGHV_PT_ADDR_MASK) | (va & 0xFFFULL);
}

static uint64_t yghv_protect_resolve_va_for(yghv_protect_target_t *t,
    uint64_t target_va) {
    KAPC_STATE apc;
    uint64_t pa;

    if (!t || !t->process)
        return 0;

    KeStackAttachProcess(t->process, &apc);
    pa = MmGetPhysicalAddress((PVOID)target_va).QuadPart;
    KeUnstackDetachProcess(&apc);
    if (pa)
        return pa;

    /* Demand-paged pages have no present PTE; fault one byte in. */
    {
        SIZE_T copied = 0;
        UCHAR tmp;
        NTSTATUS st = MmCopyVirtualMemory(t->process, (PVOID)target_va,
                                          IoGetCurrentProcess(), &tmp, 1,
                                          KernelMode, &copied);
        if (!NT_SUCCESS(st) || copied != 1)
            return 0;
    }

    KeStackAttachProcess(t->process, &apc);
    pa = MmGetPhysicalAddress((PVOID)target_va).QuadPart;
    KeUnstackDetachProcess(&apc);
    return pa;
}

static uint64_t yghv_protect_resolve_va(uint64_t target_va) {
    return yghv_protect_resolve_va_for(&g_protect.targets[0], target_va);
}

NTSTATUS yghv_protect_init(void) {
    yghv_protect_fake_init();
    ExInitializeFastMutex(&g_protect_lock);
    RtlZeroMemory(&g_protect, sizeof(g_protect));
    RtlZeroMemory(g_protect_hooks, sizeof(g_protect_hooks));
    g_protect.target_count = 1;
    g_protect.config.auto_disarm = 1;
    g_protect.config.deny_status = 0xC0000022;
    /* 9.303: the guard starts OFF with the documented default period. */
    g_protect.config.sync_enable = 0;
    g_protect.config.sync_interval_ms = YGHV_SYNC_DEFAULT_MS;
    yghv_protect_refresh_cr3_list_locked();
    /* 9.299 (C line): start the polling guard thread. It idles until
     * IOCTL_YGHV_SET_SYNC switches the master mode on, so an unused guard
     * costs one sleeping thread and no polling. */
    yghv_protect_sync_init();
    /* 9.304: restore persisted settings. Must come AFTER sync_init (the guard
     * thread exists to receive the mode) and is safe here: both the interval
     * setter and sync_mode_set are callable from this context. */
    yghv_protect_config_load();
    return STATUS_SUCCESS;
}

void yghv_protect_cleanup(void) {
    uint32_t i, t;
    /* 9.299 (C line): join the guard thread FIRST — it dereferences page
     * snapshots and reads g_protect targets, so it must be gone before the
     * structures below are torn down. */
    yghv_protect_sync_stop();
    ExAcquireFastMutex(&g_protect_lock);
    for (i = 0; i < YGHV_PROTECT_MAX_HOOKS; i++) {
        if (g_protect_hooks[i].installed)
            yghv_protect_remove_hook_locked(i);
    }
    yghv_protect_stop_locked();
    for (t = 0; t < g_protect.target_count; t++) {
        if (g_protect.targets[t].process) {
            ObDereferenceObject(g_protect.targets[t].process);
            g_protect.targets[t].process = NULL;
        }
    }
    RtlZeroMemory(&g_protect, sizeof(g_protect));
    RtlZeroMemory(g_protect_hooks, sizeof(g_protect_hooks));
    RtlZeroMemory(g_protect_cr3_list, sizeof(g_protect_cr3_list));
    ExReleaseFastMutex(&g_protect_lock);
    yghv_hook_diag_flush();
}

NTSTATUS yghv_protect_set_target(uint32_t pid) {
    PEPROCESS proc = NULL;
    uint64_t cr3;
    NTSTATUS st;
    uint32_t idx;
    yghv_protect_target_t *t;
    uint32_t i;

    if (pid == 0)
        return STATUS_INVALID_PARAMETER;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect set_target: lookup pid %u failed 0x%x", pid, st);
        return st;
    }
    if (PsGetProcessId(proc) != (HANDLE)(ULONG_PTR)pid) {
        ObDereferenceObject(proc);
        LOG_ERROR("protect set_target: pid mismatch %u", pid);
        return STATUS_INVALID_PARAMETER;
    }

    /* KPROCESS.DirectoryTableBase on 19045 is at offset 0x028. */
    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
    if (!cr3) {
        ObDereferenceObject(proc);
        LOG_ERROR("protect set_target: pid %u has no CR3", pid);
        return STATUS_INVALID_PARAMETER;
    }
    ExAcquireFastMutex(&g_protect_lock);
    t = yghv_protect_find_target_by_pid_locked(pid);
    if (t) {
        idx = (uint32_t)(t - g_protect.targets);
    } else if (g_protect.targets[0].pid == 0) {
        idx = 0;
    } else if (g_protect.target_count < YGHV_PROTECT_MAX_TARGETS) {
        idx = g_protect.target_count++;
        RtlZeroMemory(&g_protect.targets[idx], sizeof(g_protect.targets[idx]));
    } else {
        ExReleaseFastMutex(&g_protect_lock);
        ObDereferenceObject(proc);
        LOG_ERROR("protect set_target: target table full");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    t = &g_protect.targets[idx];
    if (t->page_count) {
        BOOLEAN was_armed[YGHV_PROTECT_MAX_PAGES];
        ULONG j;
        for (j = 0; j < t->page_count; j++)
            was_armed[j] = t->pages[j].armed ? TRUE : FALSE;
        for (i = 0; i < t->page_count; i++) {
            if (t->pages[i].armed) {
                int ds = yghv_protect_disarm_page_locked(&t->pages[i]);
                if (ds) {
                    LOG_ERROR("protect set_target: disarm page %u failed 0x%x",
                        i, ds);
                    /* REV-044: roll back the pages already disarmed so the
                       slot keeps its original armed state on this failure
                       path instead of a mixed armed/disarmed state. */
                    for (j = 0; j < t->page_count; j++) {
                        if (was_armed[j] && !t->pages[j].armed)
                            yghv_protect_arm_page_locked(&t->pages[j]);
                    }
                    ExReleaseFastMutex(&g_protect_lock);
                    ObDereferenceObject(proc);
                    return STATUS_UNSUCCESSFUL;
                }
            }
        }
        t->page_count = 0;
    }
    if (t->process)
        ObDereferenceObject(t->process);
    t->pid = pid;
    t->process = proc;
    t->cr3 = cr3;
    yghv_protect_refresh_cr3_list_locked();
    ExReleaseFastMutex(&g_protect_lock);
    LOG_ERROR("protect target: pid=%u process=0x%llx cr3=0x%llx", pid,
        (uint64_t)proc, cr3);
    return STATUS_SUCCESS;
}

BOOLEAN yghv_protect_is_target_cr3(uint64_t cr3) {
    BOOLEAN result;
    ExAcquireFastMutex(&g_protect_lock);
    result = yghv_protect_is_target_cr3_locked(cr3);
    ExReleaseFastMutex(&g_protect_lock);
    return result;
}

NTSTATUS yghv_protect_add_page(uint64_t target_va) {
    NTSTATUS st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_add_page_for_locked(&g_protect.targets[0], target_va);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

NTSTATUS yghv_protect_add_page_for(uint64_t cr3, uint64_t target_va) {
    NTSTATUS st;
    yghv_protect_target_t *t;
    ExAcquireFastMutex(&g_protect_lock);
    t = yghv_protect_find_target_by_cr3_locked(cr3);
    if (!t) {
        ExReleaseFastMutex(&g_protect_lock);
        return STATUS_ACCESS_DENIED;
    }
    st = yghv_protect_add_page_for_locked(t, target_va);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

#if defined(YGHV_BAREMETAL_STEP) && (YGHV_BAREMETAL_STEP == 206)
/* 9.253: island-safe armed-page ring. The 9.252 progress.log traces ran under
 * ExAcquireFastMutex (APC_LEVEL) while all cores were virtualized; the run_c10
 * wedge was exactly such a synchronous ZwWriteFile that never completed (the
 * selftest thread sat 40min between the two adjacent trace calls). Runtime
 * file I/O from this driver is banned — record to memory here, flush in
 * DriverUnload after Sv206CoopUnload brings the cores back to bare metal. */
static uint64_t g_s206_addlog[16];
static uint32_t g_s206_addidx;
static void yghv_s206_addlog_record(uint64_t va, uint64_t gpa) {
    if (g_s206_addidx + 2 <= 16) {
        g_s206_addlog[g_s206_addidx++] = va;
        g_s206_addlog[g_s206_addidx++] = gpa;
    }
}
void yghv_s206_flush_addlog(void) {
    uint32_t i;
    for (i = 0; i + 1 < g_s206_addidx; i += 2) {
        yghv_trace_u64("s206 addlog va", g_s206_addlog[i]);
        yghv_trace_u64("s206 addlog gpa", g_s206_addlog[i + 1]);
    }
}
#endif

static NTSTATUS yghv_protect_add_page_for_locked(yghv_protect_target_t *t,
    uint64_t target_va) {
    uint64_t gpa;
    yghv_protect_page_t *p;
    uint32_t i;
    if (!t || t->page_count >= YGHV_PROTECT_MAX_PAGES)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!t->cr3)
        return STATUS_INVALID_PARAMETER;
    /* Duplicate (target, va) entries would alias the same gpa in two slots
       and corrupt the armed/disarm accounting (find_page only ever sees the
       first). Idempotent no-op keeps a repeated protect-page call safe. */
    for (i = 0; i < t->page_count; i++) {
        if (t->pages[i].target_va == target_va)
            return STATUS_SUCCESS;
    }
    gpa = yghv_protect_resolve_va_for(t, target_va);
    gpa &= ~(uint64_t)0xFFFULL;
    if (!gpa) {
        LOG_ERROR("protect add_page: va 0x%llx not mapped", target_va);
        return STATUS_INVALID_ADDRESS;
    }
    /* 9.258 (206-C3): fill the entry BEFORE publishing page_count — the
     * island's lock-free readers scan by page_count, so incrementing first
     * exposed a garbage entry window (batch-change quiesce, store-order
     * release on x86). */
    p = &t->pages[t->page_count];
    p->gpa = gpa;
    p->target_va = target_va;
    p->flags = YGHV_PROTECT_MEM;
    p->armed = 0;
    t->page_count++;
#if defined(YGHV_BAREMETAL_STEP) && (YGHV_BAREMETAL_STEP == 206)
    /* 9.253: island ring only (was 9.252 progress.log traces — file I/O under
       FastMutex during live virtualization wedged run_c10 for 40min). */
    yghv_s206_addlog_record(target_va, gpa);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS yghv_protect_remove_page(uint64_t target_va) {
    NTSTATUS st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_remove_page_for_locked(&g_protect.targets[0], target_va);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

NTSTATUS yghv_protect_remove_page_for(uint64_t cr3, uint64_t target_va) {
    NTSTATUS st;
    yghv_protect_target_t *t;
    ExAcquireFastMutex(&g_protect_lock);
    t = yghv_protect_find_target_by_cr3_locked(cr3);
    if (!t) {
        ExReleaseFastMutex(&g_protect_lock);
        return STATUS_ACCESS_DENIED;
    }
    st = yghv_protect_remove_page_for_locked(t, target_va);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

/* 206-C4: cross-process protection. The 0x801/0x802 variants resolve the
 * target slot by the CALLER's CR3, which only works when the controlling
 * process IS the target (selftest). Product semantics need an external
 * controller to arm a page inside another process, so these resolve the
 * slot by pid instead; the target itself must already be registered via
 * set-target. resolve_va_for already walks the TARGET's page tables
 * (KeStackAttachProcess), so no further changes are needed. General code,
 * deliberately not 206-gated: protect.c compiles in the default build too
 * and an unmatched-symbol default link (LNK2019) is worse than dead code. */
NTSTATUS yghv_protect_add_page_for_pid(uint32_t pid, uint64_t target_va) {
    NTSTATUS st;
    yghv_protect_target_t *t;
    if (pid == 0)
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g_protect_lock);
    t = yghv_protect_find_target_by_pid_locked(pid);
    if (!t) {
        ExReleaseFastMutex(&g_protect_lock);
        return STATUS_NOT_FOUND;
    }
    st = yghv_protect_add_page_for_locked(t, target_va);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

NTSTATUS yghv_protect_remove_page_for_pid(uint32_t pid, uint64_t target_va) {
    NTSTATUS st;
    yghv_protect_target_t *t;
    if (pid == 0)
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g_protect_lock);
    t = yghv_protect_find_target_by_pid_locked(pid);
    if (!t) {
        ExReleaseFastMutex(&g_protect_lock);
        return STATUS_NOT_FOUND;
    }
    st = yghv_protect_remove_page_for_locked(t, target_va);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

static NTSTATUS yghv_protect_remove_page_for_locked(yghv_protect_target_t *t,
    uint64_t target_va) {
    uint32_t i;
    if (!t)
        return STATUS_NOT_FOUND;
    for (i = 0; i < t->page_count; i++) {
        if (t->pages[i].target_va == target_va) {
            if (t->pages[i].armed) {
                int st = yghv_protect_disarm_page_locked(&t->pages[i]);
                if (st)
                    return (NTSTATUS)st;
            }
            t->pages[i] = t->pages[t->page_count - 1];
            t->page_count--;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

/* 9.282: lock-free variant for the island (same discipline as
 * on_npf_write_bare's find_page_locked usage — nonpaged table, IOCTL-time
 * mutations only, verdicts tolerate torn reads). */
yghv_protect_page_t *yghv_protect_find_page_bare(uint64_t gpa) {
    uint32_t t, i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    for (t = 0; t < g_protect.target_count; t++) {
        for (i = 0; i < g_protect.targets[t].page_count; i++) {
            if (g_protect.targets[t].pages[i].gpa == page)
                return &g_protect.targets[t].pages[i];
        }
    }
    return NULL;
}

yghv_protect_page_t *yghv_protect_find_page(uint64_t gpa) {
    yghv_protect_page_t *p;
    ExAcquireFastMutex(&g_protect_lock);
    p = yghv_protect_find_page_locked(gpa);
    ExReleaseFastMutex(&g_protect_lock);
    return p;
}

int yghv_protect_arm_page(yghv_protect_page_t *p) {
    int st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_arm_page_locked(p);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

static int yghv_protect_arm_page_locked(yghv_protect_page_t *p) {
    ULONG i;
    int st;

    /* 9.299 (C line): in sync mode the page must stay present+WRITABLE in the
     * NPT for its whole armed life. Nothing is flipped, so the hardware never
     * has to invalidate a translation and the Zen3 NPT-TLB limitation (9.297 /
     * 9.298) cannot apply. Detection moves to the polling guard thread. */
    if (InterlockedCompareExchange(&g_sync_mode, 0, 0)) {
        st = npt_split_2mb_to_4kb(&g_npt, p->gpa);
        if (st)
            return st;
        st = npt_set_page_perm(&g_npt, p->gpa,
                               NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
        if (st)
            return st;
        p->armed = 1;
        /* 9.299 FIX: _locked form — the caller already holds g_protect_lock. */
        (void)yghv_protect_sync_arm_locked(p->gpa);
        return 0;
    }

    st = npt_split_2mb_to_4kb(&g_npt, p->gpa);
    if (st)
        return st;
    st = npt_set_page_perm(&g_npt, p->gpa, NPT_PERM_PRESENT);
    if (!st) {
        p->armed = 1;
        /* 9.280: mirror into the alt NPT when fake mode is on — the alt
         * maps armed pages to their scratch buffers so a foreign kernel
         * write during a fake window lands in scratch. */
        if (InterlockedCompareExchange(&g_fake_mode, 0, 0) && g_npt_alt_ready)
            (void)yghv_protect_fake_scratch_for(p->gpa);
        for (i = 0; i < SVM_MAX_CORES; i++) {
            if (g_vcpus[i])
                g_vcpus[i]->npt_flush_pending = 1;
        }
    }
    return st;
}

int yghv_protect_disarm_page(yghv_protect_page_t *p) {
    int st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_disarm_page_locked(p);
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

static int yghv_protect_disarm_page_locked(yghv_protect_page_t *p) {
    ULONG i;
    int st;

    /* 9.299 (C line): drop the page from the polling guard. */
    yghv_protect_sync_disarm(p->gpa);

    if (InterlockedCompareExchange(&g_sync_mode, 0, 0)) {
        /* Sync mode never flipped anything, so there is no permission to put
         * back — just clear the armed flag. */
        p->armed = 0;
        return 0;
    }

    st = npt_set_page_perm(&g_npt, p->gpa,
        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    if (!st) {
        p->armed = 0;
        /* 9.280: disarm mirror — the alt maps the page back to identity RW
         * (no longer protected; writes land for real). */
        if (InterlockedCompareExchange(&g_fake_mode, 0, 0) && g_npt_alt_ready) {
            (void)npt_split_2mb_to_4kb(&g_npt_alt, p->gpa);
            (void)npt_map_page(&g_npt_alt, p->gpa, p->gpa,
                NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
        }
        for (i = 0; i < SVM_MAX_CORES; i++) {
            if (g_vcpus[i])
                g_vcpus[i]->npt_flush_pending = 1;
        }
    }
    return st;
}

/* 9.258 (206-C3): island-safe silent reopen for the vendored NPF handler's
 * NONE-on-write path (unknown/removed armed page). Lock-free, no telemetry:
 * the guest PTE is writable, so the right move is to restore the NPT to
 * writable and let the write re-execute — an injected #PF there loops
 * forever (spurious-fault retry, the c12 victim storm). TLB propagation is
 * free: this core gets TlbControl=1 from the caller, and every CR3-write
 * exit already flushes. */
void yghv_protect_reopen_page_bare(uint64_t gpa) {
    if (!g_protect.active)
        return;
    (void)npt_set_page_perm(&g_npt, gpa & ~(uint64_t)0xFFFULL,
        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
}

/* 9.258 (206-C3): target-exit watchdog. Registered via
 * PsSetCreateProcessNotifyRoutine (main.c); fires on process teardown.
 * Without this, a dead target's armed pages leak write-protected NPT state
 * onto freed physical pages — the next owner's writes hit stale NPT
 * entries (the c12 victim storm amplifier). PASSIVE_LEVEL callback context,
 * mutex-protected, memory + NPT bit ops only. */
void yghv_protect_on_process_exit(uint32_t pid) {
    yghv_protect_target_t *t;
    uint32_t i;

    if (pid == 0)
        return;
    ExAcquireFastMutex(&g_protect_lock);
    t = yghv_protect_find_target_by_pid_locked(pid);
    if (t) {
        for (i = 0; i < t->page_count; i++) {
            if (t->pages[i].armed)
                (void)yghv_protect_disarm_page_locked(&t->pages[i]);
        }
        if (t->process)
            ObDereferenceObject(t->process);
        RtlZeroMemory(t, sizeof(*t));
        yghv_protect_refresh_cr3_list_locked();
    }
    ExReleaseFastMutex(&g_protect_lock);
}

NTSTATUS yghv_protect_start(void) {
    NTSTATUS st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_start_locked();
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

static NTSTATUS yghv_protect_start_locked(void) {
    uint32_t t, i;
    ULONG total = 0;
    for (t = 0; t < g_protect.target_count; t++) {
        if (g_protect.targets[t].cr3)
            total += g_protect.targets[t].page_count;
    }
    /* REV-042: 0 pages means nothing to arm — reject (the default persistent
       path always has >= 1 page: workload + hook).  The gated baremetal
       step-9 "keepalive only" flow expects a success here; that experiment
       should add a keepalive page or be updated, it is not the default path. */
    if (!total)
        return STATUS_INVALID_PARAMETER;
    for (t = 0; t < g_protect.target_count; t++) {
        for (i = 0; i < g_protect.targets[t].page_count; i++) {
            int st = yghv_protect_arm_page_locked(&g_protect.targets[t].pages[i]);
            if (st) {
                LOG_ERROR("protect start: arm target %u page %u failed 0x%x",
                    t, i, st);
                NTSTATUS disarm_status = yghv_protect_stop_locked();
                if (disarm_status != STATUS_SUCCESS) {
                    LOG_ERROR("protect start: rollback disarm failed 0x%x",
                        disarm_status);
                    g_protect.active = TRUE;
                    return STATUS_UNSUCCESSFUL;
                }
                return (NTSTATUS)st;
            }
        }
    }
    g_protect.active = TRUE;
    LOG_ERROR("protect start: %u pages armed", total);
    return STATUS_SUCCESS;
}

NTSTATUS yghv_protect_stop(void) {
    NTSTATUS st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_stop_locked();
    ExReleaseFastMutex(&g_protect_lock);
    return st;
}

static NTSTATUS yghv_protect_stop_locked(void) {
    uint32_t t, i;
    NTSTATUS first_failure = STATUS_SUCCESS;
    for (t = 0; t < g_protect.target_count; t++) {
        for (i = 0; i < g_protect.targets[t].page_count; i++) {
            if (g_protect.targets[t].pages[i].armed) {
                int st = yghv_protect_disarm_page_locked(
                    &g_protect.targets[t].pages[i]);
                if (st && first_failure == STATUS_SUCCESS) {
                    first_failure = (NTSTATUS)st;
                    LOG_ERROR("protect stop: disarm target %u page %u failed 0x%x",
                        t, i, st);
                }
            }
        }
    }
    if (first_failure != STATUS_SUCCESS)
        return STATUS_UNSUCCESSFUL;
    g_protect.active = FALSE;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* 9.249 step206-C2: LOCK-FREE in-island NPF verdict for the vendored    */
/* upstream handler (svm_simplevm206.cpp). Constraints of that context:  */
/* IRQL = DISPATCH (SvHandleVmExit raises), GIF=0, and NO blocking —     */
/* ExAcquireFastMutex would bugcheck (DISPATCH) / deadlock (island,      */
/* 9.229 lesson twice). So this path must be pure memory + NPT           */
/* arithmetic (direct-map writes), no locks, no LOG/trace, no allocs.    */
/*                                                                       */
/* Lock-free correctness: the protected-page table (g_protect.targets)   */
/* is nonpaged and only mutated at PASSIVE under g_protect_lock by       */
/* IOCTLs. The island reads it WITHOUT the lock, so an IOCTL racing an   */
/* NPF verdict may see a torn view. Mitigations: (a) mutations are       */
/* small fixed-struct writes (single u64 gpa / u8 flags — naturally      */
/* atomic on x64 for aligned u64); (b) a wrong verdict's worst case is   */
/* one spurious #PF or one missed protection hit, self-correcting on     */
/* the next write; (c) the add/remove IOCTL paths will set               */
/* g_protect.active=0 (quiesce) around batch mutations in 206-C3.        */
/* TLB: npt_set_page_perm flips the NPT PTE; the handler sets            */
/* TlbControl=1 (flush-all on next VMRUN) when a flip happened — no      */
/* INVLPGA needed since we do not tag per-ASID in the vendored VMCB.     */
/* ------------------------------------------------------------------ */
/* 9.269: island-safe control probe for the DENY GVA decoder's walk
 * diagnostics — walks the first armed target's (cr3, va) pair, whose gpa the
 * control plane already knows. Lock-free read of the same tables the NPF
 * verdict scans; pure arithmetic; no telemetry. 0 = no armed target. */
uint64_t yghv_protect_control_walk_gpa(void) {
    uint32_t t;
    for (t = 0; t < g_protect.target_count; t++) {
        if (g_protect.targets[t].cr3 != 0 &&
            g_protect.targets[t].page_count > 0) {
            return yghv_protect_guest_va_to_pa(
                g_protect.targets[t].cr3,
                g_protect.targets[t].pages[0].target_va);
        }
    }
    return 0;
}

/* 9.280 (C6 v2): DUAL-NPT fake-write for foreign KERNEL writes (cpl=0 — the
 * WriteProcessMemory shape; both MmCopyVirtualMemory forms arrive here, see
 * run_c18). v1's in-place PTE remap failed on Zen3: TlbControl=1 does NOT
 * flush NPT (GPA->HPA) translations, so the shadowed store re-faulted forever
 * (4.4e8 NPFs, ok=1, restores=0). v2 switches the VMCB's nCR3 instead — the
 * cpp sets NCr3=alt on the fake NPF and NCr3=main on the #DB; a DIFFERENT
 * nCR3 value architecturally flushes the NPT TLB (APM 15.16), so the shadow
 * mapping is always visible. The main NPT entry is never modified: no
 * disarm/re-arm bookkeeping, no snapshot, no scratch aliasing of the real
 * page. The alt NPT is a full identity clone built at init; armed pages are
 * remapped to per-page scratch buffers when fake mode is enabled (or when
 * armed while fake is on). While a core runs on alt, only armed pages differ
 * (they write to scratch); every other translation is identity-identical. */
void yghv_protect_fake_init(void) {
    /* build the alt NPT at DriverEntry (PASSIVE, bare metal): full identity
     * clone of the 512GB window. Allocating ~514 table pages at boot costs
     * 2MB and keeps every later fake switch allocation-free. */
    if (NT_SUCCESS(npt_init(&g_npt_alt, 0x8000000000ULL)))
        g_npt_alt_ready = 1;
    LOG_ERROR("protect fake: alt npt build rc=%d pa=0x%llx", g_npt_alt_ready,
              (unsigned long long)g_npt_alt.pml4_pa);
}

int yghv_protect_fake_mode_get(void) {
    return InterlockedCompareExchange(&g_fake_mode, 0, 0);
}

void yghv_protect_fake_mode_set(int on) {
    /* enable: map every currently-armed page to its own scratch buffer in
     * the alt NPT; disable: restore identity mappings in alt (main untouched
     * either way). IOCTL context = PASSIVE; the ops are the same raw NPT
     * edits the arm path uses. */
    uint32_t t;
    ULONG i2;
    if (on) {
        for (t = 0; t < g_protect.target_count; t++) {
            uint32_t i;
            for (i = 0; i < g_protect.targets[t].page_count; i++) {
                uint64_t gpa = g_protect.targets[t].pages[i].gpa;
                PVOID sc = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE,
                                                 YGHV_TAG);
                if (!sc)
                    continue;
                RtlZeroMemory(sc, HV_PAGE_SIZE);
                if (npt_split_2mb_to_4kb(&g_npt_alt, gpa) == 0)
                    (void)npt_map_page(&g_npt_alt, gpa,
                        MmGetPhysicalAddress(sc).QuadPart,
                        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
            }
        }
        LOG_ERROR("protect fake mode: 1 (alt pa=0x%llx)",
                  (unsigned long long)g_npt_alt.pml4_pa);
    } else {
        for (t = 0; t < g_protect.target_count; t++) {
            uint32_t i;
            for (i = 0; i < g_protect.targets[t].page_count; i++) {
                uint64_t gpa = g_protect.targets[t].pages[i].gpa;
                (void)npt_split_2mb_to_4kb(&g_npt_alt, gpa);
                (void)npt_map_page(&g_npt_alt, gpa, gpa,
                    NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
            }
        }
        LOG_ERROR("protect fake mode: 0");
        /* 9.288 (C6 v5): clear the per-core NCr3 residue - any core still on
         * the alt NPT switches back to main at its next VMRUN. The definition
         * lives in the 206-only vendored TU - gate the call (LNK2019 in the
         * default build otherwise). */
#if defined(YGHV_BAREMETAL_STEP) && (YGHV_BAREMETAL_STEP == 206)
        yghv_protect_fake_ncr3_resync();
#endif
    }
    InterlockedExchange(&g_fake_mode, on ? 1 : 0);
}

UINT64 yghv_protect_fake_alt_pa(void) {
    return g_npt_alt_ready ? g_npt_alt.pml4_pa : 0;
}

#if defined(YGHV_BAREMETAL_STEP) && (YGHV_BAREMETAL_STEP == 206)
/* 9.289 (C6.1): INVLPGA the known GVA (target_va) of every armed page --
 * drops the stale NPT translation for that GVA so the shadow mapping is
 * immediately visible. Island-safe (the asm helper only). */
void yghv_protect_fake_invpga_armeds(UINT32 asid) {
    uint32_t t;
    for (t = 0; t < g_protect.target_count; t++) {
        uint32_t i;
        for (i = 0; i < g_protect.targets[t].page_count; i++) {
            SvInvlpgaByVa(g_protect.targets[t].pages[i].target_va, asid);
        }
    }
}
#endif

LONG yghv_protect_fake_alt_active(void) {
    return InterlockedCompareExchange(&g_fake_alt_active, 0, 0);
}

UINT64 yghv_protect_fake_main_pa(void) {
    return g_npt.pml4_pa;
}

/* scratch buffers handed to the alt NPT: one per armed page, allocated on
 * demand (config-fake enable / arm-while-fake-on). */
static PVOID yghv_protect_fake_scratch_for(uint64_t gpa) {
    PVOID sc = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE, YGHV_TAG);
    if (sc) {
        RtlZeroMemory(sc, HV_PAGE_SIZE);
        if (npt_split_2mb_to_4kb(&g_npt_alt, gpa) == 0)
            (void)npt_map_page(&g_npt_alt, gpa,
                MmGetPhysicalAddress(sc).QuadPart,
                NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    }
    return sc;
}

void yghv_protect_fake_diag(UINT64 out[5]) {
    out[0] = (UINT64)InterlockedCompareExchange(&g_fake_mode, 0, 0);
    out[1] = (UINT64)InterlockedCompareExchange(&g_fake_attempts, 0, 0);
    out[2] = (UINT64)InterlockedCompareExchange(&g_fake_ok, 0, 0);
    out[3] = (UINT64)InterlockedCompareExchange(&g_fake_last_reject, 0, 0);
    out[4] = (UINT64)InterlockedCompareExchange(&g_fake_restores, 0, 0);
}

/* 9.280: v2 fake_bare is a pure gate — no NPT mutation here. The cpp flips
 * the calling core's NCr3 to alt (guaranteed flush) and the #DB handler
 * flips it back. Counters stay for the 0x812 diagnostics. */
int yghv_protect_fake_bare(uint64_t gpa, uint64_t *rearm_out) {
    InterlockedIncrement(&g_fake_attempts);
    if (!InterlockedCompareExchange(&g_fake_mode, 0, 0)) {
        InterlockedExchange(&g_fake_last_reject, 1);
        return 0;
    }
    if (!g_npt_alt_ready) {
        InterlockedExchange(&g_fake_last_reject, 2);
        return 0;
    }
    InterlockedIncrement(&g_fake_ok);
    InterlockedExchange(&g_fake_last_reject, 0);
    *rearm_out = gpa | 1ULL;               /* bit0 = fake, #DB switches back */
    return 1;
}

/* 9.280: called by the #DB handler after switching NCr3 back to main —
 * counters only (the alt is not un-mapped; fake mode keeps it). */
void yghv_protect_fake_restore(uint64_t gpa) {
    ULONG i;
    UNREFERENCED_PARAMETER(gpa);
    InterlockedIncrement(&g_fake_restores);
    for (i = 0; i < SVM_MAX_CORES; i++)
        if (g_vcpus[i])
            g_vcpus[i]->npt_flush_pending = 1;
}


yghv_npf_result_t yghv_protect_on_npf_write_bare(uint64_t guest_cr3,
                                                 uint32_t cpl,
                                                 uint64_t gpa,
                                                 uint64_t *rearm_gpa_out,
                                                 int *flip_out) {
    yghv_protect_page_t *pp;
    yghv_npf_result_t result = YGHV_NPF_NONE;
    int st;

    *flip_out = 0;
    if (!g_protect.active) {
        /* REV-045: protection off — no disarm/re-arm from stray writes. */
        return YGHV_NPF_NONE;
    }
    pp = yghv_protect_find_page_locked(gpa);   /* lock-free read */
    if (pp) {
        /* 9.280 (C6 v2): foreign KERNEL write (cpl=0, any CR3 — WPM has
         * both MiDoPoolCopy and attach shapes). The cpp switches this core's
         * NCr3 to the alt NPT (architectural flush): the store lands in the
         * page's scratch buffer and the #DB switches NCr3 back. Writer sees
         * success; the real page is untouched. */
        /* 9.276: condition is cpl==0 ONLY. run_c18 evidence: MmCopyVirtual
         * Memory has TWO shapes -- MiDoPoolCopy (caller CR3, the c17 shape)
         * AND KeStackAttachProcess (TARGET CR3 + cpl=0, the c18 shape: the
         * npf ring showed the target's own CR3 on the WPM write). Gating on
         * !is_target missed the attach shape entirely. Shadowing any kernel
         * write to the armed page also covers APC/exception-delivery writes
         * (they land in scratch) -- acceptable for the experimental flag,
         * which is default-OFF and per-run opt-in. */
        if (cpl == 0 && yghv_protect_fake_bare(pp->gpa, rearm_gpa_out)) {
            *flip_out = 1;
            return YGHV_NPF_FAKE;
        }
        if (yghv_protect_is_target_cr3_locked(guest_cr3) || cpl == 0) {
            if (g_protect.config.auto_disarm) {
                st = npt_set_page_perm(&g_npt, pp->gpa,
                    NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
                if (st) {
                    result = YGHV_NPF_DENY;   /* could not reopen: deny */
                } else {
                    pp->armed = 0;
                    *rearm_gpa_out = pp->gpa;
                    *flip_out = 1;
                    result = YGHV_NPF_ALLOW;
                }
            } else {
                result = YGHV_NPF_DENY;       /* deny = inject #PF, stay armed */
            }
        } else {
            result = YGHV_NPF_DENY;           /* foreign write: deny */
        }
    }
    return result;
}

/* 9.249: re-arm (flip back to read-only) for the vendored handler's #DB path.
 * Same discipline as above: no locks. gpa comes from the vcpu's rearm slot. */
int yghv_protect_arm_page_bare(uint64_t gpa) {
    yghv_protect_page_t *pp;
    int st;

    if (!g_protect.active)
        return 0;
    pp = yghv_protect_find_page_locked(gpa);
    if (!pp || pp->armed)
        return 0;
    st = npt_split_2mb_to_4kb(&g_npt, pp->gpa);
    if (st)
        return st;
    st = npt_set_page_perm(&g_npt, pp->gpa, NPT_PERM_PRESENT);
    if (!st)
        pp->armed = 1;
    return st;
}

yghv_npf_result_t yghv_protect_on_npf_write(svm_vcpu_t *vcpu, uint64_t gpa) {
    yghv_protect_page_t *pp;
    yghv_npf_result_t result = YGHV_NPF_NONE;
    int st;

    ExAcquireFastMutex(&g_protect_lock);
    if (!g_protect.active) {
        /* REV-045: protection is off — do not disarm/re-arm pages from an
           NPF write (a page left armed after a failed stop must not be
           permanently disarmed by one write). */
        ExReleaseFastMutex(&g_protect_lock);
        return YGHV_NPF_NONE;
    }
    pp = yghv_protect_find_page_locked(gpa);
    if (pp) {
        if (yghv_protect_is_target_cr3_locked(vcpu->vmcb->state.cr3) ||
            vcpu->vmcb->state.cpl == 0) {
            if (g_protect.config.auto_disarm) {
                st = yghv_protect_disarm_page_locked(pp);
                if (st) {
                    LOG_ERROR("protect: disarm failed gpa=0x%llx st=0x%x",
                        pp->gpa, st);
                    result = YGHV_NPF_DENY;
                } else {
                    vcpu->rearm_gpa = pp->gpa;
                    vcpu->rearm_pending = 1;
                    result = YGHV_NPF_ALLOW;
                }
            } else {
                result = YGHV_NPF_DENY;
            }
        } else {
            result = YGHV_NPF_DENY;
        }
    }
    ExReleaseFastMutex(&g_protect_lock);
    return result;
}

void yghv_protect_rearm(svm_vcpu_t *vcpu) {
    uint64_t gpa;
    yghv_protect_page_t *pp;

    if (!vcpu->rearm_pending)
        return;
    ExAcquireFastMutex(&g_protect_lock);
    gpa = vcpu->rearm_gpa;
    vcpu->rearm_pending = 0;
    vcpu->rearm_gpa = 0;
    if (g_protect.active && gpa) {
        pp = yghv_protect_find_page_locked(gpa);
        if (pp && !pp->armed) {
            int st = yghv_protect_arm_page_locked(pp);
            if (st)
                LOG_ERROR("protect: re-arm failed gpa=0x%llx st=0x%x", gpa, st);
        }
    }
    ExReleaseFastMutex(&g_protect_lock);
}

void yghv_protect_get_state(ULONG *active, ULONG *pid, ULONG *page_count) {
    ExAcquireFastMutex(&g_protect_lock);
    if (active) *active = g_protect.active ? 1 : 0;
    if (pid) *pid = g_protect.targets[0].pid;
    if (page_count) *page_count = g_protect.targets[0].page_count;
    ExReleaseFastMutex(&g_protect_lock);
}

void yghv_protect_get_heartbeat(uint64_t *page_va, uint64_t *hook_va) {
    ExAcquireFastMutex(&g_protect_lock);
    if (page_va)
        *page_va = g_protect.targets[0].page_count ? g_protect.targets[0].pages[0].target_va : 0;
    if (hook_va)
        *hook_va = g_protect_hooks[0].installed ? g_protect_hooks[0].func_va : 0;
    ExReleaseFastMutex(&g_protect_lock);
}

void yghv_protect_get_target(ULONG *active, ULONG *pid, ULONG_PTR *cr3,
    ULONG *page_count, ULONG *hook_count) {
    ULONG i;
    ExAcquireFastMutex(&g_protect_lock);
    if (active) *active = g_protect.active ? 1 : 0;
    if (pid) *pid = g_protect.targets[0].pid;
    if (cr3) *cr3 = g_protect.targets[0].cr3;
    if (page_count) *page_count = g_protect.targets[0].page_count;
    if (hook_count) {
        ULONG n = 0;
        for (i = 0; i < YGHV_PROTECT_MAX_HOOKS; i++) {
            if (g_protect_hooks[i].installed)
                n++;
        }
        *hook_count = n;
    }
    ExReleaseFastMutex(&g_protect_lock);
}

void yghv_protect_get_pages_info(yghv_protect_pages_info_t *info) {
    ULONG t, i, n = 0;
    ULONG cap = info->count;
    ExAcquireFastMutex(&g_protect_lock);
    for (t = 0; t < g_protect.target_count; t++) {
        for (i = 0; i < g_protect.targets[t].page_count && n < cap; i++) {
            info->pages[n].gpa = g_protect.targets[t].pages[i].gpa;
            info->pages[n].target_va = g_protect.targets[t].pages[i].target_va;
            info->pages[n].flags = g_protect.targets[t].pages[i].flags;
            info->pages[n].armed = g_protect.targets[t].pages[i].armed;
            RtlZeroMemory(info->pages[n].reserved,
                sizeof(info->pages[n].reserved));
            n++;
        }
    }
    /* returned = number of entries actually written (≤ cap); the previous
       uncapped cross-target total could exceed the 64-entry buffer and cause
       a userland over-read. */
    info->returned = n;
    ExReleaseFastMutex(&g_protect_lock);
}

void yghv_protect_get_targets_info(yghv_protect_targets_info_t *info) {
    ULONG i, n = 0;
    ULONG cap = info->count;
    ULONG total = 0;
    ExAcquireFastMutex(&g_protect_lock);
    for (i = 0; i < g_protect.target_count; i++) {
        yghv_protect_target_t *t = &g_protect.targets[i];
        total++;
        if (n < cap) {
            info->targets[n].pid = t->pid;
            info->targets[n].cr3 = t->cr3;
            info->targets[n].page_count = t->page_count;
            info->targets[n].active = (t->cr3 != 0) ? 1 : 0;
            info->targets[n].hook_count = 0;
            n++;
        }
    }
    info->returned = total;
    ExReleaseFastMutex(&g_protect_lock);
}

void yghv_protect_get_hooks_info(yghv_protect_hooks_info_t *info) {
    ULONG i;
    ULONG n;
    ExAcquireFastMutex(&g_protect_lock);
    n = 0;
    for (i = 0; i < YGHV_PROTECT_MAX_HOOKS; i++) {
        if (!g_protect_hooks[i].installed)
            continue;
        if (n < info->count) {
            info->hooks[n].func_va = g_protect_hooks[i].func_va;
            info->hooks[n].hook_id = g_protect_hooks[i].hook_id;
            info->hooks[n].installed = 1;
            info->hooks[n].patch_len = g_protect_hooks[i].patch_len;
            info->hooks[n].reserved = 0;
        }
        n++;
    }
    info->returned = n;
    ExReleaseFastMutex(&g_protect_lock);
}

NTSTATUS yghv_protect_clear(void) {
    uint32_t i, t;
    ExAcquireFastMutex(&g_protect_lock);
    if (g_protect.active)
        yghv_protect_stop_locked();
    for (i = 0; i < YGHV_PROTECT_MAX_HOOKS; i++) {
        if (g_protect_hooks[i].installed)
            yghv_protect_remove_hook_locked(i);
    }
    for (t = 0; t < g_protect.target_count; t++) {
        yghv_protect_target_t *target = &g_protect.targets[t];
        target->page_count = 0;
        if (target->process) {
            ObDereferenceObject(target->process);
            target->process = NULL;
        }
        target->pid = 0;
        target->cr3 = 0;
    }
    g_protect.target_count = 1;
    g_protect.active = FALSE;
    yghv_protect_refresh_cr3_list_locked();
    LOG_ERROR("protect clear: target/pages/hooks cleared");
    ExReleaseFastMutex(&g_protect_lock);
    yghv_hook_diag_flush();
    return STATUS_SUCCESS;
}

NTSTATUS yghv_protect_set_config(const yghv_protect_config_t *cfg) {
    int want_sync, want_ms;
    if (!cfg)
        return STATUS_INVALID_PARAMETER;
    if (cfg->auto_disarm > 1 || cfg->deny_status == 0)
        return STATUS_INVALID_PARAMETER;
    if (cfg->sync_enable > 1)
        return STATUS_INVALID_PARAMETER;

    want_ms = (int)cfg->sync_interval_ms;
    if (want_ms < 1) want_ms = 1;
    if (want_ms > 5000) want_ms = 5000;
    want_sync = (int)cfg->sync_enable;

    ExAcquireFastMutex(&g_protect_lock);
    /* Strict deny (auto_disarm=0) makes every write to an armed page inject
       #PF. Persistent workload guests have no #PF handler, so reject the
       combination while protection is active. */
    if (cfg->auto_disarm == 0 && g_protect.active) {
        ExReleaseFastMutex(&g_protect_lock);
        return STATUS_ACCESS_DENIED;
    }
    g_protect.config.auto_disarm = cfg->auto_disarm;
    g_protect.config.deny_status = cfg->deny_status;
    g_protect.config.sync_enable = (ULONG)want_sync;
    g_protect.config.sync_interval_ms = (ULONG)want_ms;
    LOG_ERROR("protect config: auto_disarm=%u deny_status=0x%x sync=%d ms=%d",
        g_protect.config.auto_disarm, g_protect.config.deny_status,
        want_sync, want_ms);
    ExReleaseFastMutex(&g_protect_lock);

    /* 9.303: apply the guard settings OUTSIDE the lock. sync_mode_set takes
     * g_protect_lock itself, and re-acquiring a FastMutex in the same thread
     * is the non-recursive deadlock that produced the 0xE2 crash. */
    yghv_protect_sync_set_interval(want_ms);
    if (want_sync != yghv_protect_sync_mode_get())
        yghv_protect_sync_mode_set(want_sync);

    /* 9.304: persist so the guard survives a reboot. Best-effort — a failure
     * here must not fail the config change itself. */
    yghv_protect_config_save();

    return STATUS_SUCCESS;
}

/* ====================================================================
 * 9.304: config persistence.
 *
 * Settings were volatile: every reboot lost them, so an operator had to
 * re-issue `config sync 1 50` before arming each session. Store them under
 * the driver's own service key, which is where a kernel driver is expected to
 * keep its state and is already ACL'd to require elevation to write.
 *
 * The key path must be the one DriverEntry was handed (RegistryPath =
 * \Registry\Machine\System\CurrentControlSet\Services\<service>). It must NOT
 * be hardcoded: ZwCreateKey does not create intermediate components, so a
 * hardcoded name fails outright whenever the service is registered under any
 * other name (e.g. a test service like yghva6). That was the first version's
 * bug — the key was simply never created.
 *
 * Writes happen at PASSIVE from the IOCTL path; the load runs once from
 * yghv_protect_init. Both are best-effort: a missing or malformed value
 * falls back to the built-in defaults rather than failing the load.
 * ==================================================================== */
#define YGHV_REG_SUBKEY    L"\\Parameters"
#define YGHV_REG_VAL_SYNC  L"SyncEnable"
#define YGHV_REG_VAL_MS    L"SyncIntervalMs"

static WCHAR g_reg_key_path[512];      /* RegistryPath + \Parameters */
static int   g_reg_key_ready;

/* Called once from DriverEntry with the service's own RegistryPath. */
void yghv_protect_set_registry_path(PUNICODE_STRING registry_path) {
    static const WCHAR suffix[] = YGHV_REG_SUBKEY;
    size_t i = 0, k = 0;

    if (!registry_path || !registry_path->Buffer || registry_path->Length == 0)
        return;
    if ((registry_path->Length / sizeof(WCHAR)) + 12 >=
        (sizeof(g_reg_key_path) / sizeof(WCHAR)))
        return;

    while (i < (size_t)(registry_path->Length / sizeof(WCHAR)) &&
           i < (sizeof(g_reg_key_path) / sizeof(WCHAR)) - 1) {
        g_reg_key_path[i] = registry_path->Buffer[i];
        i++;
    }
    while (suffix[k] && i < (sizeof(g_reg_key_path) / sizeof(WCHAR)) - 1)
        g_reg_key_path[i++] = suffix[k++];
    g_reg_key_path[i] = 0;
    g_reg_key_ready = 1;
    LOG_ERROR("config: registry key = %ws", g_reg_key_path);
}

static HANDLE yghv_reg_open(int create) {
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;
    NTSTATUS st;

    if (!g_reg_key_ready)
        return NULL;                     /* DriverEntry did not supply it */
    RtlInitUnicodeString(&path, g_reg_key_path);
    InitializeObjectAttributes(&oa, &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    if (create) {
        ULONG disp = 0;
        st = ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL,
                         REG_OPTION_NON_VOLATILE, &disp);
    } else {
        st = ZwOpenKey(&h, KEY_ALL_ACCESS, &oa);
    }
    return NT_SUCCESS(st) ? h : NULL;
}

void yghv_protect_config_save(void) {
    HANDLE h;
    UNICODE_STRING name;
    ULONG v;

    h = yghv_reg_open(1);
    if (!h)
        return;                                  /* best-effort */

    v = g_protect.config.sync_enable;
    RtlInitUnicodeString(&name, YGHV_REG_VAL_SYNC);
    (void)ZwSetValueKey(h, &name, 0, REG_DWORD, &v, sizeof(v));

    v = g_protect.config.sync_interval_ms;
    RtlInitUnicodeString(&name, YGHV_REG_VAL_MS);
    (void)ZwSetValueKey(h, &name, 0, REG_DWORD, &v, sizeof(v));

    ZwClose(h);
    LOG_ERROR("config: persisted sync=%u ms=%u",
              g_protect.config.sync_enable, g_protect.config.sync_interval_ms);
}

/* Read one REG_DWORD out of the driver's Parameters key.
 *
 * 9.304 FIX: the first version dereferenced Data directly as a ULONG*. Two
 * things were wrong with that:
 *   1. KEY_VALUE_PARTIAL_INFORMATION.Data is a flexible array member sitting
 *      at offset 20 in the structure, so the read is UNALIGNED. The compiler
 *      is free to vectorise it into a movaps, which faults (#GP -> 0x7E
 *      SYSTEM_THREAD_EXCEPTION_NOT_HANDLED) — that is exactly the crash at
 *      yghv_a6+0x12e13, and the disassembly shows movaps [rcx-10],xmm0.
 *   2. No type or length validation: a value stored under the wrong type (or
 *      truncated) would be read past its end.
 * RtlCopyMemory into an aligned local fixes both, and the type/size check
 * makes a malformed value fall back to the default instead of misreading. */
static int yghv_reg_read_dword(HANDLE h, PCWSTR value_name, ULONG *out) {
    UNICODE_STRING name;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    ULONG len = 0;
    KEY_VALUE_PARTIAL_INFORMATION *kv = (KEY_VALUE_PARTIAL_INFORMATION *)buf;

    if (!h || !value_name || !out)
        return 0;

    RtlZeroMemory(buf, sizeof(buf));
    RtlInitUnicodeString(&name, value_name);
    if (!NT_SUCCESS(ZwQueryValueKey(h, &name, KeyValuePartialInformation,
                                    buf, sizeof(buf), &len)))
        return 0;

    if (kv->Type != REG_DWORD || kv->DataLength != sizeof(ULONG))
        return 0;

    RtlCopyMemory(out, kv->Data, sizeof(ULONG));
    return 1;
}

void yghv_protect_config_load(void) {
    HANDLE h;
    ULONG v;
    int got_sync = 0, got_ms = 0;

    h = yghv_reg_open(0);
    if (!h)
        return;                                  /* first run: keep defaults */

    if (yghv_reg_read_dword(h, YGHV_REG_VAL_SYNC, &v) && v <= 1) {
        g_protect.config.sync_enable = v;
        got_sync = 1;
    }
    if (yghv_reg_read_dword(h, YGHV_REG_VAL_MS, &v) && v >= 1 && v <= 5000) {
        g_protect.config.sync_interval_ms = v;
        got_ms = 1;
    }
    ZwClose(h);

    if (got_sync || got_ms)
        LOG_ERROR("config: loaded sync=%u ms=%u", g_protect.config.sync_enable,
                  g_protect.config.sync_interval_ms);

    /* Apply the restored values. Both helpers are lock-free and safe here. */
    yghv_protect_sync_set_interval((int)g_protect.config.sync_interval_ms);
    if (g_protect.config.sync_enable)
        yghv_protect_sync_mode_set(1);
}

void yghv_protect_get_config(yghv_protect_config_t *out) {
    ExAcquireFastMutex(&g_protect_lock);
    *out = g_protect.config;
    ExReleaseFastMutex(&g_protect_lock);
    /* 9.303: report the guard's LIVE values, so the read-back is truthful even
     * if config-sync changed them after the last config write. */
    out->sync_enable = (ULONG)yghv_protect_sync_mode_get();
    out->sync_interval_ms = (ULONG)yghv_protect_sync_interval_get();
}

BOOLEAN yghv_protect_check_target_exited(void) {
    PEPROCESS procs[YGHV_PROTECT_MAX_TARGETS];
    uint32_t pids[YGHV_PROTECT_MAX_TARGETS];
    ULONG n = 0;
    ULONG i;
    LARGE_INTEGER timeout;
    NTSTATUS st;
    BOOLEAN handled = FALSE;

    ExAcquireFastMutex(&g_protect_lock);
    for (i = 0; i < g_protect.target_count && n < YGHV_PROTECT_MAX_TARGETS;
         i++) {
        if (g_protect.targets[i].process) {
            procs[n] = g_protect.targets[i].process;
            pids[n] = g_protect.targets[i].pid;
            /* Hold our own reference so the object stays alive across the
               poll; otherwise a concurrent on_target_exit/clear can free it
               between the snapshot and KeWaitForSingleObject (UAF). */
            ObReferenceObject(procs[n]);
            n++;
        }
    }
    ExReleaseFastMutex(&g_protect_lock);
    if (!n)
        return FALSE;
    for (i = 0; i < n; i++) {
        timeout.QuadPart = 0;
        st = KeWaitForSingleObject(procs[i], Executive, KernelMode, FALSE,
                                   &timeout);
        if (st == STATUS_SUCCESS) {
            BOOLEAN match = FALSE;
            /* Re-check under the lock that this slot still owns this pid and
               this process before clearing, so a pid-reuse + retarget between
               snapshot and poll cannot clear the new target's slot. */
            ExAcquireFastMutex(&g_protect_lock);
            {
                yghv_protect_target_t *t =
                    yghv_protect_find_target_by_pid_locked(pids[i]);
                if (t && t->process == procs[i])
                    match = TRUE;
            }
            ExReleaseFastMutex(&g_protect_lock);
            if (match && yghv_protect_on_target_exit(pids[i]))
                handled = TRUE;
        }
        ObDereferenceObject(procs[i]);
    }
    return handled;
}

BOOLEAN yghv_protect_on_target_exit(ULONG pid) {
    BOOLEAN handled = FALSE;
    uint32_t i;
    yghv_protect_target_t *t;

    ExAcquireFastMutex(&g_protect_lock);
    t = yghv_protect_find_target_by_pid_locked(pid);
    if (t) {
        for (i = 0; i < t->page_count; i++) {
            if (t->pages[i].armed) {
                int ds = yghv_protect_disarm_page_locked(&t->pages[i]);
                if (ds)
                    LOG_ERROR("protect target exit: disarm page %u failed 0x%x",
                        i, ds);
            }
        }
        t->page_count = 0;
        if (t->process) {
            ObDereferenceObject(t->process);
            t->process = NULL;
        }
        t->pid = 0;
        t->cr3 = 0;
        yghv_protect_refresh_cr3_list_locked();
        handled = TRUE;
        LOG_ERROR("protect target exited: auto disarm, pid cleared");
    }
    ExReleaseFastMutex(&g_protect_lock);
    return handled;
}

static uint8_t *g_hook_stub_pages[YGHV_PROTECT_MAX_HOOKS];

static uint8_t *yghv_protect_map_writable_page(uint64_t page_va, PMDL *out_mdl) {
    PMDL mdl;
    PVOID map;

    *out_mdl = NULL;
    mdl = IoAllocateMdl((PVOID)page_va, HV_PAGE_SIZE, FALSE, FALSE, NULL);
    if (!mdl)
        return NULL;
    MmBuildMdlForNonPagedPool(mdl);
    map = MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmCached, NULL,
        HighPagePriority, NormalPagePriority);
    if (!map) {
        IoFreeMdl(mdl);
        return NULL;
    }
    *out_mdl = mdl;
    return (uint8_t *)map;
}

static void yghv_protect_unmap_writable_page(PMDL mdl, uint8_t *map) {
    if (mdl && map)
        MmUnmapLockedPages(map, mdl);
    if (mdl)
        IoFreeMdl(mdl);
}

static void yghv_emit_u8(uint8_t *p, uint8_t v) { *p = v; }
static void yghv_emit_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static void yghv_emit_rel32(uint8_t *p, uint64_t from, uint64_t to) {
    long long d = (long long)(to - (from + 5));
    p[0] = 0xE9;
    for (int i = 0; i < 4; i++) p[1 + i] = (uint8_t)((uint64_t)d >> (i * 8));
}
static void yghv_emit_abs_jmp(uint8_t *p, size_t len, uint64_t target) {
    p[0] = 0x48; p[1] = 0xB8;                  /* movabs rax, imm64 */
    yghv_emit_u64(p + 2, target);
    p[10] = 0xFF; p[11] = 0xE0;                /* jmp rax */
    for (size_t i = 12; i < len; i++)
        p[i] = 0x90;
}

static int yghv_decode_modrm(const uint8_t *p, size_t avail, size_t *pos) {
    uint8_t modrm, mod, rm, sib;
    if (*pos >= avail) return -1;
    modrm = p[*pos];
    (*pos)++;
    mod = modrm >> 6;
    rm = modrm & 7;
    if (mod != 3 && rm == 4) {
        if (*pos >= avail) return -1;
        sib = p[*pos];
        (*pos)++;
        if (mod == 0 && (sib & 7) == 5) {
            if (*pos + 4 > avail) return -1;
            *pos += 4;
        }
    }
    if (mod == 1) {
        if (*pos >= avail) return -1;
        (*pos)++;
    } else if (mod == 2) {
        if (*pos + 4 > avail) return -1;
        *pos += 4;
    } else if (mod == 0 && rm == 5) {
        if (*pos + 4 > avail) return -1;
        *pos += 4;
    }
    return 0;
}

/* Conservative x86-64 instruction length decoder. Returns 0 on any form it
   cannot classify; callers reject such targets instead of guessing. */
static int yghv_inst_len(const uint8_t *p, size_t avail, int *rip_rel) {
    size_t pos = 0;
    int rex = 0;
    int prefixes = 0;
    uint8_t b, op;

    if (rip_rel)
        *rip_rel = 0;

    for (;;) {
        if (pos >= avail) return 0;
        b = p[pos];
        if (b >= 0x40 && b <= 0x4F) {
            if (rex) return 0;
            rex = b;   /* keep the REX byte; bit 3 = W (operand size) */
            pos++;
            continue;
        }
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 ||
            b == 0x65) {
            if (++prefixes > 15) return 0;
            pos++;
            continue;
        }
        break;
    }
    if (pos >= avail) return 0;
    op = p[pos];
    pos++;

    if (op == 0x62 || op == 0xC4 || op == 0xC5)
        return 0;  /* EVEX/VEX prefixes: reject unknown forms */

    /* REV-048: DAA/DAS (0x27/0x2F) and far call/jmp (0x9A/0xEA) are invalid in
       x86-64; reject them instead of falling into the generic ModRM branch and
       mis-decoding. */
    if (op == 0x27 || op == 0x2F || op == 0x9A || op == 0xEA)
        return 0;

    if ((op >= 0x50 && op <= 0x5F) || (op >= 0x90 && op <= 0x9F) ||
        (op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF) ||
        op == 0x37 || op == 0x3F || op == 0x9B || op == 0xC3 || op == 0xC9 ||
        op == 0xCB || op == 0xCC || op == 0xCE || op == 0xCF ||
        op == 0xEC || op == 0xED || op == 0xEE || op == 0xEF ||
        op == 0xF4 || op == 0xF5 || op == 0xF8 || op == 0xF9 || op == 0xFA ||
        op == 0xFB || op == 0xFC || op == 0xFD) {
        return (int)pos;
    }
    if ((op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) || op == 0xEB) {
        if (pos + 1 > avail) return 0;
        if (rip_rel) *rip_rel = 1;
        return (int)(pos + 1);
    }
    if (op == 0xE8 || op == 0xE9) {
        if (pos + 4 > avail) return 0;
        if (rip_rel) *rip_rel = 1;
        return (int)(pos + 4);
    }
    if (op >= 0xB0 && op <= 0xB7) {
        if (pos + 1 > avail) return 0;
        return (int)(pos + 1);
    }
    if (op >= 0xB8 && op <= 0xBF) {
        /* B8+rd: REX.W -> movabs r64, imm64 (8-byte imm); no REX.W -> mov
           r32, imm32 (4-byte imm).  Every Windows syscall stub begins with
           the no-REX `mov eax, imm32` form, so sizing it as imm64 was a
           wrong-boundary bug. */
        size_t imm = (rex & 0x08) ? 8 : 4;
        if (pos + imm > avail) return 0;
        return (int)(pos + imm);
    }
    if (op >= 0xA0 && op <= 0xA3) {
        size_t sz = (op & 1) ? 8 : 4;
        if (pos + sz > avail) return 0;
        return (int)(pos + sz);
    }
    if (op == 0xA8 || op == 0xA9) {
        size_t sz = (op == 0xA8) ? 1 : 4;
        if (pos + sz > avail) return 0;
        return (int)(pos + sz);
    }
    if (op == 0x68 || op == 0x6A) {
        size_t sz = (op == 0x68) ? 4 : 1;
        if (pos + sz > avail) return 0;
        return (int)(pos + sz);
    }
    if (op == 0xC2 || op == 0xCA) {
        if (pos + 2 > avail) return 0;
        return (int)(pos + 2);
    }
    if (op == 0xCD) {
        if (pos + 1 > avail) return 0;
        return (int)(pos + 1);
    }
    if (op == 0xE4 || op == 0xE5 || op == 0xE6 || op == 0xE7) {
        if (pos + 1 > avail) return 0;
        return (int)(pos + 1);
    }

    if (op == 0x0F) {
        uint8_t op2;
        if (pos >= avail) return 0;
        op2 = p[pos];
        pos++;
        if (op2 >= 0x80 && op2 <= 0x8F) {
            if (pos + 4 > avail) return 0;
            if (rip_rel) *rip_rel = 1;
            return (int)(pos + 4);
        }
        if (op2 == 0x05 || op2 == 0x07 || op2 == 0x08 || op2 == 0x09 ||
            op2 == 0x0B || op2 == 0x0D || op2 == 0x34 || op2 == 0x35 ||
            op2 == 0x77 || op2 == 0xA2 || (op2 >= 0xC8 && op2 <= 0xCF)) {
            return (int)pos;
        }
        if (op2 == 0x38 || op2 == 0x3A) {
            size_t after;
            uint8_t op3;
            if (pos >= avail) return 0;
            op3 = p[pos];
            pos++;
            (void)op3;
            /* `after` must point at the ModRM byte (after op3); previously it
               was captured before pos++ and decode_modrm started on the op3
               opcode itself (off-by-one) and RIP-relative detection probed the
               wrong byte. */
            after = pos;
            if (rip_rel && after < avail && (p[after] & 0xC7) == 0x05)
                *rip_rel = 1;
            if (yghv_decode_modrm(p, avail, &after) < 0) return 0;
            if (op2 == 0x3A) {
                if (after + 1 > avail) return 0;
                after++;
            }
            return (int)after;
        }
        if (op2 == 0x0F || op2 == 0xBA || op2 == 0xC0 || op2 == 0xC1 ||
            op2 == 0xC4 || op2 == 0xC5) {
            size_t after = pos;
            if (rip_rel && after < avail && (p[after] & 0xC7) == 0x05)
                *rip_rel = 1;
            if (yghv_decode_modrm(p, avail, &after) < 0) return 0;
            if (after + 1 > avail) return 0;
            return (int)(after + 1);
        }
        {
            size_t after = pos;
            if (rip_rel && after < avail && (p[after] & 0xC7) == 0x05)
                *rip_rel = 1;
            if (yghv_decode_modrm(p, avail, &after) < 0) return 0;
            return (int)after;
        }
    }

    if (op == 0x69 || op == 0x6B || op == 0x80 || op == 0x81 || op == 0x83 ||
        op == 0xC0 || op == 0xC1 || op == 0xC6 || op == 0xC7) {
        size_t after = pos;
        size_t imm = (op == 0x69 || op == 0x81 || op == 0xC7)
                         ? 4 : 1;
        if (rip_rel && after < avail && (p[after] & 0xC7) == 0x05)
            *rip_rel = 1;
        if (yghv_decode_modrm(p, avail, &after) < 0) return 0;
        if (after + imm > avail) return 0;
        return (int)(after + imm);
    }

    if (op == 0xF6 || op == 0xF7) {
        size_t after = pos;
        size_t imm = 0;
        /* Group-3: only /0 (test) has an immediate; not/neg/mul/imul/div/idiv
           (F6/F7 /2../7) do not. */
        if (after < avail && ((p[after] >> 3) & 7) == 0)
            imm = (op == 0xF7) ? 4 : 1;
        if (rip_rel && after < avail && (p[after] & 0xC7) == 0x05)
            *rip_rel = 1;
        if (yghv_decode_modrm(p, avail, &after) < 0) return 0;
        if (after + imm > avail) return 0;
        return (int)(after + imm);
    }

    {
        size_t after = pos;
        if (rip_rel && after < avail && (p[after] & 0xC7) == 0x05)
            *rip_rel = 1;
        if (yghv_decode_modrm(p, avail, &after) < 0) return 0;
        return (int)after;
    }
}

int yghv_protect_validate_hook_target(uint64_t func_va) {
    const uint8_t *p;
    size_t off = 0;
    size_t avail;

    if (!func_va)
        return -1;
    avail = HV_PAGE_SIZE - (func_va & (HV_PAGE_SIZE - 1));
    if (avail < YGHV_PROTECT_PATCH_MIN)
        return -1;
    p = (const uint8_t *)func_va;
    while (off < YGHV_PROTECT_PATCH_LEN) {
        int rip_rel = 0;
        int len = yghv_inst_len(p + off, avail - off, &rip_rel);
        if (len <= 0)
            return -1;
        if (rip_rel)
            return -1;
        off += (size_t)len;
        if (off >= YGHV_PROTECT_PATCH_MIN &&
            off <= YGHV_PROTECT_PATCH_LEN)
            return (int)off;
        if (off > YGHV_PROTECT_PATCH_LEN)
            return -1;
    }
    return -1;
}

NTSTATUS yghv_protect_install_hook(uint8_t hook_id, uint64_t func_va) {
    NTSTATUS st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_install_hook_locked(hook_id, func_va);
    ExReleaseFastMutex(&g_protect_lock);
    yghv_hook_diag_flush();
    return st;
}

static NTSTATUS yghv_protect_install_hook_locked(uint8_t hook_id, uint64_t func_va) {
    uint8_t *stub;
    uint8_t patch[YGHV_PROTECT_PATCH_LEN];
    uint64_t func_pa, page_va, page_pa;
    uint8_t *orig;
    uint8_t *wmap = NULL;
    PMDL wmdl = NULL;
    yghv_protect_page_t *pp;
    yghv_protect_hook_t *h;
    int patch_len;
    NTSTATUS st = STATUS_UNSUCCESSFUL;

    if (hook_id >= YGHV_PROTECT_MAX_HOOKS) return STATUS_INVALID_PARAMETER;
    h = &g_protect_hooks[hook_id];
    if (h->installed) return STATUS_ALREADY_COMMITTED;
    patch_len = yghv_protect_validate_hook_target(func_va);
    if (patch_len < 0) {
        LOG_ERROR("protect hook %u: invalid target va=0x%llx", hook_id, func_va);
        return STATUS_INVALID_PARAMETER;
    }

    func_pa = MmGetPhysicalAddress((PVOID)func_va).QuadPart;
    page_va = func_va & ~(HV_PAGE_SIZE - 1);
    page_pa = MmGetPhysicalAddress((PVOID)page_va).QuadPart;
    h->patch_len = (uint8_t)patch_len;
    RtlCopyMemory(h->original, (void *)func_va, h->patch_len);
    h->func_va = func_va;
    h->func_pa = func_pa;
    h->hook_id = hook_id;

    stub = (uint8_t *)ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE,
        YGHV_TAG);
    if (!stub) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(stub, HV_PAGE_SIZE);
    g_hook_stub_pages[hook_id] = stub;

    /* entry: push rax/rcx/rbx; compare current CR3 against every target
       slot's CR3 (unrolled), then jump to the original slot. Native callers
       are not in guest mode, so the decision must not depend on VMMCALL. */
    uint8_t *p = stub;
    int off = 0;
    int i;
    int je_pos[YGHV_PROTECT_MAX_TARGETS];
    p[off++]=0x50; p[off++]=0x51; p[off++]=0x53;   /* push rax,rcx,rbx */
    p[off++]=0x48; p[off++]=0x0F; p[off++]=0x20; p[off++]=0xD8; /* mov rax,cr3 */
    p[off++]=0x48; p[off++]=0x25;                  /* and rax,~0xFFF */
    p[off++]=0x00; p[off++]=0xF0; p[off++]=0xFF; p[off++]=0xFF;
    p[off++]=0x48; p[off++]=0xBB;                  /* movabs rbx,&targets */
    yghv_emit_u64(p+off, (uint64_t)&g_protect.targets[0]);
    off += 8;
    for (i = 0; i < YGHV_PROTECT_MAX_TARGETS; i++) {
        uint32_t disp = (uint32_t)(i * sizeof(yghv_protect_target_t) +
            offsetof(yghv_protect_target_t, cr3));
        p[off++]=0x48; p[off++]=0x8B; p[off++]=0x8B; /* mov rcx,[rbx+disp32] */
        p[off++]=(uint8_t)disp; p[off++]=(uint8_t)(disp >> 8);
        p[off++]=(uint8_t)(disp >> 16); p[off++]=(uint8_t)(disp >> 24);
        p[off++]=0x48; p[off++]=0x81; p[off++]=0xE1;
        p[off++]=0x00; p[off++]=0xF0; p[off++]=0xFF; p[off++]=0xFF;
        p[off++]=0x48; p[off++]=0x39; p[off++]=0xC1; /* cmp rax,rcx */
        je_pos[i] = off;
        p[off++]=0x74;                               /* je allow */
        p[off++]=0;
    }
    /* deny fall-through: pop and return deny_status */
    p[off++]=0x5B; p[off++]=0x59; p[off++]=0x58;
    p[off++]=0x48; p[off++]=0xB8;
    yghv_emit_u64(p+off, (uint64_t)&g_protect.config.deny_status);
    off += 8;
    p[off++]=0x8B; p[off++]=0x00;                   /* mov eax,[rax] */
    p[off++]=0xC3;                                  /* ret */
    while (off < 0x80)
        p[off++] = 0x90;
    for (i = 0; i < YGHV_PROTECT_MAX_TARGETS; i++) {
        int disp = 0x80 - (je_pos[i] + 2);
        p[je_pos[i] + 1] = (uint8_t)disp;
    }
    /* allow: pop and jump to original slot */
    p[off++]=0x5B; p[off++]=0x59; p[off++]=0x58;
    yghv_emit_rel32(p + off, (uint64_t)(p + off), (uint64_t)(stub + 0x90));
    off += 5;

    /* original slot at offset 0x90 */
    orig = stub + 0x90;
    RtlCopyMemory(orig, h->original, h->patch_len);
    /* jump back to func_va + patch_len after the copied original bytes */
    orig[h->patch_len] = 0x49; orig[h->patch_len+1] = 0xBB;
    yghv_emit_u64(orig + h->patch_len + 2, func_va + h->patch_len);
    orig[h->patch_len+10] = 0x41; orig[h->patch_len+11] = 0xFF;
    orig[h->patch_len+12] = 0xE3;                   /* jmp r11 */

    /* patch function entry: variable-length absolute jump -> stub */
    yghv_emit_abs_jmp(patch, h->patch_len, (uint64_t)stub);
    wmap = yghv_protect_map_writable_page(page_va, &wmdl);
    if (!wmap) {
        LOG_ERROR("protect hook %u: writable map of function page 0x%llx failed",
            hook_id, page_pa);
        yghv_hook_diag_mark("install:map", st);
        st = STATUS_UNSUCCESSFUL;
        goto fail;
    }
    st = svm_core_pause_residents_for_patch();
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect hook %u: patch rendezvous failed 0x%x", hook_id, st);
        yghv_hook_diag_mark("install:pause", st);
        goto fail;
    }
    RtlCopyMemory(wmap + (func_va & (HV_PAGE_SIZE - 1)), patch,
        h->patch_len);
    KeInvalidateRangeAllCaches((PVOID)func_va, h->patch_len);
    svm_core_resume_residents();

    /* write-protect the function's page in NPT */
    st = npt_split_2mb_to_4kb(&g_npt, page_pa);
    if (st) {
        LOG_ERROR("protect hook %u: split function page failed 0x%x",
            hook_id, st);
        yghv_hook_diag_mark("install:split", st);
        goto fail;
    }
    st = npt_set_page_perm(&g_npt, page_pa, NPT_PERM_PRESENT);
    if (st) {
        LOG_ERROR("protect hook %u: set function page perm failed 0x%x",
            hook_id, st);
        yghv_hook_diag_mark("install:perm", st);
        goto fail;
    }

    /* route writes through the Task 4 NPF policy and arm the page */
    st = yghv_protect_add_page_for_locked(&g_protect.targets[0], h->func_va);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect hook %u: add function page failed 0x%x",
            hook_id, st);
        yghv_hook_diag_mark("install:addpage", st);
        goto fail;
    }
    pp = yghv_protect_find_page_locked(page_pa);
    if (!pp) {
        LOG_ERROR("protect hook %u: function page missing from table",
            hook_id);
        yghv_hook_diag_mark("install:missing", st);
        yghv_protect_remove_page_for_locked(&g_protect.targets[0], h->func_va);
        st = STATUS_UNSUCCESSFUL;
        goto fail;
    }
    st = yghv_protect_arm_page_locked(pp);
    if (st) {
        LOG_ERROR("protect hook %u: arm function page failed 0x%x",
            hook_id, st);
        yghv_hook_diag_mark("install:arm", st);
        yghv_protect_remove_page_for_locked(&g_protect.targets[0], h->func_va);
        goto fail;
    }

    h->installed = 1;
    LOG_ERROR("protect hook %u installed: va=0x%llx pa=0x%llx len=%u",
        hook_id, func_va, func_pa, h->patch_len);
    return STATUS_SUCCESS;

fail:
    yghv_hook_diag_mark("install:fail", st);
    npt_set_page_perm(&g_npt, page_pa, NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    for (i = 0; i < SVM_MAX_CORES; i++) {
        if (g_vcpus[i])
            g_vcpus[i]->npt_flush_pending = 1;
    }
    if (wmap) {
        /* REV-040/041: the patch may already be applied with residents resumed;
           re-pause before restoring the original bytes and freeing the stub so
           the guest cannot execute a torn instruction or a freed stub. */
        NTSTATUS pst = svm_core_pause_residents_for_patch();
        RtlCopyMemory(wmap + (func_va & (HV_PAGE_SIZE - 1)), h->original,
            h->patch_len);
        KeInvalidateRangeAllCaches((PVOID)func_va, h->patch_len);
        if (NT_SUCCESS(pst))
            svm_core_resume_residents();
    }
    yghv_protect_unmap_writable_page(wmdl, wmap);
    if (g_hook_stub_pages[hook_id]) {
        ExFreePoolWithTag(g_hook_stub_pages[hook_id], YGHV_TAG);
        g_hook_stub_pages[hook_id] = NULL;
    }
    h->patch_len = 0;
    return st;
}

NTSTATUS yghv_protect_remove_hook(uint8_t hook_id) {
    NTSTATUS st;
    ExAcquireFastMutex(&g_protect_lock);
    st = yghv_protect_remove_hook_locked(hook_id);
    ExReleaseFastMutex(&g_protect_lock);
    yghv_hook_diag_flush();
    return st;
}

static NTSTATUS yghv_protect_remove_hook_locked(uint8_t hook_id) {
    yghv_protect_hook_t *h;
    uint8_t *wmap = NULL;
    PMDL wmdl = NULL;
    uint64_t page_pa;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    if (hook_id >= YGHV_PROTECT_MAX_HOOKS) return STATUS_INVALID_PARAMETER;
    h = &g_protect_hooks[hook_id];
    if (!h->installed) return STATUS_NOT_FOUND;

    page_pa = MmGetPhysicalAddress(
        (PVOID)(h->func_va & ~(HV_PAGE_SIZE - 1))).QuadPart;
    wmap = yghv_protect_map_writable_page(
        h->func_va & ~(HV_PAGE_SIZE - 1), &wmdl);
    if (!wmap) {
        LOG_ERROR(
            "protect remove hook %u: writable map of function page 0x%llx failed",
            hook_id, page_pa);
        yghv_hook_diag_mark("remove:map", st);
        return STATUS_UNSUCCESSFUL;
    }

    /* remove the page from the NPF policy and restore NPT writable first */
    st = yghv_protect_remove_page_for_locked(&g_protect.targets[0], h->func_va);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect remove hook %u: remove_page failed 0x%x",
            hook_id, st);
        yghv_hook_diag_mark("remove:removepage", st);
        yghv_protect_unmap_writable_page(wmdl, wmap);
        return st;
    }

    st = svm_core_pause_residents_for_patch();
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("protect remove hook %u: patch rendezvous failed 0x%x",
            hook_id, st);
        yghv_hook_diag_mark("remove:pause", st);
        /* REV-038: still restore the original bytes and clear the hook so a
           transient pause failure cannot strand the function permanently
           patched to a stub.  pause_residents_for_patch already resumed
           residents on timeout; restoring under the un-paused guest is
           best-effort (the REV-001 lock/barrier redesign removes this race). */
        RtlCopyMemory(wmap + (h->func_va & (HV_PAGE_SIZE - 1)), h->original,
            h->patch_len);
        KeInvalidateRangeAllCaches((PVOID)h->func_va, h->patch_len);
        yghv_protect_unmap_writable_page(wmdl, wmap);
        if (g_hook_stub_pages[hook_id]) {
            ExFreePoolWithTag(g_hook_stub_pages[hook_id], YGHV_TAG);
            g_hook_stub_pages[hook_id] = NULL;
        }
        h->installed = 0;
        h->patch_len = 0;
        return st;
    }
    RtlCopyMemory(wmap + (h->func_va & (HV_PAGE_SIZE - 1)), h->original,
        h->patch_len);
    KeInvalidateRangeAllCaches((PVOID)h->func_va, h->patch_len);
    svm_core_resume_residents();
    yghv_protect_unmap_writable_page(wmdl, wmap);
    if (g_hook_stub_pages[hook_id]) {
        ExFreePoolWithTag(g_hook_stub_pages[hook_id], YGHV_TAG);
        g_hook_stub_pages[hook_id] = NULL;
    }
    h->installed = 0;
    h->patch_len = 0;
    LOG_ERROR("protect hook %u removed", hook_id);
    return STATUS_SUCCESS;
}

uint64_t yghv_protect_on_hook_query(uint8_t hook_id, uint64_t accessor_cr3) {
    uint64_t result;
    ExAcquireFastMutex(&g_protect_lock);
    if (hook_id >= YGHV_PROTECT_MAX_HOOKS || !g_protect_hooks[hook_id].installed) {
        result = YGHV_STATUS_INVALID;
    } else if (yghv_protect_is_target_cr3_locked(accessor_cr3)) {
        result = YGHV_STATUS_OK;
    } else {
        LOG_ERROR("protect hook %u denied cr3=0x%llx", hook_id, accessor_cr3);
        result = YGHV_STATUS_DENIED;
    }
    ExReleaseFastMutex(&g_protect_lock);
    return result;
}

uint64_t yghv_protect_find_func_pattern(PCWSTR name_hint, uint8_t *pat, SIZE_T pat_len) {
    UNICODE_STRING name;
    (void)pat; (void)pat_len;
    if (!name_hint) return 0;
    RtlInitUnicodeString(&name, name_hint);
    return (uint64_t)MmGetSystemRoutineAddress(&name);
}

/* 9.152: expose the generated stub page VA so main.c can map it into the
   dedicated guest CR3 (the guest calls the hooked dummy -> jumps to the stub). */
uint64_t yghv_protect_get_hook_stub_va(uint8_t hook_id) {
    if (hook_id >= YGHV_PROTECT_MAX_HOOKS)
        return 0;
    return (uint64_t)g_hook_stub_pages[hook_id];
}

/*
 * Tier 4 prerequisite (2026-10-11): remap one GPA to a different SPA and
 * report the previous mapping.
 *
 * This exists to ANSWER A QUESTION, not to ship a feature: 9.297/9.298
 * proved that NPT *permission* flips do not take effect on this CPU, and a
 * remap changes the same GPA->SPA translation the CPU may have cached. Until
 * we know whether a remap is observed, split-view (4.1), cross-process
 * aliasing (4.3) and frame takeover (4.5) all rest on an unverified premise.
 *
 * Safe to call ONLY at PASSIVE: npt_map_page reaches npt_get_pt, which calls
 * MmGetVirtualForPhysical — and 9.299 measured that returning a bogus pointer
 * for user-owned pages. Dereferencing it at DISPATCH is exactly the 0x0A we
 * took on 10-17 (P1 = page-aligned user address, P2 = IRQL 2). The IRQL check
 * below makes that path unreachable rather than relying on the caller.
 */
int yghv_protect_npt_remap(uint64_t gpa, uint64_t spa, int use_alt,
                           uint64_t *prev_spa) {
    npt_mgr_t *m = use_alt ? &g_npt_alt : &g_npt;
    int st;

    /*
     * 9.311: refuse above PASSIVE. npt_alloc_table uses
     * MmAllocateContiguousMemory (DISPATCH-capped at best) and npt_get_pt
     * uses MmGetVirtualForPhysical (returns garbage for user pages), so this
     * whole path is PASSIVE-only in practice. KeAreAllApcsDisabled() is TRUE
     * only when the IRQL is actually raised to APC or above — the same probe
     * mem_engine.c uses (KeGetCurrentIrql lowers to __readcr8, which these
     * WDK libs do not export).
     */
    if (KeAreAllApcsDisabled())
        return 0;

    /*
     * ===== 9.311 FIX: the flags argument must carry the FULL attribute set =====
     *
     * The first version passed NPT_PERM_PRESENT | NPT_PERM_WRITABLE (0x3).
     * npt_map_page() writes:
     *
     *     pt[p0].all = (spa & ~0xFFF) | (flags & 0xFFF) | (flags & (1<<63));
     *
     * so anything not in `flags` is DROPPED — including the user bit (2) and
     * the accessed bit (5) that NPT_4K_PAGE_FLAGS carries. The remapped entry
     * therefore lost its original attributes, and the machine bugchecked
     * 0x0A (IRQL_NOT_LESS_OR_EQUAL, P1 = a page-aligned USER address,
     * P2 = IRQL 2) on the next guest access through it.
     *
     * npt_split_2mb_to_4kb() avoids this by carrying a `common` mask of the
     * original bits; calling npt_map_page() directly bypasses that. Use the
     * same full set here. (We cannot read the OLD entry's exact bits without
     * widening npt_core's static helpers, and NPT_4K_PAGE_FLAGS is what a
     * normal 4K NPT entry carries, so this is the correct baseline.)
     *
     * A remap also must NOT drop NX: a page that was non-executable becoming
     * executable is a silent security change, and the reverse can fault.
     */
    st = npt_map_page(m, gpa, spa, NPT_4K_PAGE_FLAGS);
    if (NT_SUCCESS(st) && prev_spa)
        *prev_spa = 0;                  /* caller reads the "before" itself */

    return NT_SUCCESS(st) ? 1 : 0;
}
/*
 * Tier 4.6 (2026-10-17): change one page's NPT permission in the MAIN table.
 *
 * This is the mechanism behind watchpoints. Registering a watch in a table
 * alone does nothing — the page keeps its normal permission, no #NPF is ever
 * raised, and the handler never runs. The watch only exists once the NPT
 * entry stops permitting the access.
 *
 * `perm` is a NPT_PERM_* set. Passing 0 removes PRESENT entirely (any access
 * faults); passing NPT_PERM_PRESENT keeps reads working and faults writes.
 *
 * PASSIVE only: npt_set_page_perm reaches npt_get_pt -> MmGetVirtualForPhysical,
 * which 9.299 measured returning a bogus pointer for user-owned pages. Same
 * rule as yghv_protect_npt_remap (9.311).
 */
/*
 * Tier 4.4/4.6 (2026-10-17): split a 2MB large page into 4K entries before
 * touching one page's permission.
 *
 * npt_set_page_perm() targets `&pd[p1]` when the containing entry is a large
 * page, so a permission change there applies to the whole 2MB region. Callers
 * that want ONE page protected must split first — the same order
 * yghv_protect_arm_page_bare() has always used.
 *
 * PASSIVE only (npt_split_2mb_to_4kb allocates a page table).
 */
int yghv_protect_npt_split_4k(uint64_t gpa) {
    if (KeAreAllApcsDisabled())
        return 0;
    return NT_SUCCESS(npt_split_2mb_to_4kb(&g_npt, gpa)) ? 1 : 0;
}

int yghv_protect_npt_set_perm(uint64_t gpa, uint64_t perm) {
    int st;

    if (KeAreAllApcsDisabled())
        return 0;

    /*
     * 9.313 FIX: do NOT OR the whole attribute set into `perm`.
     *
     * NPT_4K_PAGE_FLAGS is present|writable|user|accessed (0x27), so
     * `perm | NPT_4K_PAGE_FLAGS` always re-adds WRITABLE — which silently
     * cancelled the very permission removal a watchpoint depends on. The
     * first run of the 4.6 test captured zero events for exactly this
     * reason: the watch was registered, the NPT write bit was never dropped,
     * and no #NPF was ever raised.
     *
     * What must be carried is only the ATTRIBUTE bits that are not
     * permissions — user (2) and accessed (5) — while the caller keeps full
     * control of present (0) and writable (1).
     */
    {
        uint64_t attrs = NPT_4K_PAGE_FLAGS &
                         ~((uint64_t)NPT_PERM_PRESENT | (uint64_t)NPT_PERM_WRITABLE);
        st = npt_set_page_perm(&g_npt, gpa, perm | attrs);
    }
    return NT_SUCCESS(st) ? 1 : 0;
}
/*
 * Tier 4.4 (2026-10-17): re-arm an instruction-level breakpoint page.
 *
 * The #DB handler calls yghv_protect_arm_page_bare(), which only knows pages
 * registered in g_protect. A breakpoint page is not there, so that call
 * silently does nothing and the page stays writable forever after the first
 * hit — the breakpoint would fire once and then never again.
 *
 * This is the matching re-arm. The NPF handler sets rearm_gpa for a bp page,
 * the #DB handler sees the g_protect lookup fail, and calls this instead.
 */
int yghv_bp_rearm_bare(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;

    if (!page)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;                       /* PASSIVE-only */

    return yghv_protect_npt_set_perm(page, NPT_PERM_PRESENT);
}
/*
 * 9.329: read one NPT leaf entry (the MAIN table).
 *
 * Tier 4.7 set PWT/PCD and was "verified" only by the call returning success —
 * which says nothing about the bits. This is the readback that lets a test
 * assert on the table itself.
 */
int yghv_protect_npt_get_entry(uint64_t gpa, uint64_t *entry) {
    if (!entry || KeAreAllApcsDisabled())
        return 0;
    return NT_SUCCESS(npt_get_entry(&g_npt, gpa & ~(uint64_t)0xFFFULL, entry))
           ? 1 : 0;
}
/*
 * 9.330: same remap, but the caller supplies the flags.
 *
 * 9.311 hardcoded NPT_4K_PAGE_FLAGS inside yghv_protect_npt_remap() to fix an
 * attribute-loss crash — correct at the time, but it made the flags parameter
 * unreachable. Tier 4.7 then computed a PWT/PCD attribute set and handed it to
 * a function that ignored it: the set call reported OK, the table never
 * changed, and only the readback added in 9.329 exposed it
 * (verify: PWT=0 PCD=0 after "set OK").
 *
 * Callers that want cache attributes use this entry point. Callers that just
 * want an identity/real remap keep using the simple form, which supplies the
 * full default attribute set exactly as before.
 */
int yghv_protect_npt_remap_flags(uint64_t gpa, uint64_t spa, int use_alt,
                                 uint64_t flags) {
    npt_mgr_t *m = use_alt ? &g_npt_alt : &g_npt;

    if (KeAreAllApcsDisabled())
        return 0;
    if (use_alt && !g_npt_alt_ready)
        return 0;

    /*
     * 9.331: carry the CURRENT entry's status bits over.
     *
     * Building the flag set from a constant alone loses whatever the hardware
     * put there. Measured: a page whose entry read 0x...067 (present|write|
     * user|accessed|DIRTY) came back as 0x...027 after a cache-attribute set
     * and reset — the dirty bit was gone. Dirty is hardware-maintained, but
     * discarding it silently makes the entry describe a state that never
     * happened, and the next thing that reads it may act on that.
     *
     * Only status bits travel (dirty, accessed, user); present/write/cache/NX
     * stay under the caller's control.
     */
    {
        uint64_t old = 0;
        uint64_t keep = NPT_4K_PAGE_FLAGS &
                        ~((uint64_t)NPT_PERM_PRESENT | (uint64_t)NPT_PERM_WRITABLE);
        uint64_t base;

        if (NT_SUCCESS(npt_get_entry(m, gpa, &old)))
            keep |= old & ((1ULL << 6) | (1ULL << 5));   /* dirty | accessed */

        base = keep & ~((uint64_t)NPT_PERM_PRESENT | (uint64_t)NPT_PERM_WRITABLE);
        return NT_SUCCESS(npt_map_page(m, gpa, spa, flags | base)) ? 1 : 0;
    }
}
/*
 * 9.336 (Tier 7.1): remove one page from the guest's identity map.
 *
 * Distinct from yghv_protect_npt_set_perm(page, 0): that clears PRESENT through
 * npt_set_page_perm, which is the same entry shape a breakpoint uses. Hiding
 * wants the leaf entry GONE, so the guest's walk finds nothing at all rather
 * than a non-present entry it might treat as a demand-fault candidate.
 *
 * npt_exclude_pa() already does exactly this (split first, then clear), and is
 * what main.c's own exclude pass uses — so this wrapper only owns the PASSIVE
 * check and the main-table selection.
 */
int yghv_protect_npt_unmap_page(uint64_t gpa) {
    if (KeAreAllApcsDisabled())
        return 0;
    return NT_SUCCESS(npt_exclude_pa(&g_npt, gpa & ~(uint64_t)0xFFFULL))
           ? 1 : 0;
}