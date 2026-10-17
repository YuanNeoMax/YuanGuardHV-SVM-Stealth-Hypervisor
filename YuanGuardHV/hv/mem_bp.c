/*
 * mem_bp.c — Tier 4.4: instruction-level access control.
 * See common/mem_bp.h for the three mechanisms and when to use which.
 */

#include "mem_bp.h"
#include "protect.h"
#include "npt.h"
#include "common/mem_engine.h"
#include "mem_pa.h"

static yghv_bp_t g_bp[YGHV_BP_MAX];
static volatile LONG g_bp_count;

/* Virtual DR bank (9.327). The guest's DR0-DR3 accesses are intercepted, so
 * these values are what the target sees; the real DRs are ours. */
static uint64_t g_dr_shadow[YGHV_BP_DR_SLOTS];
static volatile LONG g_dr_used;

/* Hit ring (single-producer discipline: island appends, PASSIVE drains).
 * 6 qwords per entry. */
#define YGHV_BP_RING 128
static uint64_t g_ring[YGHV_BP_RING * 6];
static volatile LONG g_head;
static volatile LONG g_tail;

static volatile LONG64 g_bp_hits;
static volatile LONG64 g_bp_recorded;
static volatile LONG64 g_bp_missed;
static volatile LONG64 g_bp_refusals;

/* 9.325: one counter per guard, so a single run names the failing stage
 * instead of requiring another crash to narrow it down. */
static volatile LONG64 g_bp_fail_irql;
static volatile LONG64 g_bp_fail_engine;
static volatile LONG64 g_bp_fail_split;
static volatile LONG64 g_bp_fail_perm;
static volatile LONG64 g_bp_fail_table;
static volatile LONG64 g_bp_fail_mode;
static volatile LONG64 g_bp_fail_va;
static volatile LONG64 g_bp_fail_drslot;
static volatile LONG64 g_bp_fail_patch;

static volatile LONG64 g_bp_seq;
uint64_t g_dbg_args[8];   /* 9.327: last bp_add_ex arguments, for diagnosis */

/* ---------- lookup ---------- */

static int yghv_bp_find(uint64_t gpa, uint64_t rip) {
    int i;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);

    for (i = 0; i < n && i < YGHV_BP_MAX; i++) {
        if (!InterlockedCompareExchange(&g_bp[i].active, 0, 0))
            continue;
        if (g_bp[i].mode != YGHV_BP_MODE_NPF)
            continue;
        if (g_bp[i].gpa == page &&
            (g_bp[i].rip == 0 || g_bp[i].rip == rip))
            return i;
    }
    return -1;
}

int yghv_bp_find_int3(uint64_t va) {
    int i;
    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);

    for (i = 0; i < n && i < YGHV_BP_MAX; i++) {
        if (!InterlockedCompareExchange(&g_bp[i].active, 0, 0))
            continue;
        if (g_bp[i].mode == YGHV_BP_MODE_INT3 && g_bp[i].va == va)
            return i;
    }
    return -1;
}

int yghv_bp_int3_armed(uint64_t va) {
    return yghv_bp_find_int3(va) >= 0 ? 1 : 0;
}

int yghv_bp_int3_orig(uint64_t va, uint8_t *out) {
    int slot = yghv_bp_find_int3(va);
    if (slot < 0 || !out)
        return 0;
    *out = g_bp[slot].orig_byte;
    return 1;
}

int yghv_bp_is_armed(uint64_t gpa) {
    return yghv_bp_find(gpa, 0) >= 0 ? 1 : 0;
}

int yghv_bp_dr_slot_for_va(uint64_t va) {
    int i;
    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);

    for (i = 0; i < n && i < YGHV_BP_MAX; i++) {
        if (InterlockedCompareExchange(&g_bp[i].active, 0, 0) &&
            g_bp[i].mode == YGHV_BP_MODE_DR && g_bp[i].va == va)
            return (int)g_bp[i].dr_slot;
    }
    return -1;
}

