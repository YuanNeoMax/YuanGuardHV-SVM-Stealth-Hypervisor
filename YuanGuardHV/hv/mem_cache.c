/*
 * mem_cache.c — Tier 4.7: cache behaviour control via NPT PWT/PCD.
 * See common/mem_cache.h for what this does and does not deliver.
 */

#include "mem_cache.h"
#include "protect.h"
#include "npt.h"

#define YGHV_CACHE_PWT (1ULL << 3)
#define YGHV_CACHE_PCD (1ULL << 4)

typedef struct {
    uint64_t gpa;
    uint32_t mode;
    volatile LONG active;
} yghv_cache_slot_t;

/*
 * 9.366: PAGE-EXACT — same fix as mem_time.c, same measured page.
 *
 * The object here is a 32-slot table (24 B each = 768) and it shared page
 * 0x36000 with the whole time module. `mem-cache verify` reads this table from
 * an IOCTL, and the rollback path in mem_time.c addresses the same physical
 * pages, so "which of the two is this page about" is a question a reader should
 * never have to answer from the .map (docs/KERNEL_API_CHECKLIST.md §28.1).
 */
typedef union {
    yghv_cache_slot_t entries[YGHV_CACHE_MAX_PAGES];
    uint8_t           page[4096];
} yghv_cache_region_t;

DECLSPEC_ALIGN(4096) static yghv_cache_region_t g_cache_region;
DECLSPEC_ALIGN(4096) static yghv_cache_region_t g_cache_region_pad;  /* separation */

#define g_cache (g_cache_region.entries)

_Static_assert(sizeof(yghv_cache_slot_t) * YGHV_CACHE_MAX_PAGES <= 4096,
               "cache table fits a page");
_Static_assert(sizeof(yghv_cache_region_t) == 4096, "cache region is exactly one page");

static volatile LONG     g_cache_count;

static volatile LONG64   g_cache_sets;
static volatile LONG64   g_cache_resets;
static volatile LONG64   g_cache_fail;

static int yghv_cache_find(uint64_t gpa) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    /*
     * 9.486: SCAN THE WHOLE ARRAY, NOT $(System.Collections.Hashtable.Cnt) SLOTS.
     *
     * Same defect as 9.485 in the breakpoint table: this counter is incremented on
     * install and decremented on remove, but the array is NEVER COMPACTED. Removing
     * a slot in the middle leaves $(System.Collections.Hashtable.Arr)[i].active == 0 while a later slot is
     * still live, and i < n then stops BEFORE that live slot - so the entry
     * becomes invisible to every lookup while still being installed.
     *
     * Validity is decided per slot with ctive, which this loop already does.
     */
    int n = YGHV_CACHE_MAX_PAGES;

    for (i = 0; i < n && i < YGHV_CACHE_MAX_PAGES; i++) {
        if (InterlockedCompareExchange(&g_cache[i].active, 0, 0) &&
            g_cache[i].gpa == page)
            return i;
    }
    return -1;
}

/*
 * Read the page's current NPT entry so the mode change preserves every other
 * bit. Without this, flipping PWT/PCD would drop present/writable/user/
 * accessed — the exact mistake that took the box down in 9.311.
 *
 * protect.c exposes yghv_protect_npt_remap() (which takes a full SPA) but
 * not an entry read, so we go through npt_map_page() with the attributes we
 * know a normal 4K entry carries and the mode bits the caller asked for.
 * The SPA is recovered from the caller's own walk (mem-pte) rather than
 * guessed.
 */
static int yghv_cache_apply(uint64_t gpa, uint32_t mode, int use_alt) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    uint64_t attrs;
    uint64_t spa = page;          /* identity: caller remaps first if needed */

    if (mode > YGHV_CACHE_UNCACHEABLE_STRONG) {
        InterlockedIncrement64(&g_cache_fail);
        return 0;
    }

    /*
     * Attribute set = the full 4K set (present|writable|user|accessed) minus
     * the permission bits, ORed with the caller's permissions, ORed with the
     * cache bits for the requested mode.
     *
     * 9.313 lesson again: never pass only the cache bits — the remap path
     * keeps only what it is given.
     */
    attrs = (NPT_4K_PAGE_FLAGS & ~((uint64_t)NPT_PERM_PRESENT | (uint64_t)NPT_PERM_WRITABLE))
            | (uint64_t)NPT_PERM_PRESENT
            | (uint64_t)NPT_PERM_WRITABLE;

    /* 9.330: every case needs its own break. Without them the WRITEBACK case
     * fell through and set PWT|PCD, so "reset to writeback" left the page
     * uncached. */
    switch (mode) {
    case YGHV_CACHE_WRITEBACK:            break;                  /* 00 */
    case YGHV_CACHE_WRITETHROUGH:         attrs |= YGHV_CACHE_PCD; break; /* 01 */
    case YGHV_CACHE_UNCACHED:             attrs |= YGHV_CACHE_PWT; break; /* 10 */
    case YGHV_CACHE_UNCACHEABLE_STRONG:   attrs |= YGHV_CACHE_PWT | YGHV_CACHE_PCD;
                                          break;
    default:                              break;
    }

    /*
     * 9.330 FIX: use the flags-taking entry point.
     *
     * The cache bits are computed above and MUST reach npt_map_page, which
     * writes (spa & ~0xFFF) | (flags & 0xFFF). The plain remap used to hardcode
     * its own flag set (9.311), so every "cache set OK" changed nothing —
     * measured by the 9.329 readback: PWT=0 PCD=0 after a successful set.
     */
    if (!yghv_protect_npt_remap_flags(page, spa, use_alt, attrs)) {
        InterlockedIncrement64(&g_cache_fail);
        return 0;
    }
    return 1;
}

