/*
 * mem_scan.c — Phase 4: cross-process signature scan (page-table driven).
 *
 * Why this is the payoff of Engine B: Engine A would have to call
 * MmCopyVirtualMemory once per candidate address. Here we walk the page
 * tables ONCE, skip whole absent subtrees, and read one page at a time.
 * A 2MB large page is reported once and covers 512 candidates, so the
 * number of reads is proportional to PAGES, not to addresses.
 *
 * Reads go through yghv_pa_read (MmCopyMemory) — the same primitive the
 * sync guard uses, verified in this project.
 */

#include "mem_engine.h"
#include "mem_pa.h"

#define YGHV_SCAN_PATTERN_MAX  64
#define YGHV_SCAN_PAGE_BUF     0x1000

typedef struct {
    const uint8_t *pat;
    const uint8_t *mask;        /* 0x00 = wildcard */
    uint32_t       pat_len;
    uint64_t       align;       /* result alignment, 0 = any */
    uint32_t       flags;       /* bit0 writable, bit1 exec, bit2 dirty-only */
    uint64_t      *out;         /* caller's result array */
    uint32_t       out_max;
    uint32_t       out_count;
    uint8_t        page[YGHV_SCAN_PAGE_BUF];
    uint64_t       pages_read;
    uint64_t       pages_skipped;
    uint64_t       bytes_scanned;
    uint64_t       pages_budget;   /* stop after this many reads (0 = unlimited) */
    /*
     * Phase 4b (2026-10-11): shared-page dedup.
     *
     * Several VAs can map the SAME physical frame — shared sections, DLL code
     * pages, KUSER_SHARED_DATA. Scanning the frame once per VA is pure waste.
     * Keyed on the frame address, so the second VA mapping it is skipped.
     *
     * Open-addressed set of frame addresses; NULL disables dedup (allocation
     * failed), in which case behaviour is exactly as before.
     */
    uint64_t      *seen;
    uint32_t       seen_mask;      /* size - 1 (power of two) */
    uint64_t       dedup_hits;     /* frames skipped because already scanned */
} yghv_scan_ctx_t;

/*
 * Mark a frame as scanned. Returns 1 if it was ALREADY there (caller should
 * skip), 0 if this is the first time (caller should scan).
 *
 * Linear probing over a power-of-two table. If the table is full we return 0
 * (scan it) rather than lying — a false "already scanned" would silently drop
 * a real result, which is much worse than doing redundant work.
 */
static int yghv_scan_dedup_seen(yghv_scan_ctx_t *c, uint64_t frame) {
    uint64_t h;
    uint32_t i;
    uint32_t start;

    if (!c->seen)
        return 0;                       /* dedup disabled */

    h = (frame >> 12) * 0x9E3779B97F4A7C15ULL;
    start = (uint32_t)(h & c->seen_mask);
    i = start;

    for (;;) {
        if (c->seen[i] == frame)
            return 1;                   /* already scanned */
        if (c->seen[i] == 0) {
            c->seen[i] = frame;         /* claim this slot */
            return 0;
        }
        i = (i + 1) & c->seen_mask;
        if (i == start)
            return 0;                   /* wrapped: table full, scan it */
    }
}

/*
 * Default per-call read budget.
 *
 * The walk itself is cheap (it skips absent subtrees), but each *mapped* page
 * costs an MmCopyMemory. A full user-space scan of a real process can touch
 * hundreds of thousands of pages, which at ~1us each is minutes — long enough
 * to look like a hang from an IOCTL caller. The budget bounds one call; the
 * caller narrows the range or repeats with a new start.
 */
#define YGHV_SCAN_DEFAULT_PAGE_BUDGET 8192

static int yghv_scan_match(const uint8_t *p, const yghv_scan_ctx_t *c) {
    uint32_t i;
    for (i = 0; i < c->pat_len; i++) {
        if (c->mask[i] == 0x00)
            continue;                       /* wildcard byte */
        if (p[i] != c->pat[i])
            return 0;
    }
    return 1;
}

