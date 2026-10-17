/*
 * mem_time.c — Tier 4.2: O(1) snapshot / rollback via NPT remap.
 * See common/mem_time.h.
 */

#include "mem_time.h"
#include "mem_pa.h"
#include "mem_pte.h"
#include "protect.h"
#include "mem_island.h"

/*
 * CR3 read. Same inline-asm workaround as mem_selfmap.c: the WDK km libs for
 * this target do not export __readcr3, so a call would leave an unresolved
 * __imp___readcr3 at link time.
 */
static uint64_t yghv_time_readcr3(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static yghv_time_page_t g_time[YGHV_TIME_MAX_PAGES];
static volatile LONG    g_time_count;

static volatile LONG64  g_time_snapshots;
static volatile LONG64  g_time_switches;
static volatile LONG64  g_time_fail;
static volatile LONG64  g_time_seq;

static int yghv_time_find(uint64_t gpa) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int n = (int)InterlockedCompareExchange(&g_time_count, 0, 0);

    for (i = 0; i < n && i < YGHV_TIME_MAX_PAGES; i++) {
        if (InterlockedCompareExchange(&g_time[i].active, 0, 0) &&
            g_time[i].gpa == page)
            return i;
    }
    return -1;
}

/*
 * Allocate one version buffer and fill it from `src_pa`.
 *
 * Nonpaged pool: the frame must stay resident, because the NPT entry will
 * point at its physical address and nothing may move it. That is the same
 * property npt_alloc_table relies on for the tables themselves.
 */
static uint64_t yghv_time_alloc_version(uint64_t src_pa) {
    void *buf = ExAllocatePoolWithTag(NonPagedPool, HV_PAGE_SIZE, 'VTYG');
    uint64_t spa;

    if (!buf)
        return 0;

    spa = MmGetPhysicalAddress(buf).QuadPart;
    if (!spa) {
        ExFreePoolWithTag(buf, 'VTYG');
        return 0;
    }

    /* Seed from the live frame when one is given. */
    if (src_pa)
        (void)yghv_pa_read(src_pa, buf, HV_PAGE_SIZE);
    else
        RtlZeroMemory(buf, HV_PAGE_SIZE);

    return spa;
}

int yghv_time_track(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    yghv_pte_info_t pi;
    uint64_t cr3;
    int slot = yghv_time_find(page);
    int i;
    uint64_t spa;

    if (!page)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;                       /* PASSIVE-only */
    if (slot >= 0)
        return 1;                       /* already tracked */

    /*
     * 9.315 FIX: a GPA is already a PHYSICAL address — there is nothing to
     * translate. The first version called yghv_pte_walk(readcr3(), gpa),
     * which walks OUR page table looking for a VA equal to the GPA. That
     * almost never hits (a physical address is not a valid VA), so track()
     * always failed. It also used the CALLER's CR3, which is the wrong
     * address space entirely when the page belongs to a target process.
     *
     * The frame is the input, so no walk is needed at all.
     */
    RtlZeroMemory(&pi, sizeof(pi));
    pi.pa = page;
    cr3 = 0;
    (void)cr3;

    for (i = 0; i < YGHV_TIME_MAX_PAGES; i++) {
        if (InterlockedCompareExchange(&g_time[i].active, 0, 0))
            continue;

        spa = yghv_time_alloc_version(pi.pa);
        if (!spa) {
            InterlockedIncrement64(&g_time_fail);
            return 0;
        }
        g_time[i].gpa = page;
        g_time[i].ver[0].spa = spa;
        g_time[i].ver[0].taken_seq = (uint64_t)InterlockedIncrement64(&g_time_seq);
        g_time[i].count = 1;
        g_time[i].current = 0;
        InterlockedExchange(&g_time[i].active, 1);
        InterlockedIncrement(&g_time_count);

        /*
         * 9.316 FIX: point the NPT at version 0 NOW.
         *
         * The first version allocated the buffer and copied the frame into it
         * but never remapped, so the guest kept using the ORIGINAL frame.
         * Every later write therefore landed in the original, while `goto`
         * switched to a buffer that still held the stale copy — the rollback
         * appeared to work but the versions were meaningless (measured: after
         * goto 1 the page still read AAAA, because version 1 had been
         * snapshotted from a buffer nothing had ever written to).
         *
         * Tracking only means something once the live view IS version 0.
         */
        if (!yghv_protect_npt_remap(page, spa, 0, NULL)) {
            InterlockedExchange(&g_time[i].active, 0);
            InterlockedDecrement(&g_time_count);
            InterlockedIncrement64(&g_time_fail);
            return 0;
        }
        return 1;
    }

    InterlockedIncrement64(&g_time_fail);
    return 0;                           /* table full */
}