uint64_t yghv_bp_dr_get(int slot) {
    if (slot < 0 || slot >= YGHV_BP_DR_SLOTS)
        return 0;
    return g_dr_shadow[slot];
}

int yghv_bp_dr_set(int slot, uint64_t value) {
    if (slot < 0 || slot >= YGHV_BP_DR_SLOTS)
        return 0;
    g_dr_shadow[slot] = value;
    return 1;
}

/* ---------- record ---------- */

static void yghv_bp_record(uint64_t rip, uint64_t gpa, uint64_t va,
                           uint32_t kind, uint32_t cpl, uint32_t mode,
                           uint64_t cr3) {
    LONG head = InterlockedCompareExchange(&g_head, 0, 0);
    LONG tail = InterlockedCompareExchange(&g_tail, 0, 0);
    uint64_t *e;

    if (((head + 1) & (YGHV_BP_RING - 1)) == (tail & (YGHV_BP_RING - 1))) {
        InterlockedIncrement64(&g_bp_missed);
        return;                         /* full: drop, never block */
    }

    e = &g_ring[(head & (YGHV_BP_RING - 1)) * 6];
    e[0] = rip;
    e[1] = gpa & ~(uint64_t)0xFFFULL;
    e[2] = (uint64_t)kind | ((uint64_t)(cpl & 0xFF) << 8)
                          | ((uint64_t)(mode & 0xFF) << 16);
    e[3] = cr3;
    e[4] = va;
    e[5] = (uint64_t)InterlockedIncrement64(&g_bp_seq);

    InterlockedExchange(&g_head, head + 1);
    InterlockedIncrement64(&g_bp_recorded);
}

/* ---------- add ---------- */

int yghv_bp_add(uint64_t gpa, uint64_t rip, uint32_t kinds) {
    return yghv_bp_add_ex(gpa, 0, rip, kinds, YGHV_BP_MODE_NPF);
}

