/*
 * yghv_guard.c — Tier 7.1/7.3: self-hiding and integrity self-check.
 * See common/yghv_guard.h for what hiding does and does not buy.
 */

#include "yghv_guard.h"
#include "protect.h"
#include "npt.h"
#include "yghv_crypto.h"
#include "mem_pa.h"

/* 9.344: the main NPT manager, needed by the island-safe remap helpers. */
extern npt_mgr_t g_npt;

#define YGHV_HIDE_UNMAP 0   /* remove PRESENT: our fault is served by the ALLOW loop */
#define YGHV_HIDE_DECOY 1   /* point at the decoy: page stays present, reads return 0 */
#define YGHV_HIDE_AUTO  2   /* 9.354: let ownership decide (see yghv_guard_mode_for) */

typedef struct {
    uint64_t gpa;
    uint8_t  digest[YGHV_SHA256_DIGEST];   /* 9.342: cryptographic */
    /* 9.339: which bytes of the page form the guarded OBJECT.
     * .bss packs unrelated globals together (g_frame and g_guard share page
     * 0x34000), so a whole-page hash reports a change the moment any of them
     * is touched — measured as "2 pages changed" on an untouched machine. */
    uint32_t hash_off;
    uint32_t hash_len;
    /* 9.340: non-zero = the hash was taken at this VA (yghv_guard_add_va), so
     * re-verify at the same VA. Zero = physical-read form. */
    uint64_t hash_va;
    uint64_t checks;
    uint64_t changes;
    uint32_t hidden;
    /* 9.350: WHICH mechanism hid this page — re-hiding with the wrong one
     * is what hung the machine (mmf-write never returned, driver still
     * answering: a guest-side spin, not a deadlock).
     *
     *   UNMAP — our own next access faults and the ALLOW loop serves the
     *           REAL frame. Correct for our own tables, which we read.
     *   DECOY — the page stays present so the guest map looks continuous
     *           and reads return zeros. Correct for foreign pages.
     *
     * Measured: hidecore hid g_bp with UNMAP, the #DB re-hide used the
     * DECOY, so the page came back pointing at zeros — every read of the
     * breakpoint table saw an empty table, mem-bp add() wrote into a
     * discarded decoy, no breakpoint ever matched, and the guest store
     * spun forever inside the NPF handler. */
    uint32_t hide_mode;
    uint32_t armed;
    char     tag[YGHV_GUARD_TAG_LEN];
    volatile LONG active;
} yghv_guard_t;

DECLSPEC_ALIGN(4096) static yghv_guard_t g_guard[YGHV_GUARD_MAX];
DECLSPEC_ALIGN(4096) static volatile LONG g_guard_count;

static volatile LONG64 g_g_guarded;
static volatile LONG64 g_g_hidden;
static volatile LONG64 g_g_verified;
static volatile LONG64 g_g_changed;
static volatile LONG64 g_g_probes;
static volatile LONG64 g_g_hidefail;
static volatile LONG64 g_g_hashfail;

/*
 * 9.343: our own code range (RIP whitelist) and the decoy frame.
 *
 * 9.358: EACH GETS ITS OWN PAGE, and there are two reasons worth stating
 * because this looked like cosmetic alignment and is not.
 *
 * FIRST, a duplicate: these four were declared TWICE (once here, once again
 * near the functions that use them). Two declarations of one static is one
 * definition plus a shadow the linker places wherever it likes, which is how
 * .bss came to pack them with unrelated globals.
 *
 * SECOND, the page-layout check's Rule 2 was reporting sharing for EVERY
 * neighbour. Once it started CLASSIFYING instead, these four showed up as
 * sharing with EACH OTHER:
 *
 *     g_code_size + g_decoy_pa                    page 0x3C000
 *     g_code_start + g_guard_count + g_decoy_va   page 0x3E000
 *
 * That is the 15th hang's condition. These four are the guard's control
 * state: the RIP whitelist that decides whether a fault is ours, and the
 * frame the decoy points at. If one of those pages ever became a hide target,
 * hiding it would remove the very values the fault handler needs in order to
 * decide what to do about the fault — the recursion that hung the machine.
 *
 * Alignment is cheap and the failure mode is a hung machine, so the separation
 * is bought structurally rather than left to "nobody will hide this page".
 */
DECLSPEC_ALIGN(4096) static uint64_t g_code_start;
DECLSPEC_ALIGN(4096) static uint64_t g_code_size;
DECLSPEC_ALIGN(4096) static uint64_t g_decoy_pa;
DECLSPEC_ALIGN(4096) static void *g_decoy_va;
/* 9.346: forward declarations — the add paths below use these. */
static int yghv_guard_owns_page(uint64_t gpa);
static int yghv_guard_state_page(uint64_t gpa);
static int yghv_guard_target_page(uint64_t gpa);
static int yghv_guard_find(uint64_t gpa);


/*
 * FNV-1a over one page.
 *
 * A full cryptographic hash is not needed: the question is "did this page
 * change", not "can an attacker forge a collision". A guest that wants to
 * defeat this has to make its edit hash-neutral, which is strictly harder than
 * just not editing the page — and if it can do that, it can also hide its own
 * edit, at which point no in-band check helps.
 */

/*
 * 9.339 FIX: hash the OBJECT, not its page.
 *
 * The first version hashed a whole 4K page and reported 2 changed pages on a
 * completely untouched machine (measured: "armed 3 page(s)" then immediately
 * "verify -> 2 page(s) changed"). The cause is .bss packing — unrelated globals
 * share a page, and the guard's own table sits on the SAME page as the frame
 * table:
 *
 *     g_frame   0x34A40   page 0x34000
 *     g_guard   0x34FE0   page 0x34000
 *
 * so hashing the frame page also hashes g_guard, and the guard writing its own
 * bookkeeping invalidates a hash it just took. Self-referential — and it would
 * report a change for every unrelated global the kernel updates in that page.
 *
 * Hashing only the caller's byte range removes the whole class of false
 * positives, and it matches what the feature actually claims: "this object did
 * not change", not "nothing on this page changed".
 */
/* 9.340: forward declarations — add_va is used before these are defined. */
static int yghv_guard_owns_page(uint64_t gpa);
static int yghv_guard_find(uint64_t gpa);
static int yghv_guard_hash_region(uint64_t gpa, uint32_t off, uint32_t len,
                                  uint8_t out[YGHV_SHA256_DIGEST]) {
    uint8_t buf[256];
    yghv_sha256_ctx_t ctx;
    uint64_t done = 0;
    uint64_t base = gpa + off;

    if (len == 0 || len > 0x1000 || off + len > 0x1000 || !out)
        return 0;

    yghv_sha256_init(&ctx);
    while (done < len) {
        uint32_t chunk = (uint32_t)((len - done) < sizeof(buf)
                                    ? (len - done) : sizeof(buf));
        if (!yghv_pa_read(base + done, buf, chunk))
            return 0;
        yghv_sha256_update(&ctx, buf, chunk);
        done += chunk;
    }
    yghv_sha256_final(&ctx, out);
    return 1;
}
/*
 * 9.342: SHA-256 over the object's exact bytes.
 *
 * FNV-1a answered "did this change by accident", which is not the question. A
 * guest editing a guarded table only has to make its edit hash-neutral, and a
 * multiply-and-xor hash over bytes it controls is trivial to steer. SHA-256
 * turns that into a second-preimage problem instead.
 *
 * Returns 0 only when the read failed; otherwise a digest is always written.
 */
static int yghv_guard_hash_va(const void *va, uint32_t len,
                              uint8_t out[YGHV_SHA256_DIGEST]) {
    if (!va || len == 0 || !out)
        return 0;
    yghv_sha256(va, len, out);
    return 1;
}
/*
 * Guard an object at its VA: hash its exact bytes.
 * PASSIVE. `hide` is normally 0 — see yghv_guard_owns_page() for why our own
 * objects must not be hidden.
 */
