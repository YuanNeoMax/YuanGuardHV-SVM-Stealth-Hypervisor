/*
 * mem_frame.h — Tier 4.5: physical-frame ownership takeover.
 *
 * What "ownership" means here
 * ---------------------------
 * A frame that belongs to someone else becomes readable by us AFTER its owner
 * is gone. The moment we reserve it we copy its content into a buffer only we
 * hold, in nonpaged pool, and never release it until asked. From then on the
 * content survives:
 *
 *   - the owner process exiting (the frame goes back to the PFN free list)
 *   - the frame being handed to a different process
 *   - the original physical page being reused for something else
 *
 * That is the property the host kernel cannot give you: once a frame is freed,
 * Windows may recycle it at any moment, so reading it "later" is a race you
 * lose. Holding our own copy makes "later" unbounded.
 *
 * What this does NOT do
 * ---------------------
 * It does not intercept the free path, and it does not present forged content
 * to whoever gets the frame next. Both would need a hook into the memory
 * manager's free/reallocation path that we do not have (and that 9.307 showed
 * is dangerous territory once PFN bookkeeping is involved). So the honest
 * claim is: content retained across release, not control of the reuse.
 *
 * If you need the "new owner sees something else" half, that is Tier 4.1's
 * split view: register the new owner as an observer and serve it a decoy.
 * Those two compose, but the composition is the caller's job.
 */
#ifndef YGHV_MEM_FRAME_H
#define YGHV_MEM_FRAME_H

#include "svm_defs.h"
#include <ntddk.h>

#define YGHV_FRAME_MAX      32
#define YGHV_FRAME_MAX_LEN  0x10000   /* 64 KB per reservation */

typedef struct {
    uint64_t gpa;       /* source physical address */
    uint64_t seq;       /* reservation order */
    uint32_t len;       /* bytes retained */
    uint32_t alive;     /* 1 while the source frame is still mapped to its owner */
    volatile LONG active;
} yghv_frame_info_t;

/*
 * Copy `len` bytes at physical address `gpa` into a private nonpaged buffer
 * that outlives the source. PASSIVE. Returns the slot index, or -1.
 */
int yghv_frame_reserve(uint64_t gpa, uint32_t len);

/* Release a reservation (frees the buffer). */
int yghv_frame_release(int slot);

/*
 * Read back from a retained buffer — works whether or not the source frame
 * still exists, which is the whole point.
 * PASSIVE. Returns bytes copied.
 */
uint32_t yghv_frame_read(int slot, void *out, uint32_t len, uint32_t off);

/* Enumerate: returns the number filled into `out`. */
uint32_t yghv_frame_list(yghv_frame_info_t *out, uint32_t max);

/* Release everything (used on unload so we never leak pool). */
void yghv_frame_release_all(void);

typedef struct {
    uint64_t reserved;    /* successful reservations */
    uint64_t released;
    uint64_t bytes;       /* total bytes retained right now */
    uint64_t active;      /* current reservations */
    uint64_t failures;
} yghv_frame_stats_t;

void yghv_frame_stats_get(yghv_frame_stats_t *out);
void yghv_frame_stats_reset(void);

#endif /* YGHV_MEM_FRAME_H */
