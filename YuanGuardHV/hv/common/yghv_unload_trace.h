/*
 * yghv_unload_trace.h - 9.390: a progress record that survives a fatal unload.
 *
 * THE PROBLEM THIS SOLVES
 *
 * Nineteen machine restarts. The last five all die on the unload path with the
 * same signature (0xD1 at IRQL 0xE), and the crash point is now known:
 * Sv206LaunchVm+0xD reads VpData->HostStackLayout while the VpData is already
 * freed. The remaining question is not "where" but "how far did teardown get
 * before the free happened" - and nothing on the machine can answer it.
 *
 * Two things were tried and both failed for the same reason:
 *
 *   t_stoponly.ps1 wrote stoponly.txt as its FIRST statement, then called
 *   sc stop; the file did not exist afterwards.
 *   t_stop2.ps1 opened a StreamWriter with AutoFlush=true, wrote, flushed,
 *   then stopped; that file did not exist either.
 *
 * A user-mode file write cannot survive: the unload kills the machine inside the
 * first few milliseconds. So the only observability that can work is a record
 * written by the DRIVER into memory that is (a) not freed by the unload and
 * (b) readable from a crash dump.
 *
 * WHAT THIS IS
 *
 * A dedicated page, statically allocated, never freed, never hidden, never added
 * to the guard state list. Each teardown step stamps a number and a timestamp
 * into it. When the machine dies, the dump carries that page's contents because
 * it is mapped and non-paged.
 *
 * WHY IT IS DELIBERATELY NOT PART OF g_protect
 *
 * Adding a field to yghv_protect_state_t would change its size, and its size is
 * asserted to be exactly two pages (9.368). More importantly, g_protect's pages
 * are the ones the guard HIDES - a hidden page is precisely what must not be
 * relied on for post-mortem reading. A separate page keeps the two concerns
 * separate: state that is protected, and state that is meant to be READ after a
 * crash.
 *
 * WHY THE STAMPS ARE MONOTONIC, NOT BIT FLAGS
 *
 * A bitmask says "did step N run". A monotonic stamp says "step N ran, and
 * nothing after it" - which is the actual question when the machine dies between
 * two steps. The last value is the answer.
 */

#ifndef YGHV_UNLOAD_TRACE_H
#define YGHV_UNLOAD_TRACE_H

#include "svm_defs.h"
/* LONG/LONG64 and InterlockedXxx come from here. This header is included by
 * yghv_unload_trace.c (a plain C translation unit) and by main.c, neither of
 * which pulls <ntifs.h> first, so there is no PEPROCESS collision here. */
#include <ntddk.h>

/*
 * Teardown steps, in the order DriverUnload performs them. The numbers are part
 * of the contract: a dump that ends at 40 means the stop request was made but the
 * core joins never completed. Keep them stable.
 */
#define YGHV_UT_START               1
#define YGHV_UT_NOTIFY_OFF         10
#define YGHV_UT_COOP_UNLOAD        20
#define YGHV_UT_STOP_REQUESTED     30
#define YGHV_UT_JOIN_GUEST         40
#define YGHV_UT_JOIN_203           50
#define YGHV_UT_STOP_RESIDENTS     60
#define YGHV_UT_WAIT_RESIDENTS     70
#define YGHV_UT_CONTROL_CLEANUP    80
#define YGHV_UT_PROTECT_CLEANUP    90
#define YGHV_UT_NPT_CLEANUP       100
#define YGHV_UT_CORE_CLEANUP      110
#define YGHV_UT_DONE              120

/*
 * Stamp `step`. Called from DriverUnload, so it is PASSIVE and may do anything -
 * but it is written to be lock-free and branch-light, because the interesting
 * case is when the next instruction faults.
 */
/* 9.392: allocate the page that outlives this module. Call ONCE at load, PASSIVE.
 * Returns 1 on success; the page is deliberately NEVER freed. */
int yghv_ut_init(void);

/*
 * 9.404: capture the pre-teardown truth, ONCE, before anything clears it.
 * See the note on the snapshot fields in yghv_unload_trace.c: reading these
 * AFTER the unload is meaningless because the unload writes over them.
 */
void yghv_ut_snapshot_pre_unload(UINT64 resident_threads, UINT64 power_cb_present,
                                 UINT64 vcpu_count, UINT64 s203_online,
                                 UINT64 vmrun_stopped, UINT64 cpuid_view);

void yghv_ut_mark(uint32_t step);

/*
 * Stamp `step` AND a value, for the steps where a number matters (how many
 * cores joined, how many vcpus were freed). Two separate fields so a dump reader
 * does not have to interpret one as the other.
 */
void yghv_ut_mark_val(uint32_t step, uint64_t value);

/* How many times the trace page was written, and the last step recorded. */
uint32_t yghv_ut_last_step(void);
uint64_t yghv_ut_last_value(void);

/*
 * The page's own physical address, so a dump reader can find it without symbols.
 * Printed at load; also exposed through the control device so a live machine can
 * confirm the page is where the dump will look for it.
 */
uint64_t yghv_ut_page_pa(void);

#endif /* YGHV_UNLOAD_TRACE_H */
