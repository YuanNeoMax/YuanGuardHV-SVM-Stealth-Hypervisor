/*
 * mem_bp.c — Tier 4.4: instruction-level access control.
 * See common/mem_bp.h for the semantics and the honest limits.
 */

#include "mem_bp.h"
#include "protect.h"
#include "npt.h"
#include "common/mem_engine.h"

static yghv_bp_t g_bp[YGHV_BP_MAX];
static volatile LONG g_bp_count;

/* Hit ring (same single-producer discipline as mem_watch.c). */
#define YGHV_BP_RING 64
static uint64_t g_ring[YGHV_BP_RING * 4];   /* rip, gpa, kind|cpl<<8, cr3 */
static volatile LONG g_head;
static volatile LONG g_tail;

static volatile LONG64 g_bp_hits;
static volatile LONG64 g_bp_recorded;
static volatile LONG64 g_bp_missed;
static volatile LONG64 g_bp_refusals;

static int yghv_bp_find(uint64_t gpa, uint64_t rip) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);

    for (i = 0; i < n && i < YGHV_BP_MAX; i++) {
        if (InterlockedCompareExchange(&g_bp[i].active, 0, 0) &&
            g_bp[i].gpa == page &&
            (g_bp[i].rip == 0 || g_bp[i].rip == rip))
            return i;
    }
    return -1;
}

int yghv_bp_add(uint64_t gpa, uint64_t rip, uint32_t kinds) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int i;

    /*
     * ===== 9.320 DISABLED — this path took the box down FOUR times =====
     *
     * 2026-10-17: four consecutive 0x3B / 0xC0000005 crashes (21:59, 22:05,
     * 22:20, 22:38), each with the same signature: an access violation on a
     * store inside our own IOCTL thread, with the value being written sitting
     * in Rdx and the destination (an MDL mapping VA) in Rcx.
     *
     * What each attempt fixed, and why none of them was the root cause:
     *
     *   1. GuestPaOfGhcb not set for the bp path      -> set it, still crashed
     *   2. engine not switched to PTE                 -> refused non-PTE, still crashed
     *   3. npt_set_page_perm hitting a 2MB PDE       -> split first, still crashed
     *
     * The evidence chain (kd with LOCAL symbols, which does work — the earlier
     * hangs were the symbol SERVER, not kd):
     *
     *   nt!KiPageFault+0x452
     *   yghv_g16+0x19d17   mov [rcx],r11     <- Rcx = 0xffffce01107be000
     *   yghv_g16+0x11f3c   mov rcx,r12 / mov rdx,r14 / call [imp]
     *
     *   CONTEXT: Rdx = 0xd00d0000d00d0000 (the value we were writing)
     *            R11 = 0xd00d0000d00d0000
     *            Rcx = R12 = 0xffffce01107be000 (the destination)
     *            R8  = 8
     *
     * So a memcpy into a kernel VA that is not writable. The VA is an MDL
     * mapping produced by yghv_pa_write — i.e. the write path itself is being
     * blocked by something we did to the NPT.
     *
     * The part still not understood: yghv_pa_write maps the frame through
     * MmMapLockedPagesSpecifyCache and writes through that KERNEL mapping,
     * which is a GVA->GPA translation and should not consult the NPT at all.
     * Yet it faults. Four hypotheses were tested and all four were wrong, so
     * there is a mechanism here that is not yet modelled — and modelling it by
     * trial costs the user a reboot each time.
     *
     * Do not re-enable without a full model of how the NPT entry for a page
     * relates to the host page-table entry that backs an MDL mapping of the
     * same frame. That is the open question.
     */
    (void)page; (void)rip; (void)kinds; (void)i;
    InterlockedIncrement64(&g_bp_refusals);
    return 0;
}

