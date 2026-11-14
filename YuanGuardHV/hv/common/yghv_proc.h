/*
 * yghv_proc.h -- process and thread control on top of the derived offsets (9.591).
 *
 * WHY THIS FILE EXISTS AS A SEPARATE MODULE
 *   yghv_offsets.c answers "where is the field". mem_engine.c answers "move
 *   these bytes". Neither of them is allowed to decide POLICY -- what may be
 *   suspended, whose protection byte may be rewritten, which process may be
 *   unlinked from the list. Putting the policy here, behind one entry point,
 *   means the rules are in one place that can be read in a sitting, and the
 *   IOCTL dispatcher stays a switch that validates its buffer and calls in.
 *
 * WHAT IS DERIVED VS WHAT IS ASSUMED HERE
 *   Every EPROCESS field this file touches comes from g_yghv_off through the
 *   accessors in yghv_nt.h. A field that was not derived reads as 0 and every
 *   accessor returns NULL, so a build where the probe failed disables the
 *   feature instead of writing to the object header.
 *
 *   The ONE thing that is not in the offset table is DirectoryTableBase, at
 *   KPROCESS+0x028. It is used the same way existing call sites use it
 *   (control_device.c:500, :551, :569, :922 and protect.c), so it is a
 *   convention already established in this tree rather than a new guess.
 *
 * IRQL: PASSIVE_LEVEL. Every entry point here does handle-based work
 *   (ZwOpenProcess, ZwQueryInformationThread, pool allocation) and several of
 *   them wait. The IOCTL dispatcher already runs at PASSIVE.
 */
#ifndef YGHV_PROC_H
#define YGHV_PROC_H

#include <ntddk.h>
#include <stdint.h>
#include "yghv_nt.h"

/*
 * One dispatch entry point. `in` is the raw METHOD_BUFFERED request (the
 * caller has already checked in_len), `out` the same buffer, and *info the
 * byte count to report back. Returns the NTSTATUS for the IRP.
 */
NTSTATUS yghv_proc_dispatch(const void *in, ULONG in_len,
                            void *out, ULONG out_len, ULONG *info);

/*
 * Find the two field pairs that no export can tell us: ETHREAD.ThreadListEntry
 * with EPROCESS.ThreadListHead, and EPROCESS.Token. Runs AFTER
 * yghv_offsets_init(), because the probes read through the accessors that one
 * fills in. Returns the number of probes that succeeded; 0 means the features
 * built on them report "not implemented" instead of guessing.
 *
 * It lives here rather than in yghv_offsets.c to keep the dependency one-way:
 * offsets knows nothing about proc.
 */
int yghv_proc_init(void);

/* Release every APC and event this module still owns. Called at unload so a
 * frozen thread is never left waiting on an event inside a dead image. */
void yghv_proc_cleanup(void);

/* Diagnostics: [0] ops [1] fails [2] freezes [3] thaws [4] injects
 *               [5] hides  [6] steals [7] terminates */
void yghv_proc_stats(uint64_t *out);

#endif /* YGHV_PROC_H */