int yghv_bp_add_ex(uint64_t gpa, uint64_t va, uint64_t rip,
                   uint32_t kinds, uint32_t mode) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot = -1;
    int i;

    /* 9.327 debug: stash the raw arguments so a single run shows what the
     * driver actually received (the PS side already prints what it sent). */
    g_dbg_args[0] = gpa;
    g_dbg_args[1] = va;
    g_dbg_args[2] = rip;
    g_dbg_args[3] = (uint64_t)kinds | ((uint64_t)mode << 32);

    if (!kinds) {
        InterlockedIncrement64(&g_bp_fail_mode);
        return -10;                     /* 9.327: distinct from a bad mode */
    }
    if (mode != YGHV_BP_MODE_NPF && mode != YGHV_BP_MODE_INT3 &&
        mode != YGHV_BP_MODE_DR) {
        InterlockedIncrement64(&g_bp_fail_mode);
        /* 9.327: encode the offending value so one run names it. */
        return -(20 + (int)(mode & 0xFF));
    }
    if (KeAreAllApcsDisabled()) {
        InterlockedIncrement64(&g_bp_fail_irql);
        return -1;
    }

    /* ---- NPF mode: revoke the page's NPT write permission ---- */
    if (mode == YGHV_BP_MODE_NPF) {
        if (!page) {
            InterlockedIncrement64(&g_bp_fail_mode);
            return -6;
        }
        if (yghv_bp_find(page, rip) >= 0)
            return 1;                   /* idempotent */

        if (yghv_mem_engine_get() != YGHV_ENGINE_PTE) {
            InterlockedIncrement64(&g_bp_fail_engine);
            InterlockedIncrement64(&g_bp_refusals);
            return -2;
        }
        /* 9.319: split first. npt_set_page_perm targets the PDE when the
         * containing entry is a large page, which would make the WHOLE 2MB
         * read-only. */
        if (!yghv_protect_npt_split_4k(page)) {
            InterlockedIncrement64(&g_bp_fail_split);
            return -3;
        }
        if (!yghv_protect_npt_set_perm(page, NPT_PERM_PRESENT)) {
            InterlockedIncrement64(&g_bp_fail_perm);
            return -4;
        }

        /* 9.327 FIX: claim the table slot HERE. The first version left this
         * to the shared tail, which does not exist — the permission went in
         * and the function then fell through to the final `return -6`
         * ("bad mode"), so a perfectly valid NPF add reported failure while
         * having already revoked the page's write permission. That is the
         * worst of both: reported as broken, actually half-applied. */
        for (i = 0; i < YGHV_BP_MAX; i++) {
            if (InterlockedCompareExchange(&g_bp[i].active, 0, 0))
                continue;
            g_bp[i].gpa = page;
            g_bp[i].va = 0;
            g_bp[i].rip = rip;
            g_bp[i].kinds = kinds;
            g_bp[i].mode = mode;
            g_bp[i].orig_byte = 0;
            g_bp[i].dr_slot = YGHV_BP_NO_SLOT;
            g_bp[i].hits = 0;
            InterlockedExchange(&g_bp[i].active, 1);
            InterlockedIncrement(&g_bp_count);
            return 1;
        }
        /* Table full: undo the permission change so the page is not left
         * faulting with nobody listening. */
        InterlockedIncrement64(&g_bp_fail_table);
        (void)yghv_protect_npt_set_perm(page,
                                        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
        return -5;
    }

    /* ---- INT3 mode: overwrite the instruction's first byte ---- */
    if (mode == YGHV_BP_MODE_INT3) {
        uint8_t orig = 0;
        uint64_t off;

        if (!va || !page) {
            InterlockedIncrement64(&g_bp_fail_va);
            return -7;
        }
        if (yghv_bp_find_int3(va) >= 0)
            return 1;                   /* idempotent */

        off = va & 0xFFF;
        if (!yghv_pa_read(page + off, &orig, 1)) {
            InterlockedIncrement64(&g_bp_fail_patch);
            return -9;
        }
        /* Remember the byte BEFORE patching, so a failure here cannot leave
         * an instruction we cannot restore. */
        g_dr_shadow[0] = g_dr_shadow[0];    /* no-op, keeps the compiler quiet */

        for (i = 0; i < YGHV_BP_MAX; i++) {
            if (InterlockedCompareExchange(&g_bp[i].active, 0, 0))
                continue;
            g_bp[i].gpa = page;
            g_bp[i].va = va;
            g_bp[i].rip = va;
            g_bp[i].kinds = kinds;
            g_bp[i].mode = mode;
            g_bp[i].orig_byte = orig;
            g_bp[i].dr_slot = YGHV_BP_NO_SLOT;
            g_bp[i].hits = 0;
            InterlockedExchange(&g_bp[i].active, 1);
            InterlockedIncrement(&g_bp_count);
            /* Publish the slot BEFORE patching, so the #BP handler can always
             * find it once the 0xCC is in place. */
            {
                uint8_t cc = 0xCC;
                if (!yghv_pa_write(page + off, &cc, 1)) {
                    InterlockedExchange(&g_bp[i].active, 0);
                    InterlockedDecrement(&g_bp_count);
                    InterlockedIncrement64(&g_bp_fail_patch);
                    return -9;
                }
            }
            return 1;
        }
        InterlockedIncrement64(&g_bp_fail_table);
        return -5;
    }

    /* ---- DR mode: claim one of the four hardware slots ---- */
    if (mode == YGHV_BP_MODE_DR) {
        uint32_t s;
        uint32_t used = (uint32_t)InterlockedCompareExchange(&g_dr_used, 0, 0);

        if (yghv_bp_dr_slot_for_va(va) >= 0)
            return 1;                   /* idempotent */

        for (s = 0; s < YGHV_BP_DR_SLOTS; s++) {
            if (!(used & (1u << s)))
                break;
        }
        if (s >= YGHV_BP_DR_SLOTS) {
            InterlockedIncrement64(&g_bp_fail_drslot);
            return -8;
        }

        for (i = 0; i < YGHV_BP_MAX; i++) {
            if (InterlockedCompareExchange(&g_bp[i].active, 0, 0))
                continue;
            g_bp[i].gpa = page;
            g_bp[i].va = va;
            g_bp[i].rip = va;
            g_bp[i].kinds = kinds;
            g_bp[i].mode = mode;
            g_bp[i].orig_byte = 0;
            g_bp[i].dr_slot = s;
            g_bp[i].hits = 0;
            InterlockedExchange(&g_bp[i].active, 1);
            InterlockedIncrement(&g_bp_count);
            InterlockedOr(&g_dr_used, (LONG)(1u << s));
            g_dr_shadow[s] = va;
            return 1;
        }
        InterlockedIncrement64(&g_bp_fail_table);
        return -5;
    }

    (void)slot;
    InterlockedIncrement64(&g_bp_fail_mode);
    return -6;
}