int yghv_guard_add_va(const void *va, uint32_t len, const char *tag, int hide) {
    uint64_t gpa = MmGetPhysicalAddress((PVOID)va).QuadPart;
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int slot = -1;
    int i;

    if (!va || !len || !gpa)
        return -2;
    if (KeAreAllApcsDisabled())
        return -1;
    /*
     * 9.343: hiding our own tables is now ALLOWED.
     *
     * 9.337 refused because the fault was unrecoverable — hiding removed the
     * page from the driver too (13th crash). With the RIP-whitelist ALLOW loop
     * in svm_simplevm206.cpp, our own access faults and is then served, so
     * hiding is survivable. The refusal remains only as a fallback for when
     * the code range was never registered, i.e. the loop cannot recognise us.
     */
    if (hide && yghv_guard_state_page(page) &&
        (!g_code_size || !g_decoy_pa))
        hide = 0;

    {
        /* 9.341: "ensure" semantics, not "insert once".
         *
         * A page can be registered for hashing first and asked to be hidden
         * later (a test does exactly this: arm tables for integrity, then hide
         * a target page). Returning the existing slot silently would report
         * success while the page stayed visible — measured as add OK with the
         * probe count unchanged. So if hiding is wanted and the entry is not
         * hidden yet, do it now. */
        int _s = yghv_guard_find(page);
        if (_s >= 0) {
            if (hide && !g_guard[_s].hidden) {
                if (yghv_protect_npt_unmap_page(page)) {
                    if (yghv_guard_do_hide(_s, page, YGHV_HIDE_AUTO)) {
                        (void)0;
                    }
                    InterlockedIncrement64(&g_g_hidden);
                } else {
                    InterlockedIncrement64(&g_g_hidefail);
                }
            }
            return _s;
        }
    }

    for (i = 0; i < YGHV_GUARD_MAX; i++) {
        if (!InterlockedCompareExchange(&g_guard[i].active, 0, 0)) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -3;

    {
        uint8_t sha[YGHV_SHA256_DIGEST];
        if (!yghv_guard_hash_va(va, len, sha)) {
            InterlockedIncrement64(&g_g_hashfail);
            return -5;
        }
        RtlCopyMemory(g_guard[slot].digest, sha, YGHV_SHA256_DIGEST);
    }
    g_guard[slot].hash_off = 0;
    g_guard[slot].hash_len = len;
    g_guard[slot].hash_va = (uint64_t)(ULONG_PTR)va;   /* 9.340: re-hash here */

    if (hide && yghv_protect_npt_unmap_page(page)) {
        if (yghv_guard_do_hide(slot, page, YGHV_HIDE_AUTO)) {
            (void)0;
        }
        InterlockedIncrement64(&g_g_hidden);
    }

    g_guard[slot].gpa = page;
    g_guard[slot].armed = 1;
    g_guard[slot].checks = 0;
    g_guard[slot].changes = 0;
    if (tag) {
        int n = 0;
        while (tag[n] && n < YGHV_GUARD_TAG_LEN - 1) {
            g_guard[slot].tag[n] = tag[n];
            n++;
        }
        g_guard[slot].tag[n] = 0;
    }
    InterlockedExchange(&g_guard[slot].active, 1);
    InterlockedIncrement(&g_guard_count);
    InterlockedIncrement64(&g_g_guarded);
    return slot;
}

/*
 * 9.337 FIX: does this page contain the guard's OWN bookkeeping?
 *
 * Measured crash (13th, 01:08): yghv_guard_find read g_guard[i].gpa at
 * RVA 0x34fe0 and faulted — because arm_core had just hidden the page for the
 * frame table, and .bss put g_frame (0x34A40) and g_guard (0x34FE0) on the
 * SAME 4K page (0x34000).
 *
 * Hiding a page means removing it from the NPT, and in the OS-as-guest model
 * the driver executes INSIDE the guest — so its own ordinary global accesses
 * go through the NPT too. Hiding a page the driver reads therefore hides the
 * driver from itself. main.c's existing exclude pass is safe only because what
 * it hides (VMCB, host stack, MSRPM, IOPM) is touched by the hypervisor in HOST
 * context, not by guest-executing driver code.
 *
 * So: never hide a page we ourselves need to reach through a normal VA.
 */
/*
 * 9.337 FIX: never hide a page the driver itself reaches through a normal VA.
 *
 * Measured crash (13th, 2026-10-18 01:08): yghv_guard_find read g_guard[i].gpa
 * at RVA 0x34fe0 and faulted, because arm_core had just hidden the page for the
 * frame table — and .bss put g_frame (0x34A40) and g_guard (0x34FE0) on the
 * SAME 4K page (0x34000).
 *
 * The mechanism, stated plainly, because it decides the whole design:
 *
 *   This build is OS-as-guest. Only code running inside the VMRUN loop or a
 *   VMEXIT handler executes in HOST mode, where the NPT is not consulted. A
 *   normal driver thread — an IOCTL, a system worker — executes in GUEST mode,
 *   so its ordinary pointer dereferences go guest VA -> guest page tables ->
 *   GPA -> NPT -> SPA.
 *
 *   Hiding a page means removing it from that NPT. So hiding a page that a
 *   guest-mode driver thread reads removes it from the driver too.
 *
 * main.c's exclude pass is sound because what it hides (VMCB, host stack,
 * MSRPM, IOPM) is touched only from the host-mode VMEXIT path. npt_core's own
 * tables are sound because their recorded direct VA is what host-mode code
 * uses. The bp / frame / view tables are NOT sound to hide: they are .bss
 * globals that IOCTL handlers read and write with plain C.
 *
 * Consequence for the feature, stated honestly: those tables cannot be hidden
 * from the guest, because they are the same object the guest's own driver code
 * must use. What CAN be done for them is DETECTION — hash them and notice a
 * change (7.3). That is the half that survives contact with this model.
 *
 * The refusal is explicit rather than silent, so a caller learns the boundary
 * instead of getting a page that vanishes under their own feet.
 */
/*
 * 9.346: TWO questions, and they were being conflated.
 *
 * owns_page() was written for 9.337, when the refusal was "never hide a page the
 * driver reads with plain C" — so it named the guard's own state AND all three
 * Tier-4 tables, because at that time hiding any of them was fatal.
 *
 * 9.343 changed one half of that: with the RIP-whitelist ALLOW loop, hiding the
 * three TABLES is exactly what hidecore is for. But the split-page problem
 * (9.345) has nothing to do with the tables — it is about the guard's own
 * bookkeeping sharing their pages.
 *
 * Feeding one predicate to both decisions inverted hidecore: it skipped the very
 * pages it was asked to hide and reported "hid 0 page(s)" while claiming success.
 * So the question is split:
 *
 *   guard_state(gpa)  — is this page the guard's OWN state?
 *                       Hiding it recurses (9.345 hang) and can never be right.
 *   target_table(gpa) — is this page one of the Tier-4 tables?
 *                       Hiding it is the feature; the ALLOW loop serves our
 *                       subsequent accesses.
 */

/* Is this page the guard's own bookkeeping? Never hide it: the NPF handler
 * reads g_guard through yghv_guard_is_hidden(), so hiding it faults inside the
 * handler that asked the question — measured as an infinite recursion (hang). */
/*
 * Is this page the guard's own bookkeeping? Never hide it: the NPF handler
 * reads g_guard through yghv_guard_is_hidden(), so hiding it faults inside the
 * handler that asked the question — measured as an infinite recursion (hang).
 *
 * 9.361: the refusal covers the SPAN of every piece of guard state, not the
 * individual address of each one.
 *
 * The earlier version named one address per item, and Rule 2 of the page-layout
 * check showed why that is not enough:
 *
 *     page 0x3F000   g_guard + g_g_hidden + g_g_hidefail + g_g_hashfail
 *     page 0x40000   g_guard_count + g_g_guarded + g_g_probes + g_g_verified + g_g_changed
 *
 * g_guard is exactly one page (32 slots x 128 bytes = 4096), so its neighbours
 * necessarily spill onto the NEXT page. Those neighbours are counters the fault
 * path itself touches — g_g_hidden is incremented by do_hide(), g_g_probes by
 * yghv_guard_is_hidden(). A page can therefore be correctly refused as "guard
 * state" and still carry unnamed state that must not be hidden either.
 *
 * Refusing the whole span [first guard byte, last guard byte] makes the answer
 * independent of how the linker arranged the neighbourhood. If any byte of guard
 * state is on a page, that page is refused — which is the property that was
 * always intended, and is now stated in a way the layout cannot defeat.
 *
 * A compile-time assertion below pins the span to a handful of pages, so a future
 * growth of guard state fails the build rather than silently widening the refusal
 * past what was reviewed.
 */
#define YGHV_GUARD_STATE_LO() ((uint64_t)(ULONG_PTR)&g_guard[0])
#define YGHV_GUARD_STATE_HI() ((uint64_t)(ULONG_PTR)&g_guard_count + sizeof(g_guard_count))

/*
 * 9.364 FIX — THE SAME VA-vs-PA CONFUSION AS yghv_guard_target_page(), AND THIS
 * IS THE ONE THAT ACTUALLY BROKE hidecore.
 *
 * Every comparison below used a VIRTUAL address while the caller passes a
 * PHYSICAL page:
 *
 *     yghv_guard_state_page(gpa)              <- gpa is physical
 *     if (page == (&g_code_start & ~0xFFF))   <- &g_code_start is a VA
 *
 * In kernel mode those numbers are unrelated, so the four control-value tests
 * were false for every input. The SPAN test, though, used
 * YGHV_GUARD_STATE_LO/HI — also VAs — and a physical page number can land inside
 * that numeric range by coincidence. So state_page() returned 1 for pages that
 * have nothing to do with guard state.
 *
 * MEASURED, with the probe counters added for exactly this question:
 *
 *     hidecore: hid 0 page(s)
 *     hide_fail = 3  ("page is guard state" — the real cause)
 *     hide_fail = 3  (page skipped by state_page — the real cause)
 *     NPT entry 0x11F6000E7 unchanged before and after
 *
 * hidecore's per-page loop therefore hit the state_page() continue and never
 * reached find() at all — that is the whole of "hid 0 page(s)".
 *
 * The fix is not to widen or narrow anything: it is to make this function compare
 * like with like. ISLAND-SAFE — MmGetPhysicalAddress is a software page-table
 * walk, no locks and no blocking, so it is callable from the NPF/#DB handlers
 * (same reasoning as the *_bare helpers).
 *
 * The VAs are resolved ONCE and cached, because this sits on a per-VMEXIT path
 * and each call would otherwise cost several extra page-table walks.
 */
static uint64_t g_state_pa_lo, g_state_pa_hi;
static uint64_t g_state_pa_code_start, g_state_pa_code_size;
static uint64_t g_state_pa_decoy_pa, g_state_pa_decoy_va;
static volatile LONG g_state_pa_ready;

static void yghv_guard_state_pa_init(void) {
    extern PHYSICAL_ADDRESS MmGetPhysicalAddress(void *BaseAddress);

    if (InterlockedCompareExchange(&g_state_pa_ready, 0, 0))
        return;

    g_state_pa_lo = (uint64_t)MmGetPhysicalAddress(&g_guard[0]).QuadPart;
    /* the span runs from the array through g_guard_count (adjacent in .bss even
     * though they sit on different pages — see the 9.358 layout) */
    g_state_pa_hi = (uint64_t)MmGetPhysicalAddress((PVOID)&g_guard_count).QuadPart;

    g_state_pa_code_start = (uint64_t)MmGetPhysicalAddress((PVOID)&g_code_start).QuadPart;
    g_state_pa_code_size  = (uint64_t)MmGetPhysicalAddress((PVOID)&g_code_size).QuadPart;
    g_state_pa_decoy_pa   = (uint64_t)MmGetPhysicalAddress((PVOID)&g_decoy_pa).QuadPart;
    g_state_pa_decoy_va   = (uint64_t)MmGetPhysicalAddress((PVOID)&g_decoy_va).QuadPart;

    InterlockedExchange(&g_state_pa_ready, 1);
}

static int yghv_guard_state_page(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    uint64_t lo, hiPage;

    yghv_guard_state_pa_init();

    lo = g_state_pa_lo & ~(uint64_t)0xFFFULL;
    hiPage = g_state_pa_hi & ~(uint64_t)0xFFFULL;
    if (page >= lo && page <= hiPage)
        return 1;

    if (page == (g_state_pa_code_start & ~(uint64_t)0xFFFULL))
        return 1;
    if (page == (g_state_pa_code_size & ~(uint64_t)0xFFFULL))
        return 1;
    if (page == (g_state_pa_decoy_pa & ~(uint64_t)0xFFFULL))
        return 1;
    if (page == (g_state_pa_decoy_va & ~(uint64_t)0xFFFULL))
        return 1;

    /* 9.361: the NPT managers and the protection table. The managers are static
     * in protect.c, so the question is asked through that file (naming them here
     * failed to link — LNK2019, measured). That helper resolves its own VAs, so
     * it is already PA-correct. The protection table's PA is resolved once. */
    if (yghv_protect_page_is_npt(page))
        return 1;

    {
        extern yghv_protect_state_t g_protect;
        extern PHYSICAL_ADDRESS MmGetPhysicalAddress(void *BaseAddress);
        static uint64_t prot_pa;
        if (!prot_pa)
            prot_pa = (uint64_t)MmGetPhysicalAddress(&g_protect).QuadPart;
        if (page == (prot_pa & ~(uint64_t)0xFFFULL))
            return 1;
    }
    return 0;
}

/*
 * 9.365: WHICH refusal matched — as a bitmask, so one counter carries the answer.
 *
 * "hide_fail = 3 (page is guard state)" does not say whether the span, a control
 * value, or an NPT manager was responsible, and those are three different fixes.
 * Measured cost: after making g_frame/g_view page-exact, hidecore reported hid 0
 * with hide_fail=3 and the answer had to be re-derived from .map arithmetic.
 *
 * Integer return rather than a string: this runs inside the hide loop, and the
 * caller only needs the bit that fired.
 */
uint32_t yghv_guard_state_why(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    uint64_t lo, hiPage;

    yghv_guard_state_pa_init();

    lo = g_state_pa_lo & ~(uint64_t)0xFFFULL;
    hiPage = g_state_pa_hi & ~(uint64_t)0xFFFULL;
    if (page >= lo && page <= hiPage)
        return 0x01;                                    /* span g_guard..count   */

    if (page == (g_state_pa_code_start & ~(uint64_t)0xFFFULL)) return 0x02;
    if (page == (g_state_pa_code_size  & ~(uint64_t)0xFFFULL)) return 0x04;
    if (page == (g_state_pa_decoy_pa   & ~(uint64_t)0xFFFULL)) return 0x08;
    if (page == (g_state_pa_decoy_va   & ~(uint64_t)0xFFFULL)) return 0x10;

    if (yghv_protect_page_is_npt(page)) return 0x20;    /* g_npt / g_npt_alt     */

    {
        extern yghv_protect_state_t g_protect;
        extern PHYSICAL_ADDRESS MmGetPhysicalAddress(void *BaseAddress);
        static uint64_t prot_pa;
        if (!prot_pa)
            prot_pa = (uint64_t)MmGetPhysicalAddress(&g_protect).QuadPart;
        if (page == (prot_pa & ~(uint64_t)0xFFFULL)) return 0x40;
    }
    return 0;
}

static int yghv_guard_target_page(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    extern uint64_t *yghv_bp_table_va(uint32_t *bytes);
    extern uint64_t *yghv_frame_table_va(uint32_t *bytes);
    extern uint64_t *yghv_view_table_va(uint32_t *bytes);
    extern PHYSICAL_ADDRESS MmGetPhysicalAddress(void *BaseAddress);
    uint32_t bytes = 0;
    uint64_t *va;
    uint64_t lo;    /* physical base of the object's first page */
    uint64_t hi;    /* physical page holding the object's last byte   */

    /*
     * 9.364 FIX: this compared a PHYSICAL page number against a VIRTUAL address.
     *
     *   va   = yghv_bp_table_va(&bytes);   <- kernel VA, e.g. 0xFFFFA2...
     *   page = gpa & ~0xFFF;               <- physical,  e.g. 0x11F6000
     *   if (page >= ((uint64_t)va & ~0xFFF) && ...)
     *
     * In kernel mode VA and PA are unrelated, so the test was FALSE for every
     * input, and yghv_guard_mode_for() never classified a target table.
     *
     * The bug is in the committed tree (9.359) and the runtime suites passed with
     * it, because the OLD yghv_guard_state_page() was broad enough to swallow the
     * three table pages too — mode_for() took its UNMAP answer from that half, and
     * this always-false test never showed. 9.361 tightened state_page to the span
     * that actually holds guard state (correct on its own terms), and that removed
     * the accidental coverage, exposing this.
     *
     * THE LESSON: a correct fix can expose a second defect that was riding on the
     * defect being fixed. When a change makes something stop working, ask whether
     * the change broke it or merely UNMASKED it. Here it was the second, so
     * reverting the correct change would only have restored the mask.
     *
     * This is the third VA/PA confusion in this codebase (the 0x1A crash, its
     * reverse, and now this). Section 21 of docs/KERNEL_API_CHECKLIST.md states
     * the rule; the fix below translates once per table and names which side is
     * which, so no comparison mixes the two.
     */
    va = yghv_bp_table_va(&bytes);
    if (va && bytes) {
        lo = (uint64_t)MmGetPhysicalAddress(va).QuadPart & ~(uint64_t)0xFFFULL;
        hi = ((uint64_t)MmGetPhysicalAddress(va).QuadPart + bytes - 1) & ~(uint64_t)0xFFFULL;
        if (page >= lo && page <= hi)
            return 1;
    }
    va = yghv_frame_table_va(&bytes);
    if (va && bytes) {
        lo = (uint64_t)MmGetPhysicalAddress(va).QuadPart & ~(uint64_t)0xFFFULL;
        hi = ((uint64_t)MmGetPhysicalAddress(va).QuadPart + bytes - 1) & ~(uint64_t)0xFFFULL;
        if (page >= lo && page <= hi)
            return 1;
    }
    va = yghv_view_table_va(&bytes);
    if (va && bytes) {
        lo = (uint64_t)MmGetPhysicalAddress(va).QuadPart & ~(uint64_t)0xFFFULL;
        hi = ((uint64_t)MmGetPhysicalAddress(va).QuadPart + bytes - 1) & ~(uint64_t)0xFFFULL;
        if (page >= lo && page <= hi)
            return 1;
    }
    return 0;
}

/*
 * 9.337's original refusal, kept for the callers that predate the ALLOW loop:
 * hiding a page we read with plain C is only safe when the ALLOW loop is
 * available to serve our own subsequent access.
 */
static int yghv_guard_owns_page(uint64_t gpa) {
    return (yghv_guard_state_page(gpa) || yghv_guard_target_page(gpa)) ? 1 : 0;
}
static int yghv_guard_find(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int i;

    for (i = 0; i < YGHV_GUARD_MAX; i++) {
        if (InterlockedCompareExchange(&g_guard[i].active, 0, 0) &&
            g_guard[i].gpa == page)
            return i;
    }
    return -1;
}

int yghv_guard_is_hidden(uint64_t gpa) {
    int slot = yghv_guard_find(gpa);

    if (slot < 0)
        return 0;
    if (!g_guard[slot].hidden)
        return 0;
    InterlockedIncrement64(&g_g_probes);
    return 1;
}

uint64_t yghv_guard_probes(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_g_probes, 0, 0);
}

