/*
 * mem_bp.h — Tier 4.4: instruction-level access control (three mechanisms).
 *
 * A breakpoint here can be enforced three different ways. Each has a property
 * the others do not, so the caller picks per breakpoint:
 *
 *   NPF   take the page's NPT permission away. The guest's access raises
 *         #NPF and we record it. No code bytes change, no DR register is
 *         touched, so nothing the target can inspect reveals it. Granularity
 *         is the PAGE, and the cost is one VMEXIT per access to that page.
 *
 *   INT3  write 0xCC over the first byte of the instruction. The guest takes
 *         #BP exactly at that instruction — true instruction granularity, and
 *         no per-access exit cost once armed. The cost is that the code byte
 *         REALLY IS modified (a target that checksums its own code sees it),
 *         and we must restore the original byte before resuming.
 *
 *   DR    program the debug registers. x86 gives four, and a DR access by the
 *         guest is intercepted (VMCB dr_read/dr_write_intercepts) so we can
 *         present a virtual DR bank. DR breakpoints are exact on the address
 *         and cost nothing until they fire, but they consume the four
 *         hardware slots.
 *
 * The three share one table so they share the hit ring, stats and drain
 * interface. A page can carry a NPF breakpoint while a specific instruction
 * on it carries an INT3 breakpoint — they compose.
 *
 * What NPF alone cannot do (measured, see docs/TIER4_WATCH_DESIGN.md):
 * execution monitoring. The NPT has only NX, and NX is per-page, so
 * "execute faults but the page is still runnable" does not exist. That is
 * exactly why INT3 and DR modes are here.
 */
#ifndef YGHV_MEM_BP_H
#define YGHV_MEM_BP_H

#include "svm_defs.h"
#include <ntddk.h>

/* 9.327: 8 -> 64. NPF mode needs no hardware slot, so the only real limit is
 * the table; 64 covers a whole function's worth of sites. */
#define YGHV_BP_MAX  64

/* Access kinds */
#define YGHV_BP_READ   0x1
#define YGHV_BP_WRITE  0x2
#define YGHV_BP_EXEC   0x4

/* 9.327: how the breakpoint is enforced */
#define YGHV_BP_MODE_NPF   1   /* NPT permission revoked; page granularity */
#define YGHV_BP_MODE_INT3  2   /* 0xCC written at the instruction */
#define YGHV_BP_MODE_DR    4   /* debug register slot */

/* 9.327: hardware DR slots available (x86 gives exactly four) */
#define YGHV_BP_DR_SLOTS   4
#define YGHV_BP_NO_SLOT    0xFFFFFFFFu

typedef struct {
    uint64_t gpa;        /* page watched (NPF mode: the armed page) */
    uint64_t va;         /* guest VA (INT3 mode: the patched instruction) */
    uint64_t rip;        /* instruction start (0 = any instruction) */
    uint32_t kinds;      /* bitmask of YGHV_BP_* */
    uint32_t mode;       /* YGHV_BP_MODE_* */
    uint8_t  orig_byte;  /* INT3 mode: the byte we replaced with 0xCC */
    uint32_t dr_slot;    /* DR mode: which of DR0-DR3, or YGHV_BP_NO_SLOT */
    uint64_t hits;       /* times this breakpoint matched */
    volatile LONG active;
} yghv_bp_t;

/*
 * Add a breakpoint (NPF mode; `rip` 0 means "any instruction on this page").
 *
 * PASSIVE. Returns 1 on success, or a negative stage code:
 *   -1 wrong IRQL, -2 engine is not PTE, -3 split failed, -4 permission failed,
 *   -5 table full, -6 bad mode, -7 INT3 needs a VA, -8 no DR slot free,
 *   -9 could not patch the instruction byte.
 */
int yghv_bp_add(uint64_t gpa, uint64_t rip, uint32_t kinds);

/* Full form: choose the mode and supply the VA for INT3. */
int yghv_bp_add_ex(uint64_t gpa, uint64_t va, uint64_t rip,
                   uint32_t kinds, uint32_t mode);

