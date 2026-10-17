/*
 * mem_coop.h — Phase 7: guard cooperation + zero-copy shared window.
 *
 * Two abilities, both built on machinery that already exists:
 *
 * 2.3 — Guard-aware write ("protected write")
 *   The sync guard (protect.c, the C line) rolls a page back when its
 *   content drifts from a snapshot. A write from Engine B goes to the
 *   PHYSICAL FRAME, which the guard's poll then sees as drift and undoes —
 *   so an ordinary write to a guarded page does not stick.
 *
 *   This module makes the write COOPERATE: it tells the guard "this page is
 *   about to change by my hand", so the guard re-snapshots instead of
 *   restoring. The write then persists without disarming protection.
 *
 *   Why this is only possible here: the guard is ours. A host-side writer
 *   has no way to inform it, so its writes get rolled back — the page is
 *   effectively write-protected against everyone except us.
 *
 * 2.4 — Zero-copy shared window
 *   Instead of an IOCTL round trip per read, a page is mapped once into BOTH
 *   the target and the caller, and both sides read/write it directly. The
 *   IOCTL is needed only to set the window up, not per access.
 *
 *   Honest limit: the two views share one frame only if both map the SAME
 *   physical page. For a target in another process that means locating its
 *   frame and mapping it into the caller too — which is what the guard's
 *   frame bookkeeping already tracks.
 */
#ifndef YGHV_MEM_COOP_H
#define YGHV_MEM_COOP_H

#include "svm_defs.h"
#include <ntddk.h>

/*
 * Notify the guard that a page is being modified deliberately, so its poll
 * re-snapshots instead of restoring.
 *
 * Returns 1 if the page is guarded and the guard accepted the update, 0 if
 * the page is not guarded (caller should write normally).
 * Safe to call with the guard off.
 */
int yghv_coop_notify_write(uint64_t gpa);

/*
 * The cooperative write: notify, then write the frame.
 *
 * `pa` is the physical address (from a page-table walk), `len` may span
 * pages. Returns 1 on success. PASSIVE_LEVEL (uses the MDL path).
 */
int yghv_coop_write(uint64_t pa, const void *src, SIZE_T len);

/* Is a gpa currently under guard? */
int yghv_coop_is_guarded(uint64_t gpa);

/* Diagnostics */
typedef struct {
    uint64_t notifies;       /* cooperative writes attempted */
    uint64_t guarded_hits;   /* page was guarded and accepted */
    uint64_t unguarded;      /* page not guarded (plain write) */
    uint64_t rollbacks_avoided;
} yghv_coop_stats_t;

void yghv_coop_stats_get(yghv_coop_stats_t *out);
void yghv_coop_stats_reset(void);

#endif /* YGHV_MEM_COOP_H */
