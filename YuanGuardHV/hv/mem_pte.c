/*
 * mem_pte.c — Phase 1: page-table walk (Engine B foundation).
 * See common/mem_pte.h for the contract and the reasoning.
 *
 * Design rules taken from docs/KERNEL_API_CHECKLIST.md:
 *   - No locks here. The walk is pure arithmetic + reads so it is callable
 *     from the island (DISPATCH_LEVEL, GIF=0) and from IOCTL context alike.
 *   - No __try/__except. This build uses /EHs-c-, which generates NO handler
 *     at all (section 4 of the checklist) — the guard would be decorative.
 *     Every table read is validated by hand instead.
 *   - MmGetVirtualForPhysical is used ONLY for page-table pages (system
 *     pages). Section 3: it does not map user pages.
 */

#include "mem_pte.h"

static const uint64_t YGHV_PTE_PA_MASK = YGHV_PTE_ADDR_MASK;

/* Lock-free counters; written from possibly-multiple cores, so use interlocked. */
static volatile LONG64 g_pte_walks;
static volatile LONG64 g_pte_4k;
static volatile LONG64 g_pte_2m;
static volatile LONG64 g_pte_1g;
static volatile LONG64 g_pte_np;
static volatile LONG64 g_pte_badcr3;
static volatile LONG64 g_pte_tablereads;

/*
 * Read one 8-byte entry out of a page table.
 *
 * Uses MmCopyMemory(PHYSICAL) rather than MmGetVirtualForPhysical.
 * That is the 9.299 FIX-2 lesson applied to the walk itself: the VA-based
 * lookup only works for physical pages the SYSTEM already direct-maps, and
 * a user process's page-table pages are NOT guaranteed to be among them.
 * It returns a pointer for some of them and NULL for others, which is how
 * a walk can "succeed" at PML4 and then silently read garbage further down
 * (observed: explorer's module base reported NOT MAPPED while
 * MmCopyVirtualMemory read the very same VA fine).
 *
 * MmCopyMemory takes the physical address directly, needs no mapping, and
 * reports failure through its return status — so there is no bogus pointer
 * to dereference. It is safe at DISPATCH_LEVEL and takes no locks.
 */
uint64_t yghv_pte_read_entry(uint64_t table_pa, uint64_t index) {
    uint64_t e = 0;
    SIZE_T done = 0;
    MM_COPY_ADDRESS src;

    if (!table_pa || index > 511)
        return 0;

    src.PhysicalAddress.QuadPart = (LONGLONG)(table_pa + index * 8);
    if (!NT_SUCCESS(MmCopyMemory(&e, src, sizeof(e), MM_COPY_MEMORY_PHYSICAL, &done)))
        return 0;
    if (done != sizeof(e))
        return 0;

    InterlockedIncrement64(&g_pte_tablereads);
    return e;
}

