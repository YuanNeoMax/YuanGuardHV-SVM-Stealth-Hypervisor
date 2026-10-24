/*
 * mem_watch.h — Tier 4.6: NPF-based watchpoints (debug-register superset).
 *
 * x86 gives a debugger FOUR hardware breakpoints (DR0-DR3), and software
 * breakpoints need int3 (a code byte change the target can see). This module
 * uses the nested-page-fault mechanism instead:
 *
 *   - take the page's NPT permission away (no present / no write)
 *   - any access by the guest raises #NPF
 *   - the handler records (CR3, RIP, GPA, read/write/execute) and then lets
 *     the access through by restoring the permission
 *
 * That gives an unbounded number of watchpoints, no code modification, and no
 * DR registers touched (so a target that reads DR0-DR3 sees nothing).
 *
 * What the island can and cannot do here
 * --------------------------------------
 * The handler runs at DISPATCH with GIF=0. It may only touch nonpaged memory
 * and must not call anything that blocks. Recording an event is therefore a
 * few stores into a preallocated ring — no allocation, no locks, no logging.
 * The ring is drained later from PASSIVE (the IOCTL that reads it).
 *
 * Honest limit
 * ------------
 * "Execute" watchpoints are approximate: x86 has no per-page execute fault on
 * NPT except NX, and NX faults the whole page, not one address. Read and
 * write are exact (per-page). A watch on one page therefore reports any
 * access to that page, not to one byte inside it.
 */
#ifndef YGHV_MEM_WATCH_H
#define YGHV_MEM_WATCH_H

#include "svm_defs.h"
#include <ntddk.h>

#define YGHV_WATCH_MAX_PAGES  16
#define YGHV_WATCH_RING       256     /* events retained (power of two) */

/* Access kinds as reported */
#define YGHV_WATCH_READ       0x1
#define YGHV_WATCH_WRITE      0x2
#define YGHV_WATCH_EXEC       0x4

typedef struct {
    uint64_t cr3;
    uint64_t rip;
    uint64_t gpa;
    uint32_t kind;      /* YGHV_WATCH_* */
    uint32_t cpl;
    uint64_t seq;       /* monotonically increasing */
    /*
     * 9.366: padding that exists so the ring ends on a page boundary.
     *
     * The event is 40 bytes, and 40 * 256 = 10240 = two and a half pages — so
     * the ring's tail left 2048 bytes for the linker to fill with whatever
     * cold scalar it liked, on a page the island writes on every hit. Padding
     * the entry to 48 makes 256 * 48 = 12288 exactly three pages, which the
     * compile-time assertion in mem_watch.c enforces.
     *
     * The drain side copies four qwords per event and never reads this, so the
     * wire format is unchanged.
     */
    uint64_t _pad;
} yghv_watch_event_t;

typedef struct {
    uint64_t gpa;        /* watched page */
    uint32_t kinds;      /* which accesses to report (bitmask) */
    uint32_t hits;       /* times this page was reported */
    volatile LONG active;
} yghv_watch_page_t;

/* Add/remove a watch. PASSIVE (touches NPT). Returns 1 on success. */
int yghv_watch_add(uint64_t gpa, uint32_t kinds);
int yghv_watch_remove(uint64_t gpa);

/* Is this GPA watched, and does it want this access kind? Lock-free, island-safe. */
int yghv_watch_match(uint64_t gpa, uint32_t kind);

/*
 * Record one event. ISLAND-SAFE: stores only, no calls, no allocation.
 * Drops the event if the ring is full (and counts the drop).
 */
void yghv_watch_record(uint64_t cr3, uint64_t rip, uint64_t gpa,
                       uint32_t kind, uint32_t cpl);

/* Drain up to `max` events into `out`. PASSIVE. Returns the count. */
uint32_t yghv_watch_drain(yghv_watch_event_t *out, uint32_t max);

/*
 * 9.368 (A3, Tier 5.6): NON-DESTRUCTIVE copy of the retained trace.
 *
 * drain() is a queue consumer — an event read once is gone. A trace needs the
 * opposite: read the same window again, look at a different part of it, size a
 * buffer before allocating. So:
 *
 *   yghv_watch_snapshot(out, max)  copies up to `max` events in ring order
 *                                  (oldest first) and does NOT advance tail.
 *                                  Returns how many were copied.
 *   yghv_watch_depth(void)         how many events are retained right now,
 *                                  without moving anything.
 *
 * The producer keeps writing while the copy runs, so a snapshot may gain events
 * at its end; the returned count is "at least this many were present when the
 * read started". A torn event can only mean one entry is old or repeated.
 *
 * NOT a substitute for drain(): draining is what frees slots. A caller that only
 * ever snapshots will eventually see yghv_watch_dropped climb.
 */
uint32_t yghv_watch_snapshot(yghv_watch_event_t *out, uint32_t max);
uint32_t yghv_watch_depth(void);

/* Diagnostics */
typedef struct {
    uint64_t pages;
    uint64_t total_hits;
    uint64_t recorded;
    uint64_t dropped;
    uint64_t seq;
} yghv_watch_stats_t;

void yghv_watch_stats_get(yghv_watch_stats_t *out);
void yghv_watch_stats_reset(void);

#endif /* YGHV_MEM_WATCH_H */