static int yghv_bp_add_unsafe_do_not_call(uint64_t gpa, uint64_t rip, uint32_t kinds) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int i;

    if (!page || !kinds)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;                       /* PASSIVE-only */
    if (yghv_bp_find(page, rip) >= 0)
        return 1;                       /* already there */

    /*
     * 9.318 SAFETY VALVE: refuse while the engine is not PTE.
     *
     * Measured 2026-10-17 22:05 (0x3B / 0xC0000005): a breakpoint page has its
     * NPT write permission revoked, so ANY guest store to that VA faults in
     * the NPT layer. Engine A (MmCopyVirtualMemory) performs exactly such a
     * store from the IOCTL thread — it is not in the VMEXIT path, so our NPF
     * branch never runs, and KiPageFault escalates the nested fault to a
     * bugcheck. The stack proved it (KiPageFault -> yghv_g16, no SvHandleVmExit
     * frame anywhere).
     *
     * Engine B writes the PHYSICAL FRAME through an MDL, which does not go
     * through the guest's NPT mapping at all — so it is safe.
     *
     * The caller must select Engine B before arming a breakpoint. Rather than
     * silently switching it (which would surprise the caller), refuse.
     */
    if (yghv_mem_engine_get() != YGHV_ENGINE_PTE) {
        InterlockedIncrement64(&g_bp_refusals);
        return 0;
    }

    /*
     * THE COMPLETE MECHANISM, not half of it.
     *
     * 4.6 froze the machine because it revoked the permission and never put
     * it back: the NPF handler had no branch for the page, injected #PF, and
     * the guest spun. Here the permission is revoked AND the handler has the
     * matching branch (yghv_bp_on_npf, wired in svm_simplevm206.cpp), which
     * restores the permission and single-steps. The pair is the feature.
     */
    /*
     * 9.319 FIX: SPLIT FIRST, then revoke.
     *
     * npt_set_page_perm() writes `e = &pd[p1]` when the containing entry is a
     * large page — that is the PDE, so clearing the write bit there makes the
     * ENTIRE 2MB region read-only. Measured three times as 0x3B / 0xC0000005
     * (registers showed Rdx = the value we were writing, Rcx = an MDL mapping
     * VA in the same 2MB window as the breakpoint page).
     *
     * The armed-page path has always done this in the right order:
     *   npt_split_2mb_to_4kb(&g_npt, gpa);   // 2MB PDE -> 512 4K PTEs
     *   npt_set_page_perm(&g_npt, gpa, ...); // now e = &pt[p0], one page only
     * Skipping the split is why the first three bp attempts took the box down.
     */
    /*
     * 9.319 FIX: SPLIT FIRST, then revoke.
     *
     * npt_set_page_perm() writes `e = &pd[p1]` when the containing entry is a
     * large page — that is the PDE, so clearing the write bit there makes the
     * ENTIRE 2MB region read-only. Measured three times as 0x3B / 0xC0000005
     * (registers showed Rdx = the value we were writing, Rcx = an MDL mapping
     * VA in the same 2MB window as the breakpoint page).
     *
     * The armed-page path has always done this in the right order:
     *   npt_split_2mb_to_4kb(&g_npt, gpa);   // 2MB PDE -> 512 4K PTEs
     *   npt_set_page_perm(&g_npt, gpa, ...); // now e = &pt[p0], one page only
     * Skipping the split is why the first three bp attempts took the box down.
     */
    if (!yghv_protect_npt_split_4k(page))
        return 0;
    if (!yghv_protect_npt_set_perm(page, NPT_PERM_PRESENT))
        return 0;

    for (i = 0; i < YGHV_BP_MAX; i++) {
        if (!InterlockedCompareExchange(&g_bp[i].active, 0, 0)) {
            g_bp[i].gpa   = page;
            g_bp[i].rip   = rip;
            g_bp[i].kinds = kinds;
            g_bp[i].hits  = 0;
            InterlockedExchange(&g_bp[i].active, 1);
            InterlockedIncrement(&g_bp_count);
            return 1;
        }
    }

    /* Table full: undo the permission change so we do not leave the page
     * faulting with nobody listening. */
    (void)yghv_protect_npt_set_perm(page,
                                    NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    return 0;
}