uint64_t yghv_pte_walk(uint64_t cr3, uint64_t va, yghv_pte_info_t *out) {
    uint64_t pml4_pa, pdpt_pa, pd_pa, pt_pa;
    uint64_t pml4e, pdpte, pde, pte;
    yghv_pte_info_t local;
    yghv_pte_info_t *o = out ? out : &local;

    RtlZeroMemory(o, sizeof(*o));
    InterlockedIncrement64(&g_pte_walks);

    /* CR3 holds the PML4 physical address (low 12 bits are flags/PCID).
     * 9.249: mask off PCID so a switched CR3 still matches. */
    pml4_pa = cr3 & YGHV_PTE_PA_MASK;
    if (!pml4_pa) {
        InterlockedIncrement64(&g_pte_badcr3);
        return 0;
    }

    /* --- level 4: PML4 ------------------------------------------------- */
    pml4e = yghv_pte_read_entry(pml4_pa, YGHV_PTE_INDEX_PML4(va));
    if (!(pml4e & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pte_np);
        return 0;
    }
    pdpt_pa = pml4e & YGHV_PTE_PA_MASK;

    /* --- level 3: PDPT (may hold a 1GB page) --------------------------- */
    pdpte = yghv_pte_read_entry(pdpt_pa, YGHV_PTE_INDEX_PDPT(va));
    if (!(pdpte & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pte_np);
        return 0;
    }
    if (pdpte & YGHV_PTE_LARGE) {
        /* 1GB page: address is bits 30..51, offset is bits 0..29 */
        o->pa        = (pdpte & YGHV_PTE_ADDR_MASK_1G) | (va & (YGHV_PTE_SIZE_1G - 1));
        o->page_size = YGHV_PTE_SIZE_1G;
        o->kind      = YGHV_PTE_KIND_1G;
        o->entry     = pdpte;
        o->levels    = 3;
        InterlockedIncrement64(&g_pte_1g);
        goto fill_attrs;
    }
    pd_pa = pdpte & YGHV_PTE_PA_MASK;

    /* --- level 2: PD (may hold a 2MB page) ----------------------------- */
    pde = yghv_pte_read_entry(pd_pa, YGHV_PTE_INDEX_PD(va));
    if (!(pde & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pte_np);
        return 0;
    }
    if (pde & YGHV_PTE_LARGE) {
        /* 2MB page: address is bits 21..51, offset is bits 0..20 */
        o->pa        = (pde & YGHV_PTE_ADDR_MASK_2M) | (va & (YGHV_PTE_SIZE_2M - 1));
        o->page_size = YGHV_PTE_SIZE_2M;
        o->kind      = YGHV_PTE_KIND_2M;
        o->entry     = pde;
        o->levels    = 3;
        InterlockedIncrement64(&g_pte_2m);
        goto fill_attrs;
    }
    pt_pa = pde & YGHV_PTE_PA_MASK;

    /* --- level 1: PT (4KB page) ---------------------------------------- */
    pte = yghv_pte_read_entry(pt_pa, YGHV_PTE_INDEX_PT(va));
    if (!(pte & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pte_np);
        return 0;
    }
    o->pa        = (pte & YGHV_PTE_PA_MASK) | (va & (YGHV_PTE_SIZE_4K - 1));
    o->page_size = YGHV_PTE_SIZE_4K;
    o->kind      = YGHV_PTE_KIND_4K;
    o->entry     = pte;
    o->levels    = 4;
    InterlockedIncrement64(&g_pte_4k);

fill_attrs:
    o->page_base = o->pa & ~(o->page_size - 1);
    o->present   = (uint8_t)((o->entry & YGHV_PTE_PRESENT) ? 1 : 0);
    o->writable  = (uint8_t)((o->entry & YGHV_PTE_WRITABLE) ? 1 : 0);
    o->user      = (uint8_t)((o->entry & YGHV_PTE_USER) ? 1 : 0);
    o->accessed  = (uint8_t)((o->entry & YGHV_PTE_ACCESSED) ? 1 : 0);
    o->dirty     = (uint8_t)((o->entry & YGHV_PTE_DIRTY) ? 1 : 0);
    o->nx        = (uint8_t)((o->entry & YGHV_PTE_NX) ? 1 : 0);
    o->global    = (uint8_t)((o->entry & YGHV_PTE_GLOBAL) ? 1 : 0);
    return o->pa;
}

uint64_t yghv_pte_va_to_pa(uint64_t cr3, uint64_t va) {
    return yghv_pte_walk(cr3, va, NULL);
}

int yghv_pte_query(uint64_t cr3, uint64_t va, yghv_pte_info_t *out) {
    if (!out)
        return 0;
    return yghv_pte_walk(cr3, va, out) != 0;
}

void yghv_pte_stats_get(yghv_pte_stats_t *out) {
    if (!out)
        return;
    out->walks       = (uint64_t)InterlockedCompareExchange64(&g_pte_walks, 0, 0);
    out->hit_4k      = (uint64_t)InterlockedCompareExchange64(&g_pte_4k, 0, 0);
    out->hit_2m      = (uint64_t)InterlockedCompareExchange64(&g_pte_2m, 0, 0);
    out->hit_1g      = (uint64_t)InterlockedCompareExchange64(&g_pte_1g, 0, 0);
    out->not_present = (uint64_t)InterlockedCompareExchange64(&g_pte_np, 0, 0);
    out->bad_cr3     = (uint64_t)InterlockedCompareExchange64(&g_pte_badcr3, 0, 0);
    out->table_reads = (uint64_t)InterlockedCompareExchange64(&g_pte_tablereads, 0, 0);
}