/* ---------- remove ---------- */

int yghv_bp_remove(uint64_t gpa, uint64_t rip) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot = yghv_bp_find(page, rip);
    int i;

    if (slot < 0) {
        /* Also try INT3, whose key is the VA not the page. */
        slot = yghv_bp_find_int3(rip);
        if (slot < 0 && gpa)
            slot = yghv_bp_find_int3(gpa);
    }
    if (slot < 0) {
        /* 9.327 FIX: DR entries are keyed by VA too, and were not reachable
         * from remove at all — clear_mode() reported 0 removed because every
         * DR slot lookup failed. */
        int d = yghv_bp_dr_slot_for_va(rip);
        if (d < 0 && gpa)
            d = yghv_bp_dr_slot_for_va(gpa);
        if (d >= 0) {
            for (i = 0; i < YGHV_BP_MAX; i++) {
                if (InterlockedCompareExchange(&g_bp[i].active, 0, 0) &&
                    g_bp[i].mode == YGHV_BP_MODE_DR &&
                    g_bp[i].dr_slot == (uint32_t)d) {
                    slot = i;
                    break;
                }
            }
        }
    }
    if (slot < 0)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;

    if (g_bp[slot].mode == YGHV_BP_MODE_NPF) {
        /* Give the page its normal access back BEFORE dropping the slot. */
        (void)yghv_protect_npt_set_perm(g_bp[slot].gpa,
                                        NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
    } else if (g_bp[slot].mode == YGHV_BP_MODE_INT3) {
        uint64_t off = g_bp[slot].va & 0xFFF;
        uint8_t orig = g_bp[slot].orig_byte;
        /* Drop the slot FIRST so a concurrent #BP cannot find a half-removed
         * entry, then restore the instruction byte. */
        InterlockedExchange(&g_bp[slot].active, 0);
        InterlockedDecrement(&g_bp_count);
        (void)yghv_pa_write(g_bp[slot].gpa + off, &orig, 1);
        RtlZeroMemory(&g_bp[slot], sizeof(g_bp[slot]));
        return 1;
    } else if (g_bp[slot].mode == YGHV_BP_MODE_DR) {
        uint32_t s = g_bp[slot].dr_slot;
        if (s < YGHV_BP_DR_SLOTS) {
            g_dr_shadow[s] = 0;
            InterlockedAnd(&g_dr_used, (LONG)~(1u << s));
        }
    }

    InterlockedExchange(&g_bp[slot].active, 0);
    InterlockedDecrement(&g_bp_count);
    RtlZeroMemory(&g_bp[slot], sizeof(g_bp[slot]));
    return 1;
}

