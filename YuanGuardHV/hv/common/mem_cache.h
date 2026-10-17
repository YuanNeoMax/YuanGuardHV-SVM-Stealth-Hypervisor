/*
 * mem_cache.h — Tier 4.7: per-observer cache behaviour.
 *
 * PWT (bit 3) and PCD (bit 4) in an NPT entry control how the CPU caches the
 * page the guest touches: 00 = writeback (normal), 01 = write-through,
 * 10 = uncacheable, 11 = uncacheable-strong. They live in the NPT table, so
 * they are set per GPA in the NPT, and the guest sees them on every access
 * through that entry.
 *
 * What this module actually does
 * ------------------------------
 *   yghv_cache_set(gpa, mode)  — set PWT/PCD for one page
 *   yghv_cache_reset(gpa)      — back to writeback
 *
 * Two things this module does NOT claim:
 *
 *   1. "The guest does not notice". It DOES notice: uncached memory is
 *      dramatically slower for the guest (every access leaves the cache
 *      hierarchy), and a target that measures its own throughput can detect
 *      that. This is a behaviour change, not a stealth one.
 *
 *   2. "Per-observer". PWT/PCD are in the NPT entry, which is shared by
 *      everything that translates through that GPA. Making OUR reads
 *      uncached while the target's reads stay cached needs the split-view
 *      mechanism (Tier 4.1): two NPTs, one with PCD set for the page, one
 *      without. That is exactly what the alt table gives us — see
 *      yghv_cache_set_for_alt().
 *
 * Honest value
 * ------------
 * The one use that is real today: a page whose content must be read at the
 * moment it is written (MMIO-like semantics, or a race window the guest
 * could otherwise hide behind store-buffer coalescing). "Hiding from cache
 * side channels" is NOT delivered — that needs per-observer views plus
 * PAT/MTRR control we do not have.
 */
#ifndef YGHV_MEM_CACHE_H
#define YGHV_MEM_CACHE_H

#include "svm_defs.h"
#include <ntddk.h>

/* Cache modes (the two NPT bits, PWT:PCD) */
#define YGHV_CACHE_WRITEBACK   0   /* 00 — default */
#define YGHV_CACHE_WRITETHROUGH 1   /* 01 */
#define YGHV_CACHE_UNCACHED    2   /* 10 */
#define YGHV_CACHE_UNCACHEABLE_STRONG 3

/* Max pages we track (for reset-on-stop and diagnostics). */
#define YGHV_CACHE_MAX_PAGES   32

/*
 * Set the cache mode for one page in the MAIN table. 0 = writeback restores
 * normal behaviour. PASSIVE (touches the NPT). Returns 1 on success.
 */
int yghv_cache_set(uint64_t gpa, uint32_t mode);

/*
 * Same for the ALT table — combined with a split-view observer registration
 * (mem_view.h) this is what gives a *different* observer a different cache
 * behaviour for the same page. PASSIVE.
 */
int yghv_cache_set_alt(uint64_t gpa, uint32_t mode);

/* Back to writeback, both tables. */
int yghv_cache_reset(uint64_t gpa);

typedef struct {
    uint64_t sets;      /* successful mode changes */
    uint64_t resets;
    uint64_t failures;
    uint64_t tracked;
} yghv_cache_stats_t;

void yghv_cache_stats_get(yghv_cache_stats_t *out);
void yghv_cache_stats_reset(void);

#endif /* YGHV_MEM_CACHE_H */
