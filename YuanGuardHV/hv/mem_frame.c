/*
 * mem_frame.c — Tier 4.5: physical-frame ownership takeover.
 * See common/mem_frame.h for exactly what is and is not claimed.
 */

#include "mem_frame.h"
#include "mem_pa.h"

typedef struct {
    uint64_t gpa;
    uint64_t seq;
    void    *buf;
    uint32_t len;
    uint32_t alive;
    volatile LONG active;
} yghv_frame_t;

static yghv_frame_t g_frame[YGHV_FRAME_MAX];
static volatile LONG g_frame_count;

static volatile LONG64 g_frm_reserved;
static volatile LONG64 g_frm_released;
static volatile LONG64 g_frm_bytes;
static volatile LONG64 g_frm_fail;
static volatile LONG64 g_frm_seq;

static int yghv_frame_find(uint64_t gpa) {
    int i;
    for (i = 0; i < YGHV_FRAME_MAX; i++) {
        if (InterlockedCompareExchange(&g_frame[i].active, 0, 0) &&
            g_frame[i].gpa == gpa)
            return i;
    }
    return -1;
}

int yghv_frame_reserve(uint64_t gpa, uint32_t len) {
    void *buf;
    int slot = -1;
    int i;

    if (!gpa || !len) {
        InterlockedIncrement64(&g_frm_fail);
        return -1;
    }
    if (len > YGHV_FRAME_MAX_LEN)
        len = YGHV_FRAME_MAX_LEN;
    if (KeAreAllApcsDisabled()) {
        InterlockedIncrement64(&g_frm_fail);
        return -1;
    }

    /* Reserving the same frame twice keeps one copy (the older one), which is
     * the useful semantic: the earliest content is the one you cannot get back
     * once the owner starts writing. */
    slot = yghv_frame_find(gpa);
    if (slot >= 0)
        return slot;

    for (i = 0; i < YGHV_FRAME_MAX; i++) {
        if (!InterlockedCompareExchange(&g_frame[i].active, 0, 0)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        InterlockedIncrement64(&g_frm_fail);
        return -1;
    }

    /* NonPagedPool: the copy must not be pageable, because the whole point is
     * that it stays reachable when the machine is under memory pressure. */
    buf = ExAllocatePoolWithTag(NonPagedPool, len, 'MRFY');
    if (!buf) {
        InterlockedIncrement64(&g_frm_fail);
        return -1;
    }

    /*
     * Snapshot the frame through the validated physical read (MmCopyMemory).
     * This does not touch the guest's mapping, so it works regardless of the
     * owner's page-table state and cannot fault on a revoked NPT permission.
     */
    if (!yghv_pa_read(gpa, buf, len)) {
        ExFreePoolWithTag(buf, 'MRFY');
        InterlockedIncrement64(&g_frm_fail);
        return -1;
    }

    g_frame[slot].gpa = gpa;
    g_frame[slot].buf = buf;
    g_frame[slot].len = len;
    g_frame[slot].alive = 1;
    g_frame[slot].seq = (uint64_t)InterlockedIncrement64(&g_frm_seq);
    InterlockedExchange(&g_frame[slot].active, 1);
    InterlockedIncrement(&g_frame_count);
    InterlockedIncrement64(&g_frm_reserved);
    InterlockedExchangeAdd64(&g_frm_bytes, (LONG64)len);
    return slot;
}

int yghv_frame_release(int slot) {
    if (slot < 0 || slot >= YGHV_FRAME_MAX)
        return 0;
    if (!InterlockedCompareExchange(&g_frame[slot].active, 0, 0))
        return 0;

    InterlockedExchange(&g_frame[slot].active, 0);
    if (g_frame[slot].buf) {
        ExFreePoolWithTag(g_frame[slot].buf, 'MRFY');
        InterlockedExchangeAdd64(&g_frm_bytes, -(LONG64)g_frame[slot].len);
    }
    RtlZeroMemory(&g_frame[slot], sizeof(g_frame[slot]));
    InterlockedDecrement(&g_frame_count);
    InterlockedIncrement64(&g_frm_released);
    return 1;
}

void yghv_frame_release_all(void) {
    int i;
    for (i = 0; i < YGHV_FRAME_MAX; i++) {
        if (InterlockedCompareExchange(&g_frame[i].active, 0, 0))
            (void)yghv_frame_release(i);
    }
}

uint32_t yghv_frame_read(int slot, void *out, uint32_t len, uint32_t off) {
    uint32_t n;

    if (slot < 0 || slot >= YGHV_FRAME_MAX || !out || !len)
        return 0;
    if (!InterlockedCompareExchange(&g_frame[slot].active, 0, 0))
        return 0;
    if (off >= g_frame[slot].len)
        return 0;

    n = g_frame[slot].len - off;
    if (n > len)
        n = len;
    RtlCopyMemory(out, (uint8_t *)g_frame[slot].buf + off, n);
    return n;
}

uint32_t yghv_frame_list(yghv_frame_info_t *out, uint32_t max) {
    uint32_t n = 0;
    int i;

    if (!out || !max)
        return 0;
    for (i = 0; i < YGHV_FRAME_MAX && n < max; i++) {
        if (!InterlockedCompareExchange(&g_frame[i].active, 0, 0))
            continue;
        out[n].gpa = g_frame[i].gpa;
        out[n].seq = g_frame[i].seq;
        out[n].len = g_frame[i].len;
        out[n].captured = g_frame[i].alive;
        out[n].active = 1;
        n++;
    }
    return n;
}

void yghv_frame_stats_get(yghv_frame_stats_t *out) {
    if (!out)
        return;
    out->reserved = (uint64_t)InterlockedCompareExchange64(&g_frm_reserved, 0, 0);
    out->released = (uint64_t)InterlockedCompareExchange64(&g_frm_released, 0, 0);
    out->bytes    = (uint64_t)InterlockedCompareExchange64(&g_frm_bytes, 0, 0);
    out->active   = (uint64_t)InterlockedCompareExchange(&g_frame_count, 0, 0);
    out->failures = (uint64_t)InterlockedCompareExchange64(&g_frm_fail, 0, 0);
}

void yghv_frame_stats_reset(void) {
    InterlockedExchange64(&g_frm_reserved, 0);
    InterlockedExchange64(&g_frm_released, 0);
    InterlockedExchange64(&g_frm_fail, 0);
    InterlockedExchange64(&g_frm_seq, 0);
}


/*
 * 9.336 (Tier 7.1): expose the table's address and size so the guard can hide
 * and hash it. This table is ordinary pool memory, guest-reachable, and every
 * newer feature depends on it — main.c's exclude pass predates its existence.
 */
uint64_t *yghv_frame_table_va(uint32_t *bytes) {
    if (bytes)
        *bytes = (uint32_t)sizeof(g_frame);
    return (uint64_t *)(void *)g_frame;
}