int yghv_bp_remove(uint64_t gpa, uint64_t rip) {
    int slot = yghv_bp_find(gpa, rip);

    if (slot < 0)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;

    /* Give the page its normal access back BEFORE dropping the slot. */
    (void)yghv_protect_npt_set_perm(g_bp[slot].gpa,
                                    NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    InterlockedExchange(&g_bp[slot].active, 0);
    InterlockedDecrement(&g_bp_count);
    return 1;
}

/*
 * ISLAND-SAFE. The caller (the NPF handler) is responsible for the ALLOW
 * path: restore the permission, set TF, and re-arm on #DB. This function
 * only decides whether the fault is ours and records it.
 */
int yghv_bp_on_npf(uint64_t gpa, uint64_t rip, uint32_t kind, uint32_t cpl,
                   uint64_t cr3) {
    int slot;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    LONG head, tail;

    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);
    if (n <= 0)
        return 0;

    slot = yghv_bp_find(page, rip);
    if (slot < 0) {
        InterlockedIncrement64(&g_bp_refusals);
        return 0;
    }
    if (!(g_bp[slot].kinds & kind)) {
        InterlockedIncrement64(&g_bp_refusals);
        return 0;
    }

    InterlockedIncrement((volatile LONG *)&g_bp[slot].hits);
    InterlockedIncrement64(&g_bp_hits);

    /* Record. Ring full -> count the miss, never block. */
    head = InterlockedCompareExchange(&g_head, 0, 0);
    tail = InterlockedCompareExchange(&g_tail, 0, 0);
    if (((head + 1) & (YGHV_BP_RING - 1)) == (tail & (YGHV_BP_RING - 1))) {
        InterlockedIncrement64(&g_bp_missed);
        return 1;                       /* still a match, just not recorded */
    }

    {
        LONG s = head & (YGHV_BP_RING - 1);
        g_ring[s*4+0] = rip;
        g_ring[s*4+1] = page;
        g_ring[s*4+2] = (uint64_t)kind | ((uint64_t)cpl << 8);
        g_ring[s*4+3] = cr3;
        InterlockedExchange(&g_head, head + 1);
        InterlockedIncrement64(&g_bp_recorded);
    }
    return 1;
}

uint32_t yghv_bp_drain(uint64_t *out, uint32_t max) {
    uint32_t n = 0;
    LONG tail, head;

    if (!out || !max)
        return 0;

    for (;;) {
        tail = InterlockedCompareExchange(&g_tail, 0, 0);
        head = InterlockedCompareExchange(&g_head, 0, 0);
        if (tail == head || n + 4 > max)
            break;

        out[n+0] = g_ring[(tail & (YGHV_BP_RING - 1))*4 + 0];
        out[n+1] = g_ring[(tail & (YGHV_BP_RING - 1))*4 + 1];
        out[n+2] = g_ring[(tail & (YGHV_BP_RING - 1))*4 + 2];
        out[n+3] = g_ring[(tail & (YGHV_BP_RING - 1))*4 + 3];
        n += 4;
        InterlockedExchange(&g_tail, tail + 1);
    }
    return n;
}

void yghv_bp_stats_get(yghv_bp_stats_t *out) {
    if (!out)
        return;
    out->count    = (uint64_t)InterlockedCompareExchange(&g_bp_count, 0, 0);
    out->hits     = (uint64_t)InterlockedCompareExchange64(&g_bp_hits, 0, 0);
    out->recorded = (uint64_t)InterlockedCompareExchange64(&g_bp_recorded, 0, 0);
    out->missed   = (uint64_t)InterlockedCompareExchange64(&g_bp_missed, 0, 0);
    out->refusals = (uint64_t)InterlockedCompareExchange64(&g_bp_refusals, 0, 0);
}

void yghv_bp_stats_reset(void) {
    InterlockedExchange64(&g_bp_hits, 0);
    InterlockedExchange64(&g_bp_recorded, 0);
    InterlockedExchange64(&g_bp_missed, 0);
    InterlockedExchange64(&g_bp_refusals, 0);
    InterlockedExchange(&g_head, 0);
    InterlockedExchange(&g_tail, 0);
}
