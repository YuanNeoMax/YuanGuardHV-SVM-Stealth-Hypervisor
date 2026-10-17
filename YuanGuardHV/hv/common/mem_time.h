/*
 * mem_time.h — Tier 4.2: time-dimension control (O(1) snapshot / rollback).
 *
 * Keep several physical copies of one guest page and switch between them by
 * rewriting one NPT entry. Rollback costs a single store, not a 4KB copy, so
 * a page can be reverted instantly and repeatedly.
 *
 * Why the host cannot do this: in the host, one VA has exactly one content.
 * Making "version 0" and "version 1" both exist for the same address at the
 * same time is not expressible — only the translation layer can do it.
 *
 * How the copies are held
 * -----------------------
 * Each version is a page-sized buffer from nonpaged pool. Its physical
 * address is what the NPT entry points at. Nonpaged pool is guaranteed
 * resident, so the frame cannot be paged out or moved underneath us — the
 * same property npt_alloc_table relies on.
 *
 * Honest limits
 * -------------
 *   - Cost is one 4KB buffer per retained version. The cap is deliberate.
 *   - A rollback changes what the guest sees, but the CPU may still hold the
 *     previous translation. 9.311 measured that a remap IS observed (unlike a
 *     permission flip), so this works; a stale window of one poll interval is
 *     still possible if the guest runs on another core at that instant.
 *   - The snapshots are plain copies taken when asked; this is not a
 *     copy-on-write history. Nothing is captured automatically.
 */
#ifndef YGHV_MEM_TIME_H
#define YGHV_MEM_TIME_H

#include "svm_defs.h"
#include <ntddk.h>

#define YGHV_TIME_MAX_PAGES     8
#define YGHV_TIME_MAX_VERSIONS  4

typedef struct {
    uint64_t spa;        /* physical address of this version's buffer */
    uint64_t taken_seq;  /* ordering stamp */
} yghv_time_version_t;

typedef struct {
    uint64_t gpa;                                   /* the guest page */
    yghv_time_version_t ver[YGHV_TIME_MAX_VERSIONS];
    uint32_t count;
    uint32_t current;    /* which version the NPT currently points at */
    volatile LONG active;
} yghv_time_page_t;

/*
 * Start tracking a page. `gpa` must already be mapped; its current frame is
 * captured as version 0. PASSIVE. Returns 1 on success.
 */
int yghv_time_track(uint64_t gpa);

/* Stop tracking and release every buffer. Restores the original frame first. */
int yghv_time_untrack(uint64_t gpa);

/*
 * Capture the page's CURRENT content as a new version and make it current.
 * This is "snapshot now". PASSIVE. Returns the version index, or -1.
 */
int yghv_time_snapshot(uint64_t gpa);

/*
 * Point the page at an existing version. O(1) — one NPT store, no copy.
 * PASSIVE (calls npt remap). Returns 1 on success.
 */
int yghv_time_goto(uint64_t gpa, uint32_t version);

/* Which version is live? Returns -1 if the page is not tracked. */
int yghv_time_current(uint64_t gpa);

/*
 * Write new content into a version buffer WITHOUT making it live.
 * Lets the caller stage a version and switch to it later.
 * PASSIVE. Returns 1 on success.
 */
int yghv_time_stage(uint64_t gpa, uint32_t version, const void *src, SIZE_T len);

typedef struct {
    uint64_t pages;
    uint64_t snapshots;
    uint64_t switches;
    uint64_t failures;
} yghv_time_stats_t;

void yghv_time_stats_get(yghv_time_stats_t *out);
void yghv_time_stats_reset(void);

#endif /* YGHV_MEM_TIME_H */
