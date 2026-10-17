/*
 * mem_view.h — Tier 4.1 + 4.3: split-view memory and cross-process aliasing.
 *
 * What these are
 * --------------
 * 4.1 split-view: the SAME guest-physical page presents DIFFERENT content
 *     depending on who is looking. The target sees its real frame; a
 *     detector (or our own reader) sees a decoy frame.
 *
 * 4.3 aliasing: process A's VA is made to point at process B's frame —
 *     established unilaterally, with no IPC, no shared section, no handle,
 *     and without either process being told.
 *
 * Both reduce to the same primitive, and 9.311 proved it works on this CPU:
 * rewriting an NPT entry so a GPA resolves to a different SPA IS observed by
 * the guest. (9.297/9.298's "NPT translations ignore software flushes" finding
 * was about PERMISSION flips; address remaps behave differently — measured.)
 *
 * How the view is selected
 * ------------------------
 * The NPT base is per-VCPU (VMCB ControlArea.NCr3). Switching it per-VCPU is
 * a whole-table switch, which is too coarse for "who is reading". Instead:
 *
 *   - The MAIN NPT carries the real mapping.
 *   - The ALT NPT carries the decoy mapping for the same GPA.
 *   - A VMEXIT hook picks the NPT for the next run based on the guest's CR3:
 *     if CR3 belongs to a registered "observer" process, run on ALT;
 *     otherwise run on MAIN.
 *
 * The switch happens at VMRUN, so it costs one store per exit, not per
 * access. That is the only way to do this without a per-access hook.
 *
 * Honest limits
 * -------------
 *   - The choice is per-CORE, not per-access: if the target and an observer
 *     run on the same core in the same time slice, the last VMRUN wins. For
 *     the intended use (detector runs in its own process, usually its own
 *     time slice) this is fine; it is NOT instruction-granular.
 *   - 9.311's remap works, but whether a GPA->SPA change is picked up WITHOUT
 *     a TLB flush depends on the CPU not having the old translation cached.
 *     We therefore flush the relevant entry (INVLPGA for the GVA, or a full
 *     flush) when a view is installed — the same mechanism the C-line guard
 *     abandoned for permissions, but remaps are re-read (9.311).
 */
#ifndef YGHV_MEM_VIEW_H
#define YGHV_MEM_VIEW_H

#include "svm_defs.h"
#include <ntddk.h>
#include "mem_pte.h"

#define YGHV_VIEW_MAX_OBSERVERS  8
#define YGHV_VIEW_MAX_PAGES      32

/* One split page: a GPA whose SPA differs between the two views. */
typedef struct {
    uint64_t gpa;          /* guest physical page (page-aligned) */
    uint64_t real_spa;     /* what the target sees (main NPT) */
    uint64_t decoy_spa;    /* what observers see (alt NPT) */
    volatile LONG active;
} yghv_view_page_t;

/* Diagnostics */
typedef struct {
    uint64_t pages;            /* split pages installed */
    uint64_t observers;        /* observer CR3s registered */
    uint64_t switches_to_alt;  /* times a run was started on the decoy view */
    uint64_t switches_to_main;
    uint64_t installs;
    uint64_t failures;
} yghv_view_stats_t;

/*
 * Bring the split view up. Clones the main NPT into the alt table (identity),
 * so the alt starts as an exact copy and only the split pages differ.
 * PASSIVE_LEVEL. Returns 1 on success.
 */
int yghv_view_init(void);

void yghv_view_stop(void);
int  yghv_view_ready(void);

/*
 * Split one GPA: real_spa for everyone, decoy_spa for registered observers.
 *
 * Requires yghv_view_init() first. PASSIVE_LEVEL.
 * Returns 1 on success.
 */
int yghv_view_add(uint64_t gpa, uint64_t real_spa, uint64_t decoy_spa);

/* Remove a split page (restores identity in both tables). */
int yghv_view_remove(uint64_t gpa);

/* Register a process (by CR3) as an observer: it sees decoy pages. */
int yghv_view_add_observer(uint64_t cr3);
int yghv_view_remove_observer(uint64_t cr3);

/*
 * Called from the VMEXIT path (or a per-core callback) to decide which NPT
 * the next run should use, given the guest's current CR3.
 *
 * Returns the NPT base PA to program into VMCB ControlArea.NCr3.
 * Lock-free (the island constraint): reads only nonpaged arrays.
 */
uint64_t yghv_view_pick_ncr3(uint64_t guest_cr3);

/* The two table bases (0 if not ready). */
uint64_t yghv_view_main_pa(void);
uint64_t yghv_view_alt_pa(void);

void yghv_view_stats_get(yghv_view_stats_t *out);
void yghv_view_stats_reset(void);

#endif /* YGHV_MEM_VIEW_H */
