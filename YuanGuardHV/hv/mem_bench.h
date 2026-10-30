/*
 * mem_bench.h — Phase 5 (Tier 1.5): A-vs-B measurement.
 *
 * See mem_bench.c for why this exists and how the comparison is kept honest.
 * The short version: the plan claims Engine B is faster on large ranges and
 * large pages, and until this file nobody had timed it.
 */
#ifndef YGHV_MEM_BENCH_H
#define YGHV_MEM_BENCH_H

#include "svm_defs.h"
#include <ntddk.h>

/*
 * Result of one head-to-head run.
 *
 * Both engines ran the SAME range with the SAME pattern, so the fields are
 * directly comparable:
 *
 *   a_reads / b_reads   how many reads each engine performed. A's unit is an
 *                       ADDRESS (one MmCopyVirtualMemory call), B's is a PAGE
 *                       (one walk step plus one physical read). This pair is the
 *                       page-table-driven claim in numeric form: on a range with
 *                       absent pages the counts diverge by the number of holes.
 *
 *   a_hits / b_hits     how many pattern matches each engine found. Reported
 *                       SEPARATELY, never averaged: if the two disagree, the
 *                       reader should see it rather than have it smoothed into a
 *                       performance number. A disagreement here is a functional
 *                       fact worth more than any timing.
 *
 *   speedup_x100        a_cycles * 100 / b_cycles. Integer hundredths, so no
 *                       float crosses the IOCTL boundary. Note the DIRECTION:
 *                       B ran first and warmed nothing for itself (see the
 *                       ordering note in mem_bench.c), so a value above 100 means
 *                       B was faster even with A given the warmer cache.
 */
typedef struct {
    uint64_t a_cycles;
    uint64_t a_reads;
    uint64_t a_bytes;
    uint64_t a_hits;
    uint64_t b_cycles;
    uint64_t b_reads;
    uint64_t b_bytes;
    uint64_t b_hits;
    uint64_t pages_visited;     /* pages B's walk actually stepped through */
    uint64_t walk_cr3;          /* CR3 the walk used (0 = could not read it) */
    uint64_t speedup_x100;      /* a_cycles * 100 / b_cycles               */
} yghv_bench_result_t;

/*
 * Run both engines over [start_va, end_va) of `proc` and fill *out.
 *
 * `page_budget` bounds what EACH engine may read (0 = no bound). Both honour the
 * same number so neither wins by doing more work.
 *
 * Returns 1 if both engines ran, 0 on unusable input. It does not return a
 * verdict - the caller sees two times and two read counts.
 *
 * PASSIVE_LEVEL: allocates a page buffer and calls MmCopyVirtualMemory,
 * which cannot run at DISPATCH.
 */
int yghv_mem_bench(PEPROCESS proc, uint64_t start_va, uint64_t end_va,
                   const uint8_t *pattern, const uint8_t *mask, uint32_t pat_len,
                   uint64_t page_budget, yghv_bench_result_t *out);

#endif /* YGHV_MEM_BENCH_H */