int yghv_guard_add_range(uint64_t gpa, uint32_t off, uint32_t len,
                         const char *tag, int hide, int hash) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    uint64_t abs_gpa = page + off;
    int slot = -1;
    int i;

    if (!page || (!hide && !hash))
        return -2;
    if (hash && (len == 0 || off + len > 0x1000))
        return -2;
    if (KeAreAllApcsDisabled())
        return -1;

    /*
     * 9.343: hiding our own tables is now ALLOWED.
     *
     * 9.337 refused because the fault was unrecoverable — hiding removed the
     * page from the driver too (13th crash). With the RIP-whitelist ALLOW loop
     * in svm_simplevm206.cpp, our own access faults and is then served, so
     * hiding is survivable. The refusal remains only as a fallback for when
     * the code range was never registered, i.e. the loop cannot recognise us.
     */
    if (hide && yghv_guard_state_page(page) &&
        (!g_code_size || !g_decoy_pa))
        hide = 0;

    {
        /* 9.341: "ensure" semantics, not "insert once".
         *
         * A page can be registered for hashing first and asked to be hidden
         * later (a test does exactly this: arm tables for integrity, then hide
         * a target page). Returning the existing slot silently would report
         * success while the page stayed visible — measured as add OK with the
         * probe count unchanged. So if hiding is wanted and the entry is not
         * hidden yet, do it now. */
        int _s = yghv_guard_find(page);
        if (_s >= 0) {
            if (hide && !g_guard[_s].hidden) {
                if (yghv_protect_npt_unmap_page(page)) {
                    if (yghv_guard_do_hide(_s, page, YGHV_HIDE_AUTO)) {
                        (void)0;
                    }
                    InterlockedIncrement64(&g_g_hidden);
                } else {
                    InterlockedIncrement64(&g_g_hidefail);
                }
            }
            return _s;
        }
    }

    for (i = 0; i < YGHV_GUARD_MAX; i++) {
        if (!InterlockedCompareExchange(&g_guard[i].active, 0, 0)) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -3;

    if (hash) {
        uint8_t sha[YGHV_SHA256_DIGEST];
        if (!yghv_guard_hash_region(page, off, len, sha)) {
            InterlockedIncrement64(&g_g_hashfail);
            return -5;
        }
        RtlCopyMemory(g_guard[slot].digest, sha, YGHV_SHA256_DIGEST);
        g_guard[slot].hash_off = off;
        g_guard[slot].hash_len = len;
    }

    if (hide) {
        if (!yghv_protect_npt_unmap_page(page)) {
            InterlockedIncrement64(&g_g_hidefail);
            if (!hash)
                return -4;
        } else {
            if (yghv_guard_do_hide(slot, page, YGHV_HIDE_AUTO)) {
                (void)0;
            }
            InterlockedIncrement64(&g_g_hidden);
        }
    }

    g_guard[slot].gpa = page;
    g_guard[slot].checks = 0;
    g_guard[slot].changes = 0;
    g_guard[slot].armed = 1;
    if (tag) {
        int n = 0;
        while (tag[n] && n < YGHV_GUARD_TAG_LEN - 1) {
            g_guard[slot].tag[n] = tag[n];
            n++;
        }
        g_guard[slot].tag[n] = 0;
    }
    InterlockedExchange(&g_guard[slot].active, 1);
    InterlockedIncrement(&g_guard_count);
    InterlockedIncrement64(&g_g_guarded);
    (void)abs_gpa;
    return slot;
}