/* Remove by (gpa, rip). Restores whatever the mode changed. */
int yghv_bp_remove(uint64_t gpa, uint64_t rip);

/* Remove every breakpoint of one mode (0 = all). */
int yghv_bp_clear_mode(uint32_t mode);

/*
 * Is this page currently guarded by a NPF breakpoint?
 *
 * The write engine MUST ask this before writing. A bp page has its NPT write
 * permission revoked, and measurement showed that our own engine's write to
 * such a frame fails hard (0x3B / 0x50 across five crashes). A breakpoint is
 * a MONITOR — it exists to observe the guest, not to let us write through it.
 *
 * Lock-free, callable at any IRQL.
 */
int yghv_bp_is_armed(uint64_t gpa);

/*
 * Called from the NPF handler. Returns 1 if this fault matches a registered
 * NPF-mode breakpoint AND the caller should take the ALLOW path (record,
 * restore, single-step, re-arm).
 *
 * ISLAND-SAFE: scans a nonpaged array, records into a preallocated ring.
 */
int yghv_bp_on_npf(uint64_t gpa, uint64_t rip, uint32_t kind, uint32_t cpl,
                   uint64_t cr3);

/*
 * 9.327: called from the #BP handler (INT3 mode). `va` is the faulting
 * instruction address — for #BP the saved RIP points AT the 0xCC, so the
 * original instruction is the one about to run. Returns 1 when a matching
 * INT3 breakpoint was found and the caller should take the INT3 resume path
 * (restore the byte, rewind RIP, single-step, re-patch).
 *
 * ISLAND-SAFE.
 */
int yghv_bp_on_int3(uint64_t va, uint32_t cpl, uint64_t cr3);

/* Find an INT3 breakpoint by its VA. Returns the slot or -1. */
int yghv_bp_find_int3(uint64_t va);

/* Is `va` a patched INT3 instruction? Lock-free. */
int yghv_bp_int3_armed(uint64_t va);

/* The original byte we displaced at `va` (valid only while armed). */
int yghv_bp_int3_orig(uint64_t va, uint8_t *out);

/*
 * 9.327: called from the #DB handler for a DR-mode breakpoint. Returns the
 * breakpoint index that fired (per DR6), or -1.
 *
 * ISLAND-SAFE.
 */
int yghv_bp_on_dr(uint64_t dr6, uint64_t rip, uint32_t cpl, uint64_t cr3);

/*
 * 9.327: the virtual DR bank. The guest's DR0-DR3 accesses are intercepted;
 * these return what the guest should see, so a DR-mode breakpoint is invisible
 * in the register set the target can read.
 */
int yghv_bp_dr_slot_for_va(uint64_t va);
uint64_t yghv_bp_dr_get(int slot);
int yghv_bp_dr_set(int slot, uint64_t value);

/*
 * Drain recorded hits. Each entry is 6 qwords:
 *   rip, gpa, kind|cpl<<8|mode<<16, cr3, va, seq
 * PASSIVE. Returns the number of qwords written.
 */
uint32_t yghv_bp_drain(uint64_t *out, uint32_t max);

typedef struct {
    uint64_t count;      /* registered breakpoints */
    uint64_t npf;        /* how many are NPF mode */
    uint64_t int3;       /* INT3 mode */
    uint64_t dr;         /* DR mode */
    uint64_t hits;       /* matched faults */
    uint64_t recorded;
    uint64_t missed;     /* matched but ring full */
    uint64_t refusals;   /* faults that matched no bp */
    /* which guard refused bp_add */
    uint64_t fail_irql;
    uint64_t fail_engine;
    uint64_t fail_split;
    uint64_t fail_perm;
    uint64_t fail_table;
    uint64_t fail_mode;
    uint64_t fail_va;
    uint64_t fail_drslot;
    uint64_t fail_patch;
} yghv_bp_stats_t;

extern uint64_t g_dbg_args[8];   /* 9.327: last bp_add_ex arguments */
void yghv_bp_stats_get(yghv_bp_stats_t *out);
void yghv_bp_stats_reset(void);

#endif /* YGHV_MEM_BP_H */
