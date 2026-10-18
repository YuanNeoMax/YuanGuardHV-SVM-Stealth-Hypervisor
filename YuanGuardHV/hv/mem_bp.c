/*
 * mem_bp.c — Tier 4.4: instruction-level access control.
 * See common/mem_bp.h for the three mechanisms and when to use which.
 */

#include "mem_bp.h"
#include "protect.h"
#include "npt.h"
#include "common/mem_engine.h"
#include "mem_pa.h"

/*
 * 9.360: compile-time assert. A negatively-sized array fails the BUILD with a
 * line and a number, which is the form that reads clearly in a build log.
 */
#define YGHV_STATIC_ASSERT_BP(cond, tag) \
    typedef char yghv_assert_##tag[(cond) ? 1 : -1]


/* 9.355: mem_bp.c has its own include scheme, so this is declared directly
 * rather than by including common/yghv_guard.h (which pulls <ntddk.h> in the
 * wrong order and redefines PEPROCESS — measured when wiring svm_simplevm206).
 * Used to refuse arming a page when the breakpoint registry itself is hidden. */
extern int yghv_guard_is_hidden(uint64_t gpa);

/*
 * 9.344: put this table on a page of its OWN.
 *
 * THIS IS THE THIRD TIME the same mistake appeared, and the fix is structural
 * rather than another special case.
 *
 * 9.339 fixed it for HASHING: .bss packs unrelated globals into one page, so a
 * whole-page hash reported changes from its neighbours and a whole page could
 * not be hidden without removing them too. Hashing was narrowed to the object's
 * bytes — but HIDING was still per-PAGE, and the 15th crash was exactly that:
 *
 *     yghv_mem_read+0x2A: lock cmpxchg [yghv_g25+0x2fed8], ecx
 *                         0x2fed8 = g_engine_sel
 *
 *   g_engine_sel shares page 0x2F000 with g_view_pages, so hiding the view table
 *   removed the engine selector — a counter yghv_mem_read touches on every call.
 *   Measured: page 0x2F000 held 62 symbols, 0x33000 held 12, 0x35000 held 22.
 *
 * Alignment + padding gives each table its own page, so "hide this object" and
 * "hide this page" become the same statement. Only after that is hiding our own
 * tables a well-defined operation at all.
 *
 * The array is not accessed through a pointer: every use in this file and the
 * extern declarations elsewhere keep compiling unchanged, and DECLSPEC_ALIGN
 * plus the padding forces the layout.
 */
/*
 * 9.360: PAGE-EXACT PLACEMENT, and the assertion that keeps it.
 *
 * DECLSPEC_ALIGN(4096) alone aligns the START, not the SIZE. Measured on the map:
 *
 *     g_bp @ 0x37000, next symbol @ 0x37E00   -> 3584 bytes used, 512 spare
 *     page 0x37000 held g_bp (HOT) AND g_dr_shadow, g_dbg_args,
 *         g_bp_fail_mode/irql/engine (COLD)
 *     page 0x38000 held g_bp_refusals (HOT) AND g_bp_fail_split/table/perm/...
 *
 * The linker fills spare space with whatever fits, and the result is a HOT
 * variable sharing a page with COLD ones. That matters here specifically: the
 * whole point of hot/cold separation is that hiding a HOT page must not remove
 * COLD state that the same fault path needs. A 512-byte gap silently defeats it.
 *
 * The fix is to make each region occupy EXACTLY one page, so there is no spare
 * space and the linker has nothing to put there. A union of the array and a
 * page-sized byte array does it: the union takes the size of its largest member
 * and the alignment of its strictest one, so 4096-aligned + 4096 bytes means the
 * object IS the page.
 *
 * The assertions below are the part that keeps it true -- they fail the BUILD,
 * with a number, rather than leaving a page-sharing condition for a machine to
 * find. Same discipline as 9.347/9.358: the earlier occurrences of this mistake
 * were each paid for with a crash or a hang.
 */
typedef union {
    yghv_bp_t entries[YGHV_BP_MAX];
    uint8_t   page[4096];
} yghv_bp_pageregion_t;

DECLSPEC_ALIGN(4096) static yghv_bp_pageregion_t g_bp_region;
DECLSPEC_ALIGN(4096) static yghv_bp_pageregion_t g_bp_region_pad;   /* separation */

#define g_bp  (g_bp_region.entries)