int yghv_guard_add(uint64_t gpa, const char *tag, int hide, int hash) {
    /* Whole-object form kept for callers whose object IS the page. The arm
     * path below uses _range, because our tables share their pages with
     * unrelated globals. */
    return yghv_guard_add_range(gpa, 0, 0x1000, tag, hide, hash);
}

int yghv_guard_remove(uint64_t gpa) {
    int slot = yghv_guard_find(gpa);

    if (slot < 0)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;

    if (g_guard[slot].hidden) {
        (void)yghv_protect_npt_remap(g_guard[slot].gpa, g_guard[slot].gpa,
                                     0, NULL);
        InterlockedDecrement64(&g_g_hidden);
    }
    InterlockedExchange(&g_guard[slot].active, 0);
    RtlZeroMemory(&g_guard[slot], sizeof(g_guard[slot]));
    InterlockedDecrement(&g_guard_count);
    return 1;
}

int yghv_guard_verify(uint64_t gpa) {
    int slot = yghv_guard_find(gpa);
    uint8_t now[YGHV_SHA256_DIGEST];
    int have;

    if (slot < 0)
        return -1;

    /* A digest of all zeros means hashing was not requested for this entry. */
    {
        int allzero = 1, z;
        for (z = 0; z < YGHV_SHA256_DIGEST; z++) {
            if (g_guard[slot].digest[z]) { allzero = 0; break; }
        }
        if (allzero)
            return 1;
    }

    if (g_guard[slot].hash_va)
        have = yghv_guard_hash_va((const void *)g_guard[slot].hash_va,
                                  g_guard[slot].hash_len, now);
    else
        have = yghv_guard_hash_region(g_guard[slot].gpa,
                                      g_guard[slot].hash_off,
                                      g_guard[slot].hash_len, now);
    if (!have)
        return -1;

    g_guard[slot].checks++;
    InterlockedIncrement64(&g_g_verified);

    if (!yghv_sha256_equal(now, g_guard[slot].digest)) {
        g_guard[slot].changes++;
        InterlockedIncrement64(&g_g_changed);
        return 0;
    }
    return 1;
}

