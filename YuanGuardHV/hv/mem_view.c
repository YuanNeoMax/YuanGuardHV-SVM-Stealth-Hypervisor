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

static yghv_view_page_t      g_view_pages[YGHV_VIEW_MAX_PAGES];
static uint64_t              g_view_observers[YGHV_VIEW_MAX_OBSERVERS];
static volatile LONG         g_view_page_count;
static volatile LONG         g_view_obs_count;
static volatile LONG         g_view_up;

static volatile LONG64 g_view_to_alt;
static volatile LONG64 g_view_to_main;
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
    int n = (int)InterlockedCompareExchange(&g_view_page_count, 0, 0);

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
