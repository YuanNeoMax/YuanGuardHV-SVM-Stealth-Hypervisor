/*
 * mem_watch.c — Tier 4.6: NPF-based watchpoints.
 * See common/mem_watch.h for why this beats DR0-DR3 and int3.
 */

#include "mem_watch.h"
#include "protect.h"
#include "npt.h"

static yghv_watch_page_t  g_watch[YGHV_WATCH_MAX_PAGES];
static volatile LONG      g_watch_count;

/* Event ring. Single producer (the island) appends; a PASSIVE reader drains.
 * Both indices are volatile LONGs and the writes are ordered so a reader
 * never sees an entry before its fields are filled. */
static yghv_watch_event_t g_ring[YGHV_WATCH_RING];
static volatile LONG      g_ring_head;    /* next write slot */
static volatile LONG      g_ring_tail;    /* next read slot  */

static volatile LONG64 g_watch_total;
static volatile LONG64 g_watch_recorded;
static volatile LONG64 g_watch_dropped;
static volatile LONG64 g_watch_seq;

static int yghv_watch_find(uint64_t gpa) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int n = (int)InterlockedCompareExchange(&g_watch_count, 0, 0);

    for (i = 0; i < n && i < YGHV_WATCH_MAX_PAGES; i++) {
        if (InterlockedCompareExchange(&g_watch[i].active, 0, 0) &&
            g_watch[i].gpa == page)
            return i;
    }
    return -1;
}

int yghv_watch_add(uint64_t gpa, uint32_t kinds) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot;
    int i;

    /*
     * ===== 9.314 DISABLED — this path froze the machine on 2026-10-17 =====
     *
     * Taking the page's NPT write permission away does raise #NPF, but the
     * handler has no branch for a watched page: yghv_protect_on_npf_write_bare
     * only knows pages registered in g_protect, so it returns YGHV_NPF_NONE,
     * the cpp injects #PF, the target's write fails, the target retries, and
     * the machine spins in #NPF forever. Measured: hard freeze at 21:06:56
     * with no dump and no bugcheck (nothing ever ran long enough to write one).
     *
     * The missing half is the ALLOW loop: on a watch hit the handler must
     * RECORD, RESTORE the write permission, set TF, and let the retried store
     * land — the same cycle armed pages already use (protect.c 1484-1557).
     * See docs/TIER4_WATCH_DESIGN.md for the complete design.
     *
     * Until that loop exists, add() refuses. Bookkeeping alone (registering
     * the page in g_watch without touching the NPT) would be harmless but
     * useless, and silently doing nothing is worse than saying no.
     */
    (void)page; (void)slot; (void)i;
    InterlockedIncrement64(&g_watch_dropped);
    return 0;
}

static int yghv_watch_add_unsafe_do_not_call(uint64_t gpa, uint32_t kinds) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot = yghv_watch_find(page);
    int i;

    if (!page || !kinds)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;                       /* PASSIVE-only (touches NPT) */

    if (slot >= 0) {
        g_watch[slot].kinds |= kinds;   /* widen an existing watch */
        return 1;
    }
    for (i = 0; i < YGHV_WATCH_MAX_PAGES; i++) {
        if (!InterlockedCompareExchange(&g_watch[i].active, 0, 0)) {
            /*
             * THE ACTUAL MECHANISM — not just bookkeeping.
             *
             * Registering a watch in this table alone does nothing: the page
             * keeps its normal NPT permission, so no #NPF is ever raised and
             * the handler never runs. The watch only exists once the page's
             * NPT entry stops permitting the access. That is what this call
             * does — and leaving it out is exactly why the first run of the
             * 4.6 test captured zero events.
             *
             * With WRITE watched: keep PRESENT, drop WRITABLE. A write then
             * faults (the handler sees it) while reads keep working, so the
             * target still runs normally.
             * Read-watch (no WRITE bit): remove PRESENT entirely. Every
             * access faults, which also makes the page unusable for the
             * target — so that is opt-in only.
             */
            if (!yghv_protect_npt_set_perm(page,
                    (kinds & YGHV_WATCH_WRITE)
                        ? NPT_PERM_PRESENT          /* write-fault, read OK */
                        : 0)) {                      /* no access at all */
                return 0;
            }
            g_watch[i].gpa   = page;
            g_watch[i].kinds = kinds;
            g_watch[i].hits  = 0;
            InterlockedExchange(&g_watch[i].active, 1);
            InterlockedIncrement(&g_watch_count);
            return 1;
        }
    }
    return 0;                           /* table full */
}

