/*
 * mem_coop.c — Phase 7: guard cooperation.
 * See common/mem_coop.h for the design and why only we can do this.
 */

#include "mem_coop.h"
#include "mem_pa.h"
#include "protect.h"

static volatile LONG64 g_coop_notifies;
static volatile LONG64 g_coop_guarded;
static volatile LONG64 g_coop_unguarded;
static volatile LONG64 g_coop_avoided;

int yghv_coop_is_guarded(uint64_t gpa) {
    return yghv_protect_sync_is_guarded(gpa);
}

int yghv_coop_notify_write(uint64_t gpa) {
    InterlockedIncrement64(&g_coop_notifies);

    if (!yghv_protect_sync_is_guarded(gpa)) {
        InterlockedIncrement64(&g_coop_unguarded);
        return 0;                       /* not guarded: plain write */
    }

    /* The page IS guarded. Refresh the baseline so the poll finds no drift
     * and leaves our write alone. */
    if (yghv_protect_sync_resnapshot(gpa)) {
        InterlockedIncrement64(&g_coop_guarded);
        InterlockedIncrement64(&g_coop_avoided);
        return 1;
    }

    /* Guarded but the refresh failed (slot vanished between the two calls):
     * report not-guarded so the caller still writes; the guard may roll it
     * back, which is the safe direction. */
    InterlockedIncrement64(&g_coop_unguarded);
    return 0;
}

int yghv_coop_write(uint64_t pa, const void *src, SIZE_T len) {
    SIZE_T off = 0;

    if (!src || !len || !pa)
        return 0;

    while (off < len) {
        uint64_t cur = pa + off;
        uint64_t page_left = 0x1000 - (cur & 0xFFF);
        SIZE_T chunk = (len - off) < page_left ? (len - off) : (SIZE_T)page_left;
        int guarded;

        /*
         * ORDER IS THE WHOLE TRICK: write first, re-snapshot second.
         *
         * The guard restores whenever the page differs from its snapshot. If
         * we re-snapshot BEFORE writing, the snapshot still holds the OLD
         * content, so the very next poll sees our new bytes as drift and
         * rolls them back — measured exactly that (wrote AAAABBBBCCCCDDDD,
         * read back the previous value, with the coop counter showing a
         * "guarded" hit, i.e. the notification had worked and the ordering
         * had not).
         *
         * Writing first and re-snapshotting after makes the NEW content the
         * baseline, so the poll finds no drift. The window between the store
         * and the re-snapshot is the exposure: if the poll runs inside it, it
         * restores once. That is survivable (the write is retried by the
         * caller at the VA level) and is far better than never persisting.
         */
        guarded = yghv_protect_sync_is_guarded(cur);

        if (!yghv_pa_write(cur, (const uint8_t *)src + off, chunk))
            return 0;

        if (guarded) {
            if (yghv_protect_sync_resnapshot(cur)) {
                InterlockedIncrement64(&g_coop_guarded);
                InterlockedIncrement64(&g_coop_avoided);
            } else {
                InterlockedIncrement64(&g_coop_unguarded);
            }
        } else {
            InterlockedIncrement64(&g_coop_unguarded);
        }

        off += chunk;
    }
    return 1;
}

void yghv_coop_stats_get(yghv_coop_stats_t *out) {
    if (!out)
        return;
    out->notifies         = (uint64_t)InterlockedCompareExchange64(&g_coop_notifies, 0, 0);
    out->guarded_hits     = (uint64_t)InterlockedCompareExchange64(&g_coop_guarded, 0, 0);
    out->unguarded        = (uint64_t)InterlockedCompareExchange64(&g_coop_unguarded, 0, 0);
    out->rollbacks_avoided= (uint64_t)InterlockedCompareExchange64(&g_coop_avoided, 0, 0);
}

void yghv_coop_stats_reset(void) {
    InterlockedExchange64(&g_coop_notifies, 0);
    InterlockedExchange64(&g_coop_guarded, 0);
    InterlockedExchange64(&g_coop_unguarded, 0);
    InterlockedExchange64(&g_coop_avoided, 0);
}