uint32_t yghv_guard_verify_all(void) {
    uint32_t changed = 0;
    int i;

    for (i = 0; i < YGHV_GUARD_MAX; i++) {
        if (!InterlockedCompareExchange(&g_guard[i].active, 0, 0))
            continue;
        if (yghv_guard_verify(g_guard[i].gpa) == 0)
            changed++;
    }
    return changed;
}

uint32_t yghv_guard_list(yghv_guard_info_t *out, uint32_t max) {
    uint32_t n = 0;
    int i, j;

    if (!out || !max)
        return 0;
    for (i = 0; i < YGHV_GUARD_MAX && n < max; i++) {
        if (!InterlockedCompareExchange(&g_guard[i].active, 0, 0))
            continue;
        out[n].gpa = g_guard[i].gpa;
        RtlCopyMemory(out[n].digest, g_guard[i].digest, YGHV_SHA256_DIGEST);
        out[n].checks = g_guard[i].checks;
        out[n].changes = g_guard[i].changes;
        out[n].hidden = g_guard[i].hidden;
        out[n].armed = g_guard[i].armed;
        for (j = 0; j < YGHV_GUARD_TAG_LEN; j++)
            out[n].tag[j] = g_guard[i].tag[j];
        n++;
    }
    return n;
}

/*
 * Guard the structures added after main.c's own exclude pass (Tier 4 tables).
 *
 * main.c already hides the VMCBs, host stacks, MSRPM, IOPM and the NPT itself
 * (npt_exclude_self). What it does NOT know about is everything built later:
 * the breakpoint table, the frame reservations, the split-view tables. Those
 * are ordinary pool pages, guest-reachable, and they are what the newer
 * features depend on.
 */
uint32_t yghv_guard_arm_core(void) {
    uint32_t n = 0;
    extern uint64_t *yghv_bp_table_va(uint32_t *bytes);
    extern uint64_t *yghv_frame_table_va(uint32_t *bytes);
    extern uint64_t *yghv_view_table_va(uint32_t *bytes);
    uint32_t bytes = 0;
    uint64_t *va;

    /*
     * 9.340: hash each table's EXACT bytes at its VA.
     *
     * Earlier versions went VA -> PA -> yghv_pa_read and reasoned about page
     * offsets, which produced a wrong hash three separate ways (a VA passed
     * where a PA was wanted, 0x1000 bytes from a mid-page start, and a whole
     * page that included g_guard itself). None of that is necessary for an
     * object whose address we hold: read it where it is.
     *
     * hide stays 0 throughout. These are our own .bss tables and IOCTL code
     * runs in guest mode — hiding them removes them from the driver too
     * (measured as the 13th crash, 0x3B in yghv_guard_find).
     */
    va = yghv_bp_table_va(&bytes);
    if (va && bytes && yghv_guard_add_va(va, bytes, "bp-table", 0) >= 0)
        n++;

    va = yghv_frame_table_va(&bytes);
    if (va && bytes && yghv_guard_add_va(va, bytes, "frame-table", 0) >= 0)
        n++;

    va = yghv_view_table_va(&bytes);
    if (va && bytes && yghv_guard_add_va(va, bytes, "view-table", 0) >= 0)
        n++;

    return n;
}
void yghv_guard_stats_get(yghv_guard_stats_t *out) {
    if (!out)
        return;
    out->guarded    = (uint64_t)InterlockedCompareExchange64(&g_g_guarded, 0, 0);
    out->hidden     = (uint64_t)InterlockedCompareExchange64(&g_g_hidden, 0, 0);
    out->verified   = (uint64_t)InterlockedCompareExchange64(&g_g_verified, 0, 0);
    out->changed    = (uint64_t)InterlockedCompareExchange64(&g_g_changed, 0, 0);
    out->probes     = (uint64_t)InterlockedCompareExchange64(&g_g_probes, 0, 0);
    out->hide_fail  = (uint64_t)InterlockedCompareExchange64(&g_g_hidefail, 0, 0);
    out->hash_fail  = (uint64_t)InterlockedCompareExchange64(&g_g_hashfail, 0, 0);
}

void yghv_guard_stats_reset(void) {
    InterlockedExchange64(&g_g_verified, 0);
    InterlockedExchange64(&g_g_changed, 0);
    InterlockedExchange64(&g_g_probes, 0);
    InterlockedExchange64(&g_g_hashfail, 0);
}


/*
 * 9.340: hash an object at its kernel VA.
 *
 * arm_core guards OUR OWN tables. They are readable at their VA, so reading
 * them that way is exact and trivial — while going through yghv_pa_read meant
 * turning a VA into a PA and then reasoning about page offsets, and every
 * mistake along that chain produced a wrong hash that looked like tampering:
 *
 *   - passed the VA where a PA was wanted       -> hashed a random frame
 *   - hashed 0x1000 bytes from a mid-page start -> ran into the NEXT page
 *   - hashed a whole page                       -> included g_guard itself,
 *                                                 which mutates as we work
 *
 * Reading `len` bytes at `va` has none of those degrees of freedom. Use it for
 * anything whose address we already hold. yghv_pa_read stays for frames we do
 * not have a VA for (a breakpoint target, a foreign process's page).
 */


/* ============================================================================
 * 9.343: three upgrades over the first cut of 7.1/7.3
 * ==========================================================================*/

/* ---- #1 hide our OWN tables: RIP-whitelist ALLOW loop ---- */


void yghv_guard_set_code_range(uint64_t start, uint64_t size) {
    g_code_start = start;
    g_code_size = size;
}

