/*
 * yghv_fail.h — Tier 7.2/7.7: failure isolation and graceful degradation.
 *
 * The problem this solves
 * -----------------------
 * Every abnormal condition used to end the same way: KeBugCheckEx. That made
 * "a feature hit an unexpected state" indistinguishable from "the machine is
 * broken", and the cost was measured all day — twelve bugchecks, each one a
 * reboot, most of them for conditions the guest could have survived.
 *
 * The rule now is: an unexpected condition is DEGRADED first and only ends in
 * a bugcheck when there is genuinely no way to continue. Concretely,
 *
 *   unknown VMEXIT   record it, do not emulate it, let the guest continue.
 *                    The guest sees whatever the hardware would have done; we
 *                    simply stop pretending to handle that exit.
 *
 *   unexpected CR    ignore the write, keep the old value. One guest store is
 *                    lost instead of the whole machine.
 *
 *   watchdog stall   a core has not made progress for several polls. Before
 *                    that meant bugcheck; now we first try to get out of the
 *                    way (drop the intercepts that can trap that core), and
 *                    only bugcheck if even that fails.
 *
 * What is deliberately NOT degraded
 * ---------------------------------
 * A state that would silently corrupt the guest is worse than a bugcheck: if
 * the NPF path cannot restore a page's permission, injecting an access
 * violation is correct and the machine keeps running; but if the NPT root
 * itself were lost there is nothing sane to continue with. Those cases still
 * record, then bugcheck — and they are listed explicitly in the source rather
 * than left implicit.
 *
 * Everything here is island-safe: no locks, no allocation, no logging calls.
 * VMEXIT handlers call it.
 */
#ifndef YGHV_FAIL_H
#define YGHV_FAIL_H

#include "svm_defs.h"
#include <ntddk.h>

/* Why we are degrading. One counter per reason so a single dump tells the
 * story instead of requiring a reproduction. */
typedef enum {
    YGHV_FAIL_UNKNOWN_EXIT = 0,  /* VMEXIT code we do not handle */
    YGHV_FAIL_BAD_CR       = 1,  /* CR access outside the 3/4 we intercept */
    YGHV_FAIL_WATCHDOG     = 2,  /* a core stopped making progress */
    YGHV_FAIL_NPF_UNRESOLVED = 3,/* NPF we could not attribute */
    YGHV_FAIL_TABLE_FULL   = 4,  /* internal table exhausted */
    YGHV_FAIL_MAX          = 5
} yghv_fail_reason_t;

/*
 * Record one degraded condition.
 *
 * Returns 1 when the caller can safely continue (the condition was handled by
 * skipping it), or 0 when there is nothing safe left to do and the caller
 * should raise its bugcheck. Island-safe.
 */
int yghv_fail_degrade(yghv_fail_reason_t reason, uint64_t a, uint64_t b,
                      uint64_t c);

/* Have we degraded at all? Once non-zero, a caller may choose a simpler path. */
int yghv_fail_degraded(void);

/* How many times, total. */
uint64_t yghv_fail_count(void);

typedef struct {
    uint64_t total;
    uint64_t by_reason[YGHV_FAIL_MAX];
    uint64_t last_reason;
    uint64_t last_a;
    uint64_t last_b;
    uint64_t last_c;
    uint64_t degraded;
    uint64_t fatal;          /* conditions where degrade said "no way out" */
} yghv_fail_stats_t;

void yghv_fail_stats_get(yghv_fail_stats_t *out);
void yghv_fail_stats_reset(void);

/* Human-readable name for a reason (for the IOCTL surface). */
const char *yghv_fail_reason_name(uint64_t reason);

#endif /* YGHV_FAIL_H */
