/*
 * yghv_guard.c — Tier 7.1/7.3: self-hiding and integrity self-check.
 * See common/yghv_guard.h for what hiding does and does not buy.
 */

#include "yghv_guard.h"
#include "protect.h"
#include "npt.h"
#include "mem_pa.h"

typedef struct {
    uint64_t gpa;
    uint64_t hash;
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
    uint32_t armed;
    char     tag[YGHV_GUARD_TAG_LEN];
    volatile LONG active;
} yghv_guard_t;

static yghv_guard_t g_guard[YGHV_GUARD_MAX];
static volatile LONG g_guard_count;

static volatile LONG64 g_g_guarded;
static volatile LONG64 g_g_hidden;
static volatile LONG64 g_g_verified;
static volatile LONG64 g_g_changed;
static volatile LONG64 g_g_probes;
static volatile LONG64 g_g_hidefail;
static volatile LONG64 g_g_hashfail;

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
static uint64_t yghv_guard_hash_region(uint64_t gpa, uint32_t off, uint32_t len) {
    uint8_t buf[256];
    uint64_t h = 0xcbf29ce484222325ULL;
    uint64_t done = 0;
    uint64_t base = gpa + off;

    if (len == 0 || len > 0x1000 || off + len > 0x1000)
        return 0;

    while (done < len) {
        uint32_t chunk = (uint32_t)((len - done) < sizeof(buf)
                                    ? (len - done) : sizeof(buf));
        int i;
        if (!yghv_pa_read(base + done, buf, chunk))
            return 0;                    /* 0 = "could not hash" */
        for (i = 0; i < (int)chunk; i++) {
            h ^= (uint64_t)buf[i];
            h *= 0x100000001b3ULL;
        }
        done += chunk;
    }
    return h ? h : 1;                    /* keep 0 reserved for failure */
}
static uint64_t yghv_guard_hash_va(const void *va, uint32_t len) {
    const uint8_t *p = (const uint8_t *)va;
    uint64_t h = 0xcbf29ce484222325ULL;
    uint32_t i;

    if (!va || len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001b3ULL;
    }
    return h ? h : 1;
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
    if (hide && yghv_guard_owns_page(page))
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
                    g_guard[_s].hidden = 1;
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
        uint64_t h = yghv_guard_hash_va(va, len);
        if (!h) {
            InterlockedIncrement64(&g_g_hashfail);
            return -5;
        }
        g_guard[slot].hash = h;
    }
    g_guard[slot].hash_off = 0;
    g_guard[slot].hash_len = len;
    g_guard[slot].hash_va = (uint64_t)(ULONG_PTR)va;   /* 9.340: re-hash here */

    if (hide && yghv_protect_npt_unmap_page(page)) {
        g_guard[slot].hidden = 1;
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
static int yghv_guard_owns_page(uint64_t gpa) {
    uint64_t page = gpa & ~(uint64_t)0xFFFULL;
    uint64_t lo, hi;

    /* the guard's own table + counters */
    lo = (uint64_t)(ULONG_PTR)&g_guard[0];
    hi = (uint64_t)(ULONG_PTR)&g_guard[YGHV_GUARD_MAX - 1] + sizeof(g_guard[0]);
    if (page >= (lo & ~(uint64_t)0xFFFULL) && page <= (hi & ~(uint64_t)0xFFFULL))
        return 1;
    if (page == ((uint64_t)(ULONG_PTR)&g_guard_count & ~(uint64_t)0xFFFULL))
        return 1;

    /* the three tables arm_core wants to protect — same argument applies to
     * each: we read them with plain C from IOCTL context, so hiding any of
     * them hides it from us. */
    {
        extern uint64_t *yghv_bp_table_va(uint32_t *bytes);
        extern uint64_t *yghv_frame_table_va(uint32_t *bytes);
        extern uint64_t *yghv_view_table_va(uint32_t *bytes);
        uint32_t bytes = 0;
        uint64_t *va = yghv_bp_table_va(&bytes);
        if (va && bytes &&
            page >= (((uint64_t)(ULONG_PTR)va) & ~(uint64_t)0xFFFULL) &&
            page <= (((uint64_t)(ULONG_PTR)va + bytes) & ~(uint64_t)0xFFFULL))
            return 1;
        va = yghv_frame_table_va(&bytes);
        if (va && bytes &&
            page >= (((uint64_t)(ULONG_PTR)va) & ~(uint64_t)0xFFFULL) &&
            page <= (((uint64_t)(ULONG_PTR)va + bytes) & ~(uint64_t)0xFFFULL))
            return 1;
        va = yghv_view_table_va(&bytes);
        if (va && bytes &&
            page >= (((uint64_t)(ULONG_PTR)va) & ~(uint64_t)0xFFFULL) &&
            page <= (((uint64_t)(ULONG_PTR)va + bytes) & ~(uint64_t)0xFFFULL))
            return 1;
    }

    return 0;
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

    if (hide && yghv_guard_owns_page(page))
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
                    g_guard[_s].hidden = 1;
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
        uint64_t h = yghv_guard_hash_region(page, off, len);
        if (!h) {
            InterlockedIncrement64(&g_g_hashfail);
            return -5;
        }
        g_guard[slot].hash = h;
        g_guard[slot].hash_off = off;
        g_guard[slot].hash_len = len;
    }

    if (hide) {
        if (!yghv_protect_npt_unmap_page(page)) {
            InterlockedIncrement64(&g_g_hidefail);
            if (!hash)
                return -4;
        } else {
            g_guard[slot].hidden = 1;
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
    uint64_t h;

    if (slot < 0)
        return -1;
    if (!g_guard[slot].hash)
        return 1;                        /* hash-only wasn't requested */

    if (g_guard[slot].hash_va)
            h = yghv_guard_hash_va((const void *)g_guard[slot].hash_va,
                                   g_guard[slot].hash_len);
        else
            h = yghv_guard_hash_region(g_guard[slot].gpa, g_guard[slot].hash_off,
                                       g_guard[slot].hash_len);
    if (!h)
        return -1;

    g_guard[slot].checks++;
    InterlockedIncrement64(&g_g_verified);

    if (h != g_guard[slot].hash) {
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
        out[n].hash = g_guard[i].hash;
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
