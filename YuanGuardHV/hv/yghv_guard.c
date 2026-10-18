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

/* 9.343: our own code range (RIP whitelist) and the decoy frame. Declared
 * here because the guard/decoy checks run in add paths defined below. */
static uint64_t g_code_start;
static uint64_t g_code_size;
static uint64_t g_decoy_pa;
static void *g_decoy_va;
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
                    if (yghv_guard_do_hide(_s, page, YGHV_HIDE_UNMAP)) {
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
        if (yghv_guard_do_hide(slot, page, YGHV_HIDE_UNMAP)) {
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
static int yghv_guard_state_page(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    uint64_t lo, hi;

    lo = (uint64_t)(ULONG_PTR)&g_guard[0];
    hi = (uint64_t)(ULONG_PTR)&g_guard[YGHV_GUARD_MAX - 1] + sizeof(g_guard[0]);
    if (page >= (lo & ~(uint64_t)0xFFFULL) && page <= (hi & ~(uint64_t)0xFFFULL))
        return 1;
    if (page == ((uint64_t)(ULONG_PTR)&g_guard_count & ~(uint64_t)0xFFFULL))
        return 1;
    if (page == ((uint64_t)(ULONG_PTR)&g_code_size & ~(uint64_t)0xFFFULL))
        return 1;
    if (page == ((uint64_t)(ULONG_PTR)&g_decoy_pa & ~(uint64_t)0xFFFULL))
        return 1;
    return 0;
}

/* Is this page one of the tables arm_core/hidecore target? */
static int yghv_guard_target_page(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    extern uint64_t *yghv_bp_table_va(uint32_t *bytes);
    extern uint64_t *yghv_frame_table_va(uint32_t *bytes);
    extern uint64_t *yghv_view_table_va(uint32_t *bytes);
    uint32_t bytes = 0;
    uint64_t *va;

    va = yghv_bp_table_va(&bytes);
    if (va && bytes && page >= ((uint64_t)(ULONG_PTR)va & ~(uint64_t)0xFFFULL) &&
        page <= (((uint64_t)(ULONG_PTR)va + bytes) & ~(uint64_t)0xFFFULL))
        return 1;
    va = yghv_frame_table_va(&bytes);
    if (va && bytes && page >= ((uint64_t)(ULONG_PTR)va & ~(uint64_t)0xFFFULL) &&
        page <= (((uint64_t)(ULONG_PTR)va + bytes) & ~(uint64_t)0xFFFULL))
        return 1;
    va = yghv_view_table_va(&bytes);
    if (va && bytes && page >= ((uint64_t)(ULONG_PTR)va & ~(uint64_t)0xFFFULL) &&
        page <= (((uint64_t)(ULONG_PTR)va + bytes) & ~(uint64_t)0xFFFULL))
        return 1;
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
                    if (yghv_guard_do_hide(_s, page, YGHV_HIDE_UNMAP)) {
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
            if (yghv_guard_do_hide(slot, page, YGHV_HIDE_UNMAP)) {
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

static uint64_t g_code_start;
static uint64_t g_code_size;

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

static uint64_t g_decoy_pa;
static void *g_decoy_va;

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

    if (!yghv_guard_hiding_armed())
        return 0;

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
                if (yghv_guard_state_page(gpa + off))
                    continue;
                {
                    int s = yghv_guard_find(gpa + off);
                    if (s < 0)
                        continue;
                    if (g_guard[s].hidden)
                        continue;
                    if (yghv_protect_npt_unmap_page(gpa + off)) {
                        if (yghv_guard_do_hide(s, gpa + off, YGHV_HIDE_UNMAP)) {
                            hid++;
                        }
                        InterlockedIncrement64(&g_g_hidden);
                        hid++;
                    }
                }
            }
        }
    }
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
int yghv_guard_do_hide(int slot, uint64_t gpa, int mode) {
    if (slot < 0 || slot >= YGHV_GUARD_MAX)
        return 0;

    if (mode == YGHV_HIDE_DECOY) {
        if (!yghv_guard_decoy_bare(gpa))
            return 0;
    } else {
        mode = YGHV_HIDE_UNMAP;
        if (!yghv_protect_npt_unmap_page(gpa))
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
