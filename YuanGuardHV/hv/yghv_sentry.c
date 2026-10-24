/*
 * yghv_sentry.c — Tier 7.4 / 7.5 / 7.6. See common/yghv_sentry.h.
 */

#include "yghv_sentry.h"

/*
 * 9.366: PAGE-EXACT, and this module is the tightest case in the tree.
 *
 * yghv_sentry_escape() is called from INSIDE SvHandleVmExit (a rejected EFER
 * write, a nested VMRUN, a foreign access to a hidden page, an unemulated CR
 * write) and it does four Interlocked operations onto g_esc[] on every one of
 * them. Meanwhile yghv_sentry_surface_add_ioctl() writes g_surface[] from the
 * IOCTL path — different privilege, different lifetime, and before this change
 * the same 4K page:
 *
 *     g_esc[0..5], g_esc_total, g_esc_last_*, g_exit_count/worst/total,
 *     g_surface[64], g_surface_armed, g_decoy_va, g_state_pa_ready
 *
 * Two of the neighbours were guard state (g_decoy_va, g_state_pa_ready), so the
 * page was already refused as a hide target and nothing could break — by
 * accident. "The only reason this is safe is that another mechanism happens to
 * refuse it" is not a property; it is a coincidence that one parameter change
 * ends. So the island-written counters get their own page, the PASSIVE-written
 * inventory gets another, and the arrangement stops depending on either one.
 */
typedef struct {
    volatile LONG64 esc[YGHV_ESC_MAX];
    volatile LONG64 esc_total;
    volatile LONG64 last_kind;
    volatile LONG64 last_detail;
    volatile LONG64 last_a;
    volatile LONG64 last_b;
    volatile LONG64 exit_count;
    volatile LONG64 exit_worst;
    volatile LONG64 exit_total;
} yghv_sentry_hot_t;

typedef union {
    yghv_sentry_hot_t s;
    uint8_t            page[4096];
} yghv_sentry_hot_region_t;

/* The inventory: written from the IOCTL/PASSIVE path, read from the same. The
 * "armed" flag lives INSIDE it for the reason the failure ring's index does:
 * a scalar beside a region is a scalar the linker may place on another module's
 * page, and this one is read once per core at prepare time. */
typedef struct {
    yghv_sentry_surface_entry_t entries[YGHV_SENTRY_MAX_SURFACE];
    volatile LONG               armed;
    uint32_t                    _pad;
} yghv_sentry_surface_t;

typedef union {
    yghv_sentry_surface_t s;
    uint8_t               page[4096];
} yghv_sentry_surface_region_t;

DECLSPEC_ALIGN(4096) static yghv_sentry_hot_region_t     g_sentry_hot;
DECLSPEC_ALIGN(4096) static yghv_sentry_surface_region_t g_sentry_surface;
DECLSPEC_ALIGN(4096) static yghv_sentry_hot_region_t     g_sentry_hot_pad;
DECLSPEC_ALIGN(4096) static yghv_sentry_surface_region_t g_sentry_surface_pad;

#define g_esc            (g_sentry_hot.s.esc)
#define g_esc_total      (g_sentry_hot.s.esc_total)
#define g_esc_last_kind  (g_sentry_hot.s.last_kind)
#define g_esc_last_detail (g_sentry_hot.s.last_detail)
#define g_esc_last_a     (g_sentry_hot.s.last_a)
#define g_esc_last_b     (g_sentry_hot.s.last_b)
#define g_exit_count     (g_sentry_hot.s.exit_count)
#define g_exit_worst     (g_sentry_hot.s.exit_worst)
#define g_exit_total     (g_sentry_hot.s.exit_total)
#define g_surface        (g_sentry_surface.s.entries)
#define g_surface_armed  (g_sentry_surface.s.armed)

