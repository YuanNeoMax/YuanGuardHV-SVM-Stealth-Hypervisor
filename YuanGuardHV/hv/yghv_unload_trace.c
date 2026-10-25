/*
 * yghv_unload_trace.c - 9.390. See common/yghv_unload_trace.h for why this exists.
 *
 * THE ONE REQUIREMENT THAT SHAPES EVERYTHING HERE
 *
 * The last write before the machine dies must be visible in a crash dump. That
 * rules out:
 *   - pool allocations (freed during the teardown being traced)
 *   - the guard's hidden pages (hidden by design)
 *   - user-mode files (measured: two scripts, both lost their output)
 *
 * What survives is a statically allocated, non-paged, always-mapped page. So this
 * file owns exactly one such page and writes nothing else.
 *
 * THE WRITE IS A SINGLE STORE
 *
 * yghv_ut_mark is called from DriverUnload between the steps that kill the
 * machine. If it took a lock and the fault happened while holding it, the dump
 * would show the lock instead of the step. So the sequence number and the step
 * are written as two plain stores with the step LAST, and a dump reader treats
 * the pair as valid only when the sequence is the one they expect. That is a
 * weaker guarantee than a lock and the right one here: progress that is readable
 * matters more than progress that is lossless.
 */

#include "yghv_unload_trace.h"

typedef struct {
    volatile LONG64 seq;          /* bumped on every stamp */
    volatile LONG   step;         /* the step, written after seq on purpose */
    volatile LONG   pad;
    volatile LONG64 value;        /* step-specific number (cores joined, etc.) */
    volatile LONG64 tsc;          /* when, so two steps can be ordered in time */
} yghv_ut_state_t;

typedef union {
    yghv_ut_state_t s;
    uint8_t page[4096];
} yghv_ut_region_t;

/*
 * Statically allocated, page exact, and with a named pad page after it so the
 * linker cannot place a neighbour on the same page (9.348's rule).
 *
 * NOT registered with yghv_guard_state_add(): this page must stay READABLE. A
 * guard-hidden page is a page whose contents a post-mortem cannot see, which is
 * the opposite of this page's only job.
 */
DECLSPEC_ALIGN(4096) static yghv_ut_region_t g_ut;
DECLSPEC_ALIGN(4096) static yghv_ut_region_t g_ut_pad;

_Static_assert(sizeof(yghv_ut_state_t) <= 4096, "unload trace fits a page");
_Static_assert(sizeof(yghv_ut_region_t) == 4096, "unload trace is exactly one page");

#define g_seq   (g_ut.s.seq)
#define g_step  (g_ut.s.step)
#define g_val   (g_ut.s.value)
#define g_tsc   (g_ut.s.tsc)

void yghv_ut_mark_val(uint32_t step, uint64_t value) {
    /*
     * Order matters and is the whole mechanism: seq first, then the payload, then
     * step. A dump taken between any two of these stores shows a step that does
     * not match its sequence, which a reader can recognise as "this stamp was in
     * flight" rather than misreading it as a completed step.
     */
    InterlockedIncrement64(&g_seq);
    g_val = value;
    g_tsc = (LONG64)__rdtsc();
    InterlockedExchange(&g_step, (LONG)step);
}

void yghv_ut_mark(uint32_t step) {
    yghv_ut_mark_val(step, 0);
}

uint32_t yghv_ut_last_step(void) {
    return (uint32_t)InterlockedCompareExchange(&g_step, 0, 0);
}

uint64_t yghv_ut_last_value(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_val, 0, 0);
}

uint64_t yghv_ut_page_pa(void) {
    /*
     * Resolved once and cached. Called from the load path and from the control
     * device, never from an exit handler, so a direct call is fine here (unlike
     * the island paths, which cache for the same reason but under a different
     * constraint).
     */
    static volatile LONG64 cached;
    LONG64 pa = InterlockedCompareExchange64(&cached, 0, 0);

    if (pa)
        return (uint64_t)pa;

    pa = (LONG64)MmGetPhysicalAddress(&g_ut).QuadPart;
    if (pa)
        InterlockedExchange64(&cached, pa);
    return (uint64_t)pa;
}