int yghv_time_untrack(uint64_t gpa) {
    int slot = yghv_time_find(gpa);
    uint32_t v;

    if (slot < 0)
        return 0;

    /* Put the original frame back before releasing the buffers, so the guest
     * is never left pointing at memory we are about to free. */
    (void)yghv_protect_npt_remap(g_time[slot].gpa, g_time[slot].ver[0].spa, 0, NULL);

    for (v = 0; v < g_time[slot].count && v < YGHV_TIME_MAX_VERSIONS; v++) {
        uint64_t spa = g_time[slot].ver[v].spa;
        if (spa) {
            /*
             * We only know the physical address, not the VA. Convert back via
             * the direct map (9.310 discovered and validated it). If that is
             * unavailable we deliberately LEAK rather than free the wrong
             * pointer — a wrong free corrupts the pool (9.307 class).
             */
            uint8_t *va = NULL;
            if (yghv_island_base()) {
                va = (uint8_t *)(ULONG_PTR)(yghv_island_base() + spa);
            }
            if (va)
                ExFreePoolWithTag(va, 'VTYG');
        }
    }
    RtlZeroMemory(&g_time[slot], sizeof(g_time[slot]));
    InterlockedDecrement(&g_time_count);
    return 1;
}

int yghv_time_snapshot(uint64_t gpa) {
    int slot = yghv_time_find(gpa);
    uint64_t spa;

    if (slot < 0)
        return -1;
    if (KeAreAllApcsDisabled())
        return -1;

    if (g_time[slot].count >= YGHV_TIME_MAX_VERSIONS) {
        InterlockedIncrement64(&g_time_fail);
        return -1;                      /* versions exhausted */
    }

    /* Copy the CURRENT live version, not the guest's view: the live version is
     * what our own writes went to, and that is what a rollback must return to. */
    spa = yghv_time_alloc_version(g_time[slot].ver[g_time[slot].current].spa);
    if (!spa) {
        InterlockedIncrement64(&g_time_fail);
        return -1;
    }

    g_time[slot].ver[g_time[slot].count].spa = spa;
    g_time[slot].ver[g_time[slot].count].taken_seq =
        (uint64_t)InterlockedIncrement64(&g_time_seq);
    g_time[slot].count++;
    InterlockedIncrement64(&g_time_snapshots);
    return (int)(g_time[slot].count - 1);
}

int yghv_time_goto(uint64_t gpa, uint32_t version) {
    int slot = yghv_time_find(gpa);

    if (slot < 0 || version >= g_time[slot].count)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;

    /* The whole point: one NPT store, no data movement. */
    if (!yghv_protect_npt_remap(g_time[slot].gpa, g_time[slot].ver[version].spa,
                                0, NULL)) {
        InterlockedIncrement64(&g_time_fail);
        return 0;
    }
    g_time[slot].current = version;
    InterlockedIncrement64(&g_time_switches);
    return 1;
}

int yghv_time_current(uint64_t gpa) {
    int slot = yghv_time_find(gpa);
    return slot < 0 ? -1 : (int)g_time[slot].current;
}

int yghv_time_stage(uint64_t gpa, uint32_t version, const void *src, SIZE_T len) {
    int slot = yghv_time_find(gpa);
    uint8_t *va;

    if (slot < 0 || version >= g_time[slot].count || !src || !len)
        return 0;
    if (!yghv_island_base())
        return 0;                       /* no way back to a VA */
    if (len > HV_PAGE_SIZE)
        len = HV_PAGE_SIZE;

    va = (uint8_t *)(ULONG_PTR)(yghv_island_base() + g_time[slot].ver[version].spa);
    RtlCopyMemory(va, src, len);
    return 1;
}

void yghv_time_stats_get(yghv_time_stats_t *out) {
    if (!out)
        return;
    out->pages     = (uint64_t)InterlockedCompareExchange(&g_time_count, 0, 0);
    out->snapshots = (uint64_t)InterlockedCompareExchange64(&g_time_snapshots, 0, 0);
    out->switches  = (uint64_t)InterlockedCompareExchange64(&g_time_switches, 0, 0);
    out->failures  = (uint64_t)InterlockedCompareExchange64(&g_time_fail, 0, 0);
}

void yghv_time_stats_reset(void) {
    InterlockedExchange64(&g_time_snapshots, 0);
    InterlockedExchange64(&g_time_switches, 0);
    InterlockedExchange64(&g_time_fail, 0);
}
