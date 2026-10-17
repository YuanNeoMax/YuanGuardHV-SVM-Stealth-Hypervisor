/*
 * yghv_sentry.c — Tier 7.4 / 7.5 / 7.6. See common/yghv_sentry.h.
 */

#include "yghv_sentry.h"

static volatile LONG64 g_esc[YGHV_ESC_MAX];
static volatile LONG64 g_esc_total;
static volatile LONG64 g_esc_last_kind;
static volatile LONG64 g_esc_last_detail;
static volatile LONG64 g_esc_last_a;
static volatile LONG64 g_esc_last_b;

static volatile LONG64 g_exit_count;
static volatile LONG64 g_exit_worst;
static volatile LONG64 g_exit_total;

static yghv_sentry_surface_entry_t g_surface[YGHV_SENTRY_MAX_SURFACE];

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

    /* name = "ioctl:XXXX" */
    for (i = 0; i < YGHV_SENTRY_NAME_LEN; i++)
        buf[i] = 0;
    buf[0] = 'i'; buf[1] = 'o'; buf[2] = 'c'; buf[3] = 't';
    buf[4] = 'l'; buf[5] = ':';
    buf[6] = "0123456789abcdef"[(code >> 4) & 0xF];
    buf[7] = "0123456789abcdef"[code & 0xF];
    buf[8] = 0;

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

uint32_t yghv_sentry_surface_list(yghv_sentry_surface_entry_t *out, uint32_t max) {
    uint32_t n = 0;
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

void yghv_sentry_escape_stats_get(yghv_sentry_escape_stats_t *out) {
    int i;

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