_Static_assert(sizeof(yghv_sentry_hot_t) <= 4096, "sentry hot state fits a page");
_Static_assert(sizeof(yghv_sentry_hot_region_t) == 4096, "sentry hot region is exactly one page");
_Static_assert(sizeof(yghv_sentry_surface_t) <= 4096, "surface inventory fits a page");
_Static_assert(sizeof(yghv_sentry_surface_region_t) == 4096,
               "surface region is exactly one page");

/* ---------- 7.5 escape attempts ---------- */

/*
 * ISLAND-SAFE by construction: counters only, no lock, no allocation, no call
 * that can raise IRQL. This runs inside VMEXIT handlers where none of those are
 * available (9.249).
 */
void yghv_sentry_escape(yghv_esc_kind_t kind, uint64_t detail, uint64_t a,
                        uint64_t b) {
    if ((uint32_t)kind >= YGHV_ESC_MAX)
        kind = YGHV_ESC_MSR_INDEX;

    InterlockedIncrement64(&g_esc[kind]);
    InterlockedIncrement64(&g_esc_total);
    InterlockedExchange64(&g_esc_last_kind, (LONG64)kind);
    InterlockedExchange64(&g_esc_last_detail, (LONG64)detail);
    InterlockedExchange64(&g_esc_last_a, (LONG64)a);
    InterlockedExchange64(&g_esc_last_b, (LONG64)b);
}

/* ---------- 7.4 exit-latency self-measurement ---------- */

void yghv_sentry_exit_begin(uint64_t tsc) {
    /* Kept in a per-core slot is overkill here: the numbers we want are worst
     * case and average, and both are order-of-magnitude questions. */
    InterlockedExchange64(&g_esc_last_a, (LONG64)tsc);
}

void yghv_sentry_exit_end(uint64_t tsc) {
    uint64_t start = (uint64_t)InterlockedCompareExchange64(&g_esc_last_a, 0, 0);
    uint64_t delta;

    if (!start || tsc <= start)
        return;
    delta = tsc - start;

    InterlockedIncrement64(&g_exit_count);
    InterlockedExchangeAdd64(&g_exit_total, (LONG64)delta);

    /* Max: retry loop so a concurrent update cannot lose a larger value. */
    for (;;) {
        LONG64 cur = InterlockedCompareExchange64(&g_exit_worst, 0, 0);
        if ((LONG64)delta <= cur)
            break;
        if (InterlockedCompareExchange64(&g_exit_worst, (LONG64)delta, cur) == cur)
            break;
    }
}

uint64_t yghv_sentry_exit_worst(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_exit_worst, 0, 0);
}

uint64_t yghv_sentry_exit_count(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_exit_count, 0, 0);
}

/* ---------- 7.6 surface inventory ---------- */

static int yghv_sentry_find_name(const char *name) {
    int i;
    if (!name)
        return -1;
    for (i = 0; i < YGHV_SENTRY_MAX_SURFACE; i++) {
        int j;
        for (j = 0; j < YGHV_SENTRY_NAME_LEN; j++) {
            if (g_surface[i].name[j] != name[j])
                break;
            if (!name[j])
                return i;
        }
        if (j == YGHV_SENTRY_NAME_LEN && g_surface[i].name[YGHV_SENTRY_NAME_LEN - 1] == 0)
            continue;
    }
    return -1;
}

