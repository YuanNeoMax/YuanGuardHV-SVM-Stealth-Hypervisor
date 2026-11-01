/*
 * mem_view.c — Tier 4.1 split-view + 4.3 cross-process aliasing.
 * See common/mem_view.h for the design and the honest limits.
 *
 * The one primitive that matters: rewrite an NPT entry so a GPA resolves to a
 * different SPA. 9.311 measured that the guest DOES observe that (it did not
 * for permission flips — different mechanism).
 */

#include "mem_view.h"
#include "protect.h"

/*
 * 9.344: put this table on a page of its OWN — see mem_bp.c for the full note.
 *
 * The 15th crash was hiding per-PAGE when the object is smaller than the page:
 * g_engine_sel shares 0x2F000 with g_view_pages (62 symbols on that one page),
 * so hiding the view table removed a counter yghv_mem_read touches every call.
 */
/*
 * 9.365: PAGE-EXACT, not merely page-aligned.
 *
 * DECLSPEC_ALIGN(4096) aligns the START without saying anything about the SIZE.
 * Measured on the .map before this change — 13 symbols on g_view_pages' page:
 *
 *     g_view_pages, g_view_page_count, g_view_obs_count, g_view_observers,
 *     g_view_installs, g_view_to_alt, g_view_to_main, g_watch_dropped,
 *     g_watch, g_watch_count, g_ring_head, g_ring_tail, g_ring
 *
 * hidecore hides g_view_pages' page. With those neighbours on it, hiding the
 * TABLE also removes the bookkeeping that says whether the side view is installed
 * (g_view_installs) and how many switches happened (g_view_to_alt/_to_main) —
 * state the fault path and the IOCTL reporting both read.
 *
 * This is the SAME fault the comment above already describes for the 15th crash,
 * one layer up: the crash was per-PAGE hiding when the object is smaller than the
 * page, and this is the same confusion surviving inside the very table that crash
 * was about. Making the object EXACTLY one page removes the remainder the linker
 * was filling.
 */
typedef union {
    yghv_view_page_t entries[YGHV_VIEW_MAX_PAGES];
    uint8_t          page[4096];
} yghv_view_region_t;

DECLSPEC_ALIGN(4096) static yghv_view_region_t g_view_region;
DECLSPEC_ALIGN(4096) static yghv_view_region_t g_view_region_pad;  /* separation */

#define g_view_pages (g_view_region.entries)

typedef char yghv_assert_g_view_pages_fits_page[(sizeof(g_view_pages) <= 4096) ? 1 : -1];
typedef char yghv_assert_g_view_pages_is_one_page[(sizeof(yghv_view_region_t) == 4096) ? 1 : -1];

/*
 * 9.366: THE ISLAND-READ STATE, page-exact.
 *
 * yghv_view_pick_ncr3() runs on EVERY #VMEXIT to choose the NCR3, and it reads
 * g_view_up, g_view_page_count, g_view_observers and writes g_view_to_alt /
 * g_view_to_main. Before this change all of that shared page 0x34000 with
 * mem_watch's hot state (g_watch, g_ring, g_ring_head, g_ring_tail,
 * g_watch_dropped) — 12 symbols, two mechanisms, one page:
 *
 *     g_watch, g_watch_count, g_ring, g_ring_head, g_ring_tail,
 *     g_watch_dropped, g_view_page_count, g_view_obs_count, g_view_observers,
 *     g_view_installs, g_view_to_alt, g_view_to_main
 *
 * The table above (g_view_region) was already made page-exact in 9.365 and this
 * is the same fix one layer down: page-exact for the REGION did not make the
 * module's hot READ state page-exact, so the linker was free to interleave it
 * with the watch module's. Hiding the view table's page would have taken the
 * watch hit ring with it — the 15th crash's condition, relocated.
 */
typedef struct {
    volatile LONG   page_count;
    volatile LONG   obs_count;
    volatile LONG   up;
    volatile LONG   _pad;
    volatile LONG64 to_alt;
    volatile LONG64 to_main;
    uint64_t        observers[YGHV_VIEW_MAX_OBSERVERS];
} yghv_view_hot_t;

typedef union {
    yghv_view_hot_t s;
    uint8_t         page[4096];
} yghv_view_hot_region_t;

DECLSPEC_ALIGN(4096) static yghv_view_hot_region_t g_view_hot;
DECLSPEC_ALIGN(4096) static yghv_view_hot_region_t g_view_hot_pad;  /* separation */

#define g_view_page_count (g_view_hot.s.page_count)
#define g_view_obs_count  (g_view_hot.s.obs_count)
#define g_view_up         (g_view_hot.s.up)
#define g_view_to_alt     (g_view_hot.s.to_alt)
#define g_view_to_main    (g_view_hot.s.to_main)
#define g_view_observers  (g_view_hot.s.observers)