void yghv_pte_stats_reset(void) {
    InterlockedExchange64(&g_pte_walks, 0);
    InterlockedExchange64(&g_pte_4k, 0);
    InterlockedExchange64(&g_pte_2m, 0);
    InterlockedExchange64(&g_pte_1g, 0);
    InterlockedExchange64(&g_pte_np, 0);
    InterlockedExchange64(&g_pte_badcr3, 0);
    InterlockedExchange64(&g_pte_tablereads, 0);
}

/*
 * Enumerate mapped pages in [start_va, end_va).
 *
 * Walks level by level and skips whole subtrees that are absent, so an
 * unmapped 1GB region costs 1 entry read instead of 262144 walks. Large
 * pages are reported once with their true size — the caller then reads one
 * big block instead of 512 small ones.
 *
 * Callback returns non-zero to stop early.
 */
uint64_t yghv_pte_enum_range(uint64_t cr3, uint64_t start_va, uint64_t end_va,
                             yghv_pte_enum_cb_t cb, void *cookie) {
    uint64_t pml4_pa, pdpt_pa, pd_pa, pt_pa;
    uint64_t i4, i3, i2, i1;
    uint64_t i4_end, i3_start, i3_end, i2_start, i2_end;
    uint64_t visited = 0;
    uint64_t va;

    if (!cb || !cr3 || start_va >= end_va)
        return 0;

    pml4_pa = cr3 & YGHV_PTE_PA_MASK;
    if (!pml4_pa)
        return 0;

    /* Iterate the four levels by index, so we can skip absent subtrees.
     *
     * Every level must be bounded at BOTH ends by the requested range, not
     * just started at the range's index. Starting each inner loop at 0 and
     * running to 512 makes the cost depend on the range's *position* rather
     * than its size: a 16-page scan starting at 0x1ED498B0000 walked
     * 388 * 512 * 512 entries (~100M table reads) and wedged the driver in
     * STOP_PENDING. Each loop now stops at the index its end_va lands on. */
    i4_end = YGHV_PTE_INDEX_PML4(end_va - 1);
    for (i4 = YGHV_PTE_INDEX_PML4(start_va); i4 <= i4_end; i4++) {
        uint64_t pml4e = yghv_pte_read_entry(pml4_pa, i4);
        if (!(pml4e & YGHV_PTE_PRESENT))
            continue;
        pdpt_pa = pml4e & YGHV_PTE_PA_MASK;
        if (!pdpt_pa)
            continue;

        i3_start = (i4 == YGHV_PTE_INDEX_PML4(start_va))
                       ? YGHV_PTE_INDEX_PDPT(start_va) : 0;
        i3_end   = (i4 == i4_end) ? YGHV_PTE_INDEX_PDPT(end_va - 1) : 511;
        for (i3 = i3_start; i3 <= i3_end; i3++) {
            uint64_t pdpte = yghv_pte_read_entry(pdpt_pa, i3);
            if (!(pdpte & YGHV_PTE_PRESENT))
                continue;

            if (pdpte & YGHV_PTE_LARGE) {
                /* 1GB page */
                va = (i4 << 39) | (i3 << 30);
                if (va >= start_va && va < end_va) {
                    yghv_pte_info_t info;
                    RtlZeroMemory(&info, sizeof(info));
                    info.pa        = (pdpte & YGHV_PTE_ADDR_MASK_1G) | (va & (YGHV_PTE_SIZE_1G - 1));
                    info.page_size = YGHV_PTE_SIZE_1G;
                    info.kind      = YGHV_PTE_KIND_1G;
                    info.entry     = pdpte;
                    info.levels    = 3;
                    info.page_base = info.pa & ~(YGHV_PTE_SIZE_1G - 1);
                    info.present   = 1;
                    info.writable  = (uint8_t)((pdpte & YGHV_PTE_WRITABLE) ? 1 : 0);
                    info.user      = (uint8_t)((pdpte & YGHV_PTE_USER) ? 1 : 0);
                    info.nx        = (uint8_t)((pdpte & YGHV_PTE_NX) ? 1 : 0);
                    InterlockedIncrement64(&g_pte_1g);
                    visited++;
                    if (cb(va, &info, cookie))
                        return visited;
                }
                continue;
            }

            pd_pa = pdpte & YGHV_PTE_PA_MASK;
            if (!pd_pa)
                continue;

            i2_start = (i4 == YGHV_PTE_INDEX_PML4(start_va) &&
                        i3 == YGHV_PTE_INDEX_PDPT(start_va))
                           ? YGHV_PTE_INDEX_PD(start_va) : 0;
            i2_end   = (i4 == i4_end && i3 == i3_end)
                           ? YGHV_PTE_INDEX_PD(end_va - 1) : 511;
            for (i2 = i2_start; i2 <= i2_end; i2++) {
                uint64_t pde = yghv_pte_read_entry(pd_pa, i2);
                if (!(pde & YGHV_PTE_PRESENT))
                    continue;

                if (pde & YGHV_PTE_LARGE) {
                    /* 2MB page */
                    va = (i4 << 39) | (i3 << 30) | (i2 << 21);
                    if (va >= start_va && va < end_va) {
                        yghv_pte_info_t info;
                        RtlZeroMemory(&info, sizeof(info));
                        info.pa        = (pde & YGHV_PTE_ADDR_MASK_2M) | (va & (YGHV_PTE_SIZE_2M - 1));
                        info.page_size = YGHV_PTE_SIZE_2M;
                        info.kind      = YGHV_PTE_KIND_2M;
                        info.entry     = pde;
                        info.levels    = 3;
                        info.page_base = info.pa & ~(YGHV_PTE_SIZE_2M - 1);
                        info.present   = 1;
                        info.writable  = (uint8_t)((pde & YGHV_PTE_WRITABLE) ? 1 : 0);
                        info.user      = (uint8_t)((pde & YGHV_PTE_USER) ? 1 : 0);
                        info.nx        = (uint8_t)((pde & YGHV_PTE_NX) ? 1 : 0);
                        InterlockedIncrement64(&g_pte_2m);
                        visited++;
                        if (cb(va, &info, cookie))
                            return visited;
                    }
                    continue;
                }

                pt_pa = pde & YGHV_PTE_PA_MASK;
                if (!pt_pa)
                    continue;

                for (i1 = 0; i1 <= 511; i1++) {
                    uint64_t pte = yghv_pte_read_entry(pt_pa, i1);
                    if (!(pte & YGHV_PTE_PRESENT))
                        continue;

                    va = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    if (va < start_va || va >= end_va)
                        continue;

                    {
                        yghv_pte_info_t info;
                        RtlZeroMemory(&info, sizeof(info));
                        info.pa        = (pte & YGHV_PTE_PA_MASK) | (va & (YGHV_PTE_SIZE_4K - 1));
                        info.page_size = YGHV_PTE_SIZE_4K;
                        info.kind      = YGHV_PTE_KIND_4K;
                        info.entry     = pte;
                        info.levels    = 4;
                        info.page_base = info.pa & ~(YGHV_PTE_SIZE_4K - 1);
                        info.present   = 1;
                        info.writable  = (uint8_t)((pte & YGHV_PTE_WRITABLE) ? 1 : 0);
                        info.user      = (uint8_t)((pte & YGHV_PTE_USER) ? 1 : 0);
                        info.accessed  = (uint8_t)((pte & YGHV_PTE_ACCESSED) ? 1 : 0);
                        info.dirty     = (uint8_t)((pte & YGHV_PTE_DIRTY) ? 1 : 0);
                        info.nx        = (uint8_t)((pte & YGHV_PTE_NX) ? 1 : 0);
                        info.global    = (uint8_t)((pte & YGHV_PTE_GLOBAL) ? 1 : 0);
                        InterlockedIncrement64(&g_pte_4k);
                        visited++;
                        if (cb(va, &info, cookie))
                            return visited;
                    }
                }
            }
        }
    }
    return visited;
}