int yghv_bp_clear_mode(uint32_t mode) {
    int i, n = 0;

    if (KeAreAllApcsDisabled())
        return 0;

    /*
     * 9.327 FIX: undo each entry HERE instead of delegating to
     * yghv_bp_remove(). remove() looks entries up by (gpa, rip) and, for DR
     * mode, by the VA — a chain that missed half the entries and reported
     * "2 cleared" when 4 were registered. Clearing in place removes the
     * lookup entirely: we already hold the index.
     */
    for (i = 0; i < YGHV_BP_MAX; i++) {
        uint32_t m;

        if (!InterlockedCompareExchange(&g_bp[i].active, 0, 0))
            continue;
        m = g_bp[i].mode;
        if (mode && m != mode)
            continue;

        if (m == YGHV_BP_MODE_NPF) {
            (void)yghv_protect_npt_set_perm(g_bp[i].gpa,
                                            NPT_PERM_PRESENT | NPT_PERM_WRITABLE);
        } else if (m == YGHV_BP_MODE_INT3) {
            uint64_t off = g_bp[i].va & 0xFFF;
            uint8_t orig = g_bp[i].orig_byte;
            InterlockedExchange(&g_bp[i].active, 0);
            InterlockedDecrement(&g_bp_count);
            (void)yghv_pa_write(g_bp[i].gpa + off, &orig, 1);
            RtlZeroMemory(&g_bp[i], sizeof(g_bp[i]));
            n++;
            continue;
        } else if (m == YGHV_BP_MODE_DR) {
            uint32_t s = g_bp[i].dr_slot;
            if (s < YGHV_BP_DR_SLOTS) {
                g_dr_shadow[s] = 0;
                InterlockedAnd(&g_dr_used, (LONG)~(1u << s));
            }
        }

        InterlockedExchange(&g_bp[i].active, 0);
        InterlockedDecrement(&g_bp_count);
        RtlZeroMemory(&g_bp[i], sizeof(g_bp[i]));
        n++;
    }
    return n;
}
/* ---------- fault handlers ---------- */

int yghv_bp_on_npf(uint64_t gpa, uint64_t rip, uint32_t kind, uint32_t cpl,
                   uint64_t cr3) {
    int slot;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);

    if (n <= 0)
        return 0;

    slot = yghv_bp_find(page, rip);
    if (slot < 0) {
        InterlockedIncrement64(&g_bp_refusals);
        return 0;
    }
    /*
     * 9.330 FIX: do not filter by kind once the page matches.
     *
     * A read-watch removes PRESENT entirely (the NPT has no read-disable bit),
     * so EVERY access to the page faults — writes included. The old code then
     * required (kinds & kind), a read-watch failed the check for a write fault,
     * returned 0, and the caller fell through to "reflect as #PF". The guest
     * retried forever: measured as a 30-second hang on the very first mmf-write
     * after `mem-watch add ... r`.
     *
     * Once a page is guarded, any fault on it is ours to handle and release.
     * The recorded kind is the ACTUAL access, so a read-watch on a page that
     * is also written reports both — which is the honest semantic: the NPT
     * cannot express "read only", so a read-watch is a full-access watch.
     */
    InterlockedIncrement((volatile LONG *)&g_bp[slot].hits);
    InterlockedIncrement64(&g_bp_hits);
    yghv_bp_record(rip, page, 0, kind, cpl, YGHV_BP_MODE_NPF, cr3);
    return 1;
}

int yghv_bp_on_int3(uint64_t va, uint32_t cpl, uint64_t cr3) {
    int slot;
    uint32_t kind = YGHV_BP_EXEC;
    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);

    if (n <= 0)
        return 0;

    slot = yghv_bp_find_int3(va);
    if (slot < 0) {
        InterlockedIncrement64(&g_bp_refusals);
        return 0;
    }
    if (!(g_bp[slot].kinds & kind))
        kind = g_bp[slot].kinds;        /* report what it was armed for */

    InterlockedIncrement((volatile LONG *)&g_bp[slot].hits);
    InterlockedIncrement64(&g_bp_hits);
    yghv_bp_record(va, g_bp[slot].gpa, va, kind, cpl,
                   YGHV_BP_MODE_INT3, cr3);
    return 1;
}

