/*
 * mem_selfmap.h — Phase 2b: self-built physical mapping window.
 *
 * Why this exists
 * ---------------
 * Engine B currently reads through MmCopyMemory and writes through an MDL
 * per page. Both work, but both pay a per-access API cost (~1-3us) and both
 * are still NT calls. This module builds a WINDOW we own: one 4KB virtual
 * page whose PTE we overwrite to point at whatever physical frame we want,
 * so a subsequent access is a plain load/store — no API on the path at all.
 *
 * How it works
 * ------------
 *   1. Reserve a page-aligned VA with MmAllocateMappingAddress (a documented
 *      "mapping address" is reserved and never backed by real memory).
 *   2. Lock a scratch MDL on that VA with MmMapLockedPagesSpecifyCache so the
 *      address has a valid, writable PTE we are allowed to repoint.
 *   3. To target a frame: walk our own CR3 for the window VA, take the leaf
 *      entry's own physical address (yghv_pte_info_t.entry_pa), and rewrite
 *      that 8-byte entry so its PFN field is the target's.
 *   4. Flush the TLB entry for the window (INVLPG) and access.
 *
 * Honest limits
 * -------------
 *   - Repointing a PTE does NOT copy data: the window aliases the target
 *     frame. A write through the window changes the target page, which is
 *     the point — but it also means this is not a sandbox.
 *   - INVLPG is required after every retarget; without it the CPU may keep
 *     using the stale translation. On this CPU (Zen3) INVLPG works for our
 *     OWN address space (it is the NPT/GPA->HPA layer that ignores flushes,
 *     per 9.297/9.298 — a different mechanism entirely).
 *   - PASSIVE_LEVEL only: reserving/locking involves the memory manager.
 *   - One window, one page. Concurrent retargets from two threads would race,
 *     so callers must serialize (the engine holds a lock around use).
 */
#ifndef YGHV_MEM_SELFMAP_H
#define YGHV_MEM_SELFMAP_H

#include "svm_defs.h"
#include <ntddk.h>
#include "mem_pte.h"

/* Bring the window up. Returns 1 on success. PASSIVE_LEVEL. */
int  yghv_selfmap_init(void);

/* Tear the window down (idempotent). */
void yghv_selfmap_stop(void);

/* Is the window usable right now? */
int  yghv_selfmap_ready(void);

/*
 * Point the window at a physical frame and return a pointer to it.
 *
 * The returned pointer is valid until the next retarget. `offset` is the
 * byte offset within the frame and must be < 0x1000.
 * Returns NULL if the window is down or the target is the window itself.
 * PASSIVE_LEVEL only.
 */
void *yghv_selfmap_map(uint64_t target_pa, uint32_t offset);

/* The window's own VA (for diagnostics). */
uint64_t yghv_selfmap_window_va(void);

/* Read/write helpers built on the window. PASSIVE_LEVEL. */
int yghv_selfmap_read (uint64_t pa, void *dst, SIZE_T len);
int yghv_selfmap_write(uint64_t pa, const void *src, SIZE_T len);

/* Diagnostics */
typedef struct {
    uint64_t inits;
    uint64_t retargets;
    uint64_t reads;
    uint64_t writes;
    uint64_t failures;
    uint64_t tlb_flushes;
} yghv_selfmap_stats_t;

void yghv_selfmap_stats_get(yghv_selfmap_stats_t *out);
void yghv_selfmap_stats_reset(void);

#endif /* YGHV_MEM_SELFMAP_H */