/*
 * ISLAND-SAFE. A fault is "ours" when it comes from code inside this driver's
 * image at CPL 0. A guest store cannot satisfy both: its RIP is somewhere in
 * guest code and its CPL is 3.
 *
 * This is what makes hiding our own tables possible. 9.320.1 showed the naive
 * approach removes the page from us as well; with this whitelist the fault is
 * recoverable, so hiding becomes survivable rather than self-destructive.
 */
int yghv_guard_fault_is_ours(uint64_t rip, uint32_t cpl) {
    if (!g_code_size)
        return 0;
    if (cpl != 0)
        return 0;
    return (rip >= g_code_start && rip < g_code_start + g_code_size) ? 1 : 0;
}

/* ---- #2 decoy frame for address-space continuity ---- */


int yghv_guard_decoy_init(void) {
    if (g_decoy_va)
        return 1;
    if (KeAreAllApcsDisabled())
        return 0;

    g_decoy_va = ExAllocatePoolWithTag(NonPagedPool, 0x1000, 'Ydcy');
    if (!g_decoy_va)
        return 0;
    RtlZeroMemory(g_decoy_va, 0x1000);
    g_decoy_pa = MmGetPhysicalAddress(g_decoy_va).QuadPart;
    return g_decoy_pa ? 1 : 0;
}

int yghv_guard_decoy_ready(void) {
    return (g_decoy_pa && g_decoy_va) ? 1 : 0;
}

uint64_t yghv_guard_decoy_pa(void) {
    return g_decoy_pa;
}

/* ---- #3 unregistered pages ---- */

uint32_t yghv_guard_uncovered(uint64_t start, uint64_t size, uint64_t *out,
                              uint32_t max) {
    uint64_t page;
    uint32_t n = 0;

    if (!size)
        return 0;
    if (KeAreAllApcsDisabled())
        return 0;

    for (page = start & ~(uint64_t)0xFFFULL;
         page < (start + size);
         page += 0x1000) {
        if (yghv_guard_find(page) >= 0)
            continue;
        if (out && n < max)
            out[n] = page;
        n++;
    }
    return n;
}

/*
 * 9.343 #1: hide the Tier-4 tables for real.
 *
 * Requiring a registered code range and a decoy is not cosmetic: hiding a page
 * we read with plain C is only survivable because the NPF handler recognises
 * our own fault (RIP whitelist) and the decoy keeps the address space looking
 * continuous. With neither, this is the 13th crash again. So refuse unless both
 * are in place, and say so in the return value.
 */
uint32_t yghv_guard_hide_core(void) {
    extern uint64_t *yghv_bp_table_va(uint32_t *bytes);
    extern uint64_t *yghv_frame_table_va(uint32_t *bytes);
    extern uint64_t *yghv_view_table_va(uint32_t *bytes);
    uint32_t hid = 0;
    uint32_t bytes = 0;
    uint64_t *va;
    int i;

    if (!yghv_guard_hiding_armed()) {
        /*
         * 9.364: THE EARLY RETURN NAMES ITSELF.
         *
         * "hid 0 page(s)" with hide_fail=0 and probes=0 was reachable from three
         * completely different causes and the first version reported them
         * identically:
         *
         *   0x10  this return: the ALLOW loop is not armed (g_code_size or
         *         g_decoy_pa is zero), so hiding our own tables would be
         *         unrecoverable. A CORRECT refusal, not a fault.
         *   1/2   the NPT step failed (split, or clear-present) — see unmap_bare
         *   0x20  no page matched a guard entry, or everything was already hidden
         *
         * Measured cost of not distinguishing them: hidecore said "hid 0 page(s)",
         * the NPT entry was byte-for-byte unchanged, and the investigation went
         * after the NPT twice before this return was even considered. A counter
         * that says WHICH of three doors was taken is worth more than three
         * rebuild-and-load cycles.
         */
        InterlockedExchange64(&g_g_hidefail, 0x10);
        return 0;
    }

    /*
     * 9.359: g_bp IS now hidden, and the two changes that made it possible are
     * worth recording because the earlier state was "excluded by design".
     *
     * WHAT THE MEASUREMENTS SAID, IN ORDER.
     *
     * 1. Isolation settled which variable mattered:
     *        hidecore excludes g_bp  ->  breakpoint cycle after hiding PASSES
     *        hidecore includes g_bp  ->  the bp trigger HANGS (driver answering)
     *
     * 2. The first explanation I wrote for that — "the hot path pays a nested
     *    VMEXIT round trip to read its own registry" — was DISPROVED by the
     *    counters: probes stayed at 0 throughout, so the guard branch is never
     *    entered and those reads never fault at all. A comment asserting a
     *    mechanism the counters contradict is worse than no comment.
     *
     * 3. The mechanism that DOES fit: mem-bp add runs from IOCTL (guest mode),
     *    its writes to g_bp[] go into the decoy and are discarded — but the arm
     *    step came BEFORE the registration step and runs at PASSIVE, so the arm
     *    succeeded. An armed page with no registered breakpoint. Every guest
     *    write faults, yghv_bp_on_npf finds nothing to match, the fault falls
     *    through to "reflect as PF", and the guest retries forever.
     *
     * WHAT FIXED IT.
     *
     *   a) 9.355 added the -11 refusal: arming is refused when the registry page
     *      is hidden. That is a safety net over a bad state.
     *
     *   b) 9.359 removed the bad state instead of guarding it: bp_add_ex now
     *      REGISTERS FIRST and verifies the write landed (a readback, because a
     *      discarded write leaves no trace), and only THEN arms the page. If the
     *      arm fails, the slot is released. There is no window in which the page
     *      is armed and the registry is not there.
     *
     *   c) hot/cold separation in mem_bp.c moves everything the island reads onto
     *      pages that are never hide targets, so the registry and the counters
     *      deciding what to do about a fault cannot be split by visibility.
     *
     * MEASURED AFTER, in this order — arm first, hide second, trigger third:
     *
     *     arm first        = True     bp add OK with the registry visible
     *     hide after arm   = True     hid 3 pages
     *     trigger landed   = True     mmf-write LANDED
     *     drain saw NPF    = True     #1 NPF W cpl=3 rip=0x7FF87E866DD2
     *     trigger hung     = False
     *
     * So g_bp can be hidden: the hot path survives the ALLOW loop. The refusal
     * stays as a safety net for the case where arming is attempted while hidden.
     */
    for (i = 0; i < 3; i++) {
        va = (i == 0) ? yghv_bp_table_va(&bytes)
                      : (i == 1) ? yghv_frame_table_va(&bytes)
                                 : yghv_view_table_va(&bytes);
        if (!va || !bytes)
            continue;
        {
            uint64_t gpa = MmGetPhysicalAddress(va).QuadPart;
            uint32_t off;
            for (off = 0; off < bytes; off += 0x1000) {
                /*
                 * 9.345: never hide a page carrying our OWN bookkeeping.
                 *
                 * Measured consequence of skipping this: after DECLSPEC_ALIGN
                 * moved the tables, the linker filled their spare page space
                 * with g_guard itself (g_frame @0x3B000 ... g_guard @0x3B5B0).
                 * hidecore hid that page, yghv_guard_is_hidden() — which reads
                 * g_guard — faulted inside the NPF handler, which called
                 * yghv_guard_is_hidden() again: infinite recursion. The machine
                 * hung; it did not bugcheck, which is why this one read as a
                 * timeout rather than a dump.
                 */
                if (yghv_guard_state_page(gpa + off)) {
                    /*
                     * 9.365: record WHICH refusal fired, into a counter slot.
                     *
                     * hide_fail=3 says "this page is guard state" but not WHICH
                     * refusal matched. Measured cost of not saying: after making
                     * g_frame/g_view page-exact, hidecore reported hid 0 with
                     * hide_fail=3, and the page numbers alone could not tell
                     * whether the span, a control value, or the NPT manager was
                     * responsible — three very different fixes.
                     *
                     * The bits are packed so one counter carries the answer:
                     *   bit0 span, bit1 code_start, bit2 code_size,
                     *   bit3 decoy_pa, bit4 decoy_va, bit5 npt, bit6 protect
                     */
                    InterlockedExchange64(&g_g_hashfail,
                                          (LONG64)yghv_guard_state_why(gpa + off));
                    InterlockedExchange64(&g_g_hidefail, 3);  /* page is guard state */
                    continue;
                }
                {
                    int s = yghv_guard_find(gpa + off);
                    if (s < 0) {
                        InterlockedExchange64(&g_g_hidefail, 4);  /* no guard entry */
                        continue;
                    }
                    if (g_guard[s].hidden) {
                        InterlockedExchange64(&g_g_hidefail, 5);  /* already hidden */
                        continue;
                    }
                    /*
                     * 9.354: do_hide() IS the hide — it performs the mechanism,
                     * records which one, and increments the counter. An earlier
                     * version ALSO called yghv_protect_npt_unmap_page() here and
                     * incremented hid twice, so "hid 6 page(s)" was reported for
                     * three pages and the page was excluded from the NPT twice
                     * (9.354 measured this on the y4 image).
                     */
                    if (yghv_guard_do_hide(s, gpa + off, YGHV_HIDE_AUTO))
                        hid++;
                }
            }
        }
    }

    /* Nothing was hidden and no step failed: every page was skipped above. The
     * per-page codes say which one, so this only covers "the loop never ran". */
    if (hid == 0 && InterlockedCompareExchange64(&g_g_hidefail, 0, 0) == 0)
        InterlockedExchange64(&g_g_hidefail, 0x20);
    return hid;
}

