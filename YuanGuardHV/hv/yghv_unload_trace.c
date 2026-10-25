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
 * 9.392: THE STORE MUST OUTLIVE THE MODULE, SO IT IS NOT IN THE MODULE.
 *
 * 9.390 put this in a static (DECLSPEC_ALIGN(4096) region). Measured on the 20th
 * restart:
 *
 *     Unloaded modules:
 *     fffff801`c9d40000 fffff801`c9da4000   yghv_tr.sys   <-- already unloaded
 *
 * and the crash address was inside that range. A static lives in the driver's
 * data section - when the driver is unloaded, so is that section. So the page
 * that was supposed to record the unload was destroyed BY the unload, before the
 * crash that needed recording.
 *
 * The fix has to be memory this module does NOT own. There are three ways and
 * this one is chosen because it needs no reboot and no second machine:
 *
 *   allocate ONE page at load with MmAllocateContiguousMemory
 *   NEVER free it
 *
 * That is a deliberate, documented leak of 4 KB per load. The trade is explicit:
 * a 4 KB leak per load buys a record that survives the unload, in a machine that
 * leaks nothing else per load. A cleanup path that freed it would put it back in
 * the same class as the static, which is the defect being fixed.
 *
 * The physical address is stable for the machine's life, so it can be printed at
 * load and read out of a kernel dump afterwards.
 */
static volatile LONG64 g_ut_pa;
static PVOID           g_ut_page;      /* kept only so the VA is not lost */

#define UT_STATE ((volatile yghv_ut_state_t *)g_ut_page)

/*
 * 9.392: called once from the load path, at PASSIVE. Allocates the page that will
 * outlive this module. Returns 1 on success.
 */
int yghv_ut_init(void) {
    PHYSICAL_ADDRESS highest;

    if (g_ut_page)
        return 1;

    highest.QuadPart = 0xFFFFFFFFLL;
    g_ut_page = MmAllocateContiguousMemory(4096, highest);
    if (!g_ut_page)
        return 0;

    RtlZeroMemory(g_ut_page, 4096);
    InterlockedExchange64(&g_ut_pa,
                          (LONG64)MmGetPhysicalAddress(g_ut_page).QuadPart);
    return 1;
}

void yghv_ut_mark_val(uint32_t step, uint64_t value) {
    /*
     * Order matters and is the whole mechanism: seq first, then the payload, then
     * step. A dump taken between any two of these stores shows a step that does
     * not match its sequence, which a reader can recognise as "this stamp was in
     * flight" rather than misreading it as a completed step.
     */
    volatile yghv_ut_state_t *st = UT_STATE;

    if (!st)
        return;                     /* not initialised: nothing to record into */

    InterlockedIncrement64(&st->seq);
    st->value = (LONG64)value;
    st->tsc = (LONG64)__rdtsc();
    InterlockedExchange(&st->step, (LONG)step);
}

void yghv_ut_mark(uint32_t step) {
    yghv_ut_mark_val(step, 0);
}

uint32_t yghv_ut_last_step(void) {
    volatile yghv_ut_state_t *st = UT_STATE;
    return st ? (uint32_t)InterlockedCompareExchange(&st->step, 0, 0) : 0;
}

uint64_t yghv_ut_last_value(void) {
    volatile yghv_ut_state_t *st = UT_STATE;
    return st ? (uint64_t)InterlockedCompareExchange64(&st->value, 0, 0) : 0;
}

uint64_t yghv_ut_page_pa(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_ut_pa, 0, 0);
}