YGHV_STATIC_ASSERT_BP(sizeof(g_bp) <= 4096, g_bp_fits_one_page);
YGHV_STATIC_ASSERT_BP(sizeof(g_bp_region) == 4096, g_bp_is_exactly_one_page);
/*
 * ============================================================================
 * 9.359: HOT / COLD SEPARATION.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT COSMETIC.
 *
 * The guard layer can hide a page of ours by clearing PRESENT in the NPT. Our
 * own next access then faults, the NPF handler recognises the RIP as ours, serves
 * the real frame for one instruction, and the #DB handler re-hides. That works —
 * measured — for pages read from IOCTL context.
 *
 * It does NOT work for a page read from inside the NPF handler itself, and the
 * measurement that settled it was isolating one variable:
 *
 *     hidecore excludes g_bp  ->  breakpoint cycle after hiding PASSES
 *     hidecore includes g_bp  ->  the bp trigger HANGS (driver still answering)
 *
 * The earlier explanation I wrote for this ("the hot path takes a nested VMEXIT
 * round trip") was DISPROVED by the counters: probes stayed at 0, so the guard
 * branch is never entered, so those reads never fault at all. The mechanism that
 * does fit the measurement:
 *
 *     mem-bp add runs from IOCTL (guest mode) and writes g_bp[]; when the page is
 *     hidden those writes land in the decoy and are discarded. The LAST step of
 *     the add clears WRITABLE on the target page, and THAT call runs at PASSIVE,
 *     so it succeeds. The page is then armed with no registered breakpoint. Every
 *     guest write faults, yghv_bp_on_npf finds nothing to match (its g_bp reads
 *     see the same discarded decoy), returns 0, and the fault falls through to
 *     "reflect as PF" — so the guest retries forever.
 *
 *     9.355 added a refusal for exactly that state (-11). This is the other half:
 *     make the state impossible by not letting the two halves of a breakpoint live
 *     on the same page.
 *
 * THE SPLIT.
 *
 * Everything the island reads goes on ONE page, aligned so nothing else shares it:
 *
 *     g_bp        the registry        yghv_bp_find, yghv_bp_on_npf/int3/dr
 *     g_bp_count  slot count          same
 *     g_head      ring write index    yghv_bp_record
 *     g_tail      ring read index     same
 *     g_bp_hits       hit counters    same
 *     g_bp_refusals
 *     g_bp_recorded
 *     g_bp_missed
 *     g_bp_seq
 *     g_ring      the hit records     same
 *
 * Everything only IOCTL context reads stays elsewhere (unaligned, packed):
 * the per-stage failure counters, the DR shadow bank, g_dbg_args.
 *
 * WHY THIS MAKES HIDING g_bp POSSIBLE: the registry and the counters that decide
 * what to do about a fault now move together. There is no cold state on that page
 * whose arming could disagree with the registry's visibility — which is what
 * produced the "armed page with no breakpoint" state above.
 *
 * NOTE ON WHAT IS STILL NOT SOLVED: the NPF handler reading a hidden page is
 * still a fault the handler must serve, and whether THAT is safe for g_bp is
 * exactly what the soak must decide. The split removes the arming/registration
 * divergence; it does not by itself prove the hot read path survives hiding.
 * ============================================================================
 */

/* ---- HOT: read from inside the NPF / #DB / #BP handlers ---- */
/* Hit ring: 6 qwords per entry (single-producer: island appends, PASSIVE drains). */
#define YGHV_BP_RING 128

/*
 * 9.360: the hot scalars are ONE page-exact region, not nine aligned scalars.
 *
 * Declaring each as DECLSPEC_ALIGN(4096) aligns it but does not pad it: a 4-byte
 * counter still leaves 4092 bytes for the linker to fill, and measured, it filled
 * them with COLD state. page 0x38000 held g_bp_refusals (HOT, read by the NPF
 * handler on every breakpoint hit) together with g_bp_fail_split, g_bp_fail_table,
 * g_bp_fail_perm, g_bp_fail_va, g_bp_fail_patch, g_bp_fail_drslot and g_dr_used.
 *
 * Grouping them into one union member set makes the region exactly one page, so
 * hot and cold cannot be interleaved by the linker at all. The assertion below
 * fails the BUILD if the struct ever outgrows a page, because the failure that
 * follows (hiding a hot page removes cold state the same fault path needs) has
 * already cost this project a hang.
 */
typedef struct {
    volatile LONG   head;
    volatile LONG   tail;
    volatile LONG   bp_count;
    volatile LONG64 bp_hits;
    volatile LONG64 bp_refusals;
    volatile LONG64 bp_recorded;
    volatile LONG64 bp_missed;
    volatile LONG64 bp_seq;
} yghv_bp_hot_t;

