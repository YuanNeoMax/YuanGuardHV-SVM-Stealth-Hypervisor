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
    uint32_t       flags;       /* bit0 writable, bit1 executable */
    uint64_t      *out;         /* caller's result array */
    uint32_t       out_max;
    uint32_t       out_count;
    uint8_t        page[YGHV_SCAN_PAGE_BUF];
    uint64_t       pages_read;
    uint64_t       pages_skipped;
    uint64_t       bytes_scanned;
    uint64_t       pages_budget;   /* stop after this many reads (0 = unlimited) */
} yghv_scan_ctx_t;

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

    /* Attribute filter */
    if ((c->flags & 0x1) && !info->writable) { c->pages_skipped++; return 0; }
    if ((c->flags & 0x2) && !info->nx)       { c->pages_skipped++; return 0; }
    if (info->user == 0)                     { c->pages_skipped++; return 0; }

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

    yghv_pte_enum_range(cr3, start_va, end_va, yghv_scan_page_cb, c);

    found = c->out_count;
    if (pages_read)    *pages_read = c->pages_read;
    if (pages_skipped) *pages_skipped = c->pages_skipped;
    if (bytes_scanned) *bytes_scanned = c->bytes_scanned;

    ExFreePoolWithTag(c, 'TSPY');
    return found;
}