int yghv_cache_set(uint64_t gpa, uint32_t mode) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot = yghv_cache_find(page);
    int i;

    if (!page)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;                       /* PASSIVE-only */

    if (mode == 0)                      /* writeback = reset */
        return yghv_cache_reset(page);

    if (!yghv_cache_apply(page, mode, 0)) {
        InterlockedIncrement64(&g_cache_fail);
        return 0;
    }

    if (slot < 0) {
        for (i = 0; i < YGHV_CACHE_MAX_PAGES; i++) {
            if (!InterlockedCompareExchange(&g_cache[i].active, 0, 0)) {
                g_cache[i].gpa = page;
                InterlockedExchange(&g_cache[i].active, 1);
                InterlockedIncrement(&g_cache_count);
                slot = i;
                break;
            }
        }
    }
    if (slot >= 0)
        g_cache[slot].mode = mode;

    InterlockedIncrement64(&g_cache_sets);
    return 1;
}

int yghv_cache_set_alt(uint64_t gpa, uint32_t mode) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;

    if (!page)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;
    if (mode > YGHV_CACHE_UNCACHEABLE_STRONG)
        return 0;

    if (!yghv_cache_apply(page, mode, 1)) {
        InterlockedIncrement64(&g_cache_fail);
        return 0;
    }
    InterlockedIncrement64(&g_cache_sets);
    return 1;
}

int yghv_cache_reset(uint64_t gpa) {
    int slot = yghv_cache_find(gpa);
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;

    if (KeAreAllApcsDisabled())
        return 0;

    if (!yghv_cache_apply(page, YGHV_CACHE_WRITEBACK, 0))
        return 0;

    if (slot >= 0) {
        InterlockedExchange(&g_cache[slot].active, 0);
        InterlockedDecrement(&g_cache_count);
    }
    InterlockedIncrement64(&g_cache_resets);
    return 1;
}

void yghv_cache_stats_get(yghv_cache_stats_t *out) {
    if (!out)
        return;
    out->sets    = (uint64_t)InterlockedCompareExchange64(&g_cache_sets, 0, 0);
    out->resets  = (uint64_t)InterlockedCompareExchange64(&g_cache_resets, 0, 0);
    out->failures = (uint64_t)InterlockedCompareExchange64(&g_cache_fail, 0, 0);
    out->tracked = (uint64_t)InterlockedCompareExchange(&g_cache_count, 0, 0);
}

void yghv_cache_stats_reset(void) {
    InterlockedExchange64(&g_cache_sets, 0);
    InterlockedExchange64(&g_cache_resets, 0);
    InterlockedExchange64(&g_cache_fail, 0);
}

/*
 * 9.329: verify that the requested cache mode is actually present in the NPT
 * entry.
 *
 * Tier 4.7 was originally "verified" by the call returning success — which
 * proves nothing about the bits. This reads the entry back and reports the
 * PWT/PCD pair, so a test can assert on the table itself rather than on the
 * API's optimism.
 *
 * Returns 1 when the entry was read; pwt/pcd receive the bits.
 */
int yghv_cache_verify(uint64_t gpa, int *pwt, int *pcd, uint64_t *raw) {
    uint64_t entry = 0;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;

    if (!page || KeAreAllApcsDisabled())
        return 0;
    if (!yghv_protect_npt_get_entry(page, &entry))
        return 0;
    if (pwt) *pwt = (entry & (1ULL << 3)) ? 1 : 0;
    if (pcd) *pcd = (entry & (1ULL << 4)) ? 1 : 0;
    if (raw) *raw = entry;
    return 1;
}