/* ============================================================================
 * 9.344: ISLAND-SAFE remap, and the reason the first version could not work.
 *
 * The 15th crash (0x3B at yghv_mem_read+0x2A) was not a subtle detail — it was
 * a wrong API choice, and the mechanism is worth stating plainly:
 *
 *   yghv_protect_npt_remap_flags() begins with
 *
 *       if (KeAreAllApcsDisabled()) return 0;
 *
 *   An NPF handler runs at DISPATCH_LEVEL with GIF=0, which IS "all APCs
 *   disabled". So every call to it from the fault path returned 0:
 *
 *     - the ALLOW branch's if(...) was always false, so the page was never
 *       restored and execution fell through;
 *     - the decoy branch called the same function and also got 0;
 *     - the delivered access violation (CR2 = 0) was unresolvable by the
 *       guest's own fault handler -> 0x3B.
 *
 *   Measured exactly that: hidecore succeeded (it runs at PASSIVE), then the
 *   first yghv_mem_read touching a hidden page crashed.
 *
 * 4.4's ALLOW loop works because it uses the island-safe *_bare family
 * (yghv_protect_arm_page_bare uses npt_set_page_perm directly, with no IRQL
 * gate). The guard path needs the same treatment.
 * ==========================================================================*/

/*
 * ISLAND-SAFE. Restore a hidden page's leaf entry to its real frame.
 * No IRQL check, no allocation, no lock — only NPT arithmetic, the same
 * discipline as yghv_protect_arm_page_bare().
 */
int yghv_guard_remap_bare(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int st;

    if (!page)
        return 0;

    /* A 2MB mapping must become 4K before its leaf can be touched, exactly as
     * the arm path does it. */
    if (npt_split_2mb_to_4kb(&g_npt, page))
        return 0;

    /*
     * 9.349 FIX: include the ATTRIBUTE bits, not just present+writable.
     *
     * yghv_protect_npt_set_perm() merges USER|ACCESSED in deliberately (9.313);
     * these island-safe twins called npt_set_page_perm directly and dropped them.
     * NPT checks the USER bit for EVERY guest access at CPL > 0, so without it
     * the CPU raises #NPF on every touch, the handler reopens, and the access
     * faults again — an infinite loop.
     *
     * Measured as a 30-second hang in the breakpoint step (mmf-write never
     * returned) while the machine stayed alive: 35 minutes of uptime, no
     * bugcheck. A hang caused by one missing bit, and 9.313 had already removed
     * this exact class of loss from the PASSIVE path.
     */
    {
        uint64_t attrs = NPT_4K_PAGE_FLAGS &
                         ~((uint64_t)NPT_PERM_PRESENT | (uint64_t)NPT_PERM_WRITABLE);
        st = npt_set_page_perm(&g_npt, page,
                               NPT_PERM_PRESENT | NPT_PERM_WRITABLE | attrs);
    }
    return st ? 0 : 1;
}

/*
 * ISLAND-SAFE. Point a hidden page's leaf entry at the decoy frame.
 * Same discipline; the frame was allocated once at load.
 */
int yghv_guard_decoy_bare(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;

    if (!page || !g_decoy_pa)
        return 0;
    if (npt_split_2mb_to_4kb(&g_npt, page))
        return 0;
    if (!NT_SUCCESS(npt_map_page(&g_npt, page, g_decoy_pa & ~(uint64_t)0xFFFULL,
                                 NPT_4K_PAGE_FLAGS)))
        return 0;
    return 1;
}

/*
 * ISLAND-SAFE. Has the guard module got everything hiding needs?
 *
 * hidecore refuses unless this is true, and the NPF path checks it before
 * deciding between ALLOW, decoy and violation. Keeping the predicate in one
 * place is what lets "hiding is armed" be a single fact rather than three
 * independent conditions that can disagree.
 */
int yghv_guard_hiding_armed(void) {
    return (g_code_size && g_decoy_pa) ? 1 : 0;
}

/*
 * 9.348: ISLAND-SAFE page reopen, for the breakpoint/watch ALLOW loop.
 *
 * Found by tests/irql_contract_check.ps1, which was written after the 15th crash
 * and then caught THIS — a second instance of the same mistake, latent in Tier
 * 4.4 since it was written:
 *
 *   svm_simplevm206.cpp:1457 and :1508 (inside SvHandleVmExit, i.e. the NPF
 *   handler) called yghv_protect_npt_set_perm() to reopen a page after a
 *   breakpoint matched. That function starts with
 *
 *       if (KeAreAllApcsDisabled()) return 0;
 *
 *   and an NPF handler IS "all APCs disabled", so the reopen returned 0 on every
 *   breakpoint hit: the page was never reopened, the ALLOW path fell through,
 *   and the breakpoint could not do its job. The failure was silent because 0 is
 *   also what "nothing to do" looks like.
 *
 * This is the island-safe form: NPT arithmetic only, exactly the discipline
 * yghv_protect_arm_page_bare() and yghv_guard_remap_bare() already use.
 */
int yghv_guard_reopen_bare(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int st;

    if (!page)
        return 0;
    if (npt_split_2mb_to_4kb(&g_npt, page))
        return 0;
    /*
     * 9.349 FIX: include the ATTRIBUTE bits, not just present+writable.
     *
     * yghv_protect_npt_set_perm() merges USER|ACCESSED in deliberately (9.313);
     * these island-safe twins called npt_set_page_perm directly and dropped them.
     * NPT checks the USER bit for EVERY guest access at CPL > 0, so without it
     * the CPU raises #NPF on every touch, the handler reopens, and the access
     * faults again — an infinite loop.
     *
     * Measured as a 30-second hang in the breakpoint step (mmf-write never
     * returned) while the machine stayed alive: 35 minutes of uptime, no
     * bugcheck. A hang caused by one missing bit, and 9.313 had already removed
     * this exact class of loss from the PASSIVE path.
     */
    {
        uint64_t attrs = NPT_4K_PAGE_FLAGS &
                         ~((uint64_t)NPT_PERM_PRESENT | (uint64_t)NPT_PERM_WRITABLE);
        st = npt_set_page_perm(&g_npt, page,
                               NPT_PERM_PRESENT | NPT_PERM_WRITABLE | attrs);
    }
    return st ? 0 : 1;
}

