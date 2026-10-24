/*
 * yghv_fail.c — Tier 7.2/7.7: failure isolation and graceful degradation.
 * See common/yghv_fail.h for the policy and what is deliberately not degraded.
 */

#include "yghv_fail.h"

/*
 * 9.366: PAGE-EXACT. yghv_fail_degrade() is called from INSIDE SvHandleVmExit
 * (an unknown exit code, a CR index we do not emulate), so everything below is
 * island-written state. Measured on the .map, it shared page 0x49000 with
 * mem_frame's bookkeeping — an IOCTL-only module, but that is a difference in
 * PRIVILEGE and LIFETIME, not a difference that makes two objects safe to put
 * on one page:
 *
 *     g_frm_seq, g_frame_count, g_frm_reserved, g_frm_bytes, g_frm_released,
 *     g_total, g_by, g_last_reason, g_last_a, g_last_b, g_last_c, g_degraded,
 *     g_ring_head, g_fatal
 *
 * g_frame's TABLE is a hide target (g_frame_region is page-exact), so the page
 * carrying its scalars is the page a future "hide the frame module" change
 * would reach for — and that page carries the counters this path increments.
 * The 15th crash was exactly that, one object up.
 */

/* Hit ring so the LAST few conditions survive even if stats are printed late.
 * 4 qwords per entry: reason, a, b, c. */
#define YGHV_FAIL_RING 32

typedef struct {
    volatile LONG64 total;
    volatile LONG64 by[YGHV_FAIL_MAX];
    volatile LONG64 last_reason;
    volatile LONG64 last_a;
    volatile LONG64 last_b;
    volatile LONG64 last_c;
    volatile LONG64 degraded;
    volatile LONG64 fatal;
    /*
     * 9.366: THE RING INDEX IS A FIELD OF THIS STRUCT, not a variable beside it.
     *
     * It was declared next to the ring and left outside every page-exact region,
     * which is how it ended up on a page belonging to another module. A counter
     * that is not inside the object it indexes is a counter the linker places
     * wherever it likes — and this one is written on the island path.
     */
    volatile LONG   ring_head;
    uint32_t        _pad;
    uint64_t        ring[YGHV_FAIL_RING * 4];
} yghv_fail_state_t;

typedef union {
    yghv_fail_state_t s;
    uint8_t           page[4096];
} yghv_fail_region_t;

DECLSPEC_ALIGN(4096) static yghv_fail_region_t g_fail;
DECLSPEC_ALIGN(4096) static yghv_fail_region_t g_fail_pad;  /* separation */

#define g_ring      (g_fail.s.ring)
#define g_ring_head (g_fail.s.ring_head)
#define g_total         (g_fail.s.total)
#define g_by            (g_fail.s.by)
#define g_last_reason   (g_fail.s.last_reason)
#define g_last_a        (g_fail.s.last_a)
#define g_last_b        (g_fail.s.last_b)
#define g_last_c        (g_fail.s.last_c)
#define g_degraded      (g_fail.s.degraded)
#define g_fatal         (g_fail.s.fatal)

_Static_assert(sizeof(yghv_fail_state_t) <= 4096,
               "failure state fits one page (grow the ring only in whole pages)");
_Static_assert(sizeof(yghv_fail_region_t) == 4096,
               "failure region is exactly one page");

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