typedef union {
    yghv_bp_hot_t s;
    uint8_t       page[4096];
} yghv_bp_hot_region_t;

/* ring stride stays 6 qwords per entry; the array is padded to whole pages */
#define YGHV_BP_RING_QWORDS (YGHV_BP_RING * 8)

typedef union {
    uint64_t ring[YGHV_BP_RING_QWORDS];
    uint8_t  page[sizeof(uint64_t) * YGHV_BP_RING_QWORDS];
} yghv_bp_ring_region_t;

/*
 * 9.360: THE HOT REGION IS TWO PAGES, and the assertion is what found that out.
 *
 * The first attempt put the scalars AND the hit ring in one struct with a single
 * page assertion. It failed the BUILD:
 *
 *     error: 'yghv_assert_bp_hot_fits_one_page' declared as an array with a
 *            negative size
 *
 * The ring is YGHV_BP_RING * 6 qwords = 128 * 6 * 8 = 6144 bytes, one and a half
 * pages before the scalars are counted. So "hot state" cannot be one page, and
 * pretending otherwise would have left the ring's second half sharing a page with
 * whatever the linker put there — the exact condition this separation exists to
 * prevent. The assertion caught a design error in the code that was supposed to
 * fix design errors, which is the most useful thing a new assertion can do.
 *
 * The split that matches reality:
 *
 *   PAGE A  the scalars the handlers read on every fault: head, tail, bp_count,
 *           the hit/refusal/recorded/missed counters, the sequence number.
 *   PAGE B  the hit ring, rounded UP to whole pages so its tail leaves no spare
 *           bytes: 128 * 8 qwords = 1024 * 8 = 8192 = exactly two pages.
 *
 * yghv_bp_record still writes e[0..5] per entry; the extra two qwords per entry
 * are padding that exists so the region ends on a page boundary.
 */
DECLSPEC_ALIGN(4096) static yghv_bp_hot_region_t  g_hot;
DECLSPEC_ALIGN(4096) static yghv_bp_ring_region_t g_ring_region;
DECLSPEC_ALIGN(4096) static yghv_bp_hot_region_t  g_hot_pad;   /* separation */
DECLSPEC_ALIGN(4096) static yghv_bp_ring_region_t g_ring_pad;  /* separation */

#define g_head         (g_hot.s.head)
#define g_tail         (g_hot.s.tail)
#define g_bp_count     (g_hot.s.bp_count)
#define g_bp_hits      (g_hot.s.bp_hits)
#define g_bp_refusals  (g_hot.s.bp_refusals)
#define g_bp_recorded  (g_hot.s.bp_recorded)
#define g_bp_missed    (g_hot.s.bp_missed)
#define g_bp_seq       (g_hot.s.bp_seq)
#define g_ring         (g_ring_region.ring)

YGHV_STATIC_ASSERT_BP(sizeof(yghv_bp_hot_t) <= 4096, bp_hot_fits_one_page);
YGHV_STATIC_ASSERT_BP(sizeof(yghv_bp_hot_region_t) == 4096, bp_hot_is_exactly_one_page);
YGHV_STATIC_ASSERT_BP((sizeof(uint64_t) * YGHV_BP_RING_QWORDS) % 4096 == 0, bp_ring_whole_pages);
YGHV_STATIC_ASSERT_BP(sizeof(yghv_bp_ring_region_t) == sizeof(uint64_t) * YGHV_BP_RING_QWORDS, bp_ring_region_exact);
/* ---- COLD: read only from IOCTL context ---- */
static volatile LONG64 g_bp_fail_irql;
static volatile LONG64 g_bp_fail_engine;
static volatile LONG64 g_bp_fail_split;
static volatile LONG64 g_bp_fail_perm;
static volatile LONG64 g_bp_fail_table;
static volatile LONG64 g_bp_fail_mode;
static volatile LONG64 g_bp_fail_va;
static volatile LONG64 g_bp_fail_drslot;
static volatile LONG64 g_bp_fail_patch;

/* Virtual DR bank (9.327). The guest's DR0-DR3 accesses are intercepted, so
 * these values are what the target sees; the real DRs are ours. COLD: only the
 * DR0-DR3 intercept handlers and IOCTL read these. */
