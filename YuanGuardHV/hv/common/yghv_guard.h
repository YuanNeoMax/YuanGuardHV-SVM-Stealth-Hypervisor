/*
 * yghv_guard.h — Tier 7.1/7.3: self-hiding and integrity self-check.
 *
 * The exposure
 * ------------
 * Our tables are ordinary physical pages below 4GB:
 *
 *     MmAllocateContiguousMemory(HV_PAGE_SIZE, {4GB})   (npt_core.c)
 *
 * The NPT identity-maps physical memory for the guest, so a guest that walks
 * its own address space can READ and WRITE those pages. It can rewrite an NPT
 * entry, blank a breakpoint slot, or scribble over a frame reservation — the
 * three structures everything else depends on. Nothing detects it, and nothing
 * stops it.
 *
 * Two answers, and they are complementary
 * ---------------------------------------
 * 7.1 SELF-HIDING (hide): take the pages out of the guest's view in the NPT.
 *     We remove their leaf entries from the identity map. The guest's access
 *     then raises #NPF; the handler records it as a probe and DELIVERS AN
 *     ACCESS VIOLATION, so from the guest's side the physical range simply is
 *     not there. Our own code never touches these pages through the guest
 *     mapping — every table access uses the recorded DIRECT VA (9.280) — so
 *     hiding them costs us nothing.
 *
 *     Honestly stated limit: the address space is now discontinuous. A guest
 *     that compares memory descriptors (MmGetPhysicalMemoryRanges, the
 *     firmware map in the registry) against what it can actually touch will
 *     see holes. Hiding from a walk is not hiding from an inventory.
 *
 * 7.3 INTEGRITY (detect): hash the guarded pages at arm time and re-check on
 *     demand and on a timer. Hiding is not perfect, so assume someone gets
 *     through: a changed hash names exactly which page changed, and the caller
 *     can refuse further work rather than act on corrupted tables.
 *
 * Why both
 * --------
 * Hiding alone is silent when it fails. Detection alone leaves the pages
 * writable. Together, an attacker either cannot reach the page or gets caught
 * trying, and the two mechanisms answer different questions ("can they?" vs
 * "did they?").
 */
#ifndef YGHV_GUARD_H
#define YGHV_GUARD_H

#include "svm_defs.h"
#include <ntddk.h>

#define YGHV_GUARD_MAX      32
#define YGHV_GUARD_TAG_LEN  16

typedef struct {
    uint64_t gpa;        /* physical page being guarded */
    uint64_t hash;       /* FNV-1a over the OBJECT bytes at arm time */
    uint32_t hash_off;   /* 9.339: where the object starts in the page */
    uint32_t hash_len;   /* 9.339: how many bytes of it are hashed */
    uint64_t checks;     /* times re-verified */
    uint64_t changes;    /* times the hash differed */
    uint32_t hidden;     /* 1 = leaf entry removed from the guest identity map */
    uint32_t armed;
    char     tag[YGHV_GUARD_TAG_LEN];   /* "npt-pml4", "bp-table", ... */
} yghv_guard_info_t;

/*
 * Guard one physical page.
 *
 * `hide` removes it from the guest's identity map (7.1); `hash` records a
 * content hash (7.3). PASSIVE.
 *
 * Returns the slot index, or a negative stage code:
 *   -1 wrong IRQL, -2 bad argument, -3 table full,
 *   -4 could not hide (no NPT entry there / remap failed),
 *   -5 could not read the page to hash it.
 */
int yghv_guard_add(uint64_t gpa, const char *tag, int hide, int hash);

/*
 * 9.339: full form with an explicit object range.
 *
 * WHY: a page holds more than one object. .bss packs the frame table and the
 * guard's own table into the SAME 4K page (0x34000), so hashing the whole page
 * also hashes the guard's bookkeeping — and the guard writing that bookkeeping
 * invalidates a hash it just took. Measured as "2 page(s) changed" on a
 * completely untouched machine.
 *
 * Hashing only `off..off+len` is both correct and what the feature claims:
 * "this object did not change", not "nothing on this page changed".
 */
int yghv_guard_add_range(uint64_t gpa, uint32_t off, uint32_t len,
                         const char *tag, int hide, int hash);

/* Stop guarding one page, restoring the guest's access if we removed it. */
int yghv_guard_add_va(const void *va, uint32_t len, const char *tag, int hide);
int yghv_guard_remove(uint64_t gpa);

/*
 * Re-check every guarded page. Returns the number whose content changed since
 * the last check. This is the 7.3 answer to "did they get through anyway".
 * PASSIVE.
 */
uint32_t yghv_guard_verify_all(void);

/* Check just this page; returns 1 intact, 0 changed, -1 not guarded. */
int yghv_guard_verify(uint64_t gpa);

/*
 * ISLAND-SAFE: is this page one we hid?
 *
 * Called from the NPF handler. When it returns 1 the caller MUST NOT restore
 * the page — it delivers an access violation instead, so the range stays
 * invisible.
 */
int yghv_guard_is_hidden(uint64_t gpa);

/* Probe counter: how many times the guest reached for a hidden page. */
uint64_t yghv_guard_probes(void);

/* Enumerate. */
uint32_t yghv_guard_list(yghv_guard_info_t *out, uint32_t max);

/*
 * Convenience: guard the NPT root itself plus everything reachable from it
 * (the tables the manager actually allocated and recorded), then the
 * breakpoint table and the frame table. Returns how many pages were guarded.
 */
uint32_t yghv_guard_arm_core(void);

typedef struct {
    uint64_t guarded;
    uint64_t hidden;
    uint64_t verified;
    uint64_t changed;      /* pages whose content differed on re-check */
    uint64_t probes;       /* guest accesses to hidden pages */
    uint64_t hide_fail;
    uint64_t hash_fail;
} yghv_guard_stats_t;

void yghv_guard_stats_get(yghv_guard_stats_t *out);
void yghv_guard_stats_reset(void);

#endif /* YGHV_GUARD_H */
