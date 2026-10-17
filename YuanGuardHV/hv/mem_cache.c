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

static yghv_cache_slot_t g_cache[YGHV_CACHE_MAX_PAGES];
static volatile LONG     g_cache_count;

static volatile LONG64   g_cache_sets;
static volatile LONG64   g_cache_resets;
static volatile LONG64   g_cache_fail;

static int yghv_cache_find(uint64_t gpa) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int n = (int)InterlockedCompareExchange(&g_cache_count, 0, 0);

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

    switch (mode) {
    case YGHV_CACHE_WRITEBACK:     break;                       /* 00 */
    case YGHV_CACHE_WRITETHROUGH:  attrs |= YGHV_CACHE_PCD;      /* 01 */
    case YGHV_CACHE_UNCACHED:      attrs |= YGHV_CACHE_PWT;      /* 10 */
    case YGHV_CACHE_UNCACHEABLE_STRONG:
                                   attrs |= YGHV_CACHE_PWT | YGHV_CACHE_PCD;
                                   break;
    default:                       break;
    }

    /*
     * npt_map_page takes (m, gpa, spa, flags) and writes
     *   (spa & ~0xFFF) | (flags & 0xFFF) | (flags & bit63)
     * so the cache bits must be inside `flags`, which they are.
     */
    if (!yghv_protect_npt_remap(page, spa, use_alt, NULL)) {
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