_Static_assert(sizeof(yghv_view_hot_t) <= 4096, "view hot state fits a page");
_Static_assert(sizeof(yghv_view_hot_region_t) == 4096, "view hot region is exactly one page");

/* Cold: only the IOCTL reporting path reads these, never the island. */
static volatile LONG64 g_view_installs;
static volatile LONG64 g_view_fail;

int yghv_view_ready(void) {
    return g_view_up && yghv_protect_fake_alt_pa() != 0;
}

uint64_t yghv_view_main_pa(void) {
    return yghv_protect_fake_main_pa();
}

uint64_t yghv_view_alt_pa(void) {
    return yghv_protect_fake_alt_pa();
}

int yghv_view_init(void) {
    if (yghv_view_ready())
        return 1;

    /* The alt table already exists (built for the C6 fake-write path) and is
     * an identity clone of the main one. Reuse it rather than building a
     * third table: fewer moving parts, and its lifetime is already managed. */
    if (!yghv_protect_fake_alt_pa()) {
        InterlockedIncrement64(&g_view_fail);
        return 0;
    }

    InterlockedExchange(&g_view_up, 1);
    return 1;
}

void yghv_view_stop(void) {
    int i;

    if (!InterlockedExchange(&g_view_up, 0))
        return;

    /* Restore identity for every split page before dropping the registrations,
     * so a still-running guest never sees a half-removed view. */
    for (i = 0; i < YGHV_VIEW_MAX_PAGES; i++) {
        if (InterlockedCompareExchange(&g_view_pages[i].active, 0, 0))
            (void)yghv_view_remove(g_view_pages[i].gpa);
    }
    InterlockedExchange(&g_view_page_count, 0);
    InterlockedExchange(&g_view_obs_count, 0);
    RtlZeroMemory(g_view_observers, sizeof(g_view_observers));
}

/* Is `gpa` already split? Returns the slot index or -1. */
static int yghv_view_find(uint64_t gpa) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    /*
     * 9.486: SCAN THE WHOLE ARRAY, NOT the LIVE COUNT SLOTS.
     *
     * Same defect as 9.485 in the breakpoint table: this counter is incremented on
     * install and decremented on remove, but the array is NEVER COMPACTED. Removing
     * a slot in the middle leaves table[i].active == 0 while a later slot is
     * still live, and i < n then stops BEFORE that live slot - so the entry
     * becomes invisible to every lookup while still being installed.
     *
     * Validity is decided per slot with active, which this loop already does.
     */
    int n = YGHV_VIEW_MAX_PAGES;

    for (i = 0; i < n && i < YGHV_VIEW_MAX_PAGES; i++) {
        if (InterlockedCompareExchange(&g_view_pages[i].active, 0, 0) &&
            g_view_pages[i].gpa == page)
            return i;
    }
    return -1;
}

int yghv_view_add(uint64_t gpa, uint64_t real_spa, uint64_t decoy_spa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot;
    int i;

    if (!yghv_view_ready())
        return 0;
    if (!page || !decoy_spa) {
        InterlockedIncrement64(&g_view_fail);
        return 0;
    }
    if (KeAreAllApcsDisabled())
        return 0;                       /* PASSIVE-only: see 9.311 */

    /* Re-splitting an existing page: just update the decoy frame. */
    slot = yghv_view_find(page);
    if (slot >= 0) {
        g_view_pages[slot].decoy_spa = decoy_spa & ~(uint64_t)0xFFFULL;
        if (real_spa)
            g_view_pages[slot].real_spa = real_spa & ~(uint64_t)0xFFFULL;
        goto program;
    }

    for (i = 0; i < YGHV_VIEW_MAX_PAGES; i++) {
        if (!InterlockedCompareExchange(&g_view_pages[i].active, 0, 0)) {
            slot = i;
            goto claim;
        }
    }
    InterlockedIncrement64(&g_view_fail);
    return 0;                           /* table full */

claim:
    g_view_pages[slot].gpa       = page;
    g_view_pages[slot].real_spa  = real_spa & ~(uint64_t)0xFFFULL;
    g_view_pages[slot].decoy_spa = decoy_spa & ~(uint64_t)0xFFFULL;
    /* Publish AFTER the fields are filled — the island scans by count. */
    InterlockedExchange(&g_view_pages[slot].active, 1);
    InterlockedIncrement(&g_view_page_count);

program:
    /*
     * Program BOTH tables explicitly.
     *
     * MAIN keeps the real frame. ALT gets the decoy. We must not rely on the
     * alt still holding an identity mapping: the C6 fake-write path may have
     * repointed the same GPA, and 9.311 showed that a wrong flags argument
     * here is what crashes the box. Use the FULL attribute set
     * (NPT_4K_PAGE_FLAGS), never just the permission bits.
     */
    if (!yghv_protect_npt_remap(page, g_view_pages[slot].real_spa, 0, NULL)) {
        InterlockedIncrement64(&g_view_fail);
        return 0;
    }
    if (!yghv_protect_npt_remap(page, g_view_pages[slot].decoy_spa, 1, NULL)) {
        InterlockedIncrement64(&g_view_fail);
        return 0;
    }

    InterlockedIncrement64(&g_view_installs);
    return 1;
}

