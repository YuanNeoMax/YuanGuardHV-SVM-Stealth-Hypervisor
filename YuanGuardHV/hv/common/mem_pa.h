/*
 * mem_pa.h — Phase 2: physical-frame read/write primitives.
 *
 * These are the ONLY places Engine B touches memory. Everything above
 * (typed access, scanning) is built on them.
 *
 * Why two different mechanisms:
 *   read  -> MmCopyMemory(PHYSICAL). Verified in this project (206c25 sync
 *            guard). Needs no mapping, reports failure by status, safe at
 *            DISPATCH_LEVEL. This is the same call mem_pte.c uses for table
 *            pages, for the reason recorded in docs/KERNEL_API_CHECKLIST.md
 *            section 3 — MmGetVirtualForPhysical is not reliable for
 *            arbitrary physical pages (it failed even on user page tables;
 *            see 9.305.2).
 *   write -> MmMapIoSpace / MmUnmapIoSpace. There is no MmCopyMemory
 *            equivalent for writing, so the page is mapped non-cached,
 *            written, and unmapped. Requires PASSIVE_LEVEL, so a physical
 *            write is NOT callable from the island.
 *
 * Callers must not hold g_protect_lock: the map/unmap pair may block.
 */
#ifndef YGHV_MEM_PA_H
#define YGHV_MEM_PA_H

#include "svm_defs.h"
#include <ntddk.h>

/* Read `len` bytes from a physical address. Returns 1 on success.
 * Safe at DISPATCH_LEVEL. */
int yghv_pa_read(uint64_t pa, void *dst, SIZE_T len);

/* Write `len` bytes to a physical address. Returns 1 on success.
 * PASSIVE_LEVEL only (maps the page). */
int yghv_pa_write(uint64_t pa, const void *src, SIZE_T len);

/* Read/write a single 8-byte aligned qword (fast path, no allocation). */
int yghv_pa_read_u64(uint64_t pa, uint64_t *out);
int yghv_pa_write_u64(uint64_t pa, uint64_t value);

/* Counters (lock-free) */
typedef struct {
    uint64_t reads;
    uint64_t reads_failed;
    uint64_t writes;
    uint64_t writes_failed;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t map_failures;
} yghv_pa_stats_t;

void yghv_pa_stats_get(yghv_pa_stats_t *out);
void yghv_pa_stats_reset(void);

#endif /* YGHV_MEM_PA_H */
