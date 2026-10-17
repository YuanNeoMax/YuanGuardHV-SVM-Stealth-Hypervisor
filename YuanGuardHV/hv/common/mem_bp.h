/*
 * mem_bp.h — Tier 4.4: instruction-level access control.
 *
 * What it is
 * ----------
 * A watchpoint that fires on ONE instruction rather than on a page. The
 * granularity comes from combining the NPF mechanism with the guest's RIP:
 *
 *   - the page's NPT permission is revoked (any access faults)
 *   - #NPF handler compares the faulting RIP against the registered
 *     instruction's RIP
 *   - match  -> record, restore permission, single-step (TF), let it land,
 *               then revoke again on #DB (the ALLOW loop, protect.c 1484-1557)
 *   - no match -> the access is refused (or the page is left protected)
 *
 * That gives "execute X only when it touches this page", which no amount of
 * DR0-DR3 can express (hardware breakpoints are per-address, not
 * per-instruction-and-address).
 *
 * What it is NOT
 * --------------
 * It is not the same as 4.6 (page-level watchpoints). 4.6 records every
 * access to a page; 4.4 records the specific instruction the caller named.
 * Both use the same primitive; 4.4 just adds the RIP filter.
 *
 * Honest limits
 * -------------
 *   - The RIP must be the instruction's start. An access from a different
 *     RIP that resolves to the same instruction (a jump into the middle) is
 *     a different match.
 *   - Same per-core limitation as 4.1: the NPT is per-VCPU, so two cores
 *     racing on the page can both miss the event.
 *   - The permission restore + single-step loop is the mechanism 4.6 was
 *     missing. It is implemented here in full (see mem_bp.c) and is what
 *     makes 4.4 safe to run.
 */
#ifndef YGHV_MEM_BP_H
#define YGHV_MEM_BP_H

#include "svm_defs.h"
#include <ntddk.h>

#define YGHV_BP_MAX  8

/* Access kinds */
#define YGHV_BP_READ   0x1
#define YGHV_BP_WRITE  0x2
#define YGHV_BP_EXEC   0x4

typedef struct {
    uint64_t gpa;        /* page watched */
    uint64_t rip;        /* instruction start (0 = any instruction) */
    uint32_t kinds;      /* bitmask */
    uint64_t hits;       /* times this bp matched */
    volatile LONG active;
} yghv_bp_t;

/*
 * Add an instruction-level breakpoint. `rip` 0 means "any instruction on
 * this page" (i.e. degrades to 4.6 semantics). PASSIVE. Returns 1 on success.
 */
int yghv_bp_add(uint64_t gpa, uint64_t rip, uint32_t kinds);

/*
 * Tier 4.4 (2026-10-17): is this page currently guarded by a breakpoint?
 *
 * The write engine MUST ask this before writing. A bp page has its NPT write
 * permission revoked, and measurement showed that our own engine's write to
 * such a frame fails hard (0x3B / 0x50 across five crashes). A breakpoint is
 * a MONITOR — it exists to observe the guest, not to let us write through it.
 * Refusing the write is the correct behaviour; attempting it is what crashed
 * the machine.
 *
 * Lock-free, callable at any IRQL.
 */
int yghv_bp_is_armed(uint64_t gpa);

/* Remove by (gpa, rip). */
int yghv_bp_remove(uint64_t gpa, uint64_t rip);

/*
 * Called from the NPF handler. Returns 1 if this fault matches a registered
 * breakpoint AND the caller should take the ALLOW path (record, restore,
 * single-step, re-arm).
 *
 * ISLAND-SAFE: scans a nonpaged array, records into a preallocated ring.
 */
int yghv_bp_on_npf(uint64_t gpa, uint64_t rip, uint32_t kind, uint32_t cpl,
                   uint64_t cr3);

/*
 * Drain recorded hits. Each entry is (rip, gpa, kind, cpl, cr3, seq).
 * PASSIVE. Returns the count.
 */
uint32_t yghv_bp_drain(uint64_t *out, uint32_t max);

typedef struct {
    uint64_t count;      /* registered breakpoints */
    uint64_t hits;       /* matched faults */
    uint64_t recorded;
    uint64_t missed;     /* matched but ring full */
    uint64_t refusals;   /* faults that matched no bp */
} yghv_bp_stats_t;

void yghv_bp_stats_get(yghv_bp_stats_t *out);
void yghv_bp_stats_reset(void);

#endif /* YGHV_MEM_BP_H */
