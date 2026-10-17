/*
 * yghv_fail.c — Tier 7.2/7.7: failure isolation and graceful degradation.
 * See common/yghv_fail.h for the policy and what is deliberately not degraded.
 */

#include "yghv_fail.h"

/* Hit ring so the LAST few conditions survive even if stats are printed late.
 * 4 qwords per entry: reason, a, b, c. */
#define YGHV_FAIL_RING 32
static uint64_t g_ring[YGHV_FAIL_RING * 4];
static volatile LONG g_ring_head;

static volatile LONG64 g_total;
static volatile LONG64 g_by[YGHV_FAIL_MAX];
static volatile LONG64 g_last_reason;
static volatile LONG64 g_last_a;
static volatile LONG64 g_last_b;
static volatile LONG64 g_last_c;
static volatile LONG64 g_degraded;
static volatile LONG64 g_fatal;

/*
 * Which reasons can be survived by skipping the thing we could not handle.
 *
 * unknown exit : the guest keeps its own state; we simply did not emulate.
 * bad CR       : one store is dropped, the old register value stands.
 * watchdog     : handled by the caller (it gets to try dropping intercepts
 *                first); reported as survive-able because there IS an action.
 * npf-unresolved: the NPF path can always inject an access violation, which
 *                the guest handles the way it handles any fault.
 * table-full   : refuse the new registration; existing ones keep working.
 */
static const uint8_t g_survive[YGHV_FAIL_MAX] = {
    1,  /* UNKNOWN_EXIT  */
    1,  /* BAD_CR        */
    1,  /* WATCHDOG      */
    1,  /* NPF_UNRESOLVED*/
    1   /* TABLE_FULL    */
};

int yghv_fail_degrade(yghv_fail_reason_t reason, uint64_t a, uint64_t b,
                      uint64_t c) {
    LONG head;
    uint64_t *e;

    if ((uint32_t)reason >= YGHV_FAIL_MAX)
        reason = YGHV_FAIL_UNKNOWN_EXIT;

    InterlockedIncrement64(&g_total);
    InterlockedIncrement64(&g_by[reason]);
    InterlockedExchange64(&g_last_reason, (LONG64)reason);
    InterlockedExchange64(&g_last_a, (LONG64)a);
    InterlockedExchange64(&g_last_b, (LONG64)b);
    InterlockedExchange64(&g_last_c, (LONG64)c);
    InterlockedExchange64(&g_degraded, 1);

    /* Island-safe ring: single producer per core is not guaranteed (several
     * VCPUs can degrade at once), so it is best-effort by design — the counters
     * above are the authoritative record, the ring is for context. */
    head = InterlockedIncrement(&g_ring_head) - 1;
    e = &g_ring[(head & (YGHV_FAIL_RING - 1)) * 4];
    e[0] = (uint64_t)reason;
    e[1] = a;
    e[2] = b;
    e[3] = c;

    if (!g_survive[reason]) {
        InterlockedIncrement64(&g_fatal);
        return 0;
    }
    return 1;
}

int yghv_fail_degraded(void) {
    return (int)InterlockedCompareExchange64(&g_degraded, 0, 0);
}

uint64_t yghv_fail_count(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_total, 0, 0);
}

void yghv_fail_stats_get(yghv_fail_stats_t *out) {
    int i;

    if (!out)
        return;
    RtlZeroMemory(out, sizeof(*out));
    out->total       = (uint64_t)InterlockedCompareExchange64(&g_total, 0, 0);
    out->last_reason = (uint64_t)InterlockedCompareExchange64(&g_last_reason, 0, 0);
    out->last_a      = (uint64_t)InterlockedCompareExchange64(&g_last_a, 0, 0);
    out->last_b      = (uint64_t)InterlockedCompareExchange64(&g_last_b, 0, 0);
    out->last_c      = (uint64_t)InterlockedCompareExchange64(&g_last_c, 0, 0);
    out->degraded    = (uint64_t)InterlockedCompareExchange64(&g_degraded, 0, 0);
    out->fatal       = (uint64_t)InterlockedCompareExchange64(&g_fatal, 0, 0);
    for (i = 0; i < YGHV_FAIL_MAX; i++)
        out->by_reason[i] = (uint64_t)InterlockedCompareExchange64(&g_by[i], 0, 0);
}

void yghv_fail_stats_reset(void) {
    int i;

    InterlockedExchange64(&g_total, 0);
    InterlockedExchange64(&g_last_reason, 0);
    InterlockedExchange64(&g_last_a, 0);
    InterlockedExchange64(&g_last_b, 0);
    InterlockedExchange64(&g_last_c, 0);
    InterlockedExchange64(&g_degraded, 0);
    InterlockedExchange64(&g_fatal, 0);
    for (i = 0; i < YGHV_FAIL_MAX; i++)
        InterlockedExchange64(&g_by[i], 0);
    InterlockedExchange(&g_ring_head, 0);
}

const char *yghv_fail_reason_name(uint64_t reason) {
    switch (reason) {
    case YGHV_FAIL_UNKNOWN_EXIT:  return "unknown-vmexit";
    case YGHV_FAIL_BAD_CR:        return "unexpected-cr";
    case YGHV_FAIL_WATCHDOG:      return "watchdog-stall";
    case YGHV_FAIL_NPF_UNRESOLVED:return "npf-unresolved";
    case YGHV_FAIL_TABLE_FULL:    return "table-full";
    default:                      return "?";
    }
}
