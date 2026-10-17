/*
 * mem_island.h — Phase 6: island (VMEXIT-context) memory access.
 *
 * The constraint chain that shapes this module
 * -------------------------------------------
 * The NPF handler runs at IRQL = DISPATCH_LEVEL with GIF=0 and must not
 * block, lock, allocate, or log (protect.c 9.249 — learned twice the hard
 * way). Checking the WDK annotations against that:
 *
 *   MmCopyMemory                 _IRQL_requires_max_(APC_LEVEL)   -> NO
 *   MmMapLockedPagesSpecifyCache _IRQL_requires_max_(PASSIVE)     -> NO
 *   MmMapIoSpace                 PASSIVE                          -> NO
 *   MmGetVirtualForPhysical      (unannotated; only maps SYSTEM pages,
 *                                 9.299 measured it failing on user pages) -> NO
 *
 * So every transfer primitive Engine B uses on the host path is illegal in
 * the island. What remains is the kernel's DIRECT MAP: the region where the
 * system maps all of physical memory at a fixed offset. Reading a frame is
 * then a plain load at (direct_map_base + pa) — no call at all, so it works
 * at any IRQL.
 *
 * The base is not exported. It is discovered once at PASSIVE (probe a frame
 * whose VA we already know, then extrapolate), cached, and from then on the
 * island does arithmetic only.
 *
 * Honest limit: the direct map is not guaranteed for every frame on every
 * build, and a wrong base reads the wrong memory rather than faulting. So the
 * base is VALIDATED against several known (PA, VA) pairs before it is
 * published, and yghv_island_ready() stays false until it is.
 */
#ifndef YGHV_MEM_ISLAND_H
#define YGHV_MEM_ISLAND_H

#include "svm_defs.h"
#include <ntddk.h>

/*
 * Discover and validate the direct-map base. PASSIVE_LEVEL, call once at
 * driver load or on demand. Returns 1 on success.
 *
 * `probe_va` is any kernel VA whose physical address the caller already knows
 * (the driver's own code or a nonpaged buffer). `probe_pa` is that address.
 * Two independent probes must agree before the base is published.
 */
int yghv_island_init(uint64_t probe_va_a, uint64_t probe_pa_a,
                     uint64_t probe_va_b, uint64_t probe_pa_b);

/* Is the island path usable? (base discovered and validated) */
int yghv_island_ready(void);

/* The discovered base (0 when not ready). */
uint64_t yghv_island_base(void);

/*
 * Island-safe physical read/write: plain loads/stores through the direct map.
 * NO calls, NO locks, NO allocation — callable at DISPATCH with GIF=0.
 *
 * Returns 1 on success, 0 if the island path is not ready or the range is not
 * covered by the direct map. Callers on the host path should fall back to
 * yghv_pa_read/yghv_pa_write when this returns 0.
 */
int yghv_island_read(uint64_t pa, void *dst, SIZE_T len);
int yghv_island_write(uint64_t pa, const void *src, SIZE_T len);

/* Diagnostics */
typedef struct {
    uint64_t base;
    uint64_t reads;
    uint64_t writes;
    uint64_t refusals;      /* island not ready / out of range */
    uint64_t probes;        /* how many (PA,VA) pairs were checked */
} yghv_island_stats_t;

void yghv_island_stats_get(yghv_island_stats_t *out);
void yghv_island_stats_reset(void);

#endif /* YGHV_MEM_ISLAND_H */