int yghv_sentry_surface_add(const char *name) {
    int i, j, slot = -1;

    if (!name || !name[0])
        return 0;
    for (i = 0; i < YGHV_SENTRY_MAX_SURFACE; i++) {
        int same = 1;
        for (j = 0; j < YGHV_SENTRY_NAME_LEN; j++) {
            if (g_surface[i].name[j] != name[j]) {
                same = 0;
                break;
            }
            if (!name[j])
                break;
        }
        if (same && g_surface[i].active) {
            g_surface[i].count++;
            return 1;
        }
        if (!g_surface[i].active && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return 0;

    for (j = 0; j < YGHV_SENTRY_NAME_LEN - 1 && name[j]; j++)
        g_surface[slot].name[j] = name[j];
    g_surface[slot].name[j] = 0;
    g_surface[slot].count = 1;
    g_surface[slot].is_ioctl = 0;
    g_surface[slot].active = 1;
    return 1;
}

int yghv_sentry_surface_remove(const char *name) {
    int i, j;

    if (!name)
        return 0;
    for (i = 0; i < YGHV_SENTRY_MAX_SURFACE; i++) {
        int same = 1;
        if (!g_surface[i].active)
            continue;
        for (j = 0; j < YGHV_SENTRY_NAME_LEN; j++) {
            if (g_surface[i].name[j] != name[j]) {
                same = 0;
                break;
            }
            if (!name[j])
                break;
        }
        if (same) {
            RtlZeroMemory(&g_surface[i], sizeof(g_surface[i]));
            return 1;
        }
    }
    return 0;
}

int yghv_sentry_surface_add_ioctl(uint32_t code) {
    char buf[YGHV_SENTRY_NAME_LEN];
    int i, slot = -1;

    /* name = "ioctl:XXX" — THREE hex digits, not two.
     *
     * 9.366: the earlier version wrote two digits, so every IOCTL from 0x800 to
     * 0x8FF collapsed into 256 distinct names and 0x800/0x810/0x820 — three
     * different requests — were indistinguishable in the inventory. The codes
     * this driver actually assigns run 0x800..0x828, so three digits covers
     * them and the format stays fixed-length (no delimiter needed). */
    for (i = 0; i < YGHV_SENTRY_NAME_LEN; i++)
        buf[i] = 0;
    buf[0] = 'i'; buf[1] = 'o'; buf[2] = 'c'; buf[3] = 't';
    buf[4] = 'l'; buf[5] = ':';
    buf[6] = "0123456789abcdef"[(code >> 8) & 0xF];
    buf[7] = "0123456789abcdef"[(code >> 4) & 0xF];
    buf[8] = "0123456789abcdef"[code & 0xF];
    buf[9] = 0;

    for (i = 0; i < YGHV_SENTRY_MAX_SURFACE; i++) {
        int same = 1, j;
        for (j = 0; j < YGHV_SENTRY_NAME_LEN; j++) {
            if (g_surface[i].name[j] != buf[j]) {
                same = 0;
                break;
            }
            if (!buf[j])
                break;
        }
        if (same && g_surface[i].active) {
            g_surface[i].count++;
            return 1;
        }
        if (!g_surface[i].active && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return 0;

    for (i = 0; i < YGHV_SENTRY_NAME_LEN - 1 && buf[i]; i++)
        g_surface[slot].name[i] = buf[i];
    g_surface[slot].name[i] = 0;
    g_surface[slot].count = 1;
    g_surface[slot].is_ioctl = 1;
    g_surface[slot].active = 1;
    return 1;
}

uint32_t yghv_sentry_surface_list(yghv_sentry_surface_entry_t *out, uint32_t max) {    uint32_t n = 0;
    int i;

    if (!out || !max)
        return 0;
    for (i = 0; i < YGHV_SENTRY_MAX_SURFACE && n < max; i++) {
        if (!g_surface[i].active)
            continue;
        RtlCopyMemory(&out[n], &g_surface[i], sizeof(g_surface[i]));
        n++;
    }
    return n;
}

void yghv_sentry_escape_stats_get(yghv_sentry_escape_stats_t *out) {    int i;

    if (!out)
        return;
    RtlZeroMemory(out, sizeof(*out));
    for (i = 0; i < YGHV_ESC_MAX; i++)
        out->escapes[i] = (uint64_t)InterlockedCompareExchange64(&g_esc[i], 0, 0);
    out->escapes_total = (uint64_t)InterlockedCompareExchange64(&g_esc_total, 0, 0);
    out->last_kind   = (uint64_t)InterlockedCompareExchange64(&g_esc_last_kind, 0, 0);
    out->last_detail = (uint64_t)InterlockedCompareExchange64(&g_esc_last_detail, 0, 0);
    out->last_a      = (uint64_t)InterlockedCompareExchange64(&g_esc_last_a, 0, 0);
    out->last_b      = (uint64_t)InterlockedCompareExchange64(&g_esc_last_b, 0, 0);
}

void yghv_sentry_timing_get(yghv_sentry_timing_t *out) {
    if (!out)
        return;
    out->exits        = (uint64_t)InterlockedCompareExchange64(&g_exit_count, 0, 0);
    out->worst_cycles = (uint64_t)InterlockedCompareExchange64(&g_exit_worst, 0, 0);
    out->total_cycles = (uint64_t)InterlockedCompareExchange64(&g_exit_total, 0, 0);
}

void yghv_sentry_reset(void) {
    int i;

    for (i = 0; i < YGHV_ESC_MAX; i++)
        InterlockedExchange64(&g_esc[i], 0);
    InterlockedExchange64(&g_esc_total, 0);
    InterlockedExchange64(&g_esc_last_kind, 0);
    InterlockedExchange64(&g_esc_last_detail, 0);
    InterlockedExchange64(&g_exit_count, 0);
    InterlockedExchange64(&g_exit_worst, 0);
    InterlockedExchange64(&g_exit_total, 0);
}

/* ============================================================================
 * 9.366 (Tier 7.6): THE INVENTORY FILLS ITSELF FROM THE INTERCEPT MASK.
 * ==========================================================================*/

/*
 * One name per intercept that can actually reject a guest input, listed in the
 * order the code sets them so a reader can check the list against
 * SvPrepareForVirtualization line by line.
 *
 * The list is DELIBERATELY short: only intercepts whose handler has a rejection
 * branch appear, because an interrupt we merely observe cannot be "attacked".
 * (The full mask is already readable in the VMCB; this answers "which of them
 * treat the guest as untrusted".)
 */
static const char *const g_intercept_names[] = {
    "cpuid",        /* SvHandleCpuid: the unload handshake */
    "msr",          /* SvHandleMsrAccess: EFER_SVME protection */
    "cr3",          /* SvHandleCr: CR value emulation, source-GPR decode */
    "cr4",          /* same emulator, PCIDE dance */
    "vmrun",        /* nested VMRUN is refused outright */
    "npf",          /* nested page faults: protection + hiding verdicts */
    "exception-db", /* the ALLOW loop's single-step re-hide */
    "exception-bp", /* INT3 mode */
    "shutdown"      /* guest triple fault becomes observable */
};
#define YGHV_SENTRY_INTERCEPT_COUNT \
    (sizeof(g_intercept_names) / sizeof(g_intercept_names[0]))

void yghv_sentry_surface_arm_intercepts(void) {
    uint32_t i;

    /*
     * Because this is called from SvPrepareForVirtualization on EVERY processor,
     * the count in each entry is the number of cores that armed it — not a
     * repeated registration. yghv_sentry_surface_add() already increments the
     * count on a name it finds, so a second core's call is exactly one more
     * "this core has it".
     */
    for (i = 0; i < (uint32_t)YGHV_SENTRY_INTERCEPT_COUNT; i++)
        (void)yghv_sentry_surface_add(g_intercept_names[i]);

    InterlockedExchange(&g_surface_armed, 1);
}

int yghv_sentry_surface_armed(void) {
    return (int)InterlockedCompareExchange(&g_surface_armed, 0, 0);
}

const char *yghv_sentry_esc_name(uint64_t kind) {
    switch (kind) {
    case YGHV_ESC_MSR_INDEX:  return "msr-index";
    case YGHV_ESC_IO_PORT:    return "io-port";
    case YGHV_ESC_CR_VALUE:   return "cr-value";
    case YGHV_ESC_VMCB_FIELD: return "vmcb-field";
    case YGHV_ESC_IOCTL_ARG:  return "ioctl-arg";
    case YGHV_ESC_GPA_GUARD:  return "guarded-page";
    default:                  return "?";
    }
}