/* Called once per mapped page (large pages come with their real size). */
static int yghv_scan_page_cb(uint64_t va, const yghv_pte_info_t *info, void *cookie) {
    yghv_scan_ctx_t *c = (yghv_scan_ctx_t *)cookie;
    uint64_t done = 0;
    uint64_t pos;
    uint32_t size;

    if (c->out_count >= c->out_max)
        return 1;                           /* result array full: stop */
    if (c->pages_budget && c->pages_read >= c->pages_budget)
        return 1;                           /* budget exhausted: stop */

    /* Attribute filter.
     * bit2 = dirty-only: scan just the frames that have been written since
     * their PTE was last cleared. That is the page-table-level pre-filter —
     * a PTE already tells us whether the frame was touched, so we never read
     * a frame we know is stale. Engine A cannot do this at all: it only has
     * "give me these bytes" and has to ask for every address. */
    if ((c->flags & 0x1) && !info->writable) { c->pages_skipped++; return 0; }
    if ((c->flags & 0x2) && !info->nx)       { c->pages_skipped++; return 0; }
    if ((c->flags & 0x4) && !info->dirty)    { c->pages_skipped++; return 0; }
    if (info->user == 0)                     { c->pages_skipped++; return 0; }

    /*
     * Phase 4b: skip a frame we have already scanned through another VA.
     * Shared sections and DLL code pages map the same frame many times, so
     * this is where a scan of a DLL-heavy process gets cheaper.
     */
    if (yghv_scan_dedup_seen(c, info->page_base)) {
        c->dedup_hits++;
        c->pages_skipped++;
        return 0;
    }

    /* Only 4K pages are scanned byte-by-byte here. A large page is handled
     * by scanning it in 4K chunks so the buffer stays one page. */
    size = (info->page_size > YGHV_SCAN_PAGE_BUF) ? YGHV_SCAN_PAGE_BUF
                                                  : (uint32_t)info->page_size;
    if (!yghv_pa_read(info->page_base, c->page, size)) {
        c->pages_skipped++;
        return 0;
    }
    c->pages_read++;
    c->bytes_scanned += size;

    if (c->pat_len > size)
        return 0;

    for (pos = 0; pos + c->pat_len <= size; pos++) {
        uint64_t hit_va = va + pos;
        if (c->align && (hit_va % c->align))
            continue;
        if (yghv_scan_match(c->page + pos, c)) {
            c->out[c->out_count++] = hit_va;
            if (c->out_count >= c->out_max)
                return 1;
        }
    }
    return 0;
}

/*
 * Scan [start_va, end_va) of `proc` for `pattern` (with `mask`).
 * Writes up to out_max addresses into out; returns the number written.
 */
uint32_t yghv_mem_scan(PEPROCESS proc, uint64_t start_va, uint64_t end_va,
                       const uint8_t *pattern, const uint8_t *mask,
                       uint32_t pat_len, uint64_t align, uint32_t flags,
                       uint64_t *out, uint32_t out_max,
                       uint64_t *pages_read, uint64_t *pages_skipped,
                       uint64_t *bytes_scanned) {
    yghv_scan_ctx_t *c;
    uint64_t cr3;
    uint32_t found;

    if (pages_read)    *pages_read = 0;
    if (pages_skipped) *pages_skipped = 0;
    if (bytes_scanned) *bytes_scanned = 0;
    if (!proc || !pattern || !mask || !out || !out_max)
        return 0;
    if (pat_len == 0 || pat_len > YGHV_SCAN_PATTERN_MAX)
        return 0;
    if (start_va >= end_va)
        return 0;

    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
    if (!cr3)
        return 0;

    /* Nonpaged: the walk reads through it and it must not fault. */
    c = (yghv_scan_ctx_t *)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*c), 'TSPY');
    if (!c)
        return 0;

    RtlZeroMemory(c, sizeof(*c));
    c->pat      = pattern;
    c->mask     = mask;
    c->pat_len  = pat_len;
    c->align    = align;
    c->flags    = flags;
    c->out      = out;
    c->out_max  = out_max;
    c->pages_budget = YGHV_SCAN_DEFAULT_PAGE_BUDGET;

    /*
     * Phase 4b: dedup table. Sized to the page budget (the most frames one
     * call can read), rounded up to a power of two. Nonpaged because the
     * callback runs from the walk and must not fault. If it cannot be
     * allocated, dedup stays off and the scan still works.
     */
    {
        uint32_t want = 1;
        while (want < YGHV_SCAN_DEFAULT_PAGE_BUDGET * 2 && want < (1u << 20))
            want <<= 1;
        c->seen = (uint64_t *)ExAllocatePoolWithTag(
            NonPagedPoolNx, (SIZE_T)want * sizeof(uint64_t), 'TSDD');
        if (c->seen) {
            RtlZeroMemory(c->seen, (SIZE_T)want * sizeof(uint64_t));
            c->seen_mask = want - 1;
        }
    }

    yghv_pte_enum_range(cr3, start_va, end_va, yghv_scan_page_cb, c);

    found = c->out_count;
    if (pages_read)    *pages_read = c->pages_read;
    if (pages_skipped) *pages_skipped = c->pages_skipped;
    if (bytes_scanned) *bytes_scanned = c->bytes_scanned;

    if (c->seen)
        ExFreePoolWithTag(c->seen, 'TSDD');
    ExFreePoolWithTag(c, 'TSPY');
    return found;
}

/* ===================== Phase 4b: reverse scan ========================= */