int yghv_view_remove(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot = yghv_view_find(page);

    if (slot < 0)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;

    /* Restore identity in the alt table; main was never changed away from
     * real, so only alt needs the reset. */
    (void)yghv_protect_npt_remap(page, page, 1, NULL);
    InterlockedExchange(&g_view_pages[slot].active, 0);
    InterlockedDecrement(&g_view_page_count);
    return 1;
}

int yghv_view_add_observer(uint64_t cr3) {
    int i;
    uint64_t pfn = cr3 & YGHV_PTE_ADDR_MASK;

    if (!yghv_view_ready() || !pfn)
        return 0;

    /* Idempotent: registering twice is a no-op, not a second slot. */
    for (i = 0; i < YGHV_VIEW_MAX_OBSERVERS; i++) {
        if (g_view_observers[i] == pfn)
            return 1;
    }
    for (i = 0; i < YGHV_VIEW_MAX_OBSERVERS; i++) {
        if (g_view_observers[i] == 0) {
            g_view_observers[i] = pfn;
            InterlockedIncrement(&g_view_obs_count);
            return 1;
        }
    }
    InterlockedIncrement64(&g_view_fail);
    return 0;                           /* table full */
}

int yghv_view_remove_observer(uint64_t cr3) {
    int i;
    uint64_t pfn = cr3 & YGHV_PTE_ADDR_MASK;

    for (i = 0; i < YGHV_VIEW_MAX_OBSERVERS; i++) {
        if (g_view_observers[i] == pfn) {
            g_view_observers[i] = 0;
            InterlockedDecrement(&g_view_obs_count);
            return 1;
        }
    }
    return 0;
}

/*
 * Pick the NPT base for the next run.
 *
 * Island-safe: no locks, no allocation, no calls — just a scan of a nonpaged
 * array. Mutations happen only at PASSIVE under the caller's discipline, and
 * a torn read here can only pick the wrong VIEW for one run, never corrupt
 * anything (the worst case is an observer seeing the real page once).
 */
uint64_t yghv_view_pick_ncr3(uint64_t guest_cr3) {
    uint64_t main_pa, alt_pa, pfn;
    int i;

    if (!g_view_up)
        return 0;                       /* caller keeps whatever it had */

    main_pa = yghv_protect_fake_main_pa();
    alt_pa  = yghv_protect_fake_alt_pa();
    if (!main_pa || !alt_pa)
        return 0;

    /* No split pages means the two tables are identical: stay on main so the
     * common case pays nothing. */
    if (InterlockedCompareExchange(&g_view_page_count, 0, 0) <= 0)
        return main_pa;

    pfn = guest_cr3 & YGHV_PTE_ADDR_MASK;
    for (i = 0; i < YGHV_VIEW_MAX_OBSERVERS; i++) {
        if (g_view_observers[i] && g_view_observers[i] == pfn) {
            InterlockedIncrement64(&g_view_to_alt);
            return alt_pa;
        }
    }

    InterlockedIncrement64(&g_view_to_main);
    return main_pa;
}

void yghv_view_stats_get(yghv_view_stats_t *out) {
    if (!out)
        return;
    out->pages     = (uint64_t)InterlockedCompareExchange(&g_view_page_count, 0, 0);
    out->observers = (uint64_t)InterlockedCompareExchange(&g_view_obs_count, 0, 0);
    out->switches_to_alt  = (uint64_t)InterlockedCompareExchange64(&g_view_to_alt, 0, 0);
    out->switches_to_main = (uint64_t)InterlockedCompareExchange64(&g_view_to_main, 0, 0);
    out->installs  = (uint64_t)InterlockedCompareExchange64(&g_view_installs, 0, 0);
    out->failures  = (uint64_t)InterlockedCompareExchange64(&g_view_fail, 0, 0);
}

void yghv_view_stats_reset(void) {
    InterlockedExchange64(&g_view_to_alt, 0);
    InterlockedExchange64(&g_view_to_main, 0);
    InterlockedExchange64(&g_view_installs, 0);
    InterlockedExchange64(&g_view_fail, 0);
}


/*
 * 9.336 (Tier 7.1): expose the table's address and size so the guard can hide
 * and hash it. This table is ordinary pool memory, guest-reachable, and every
 * newer feature depends on it — main.c's exclude pass predates its existence.
 */
uint64_t *yghv_view_table_va(uint32_t *bytes) {
    if (bytes)
        *bytes = (uint32_t)sizeof(g_view_pages);
    return (uint64_t *)(void *)g_view_pages;
}