int yghv_bp_on_dr(uint64_t dr6, uint64_t rip, uint32_t cpl, uint64_t cr3) {
    int i;
    int n = (int)InterlockedCompareExchange(&g_bp_count, 0, 0);

    if (n <= 0)
        return -1;

    /* DR6 bits 0-3 report which of DR0-DR3 matched. */
    for (i = 0; i < n && i < YGHV_BP_MAX; i++) {
        uint32_t s;
        if (!InterlockedCompareExchange(&g_bp[i].active, 0, 0))
            continue;
        if (g_bp[i].mode != YGHV_BP_MODE_DR)
            continue;
        s = g_bp[i].dr_slot;
        if (s >= YGHV_BP_DR_SLOTS)
            continue;
        if (!(dr6 & (1ULL << s)))
            continue;
        InterlockedIncrement((volatile LONG *)&g_bp[i].hits);
        InterlockedIncrement64(&g_bp_hits);
        yghv_bp_record(rip, g_bp[i].gpa, g_bp[i].va, g_bp[i].kinds,
                       cpl, YGHV_BP_MODE_DR, cr3);
        return i;
    }
    return -1;
}

/* ---------- drain ---------- */

uint32_t yghv_bp_drain(uint64_t *out, uint32_t max) {
    uint32_t n = 0;
    LONG tail, head;

    if (!out || !max)
        return 0;

    for (;;) {
        tail = InterlockedCompareExchange(&g_tail, 0, 0);
        head = InterlockedCompareExchange(&g_head, 0, 0);
        if (tail == head || n + 6 > max)
            break;
        RtlCopyMemory(out + n, &g_ring[(tail & (YGHV_BP_RING - 1)) * 6],
                      6 * sizeof(uint64_t));
        n += 6;
        InterlockedExchange(&g_tail, tail + 1);
    }
    return n;
}

/* ---------- stats ---------- */

void yghv_bp_stats_get(yghv_bp_stats_t *out) {
    int i;

    if (!out)
        return;
    RtlZeroMemory(out, sizeof(*out));
    out->count    = (uint64_t)InterlockedCompareExchange(&g_bp_count, 0, 0);
    out->hits     = (uint64_t)InterlockedCompareExchange64(&g_bp_hits, 0, 0);
    out->recorded = (uint64_t)InterlockedCompareExchange64(&g_bp_recorded, 0, 0);
    out->missed   = (uint64_t)InterlockedCompareExchange64(&g_bp_missed, 0, 0);
    out->refusals = (uint64_t)InterlockedCompareExchange64(&g_bp_refusals, 0, 0);
    out->fail_irql    = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_irql, 0, 0);
    out->fail_engine  = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_engine, 0, 0);
    out->fail_split   = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_split, 0, 0);
    out->fail_perm    = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_perm, 0, 0);
    out->fail_table   = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_table, 0, 0);
    out->fail_mode    = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_mode, 0, 0);
    out->fail_va      = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_va, 0, 0);
    out->fail_drslot  = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_drslot, 0, 0);
    out->fail_patch   = (uint64_t)InterlockedCompareExchange64(&g_bp_fail_patch, 0, 0);

    for (i = 0; i < YGHV_BP_MAX; i++) {
        if (!InterlockedCompareExchange(&g_bp[i].active, 0, 0))
            continue;
        if (g_bp[i].mode == YGHV_BP_MODE_NPF)       out->npf++;
        else if (g_bp[i].mode == YGHV_BP_MODE_INT3) out->int3++;
        else if (g_bp[i].mode == YGHV_BP_MODE_DR)   out->dr++;
    }
}

void yghv_bp_stats_reset(void) {
    InterlockedExchange64(&g_bp_hits, 0);
    InterlockedExchange64(&g_bp_recorded, 0);
    InterlockedExchange64(&g_bp_missed, 0);
    InterlockedExchange64(&g_bp_refusals, 0);
    InterlockedExchange64(&g_bp_fail_irql, 0);
    InterlockedExchange64(&g_bp_fail_engine, 0);
    InterlockedExchange64(&g_bp_fail_split, 0);
    InterlockedExchange64(&g_bp_fail_perm, 0);
    InterlockedExchange64(&g_bp_fail_table, 0);
    InterlockedExchange64(&g_bp_fail_mode, 0);
    InterlockedExchange64(&g_bp_fail_va, 0);
    InterlockedExchange64(&g_bp_fail_drslot, 0);
    InterlockedExchange64(&g_bp_fail_patch, 0);
    InterlockedExchange(&g_head, 0);
    InterlockedExchange(&g_tail, 0);
}
