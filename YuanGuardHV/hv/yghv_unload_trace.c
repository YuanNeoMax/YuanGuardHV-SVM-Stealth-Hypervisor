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

/*
 * 9.398: A SHORT HISTORY, BECAUSE ONE VALUE SLOT LOSES THE ANSWER.
 *
 * The 22nd dump showed seq=5, step=20, value=0. Reading it was impossible for a
 * reason that had nothing to do with the crash: the two stamps that carried the
 * numbers I needed (45 = loops stopped, 46 = loops expected) had already been
 * OVERWRITTEN by the stamp that recorded where it died. One value slot means the
 * last writer wins, and the last writer is always the one that crashed.
 *
 * So the record keeps the last N stamps. Four is enough for "how many stopped,
 * how many expected, where next, where after" - and 4 * 40 bytes stays trivially
 * inside the page.
 */
/*
 * 9.418: 128 SLOTS, AND NO WRAP-AROUND.
 *
 * 9.417 found that the 4-slot ring had made the readout meaningless: DriverUnload
 * runs more than once in this tree (d->DriverUnload is set twice historically),
 * and each run overwrote order 1 and 2, so "step = 10" meant "the last thing
 * written was 10", not "execution stopped at 10".
 *
 * 120 slots of 32 bytes, plus the 32-byte header and the 48-byte snapshot area,
 * comes to 3920 bytes - inside one page with room to grow. That is more than the
 * ~20 marks a single unload writes. So the index does NOT wrap: once full, writes
 * stop. That keeps
 * the FIRST marks of the first run instead of the last marks of the last one -
 * which is what a post-mortem needs, because the first failure is the one that
 * explains the rest.
 */
#define YGHV_UT_HIST 120

typedef struct {
    volatile LONG64 seq_order;    /* which stamp this is (1-based) */
    volatile LONG   step;
    volatile LONG64 value;
    volatile LONG64 tsc;
} yghv_ut_slot_t;

typedef struct {
    volatile LONG64 seq;          /* bumped on every stamp */
    volatile LONG   step;         /* the step, written after seq on purpose */
    volatile LONG   pad;
    volatile LONG64 value;        /* latest value, for a quick read */
    volatile LONG64 tsc;          /* when, so two steps can be ordered in time */
    yghv_ut_slot_t hist[YGHV_UT_HIST];

    /*
     * 9.404: A SNAPSHOT AREA THAT THE TEARDOWN CANNOT REWRITE.
     *
     * 9.403 recorded that six of eight readouts were USELESS because the value
     * meant two different things: "never started" and "started, then zeroed by
     * the very unload we are observing". g_PowerCallbackRegistration and
     * g_resident_thread_count both suffer from this - the teardown writes NULL
     * and 0 over the evidence before the machine dies.
     *
     * The fix is not to read differently; it is to WRITE FIRST. These fields are
     * filled at YGHV_UT_START, before any teardown runs, and nothing else ever
     * touches them. A dump then carries BOTH the pre-teardown truth and the
     * post-teardown leftovers, and comparing them answers "was it ever set".
     *
     * Deliberately NOT part of the history ring: a ring is written on every
     * stamp, so by the fifth stamp the snapshot would be gone - the same class of
     * mistake (one slot, many writers) that made the earlier readouts ambiguous.
     */
    volatile LONG64 snap_resident_threads;   /* g_resident_thread_count at entry */
    volatile LONG64 snap_power_cb;           /* g_PowerCallbackRegistration != 0 */
    volatile LONG64 snap_vcpu_count;         /* g_vcpu_count at entry */
    volatile LONG64 snap_s203_online;        /* g_s203_online at entry */
    volatile LONG64 snap_vmrun_stopped;      /* the stop counter at entry */
    volatile LONG64 snap_cpuid_view;         /* g_CpuidView at entry */
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

/*
 * 9.404: capture what the teardown is about to destroy. Called ONCE, at entry,
 * before any field has been cleared. Each source is a plain read, so this is
 * safe anywhere DriverUnload runs (PASSIVE).
 */
void yghv_ut_snapshot_pre_unload(UINT64 resident_threads, UINT64 power_cb_present,
                                 UINT64 vcpu_count, UINT64 s203_online,
                                 UINT64 vmrun_stopped, UINT64 cpuid_view) {
    volatile yghv_ut_state_t *st = UT_STATE;

    if (!st)
        return;

    st->snap_resident_threads = (LONG64)resident_threads;
    st->snap_power_cb         = (LONG64)power_cb_present;
    st->snap_vcpu_count       = (LONG64)vcpu_count;
    st->snap_s203_online      = (LONG64)s203_online;
    st->snap_vmrun_stopped    = (LONG64)vmrun_stopped;
    st->snap_cpuid_view       = (LONG64)cpuid_view;
}

void yghv_ut_mark_val(uint32_t step, uint64_t value) {
    /*
     * Order matters and is the whole mechanism: seq first, then the payload, then
     * step. A dump taken between any two of these stores shows a step that does
     * not match its sequence, which a reader can recognise as "this stamp was in
     * flight" rather than misreading it as a completed step.
     */
    volatile yghv_ut_state_t *st = UT_STATE;
    LONG64 order;

    if (!st)
        return;                     /* not initialised: nothing to record into */

    order = InterlockedIncrement64(&st->seq);

    /* Slot first, then the quick-read fields: if the machine dies mid-stamp, the
     * history (which is what a dump reader walks) is the more complete of the two. */
    {
        /*
         * 9.418: index by order directly, capped - no wrap. A slot used by an
         * earlier run is never reused, so a second unload cannot erase the
         * evidence of the first.
         *
         * When the table is full the SLOT write stops, but the quick-read fields
         * below still update - they are the "latest state", and a reader who
         * wants the early history reads hist[], while a reader who wants the last
         * mark reads step/value. Returning here would freeze both.
         */
        yghv_ut_slot_t *slot = NULL;

        if (order <= YGHV_UT_HIST)
            slot = (yghv_ut_slot_t *)&st->hist[order - 1];

        if (slot)
        {
            slot->step = (LONG)step;
            slot->value = (LONG64)value;
            slot->tsc = (LONG64)__rdtsc();
            slot->seq_order = order;
        }
    }

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