static uint64_t g_dr_shadow[YGHV_BP_DR_SLOTS];
static volatile LONG g_dr_used;

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
        /*
         * 9.355: REFUSE to arm a page whose registry we cannot write.
         *
         * MEASURED mechanism (probes=0 ruled out the TLB and hot-path theories):
         *
         *   hidecore UNMAPs the page holding g_bp. This function runs from IOCTL
         *   context — guest mode — so its writes to g_bp[] land in the DECOY and
         *   are discarded. Then the LAST step here clears WRITABLE on the target
         *   page, and that call runs at PASSIVE so it SUCCEEDS.
         *
         *   The page is now armed with NO registered breakpoint: every guest write
         *   faults, yghv_bp_on_npf() finds nothing to match (its g_bp reads see
         *   the same discarded decoy), returns 0, and the fault falls through to
         *   "reflect as #PF". The guest retries forever.
         *
         *   Measured: trigger hung 15s with the driver still answering every other
         *   IOCTL; probes stayed 0; bp drain returned 0 entries; "add OK" was
         *   reported on a breakpoint that never existed.
         *
         * So arming a page and recording the breakpoint are not allowed to
         * disagree. If g_bp's own page is hidden, the registration half cannot
         * complete and the arm must not happen at all.
         */
        if (yghv_guard_is_hidden(MmGetPhysicalAddress(&g_bp[0]).QuadPart)) {
            /*
             * 9.355: a code of its OWN (-11).
             *
             * The first version returned -7 here, which already means "INT3 needs
             * a VA" at the INT3 branch below — so the PS-side message said
             * "FAILED (stage -7: INT3 needs a VA)" for a failure that had nothing
             * to do with INT3. Measured on the y8 verification run. Reusing a
             * stage code makes a diagnosis worse than no diagnosis: it names the
             * wrong cause with full confidence.
             */
            InterlockedIncrement64(&g_bp_fail_table);
            return -11;     /* registry hidden: cannot register, so must not arm */
        }

        /*
         * 9.359: REGISTER FIRST, ARM SECOND -- and verify the registration
         * actually landed before arming.
         *
         * The order here was the other way round: yghv_protect_npt_set_perm()
         * revoked the page's write permission at L332, and only then did the loop
         * below claim a registry slot. That is the precise origin of the "armed
         * page with no breakpoint" state that hung the machine:
         *
         *   if the registry write cannot land (its page is hidden, so the write
         *   goes into the decoy and is discarded), the arm has ALREADY happened.
         *   The page faults on every guest write, yghv_bp_on_npf finds nothing to
         *   match, returns 0, and the fault falls through to "reflect as PF" -
         *   so the guest retries forever, with the driver still answering every
         *   other IOCTL. Measured exactly that: trigger hung 15s, probes stayed 0,
         *   bp drain returned 0 entries, and "add OK" was reported for a
         *   breakpoint that never existed.
         *
         * The -11 refusal above catches that case, but a refusal is a patch over
         * an ordering fault. Doing the work in the order that cannot produce the
         * bad state is the fix: claim and VERIFY the slot first, and only then
         * touch the target page's permissions. If the registration did not land,
         * nothing was armed and there is nothing to undo.
         *
         * The verification is a readback, not a trust in the write: a discarded
         * write leaves no trace, and the whole failure mode here is a write that
         * reported success while changing nothing (9.350's lesson).
         */
        {
            int claimed = -1;
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
                claimed = i;
                break;
            }

            if (claimed < 0) {
                InterlockedIncrement64(&g_bp_fail_table);
                return -5;      /* table full: nothing was armed, nothing to undo */
            }

            /*
             * Read the slot back through the SAME path the island will use. If
             * this disagrees with what was just written, the registry is not
             * visible to us and arming would produce an unattended fault.
             */
            if (InterlockedCompareExchange(&g_bp[claimed].active, 0, 0) != 1 ||
                g_bp[claimed].gpa != page) {
                InterlockedExchange(&g_bp[claimed].active, 0);
                InterlockedIncrement64(&g_bp_fail_table);
                return -11;     /* registration did not land: refuse to arm */
            }

            if (!yghv_protect_npt_set_perm(page, NPT_PERM_PRESENT)) {
                /* Arm failed: release the slot so the table does not fill with
                 * registrations that were never enforced. */
                InterlockedExchange(&g_bp[claimed].active, 0);
                InterlockedIncrement64(&g_bp_fail_perm);
                return -4;
            }

            InterlockedIncrement(&g_bp_count);
            return 1;
        }
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


/*
 * 9.336 (Tier 7.1): expose the table's address and size so the guard can hide
 * and hash it. This table is ordinary pool memory, guest-reachable, and every
 * newer feature depends on it — main.c's exclude pass predates its existence.
 */
uint64_t *yghv_bp_table_va(uint32_t *bytes) {
    if (bytes)
        *bytes = (uint32_t)sizeof(g_bp);
    return (uint64_t *)(void *)g_bp;
}