/*
 * Reverse lookup: given a physical frame, find the VAs in `proc` that map it.
 *
 * The forward scan asks "what is at this VA"; this asks "who points at this
 * frame". Engine A cannot express the second question at all — it has no
 * physical-address interface, so there is nothing to search with.
 *
 * Walks the page tables and records every VA whose resolved frame matches.
 * Large pages are handled by reporting the containing page's base VA (a 2MB
 * page maps 512 candidate VAs, and the caller asked about a frame, not an
 * offset within it).
 */
uint32_t yghv_mem_scan_reverse(PEPROCESS proc, uint64_t target_pa,
                               uint64_t start_va, uint64_t end_va,
                               uint64_t *out, uint32_t out_max,
                               uint64_t *pages_visited) {
    uint64_t cr3;
    uint64_t want_frame = target_pa & ~(uint64_t)0xFFFULL;
    uint32_t found = 0;
    uint64_t visited = 0;
    uint64_t va;
    uint64_t i4, i3, i2, i1;
    uint64_t pml4_pa, pdpt_pa, pd_pa, pt_pa;

    if (pages_visited) *pages_visited = 0;
    if (!proc || !out || !out_max || !want_frame)
        return 0;
    if (start_va >= end_va) {
        start_va = YGHV_USER_VA_MIN;
        end_va   = YGHV_USER_VA_MAX;
    }

    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
    if (!cr3)
        return 0;
    pml4_pa = cr3 & YGHV_PTE_ADDR_MASK;
    if (!pml4_pa)
        return 0;

    /* Same bounded loop structure as yghv_pte_enum_range — every level stops
     * at the index end_va lands on, so a narrow range does not cost a full
     * 512^4 sweep (the mistake that wedged the driver during Phase 4). */
    for (i4 = YGHV_PTE_INDEX_PML4(start_va); i4 <= YGHV_PTE_INDEX_PML4(end_va - 1); i4++) {
        uint64_t pml4e = yghv_pte_read_entry(pml4_pa, i4);
        if (!(pml4e & YGHV_PTE_PRESENT))
            continue;
        pdpt_pa = pml4e & YGHV_PTE_ADDR_MASK;
        if (!pdpt_pa)
            continue;

        for (i3 = (i4 == YGHV_PTE_INDEX_PML4(start_va)) ? YGHV_PTE_INDEX_PDPT(start_va) : 0;
             i3 <= ((i4 == YGHV_PTE_INDEX_PML4(end_va - 1)) ? YGHV_PTE_INDEX_PDPT(end_va - 1) : 511);
             i3++) {
            uint64_t pdpte = yghv_pte_read_entry(pdpt_pa, i3);
            if (!(pdpte & YGHV_PTE_PRESENT))
                continue;

            if (pdpte & YGHV_PTE_LARGE) {
                va = (i4 << 39) | (i3 << 30);
                visited++;
                if (va >= start_va && va < end_va &&
                    (pdpte & YGHV_PTE_ADDR_MASK_1G) == (want_frame & YGHV_PTE_ADDR_MASK_1G)) {
                    out[found++] = va;
                    if (found >= out_max) goto done;
                }
                continue;
            }
            pd_pa = pdpte & YGHV_PTE_ADDR_MASK;
            if (!pd_pa)
                continue;

            for (i2 = ((i4 == YGHV_PTE_INDEX_PML4(start_va) && i3 == YGHV_PTE_INDEX_PDPT(start_va))
                           ? YGHV_PTE_INDEX_PD(start_va) : 0);
                 i2 <= (((i4 == YGHV_PTE_INDEX_PML4(end_va - 1)) &&
                         (i3 == YGHV_PTE_INDEX_PDPT(end_va - 1)))
                            ? YGHV_PTE_INDEX_PD(end_va - 1) : 511);
                 i2++) {
                uint64_t pde = yghv_pte_read_entry(pd_pa, i2);
                if (!(pde & YGHV_PTE_PRESENT))
                    continue;

                if (pde & YGHV_PTE_LARGE) {
                    va = (i4 << 39) | (i3 << 30) | (i2 << 21);
                    visited++;
                    if (va >= start_va && va < end_va &&
                        (pde & YGHV_PTE_ADDR_MASK_2M) == (want_frame & YGHV_PTE_ADDR_MASK_2M)) {
                        out[found++] = va;
                        if (found >= out_max) goto done;
                    }
                    continue;
                }
                pt_pa = pde & YGHV_PTE_ADDR_MASK;
                if (!pt_pa)
                    continue;

                for (i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = yghv_pte_read_entry(pt_pa, i1);
                    if (!(pte & YGHV_PTE_PRESENT))
                        continue;
                    va = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    if (va < start_va || va >= end_va)
                        continue;
                    visited++;
                    if ((pte & YGHV_PTE_ADDR_MASK) == want_frame) {
                        out[found++] = va;
                        if (found >= out_max) goto done;
                    }
                }
            }
        }
    }

done:
    if (pages_visited) *pages_visited = visited;
    return found;
}