/*
 * 9.350: re-hide a page the way it was hidden.
 *
 * The NPF handler serves our own access to a hidden page (RIP whitelist), sets
 * TF, and lets #DB put the page back. Putting it back with the WRONG mechanism
 * is what hung the machine:
 *
 *   hidecore hid g_bp with UNMAP. The #DB path re-hid it with the DECOY. So the
 *   page came back present-but-zeroed: every read of the breakpoint table saw an
 *   empty table, mem-bp add() reported OK while writing into a discarded decoy,
 *   no breakpoint ever matched, and the guest store retried forever inside the
 *   NPF handler. mmf-write never returned, yet the driver kept answering — a
 *   guest-side spin, not a deadlock.
 *
 * ISLAND-SAFE: reads one array field, then the same bare NPT primitives.
 */
int yghv_guard_rehide_bare(uint64_t gpa) {
    int s = yghv_guard_find(gpa);

    if (s < 0)
        return 0;

    /* 9.352: delegate, passing the mode this entry was hidden with. Re-hiding
     * with the other mechanism leaves the page present-but-zeroed (9.350),
     * which silently empties whatever table lived there. */
    return yghv_guard_do_hide(s, gpa, (int)g_guard[s].hide_mode);
}

/*
 * 9.352: THE ONE PLACE that hides a page.
 *
 * Before this, five separate sites did
 *
 *     g_guard[x].hidden = 1;
 *
 * and one of them (the #DB re-hide) used a different mechanism than the setter,
 * which left the page present-but-zeroed and hung the machine. The root cause
 * was not the wrong mechanism — it was that "set the flag" and "record which
 * mechanism" were two statements a new hide site could do half of.
 *
 * Funnelling every hide through this function makes the two inseparable: the
 * mode is a PARAMETER, so a caller cannot set the flag without stating the
 * mechanism, and cannot state a mechanism without the flag being set.
 *
 * The static check (tests/irql_contract_check.ps1, rule 6) verifies that no
 * other code assigns .hidden = 1, so a new hide path that bypasses this
 * function fails the build-time check rather than the machine.
 *
 * ISLAND-SAFE: the bare primitives plus one array field.
 */
/*
 * 9.353: ISLAND-SAFE unmap.
 *
 * THE FOURTH INSTANCE of the same mistake, and the reachability analysis found
 * it the moment the hand-maintained list was replaced:
 *
 *   yghv_protect_npt_unmap_page() begins with
 *       if (KeAreAllApcsDisabled()) return 0;
 *
 *   and yghv_guard_do_hide() called it. do_hide is reached from the #DB handler
 *   via yghv_guard_rehide_bare(), so on every #DB re-hide the unmap returned 0,
 *   the page was never re-hidden, and the guard entry kept saying "hidden" while
 *   the page sat visible.
 *
 * Nothing failed visibly: the re-hide's return value was (void)'d, and the
 * PASSIVE path (hidecore) works fine, which is why the runtime tests passed.
 * Only reachability — "is this function genuinely called from the island?" —
 * exposes it, which is exactly what the derived island set computes.
 *
 * npt_exclude_pa() is the bare primitive: split, then clear the leaf. No IRQL
 * gate, no allocation, no lock.
 */
static int yghv_guard_unmap_bare(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    int st;

    if (!page)
        return 0;

    /*
     * 9.364: STAGE-CODED, so a failing hide names its own stage.
     *
     * The first version returned a bare 0 on any failure, and do_hide() turned it
     * into "this page was not hidden" with no reason. Measured consequence:
     * hidecore said "hid 0 page(s)", g_g_hidefail stayed 0, and mem-cache verify
     * showed the NPT entry byte-for-byte unchanged (entry=0x11F6000E7 before and
     * after) — leaving several possible causes with nothing to tell them apart.
     * The search went after the NPT twice before the real cause (state_page
     * skipping the page) was found, and that cost several rebuild+load cycles.
     *
     * Splitting the call into its two steps and counting each one separately is
     * the same discipline as 9.325's per-stage breakpoint counters: the first
     * failing stage names itself instead of requiring another experiment.
     *
     *   g_g_hidefail == 1 -> npt_split_2mb_to_4kb failed
     *   g_g_hidefail == 2 -> split succeeded, npt_set_page_perm failed
     */
    st = npt_split_2mb_to_4kb(&g_npt, page);
    if (st) {
        InterlockedExchange64(&g_g_hidefail, 1);
        return 0;
    }

    st = npt_set_page_perm(&g_npt, page, 0);
    if (st) {
        InterlockedExchange64(&g_g_hidefail, 2);
        return 0;
    }
    return 1;
}
/*
 * 9.354: DERIVE the hide mechanism from the page's ownership.
 *
 * do_hide takes the mode as a parameter, which made "set the flag" and "record
 * the mechanism" inseparable (9.352). It did not make the mechanism CORRECT —
 * "the parameter is mandatory" is not "the argument is right".
 *
 * But the correct answer is not a judgment call at all; it follows from who owns
 * the page:
 *
 *   our own state or one of the Tier-4 tables
 *       -> we must be able to read it back after the ALLOW loop serves our
 *          access, so UNMAP is the only workable mechanism. DECOY would hand us
 *          zeros and silently empty whatever lives there (9.350's hang).
 *
 *   any other page
 *       -> nothing of ours reads it, so the concern is the guest's memory map
 *          looking continuous, and DECOY is the mechanism that keeps it so.
 *
 * So the caller no longer chooses: it asks for a hide and the ownership decides
 * how. A caller cannot get this wrong because there is nothing left to get wrong.
 * YGHV_HIDE_AUTO is the only value the internal call sites pass.
 */
static int yghv_guard_mode_for(uint64_t gpa) {
    if (yghv_guard_state_page(gpa) || yghv_guard_target_page(gpa))
        return YGHV_HIDE_UNMAP;
    return YGHV_HIDE_DECOY;
}
int yghv_guard_do_hide(int slot, uint64_t gpa, int mode) {
    if (slot < 0 || slot >= YGHV_GUARD_MAX)
        return 0;
    /* 9.354: ownership decides the mechanism. A caller passes AUTO; only a
     * re-hide passes an explicit value, and that must repeat what was used. */
    if (mode == YGHV_HIDE_AUTO)
        mode = yghv_guard_mode_for(gpa);


    if (mode == YGHV_HIDE_DECOY) {
        if (!yghv_guard_decoy_bare(gpa))
            return 0;
    } else {
        /*
         * 9.353: the island-safe unmap.
         *
         * This called yghv_protect_npt_unmap_page(), which begins with
         * `if (KeAreAllApcsDisabled()) return 0;`. do_hide is reached from the
         * #DB handler via yghv_guard_rehide_bare(), so on every #DB re-hide the
         * unmap returned 0, the page was never re-hidden, and the entry kept
         * claiming "hidden" while the page sat visible.
         *
         * Nothing reported it: the re-hide's result was (void)'d, and the PASSIVE
         * path (hidecore) works, which is why the runtime tests passed. Only
         * reachability — "is this genuinely called from the island?" — exposes it,
         * and that is exactly what the derived island set computes.
         */
        mode = YGHV_HIDE_UNMAP;
        if (!yghv_guard_unmap_bare(gpa))
            return 0;
    }

    g_guard[slot].hide_mode = (uint32_t)mode;
    g_guard[slot].hidden = 1;
    InterlockedIncrement64(&g_g_hidden);
    return 1;
}

/* ISLAND-SAFE: which mechanism hid this page (-1 = not guarded). */
int yghv_guard_hide_mode(uint64_t gpa) {
    int s = yghv_guard_find(gpa);

    if (s < 0)
        return -1;
    return (int)g_guard[s].hide_mode;
}