int yghv_watch_remove(uint64_t gpa) {
    int slot = yghv_watch_find(gpa);

    if (slot < 0)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;

    /* Give the page its normal access back BEFORE dropping the slot, so the
     * target is never left with a permanently faulting page. */
    (void)yghv_protect_npt_set_perm(g_watch[slot].gpa,
                                    NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    InterlockedExchange(&g_watch[slot].active, 0);
    InterlockedDecrement(&g_watch_count);
    return 1;
}

/*
 * Island-safe: scans a nonpaged array. A torn read can only cause one missed
 * or one extra event, never a crash.
 */
int yghv_watch_match(uint64_t gpa, uint32_t kind) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int n = (int)InterlockedCompareExchange(&g_watch_count, 0, 0);

    if (n <= 0)
        return 0;
    for (i = 0; i < n && i < YGHV_WATCH_MAX_PAGES; i++) {
        if (InterlockedCompareExchange(&g_watch[i].active, 0, 0) &&
            g_watch[i].gpa == page &&
            (g_watch[i].kinds & kind)) {
            InterlockedIncrement((volatile LONG *)&g_watch[i].hits);
            return 1;
        }
    }
    return 0;
}

/*
 * Island-safe record: fill a slot, then advance the head.
 *
 * Order matters — the fields are written BEFORE the head moves, so a PASSIVE
 * reader that sees the new head also sees complete fields. (x86 store order is
 * release-ordered for this pattern; no explicit barrier needed on this
 * single-producer ring.)
 */
void yghv_watch_record(uint64_t cr3, uint64_t rip, uint64_t gpa,
                       uint32_t kind, uint32_t cpl) {
    LONG head = InterlockedCompareExchange(&g_ring_head, 0, 0);
    LONG tail = InterlockedCompareExchange(&g_ring_tail, 0, 0);
    yghv_watch_event_t *e;
    LONG slot;

    if (((head + 1) & (YGHV_WATCH_RING - 1)) == (tail & (YGHV_WATCH_RING - 1))) {
        InterlockedIncrement64(&g_watch_dropped);
        return;                         /* full: drop, never block */
    }

    slot = head & (YGHV_WATCH_RING - 1);
    e = &g_ring[slot];
    e->cr3  = cr3;
    e->rip  = rip;
    e->gpa  = gpa & ~(uint64_t)0xFFFULL;
    e->kind = kind;
    e->cpl  = cpl;
    e->seq  = (uint64_t)InterlockedIncrement64(&g_watch_seq);

    InterlockedExchange(&g_ring_head, head + 1);
    InterlockedIncrement64(&g_watch_recorded);
    InterlockedIncrement64(&g_watch_total);
}

uint32_t yghv_watch_drain(yghv_watch_event_t *out, uint32_t max) {
    uint32_t n = 0;
    LONG tail, head;

    if (!out || !max)
        return 0;

    for (;;) {
        tail = InterlockedCompareExchange(&g_ring_tail, 0, 0);
        head = InterlockedCompareExchange(&g_ring_head, 0, 0);
        if (tail == head || n >= max)
            break;

        out[n++] = g_ring[tail & (YGHV_WATCH_RING - 1)];
        InterlockedExchange(&g_ring_tail, tail + 1);
    }
    return n;
}

void yghv_watch_stats_get(yghv_watch_stats_t *out) {
    if (!out)
        return;
    out->pages    = (uint64_t)InterlockedCompareExchange(&g_watch_count, 0, 0);
    out->total_hits = (uint64_t)InterlockedCompareExchange64(&g_watch_total, 0, 0);
    out->recorded = (uint64_t)InterlockedCompareExchange64(&g_watch_recorded, 0, 0);
    out->dropped  = (uint64_t)InterlockedCompareExchange64(&g_watch_dropped, 0, 0);
    out->seq      = (uint64_t)InterlockedCompareExchange64(&g_watch_seq, 0, 0);
}

void yghv_watch_stats_reset(void) {
    InterlockedExchange64(&g_watch_total, 0);
    InterlockedExchange64(&g_watch_recorded, 0);
    InterlockedExchange64(&g_watch_dropped, 0);
    InterlockedExchange64(&g_watch_seq, 0);
    InterlockedExchange(&g_ring_head, 0);
    InterlockedExchange(&g_ring_tail, 0);
}
