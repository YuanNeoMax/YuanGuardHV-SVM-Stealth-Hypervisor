/*
 * mem_watch.c — Tier 4.6: NPF-based watchpoints.
 * See common/mem_watch.h for why this beats DR0-DR3 and int3.
 */

#include "mem_watch.h"
#include "protect.h"
#include "mem_bp.h"
#include "npt.h"

/*
 * 9.366: PAGE-EXACT. These three objects were interleaved on ONE page by the
 * linker, together with mem_view's observer table, so the page carried the hot
 * state of two independent mechanisms and nothing said so.
 *
 * Measured on the .map before this change (page 0x34000, 12 symbols):
 *
 *     g_watch, g_watch_count, g_ring, g_ring_head, g_ring_tail,
 *     g_watch_dropped, g_view_page_count, g_view_obs_count, g_view_observers,
 *     g_view_installs, g_view_to_alt, g_view_to_main
 *
 * Every one of those is read or written INSIDE SvHandleVmExit: the watch tables
 * by yghv_watch_match/record, the view table by yghv_view_pick_ncr3, which runs
 * on every VMEXIT to choose the NCR3. So a single page held the hit path of both
 * mechanisms, and the same page would have been the hide target of either.
 *
 * The 15th crash is the general form of that: hiding a page removes it from the
 * driver too, so an object smaller than its page takes its neighbours with it.
 * DECLSPEC_ALIGN(4096) only aligns the START — the 384-byte watch table still
 * left 3712 bytes for the linker to fill. Making each object EXACTLY its pages
 * leaves nothing to fill (docs/KERNEL_API_CHECKLIST.md §28.1).
 */
typedef union {
    yghv_watch_page_t entries[YGHV_WATCH_MAX_PAGES];
    uint8_t           page[4096];
} yghv_watch_region_t;

/* The scalars the island touches on every fault, exactly one page. */
typedef struct {
    volatile LONG   count;
    volatile LONG   ring_head;    /* next write slot */
    volatile LONG   ring_tail;    /* next read slot  */
    volatile LONG64 total;
    volatile LONG64 recorded;
    volatile LONG64 dropped;
    volatile LONG64 seq;
} yghv_watch_hot_t;

typedef union {
    yghv_watch_hot_t s;
    uint8_t          page[4096];
} yghv_watch_hot_region_t;

/*
 * Event ring. Single producer (the island) appends; a PASSIVE reader drains.
 * Both indices are volatile LONGs and the writes are ordered so a reader
 * never sees an entry before its fields are filled.
 *
 * 256 * 48 = 12288 = exactly three pages, asserted below, so its tail leaves no
 * spare bytes for a cold scalar to land in.
 */
typedef union {
    yghv_watch_event_t entries[YGHV_WATCH_RING];
    uint8_t            page[sizeof(yghv_watch_event_t) * YGHV_WATCH_RING];
} yghv_watch_ring_region_t;

DECLSPEC_ALIGN(4096) static yghv_watch_region_t      g_watch_region;
DECLSPEC_ALIGN(4096) static yghv_watch_hot_region_t  g_watch_hot;
DECLSPEC_ALIGN(4096) static yghv_watch_ring_region_t g_watch_ring;

/* Separation so the linker has no neighbour to place flush against the region.
 * The exact-size assertion already forbids a remainder; these make the layout
 * independent of allocation order as well. */
DECLSPEC_ALIGN(4096) static yghv_watch_region_t      g_watch_pad;
DECLSPEC_ALIGN(4096) static yghv_watch_hot_region_t  g_watch_hot_pad;
DECLSPEC_ALIGN(4096) static yghv_watch_ring_region_t g_watch_ring_pad;

#define g_watch        (g_watch_region.entries)
#define g_watch_count  (g_watch_hot.s.count)
#define g_ring_head    (g_watch_hot.s.ring_head)
#define g_ring_tail    (g_watch_hot.s.ring_tail)
#define g_watch_total  (g_watch_hot.s.total)
#define g_watch_recorded (g_watch_hot.s.recorded)
#define g_watch_dropped  (g_watch_hot.s.dropped)
#define g_watch_seq      (g_watch_hot.s.seq)
#define g_ring         (g_watch_ring.entries)

/* Page-exactness is a BUILD property, not something to re-derive from the map
 * after a crash: an object that outgrows its page silently re-acquires
 * neighbours, which is precisely the condition this separation exists to end. */
_Static_assert(sizeof(yghv_watch_page_t) <= 4096, "watch page entry fits a page");
_Static_assert(sizeof(yghv_watch_region_t) == 4096, "g_watch is exactly one page");
_Static_assert(sizeof(yghv_watch_hot_t) <= 4096, "watch hot scalars fit a page");
_Static_assert(sizeof(yghv_watch_hot_region_t) == 4096, "watch hot region is exactly one page");
_Static_assert((sizeof(yghv_watch_event_t) * YGHV_WATCH_RING) % 4096 == 0,
               "watch ring is whole pages");
_Static_assert(sizeof(yghv_watch_ring_region_t) ==
               sizeof(yghv_watch_event_t) * YGHV_WATCH_RING,
               "watch ring region is exact");

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
    int rc;

    /*
     * ===== 9.328: RE-ENABLED, delegating to the breakpoint engine =====
     *
     * 9.313 disabled this path because it revoked the page's NPT write
     * permission and nothing ever put it back: the NPF handler had no branch
     * for a watch page, injected #PF, and the guest spun until the box froze.
     *
     * Tier 4.4 (9.316) then implemented the missing half — the ALLOW loop:
     * record, RESTORE the permission, single-step, re-arm on #DB — and 9.327
     * generalised the table to 64 entries with a mode per entry.
     *
     * A page-level watch IS a breakpoint with no instruction filter, i.e.
     * rip = 0. So this module is now a thin alias over yghv_bp_add(): the
     * mechanism, the loop, the ring and the stats are the verified ones, and
     * there is only one implementation to keep correct.
     */
    if (!page || !kinds) {
        InterlockedIncrement64(&g_watch_dropped);
        return 0;
    }

    rc = yghv_bp_add(page, 0 /* any instruction */, kinds);
    if (rc != 1) {
        InterlockedIncrement64(&g_watch_dropped);
        return 0;
    }

    /* Mirror the registration so watch-side stats stay meaningful. */
    if (yghv_watch_find(page) < 0) {
        int i;
        for (i = 0; i < YGHV_WATCH_MAX_PAGES; i++) {
            if (!InterlockedCompareExchange(&g_watch[i].active, 0, 0)) {
                g_watch[i].gpa = page;
                g_watch[i].kinds = kinds;
                g_watch[i].hits = 0;
                InterlockedExchange(&g_watch[i].active, 1);
                InterlockedIncrement(&g_watch_count);
                break;
            }
        }
    }
    return 1;
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

    /* 9.328: the real arming lives in the breakpoint table now. */
    (void)yghv_bp_remove(gpa & ~(uint64_t)0xFFFULL, 0);

    if (slot < 0)
        return 0;
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
