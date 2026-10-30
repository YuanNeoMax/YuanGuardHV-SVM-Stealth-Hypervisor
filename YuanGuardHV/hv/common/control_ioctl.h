#ifndef YGHV_CONTROL_IOCTL_H
#define YGHV_CONTROL_IOCTL_H

#include <ntddk.h>

#define YGHV_IOCTL_DEVICE_TYPE 0x5947
#define YGHV_METHOD_BUFFERED   0
#define YGHV_FILE_ANY_ACCESS   0

#define YGHV_CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

#define IOCTL_YGHV_SET_TARGET \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x800, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_ADD_PAGE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x801, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_REMOVE_PAGE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x802, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_START_PROTECT \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x803, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_STOP_PROTECT \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x804, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GET_STATE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x805, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GET_TARGET \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x806, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GET_PAGES \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x807, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GET_HOOKS \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x808, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_CLEAR \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x809, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_SET_CONFIG \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x80A, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GET_CONFIG \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x80B, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_INSTALL_HOOK \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x80C, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_REMOVE_HOOK \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x80D, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GET_TARGETS \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x80E, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GET_LASTHIT \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x80F, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_ADD_PAGE_FOR_PID \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x810, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_REMOVE_PAGE_FOR_PID \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x811, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_SET_FAKE_MODE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x812, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)

/* 9.297 (A'): runtime TLB-flush strategy selector for the fake-write path.
   Values: 0 = legacy (TlbControl=1 + INVLPGA + nCR3, current behaviour)
           1 = TlbControl=2 (FLUSH_ALL) instead of 1
           2 = TlbControl=2 + no INVLPGA
           3 = TlbControl=1 + no INVLPGA (isolates INVLPGA's effect)
   Zero-length input reads the current value back (ULONG). */
#define IOCTL_YGHV_SET_FAKE_TLB \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x813, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)

/* 9.299 (C line): SYNC GUARD — snapshot + poll, no NPT permission flip.
   Zero-length input reads back {mode, polls, hits, interval_ms, running, pages}.
   8-byte input: {ULONG enable, ULONG interval_ms}. */
#define IOCTL_YGHV_SET_SYNC \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x814, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)

/* Phase 1 (2026-10-11): page-table walk probe — Engine B foundation.
   Read: 16-byte input {ULONG pid, ULONG pad, ULONG_PTR va} -> 16 qwords out.
   Write: zero-length input -> reset walk stats. */
#define IOCTL_YGHV_PTE_PROBE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x815, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)

/* Phase 3 (2026-10-11): typed cross-process read/write (dual engine). */
#define IOCTL_YGHV_MEM_READ \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x816, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_MEM_WRITE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x817, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
/* Engine selection + diagnostics. */
#define IOCTL_YGHV_MEM_ENGINE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x818, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
/* Phase 4 (2026-10-11): page-table driven signature scan. */
#define IOCTL_YGHV_MEM_SCAN \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x819, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
/* Phase 2b (2026-10-11): self-built physical mapping window.
   Zero-length input = read diagnostics (11 qwords).
   4-byte input {ULONG op}: 1 = init, 0 = stop, 2 = reset stats. */
#define IOCTL_YGHV_SELFMAP \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x81A, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_SELFMAP_OUT_QWORDS 11
/* Phase 7 (2026-10-11): guard cooperation diagnostics.
   Zero-length input = read 5 qwords; 4-byte input {ULONG op}: 2 = reset. */
#define IOCTL_YGHV_MEM_COOP \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x81B, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_COOP_OUT_QWORDS 5
/* Phase 4b (2026-10-11): reverse scan — find VAs that map a physical frame.
   Input:  {ULONG pid, ULONG max_results, ULONG_PTR target_pa,
            ULONG_PTR range_start, ULONG_PTR range_end}
   Output: [0] status, [1] count, [2] pages_visited, [3..] VAs */
#define IOCTL_YGHV_MEM_SCAN_REV \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x81C, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
typedef struct {
    ULONG     pid;
    ULONG     max_results;
    ULONG_PTR target_pa;
    ULONG_PTR range_start;
    ULONG_PTR range_end;
} yghv_ioctl_scan_rev_t;
#define YGHV_SCAN_REV_OUT_HDR_QWORDS 3
/* Phase 6 (2026-10-11): island (direct-map) access.
   Zero-length input = read 8 qwords of diagnostics.
   32-byte input {ULONG64 va_a, pa_a, va_b, pa_b} = init with two probes.
   16-byte input {ULONG64 pa, ULONG64 len} = island read probe. */
#define IOCTL_YGHV_ISLAND \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x81D, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_ISLAND_OUT_QWORDS 8
/* Tier 4.1/4.3 (2026-10-11): split-view + cross-process aliasing.
   Zero-length input = 8 qwords of diagnostics.
   40-byte input {ULONG op, pad, ULONG_PTR a, b, c}:
     op 0 init / 1 stop / 2 split(gpa,real,decoy) / 3 unsplit(gpa)
     op 4 add-observer(cr3) / 5 remove-observer(cr3) / 6 pick(cr3) */
#define IOCTL_YGHV_VIEW \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x81F, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_VIEW_OUT_QWORDS 8
typedef struct {
    ULONG     op;
    ULONG     _pad;
    ULONG_PTR a;
    ULONG_PTR b;
    ULONG_PTR c;
} yghv_ioctl_view_t;
/* Tier 4.6 (2026-10-17): NPF watchpoints (debug-register superset).
   Zero-length input = 6 qwords diagnostics.
   32-byte input {ULONG op, pad, ULONG_PTR a, b, c}:
     op 0 add(gpa,kinds) / 1 remove(gpa) / 2 drain(a=max) / 3 reset */
/* Tier 4.6 (2026-10-17): NPF-based watchpoints.
   32-byte input {ULONG op, pad, ULONG_PTR a, b, c}:
     op 0 add(a=gpa, b=kinds) / 1 remove(a=gpa)
     op 2 drain  -> out[0]=count, out[1]=0,        then 4 qwords per event
     op 3 reset stats
     op 4 snapshot (9.368/A3, Tier 5.6) -> SAME LAYOUT as op 2, but:
           out[0]=copied, out[1]=depth-before-copy, then 4 qwords per event.
           NON-DESTRUCTIVE: does not advance the ring tail, so the same window
           can be read again. Truncation is visible as copied < depth. A caller
           that only ever snapshots and never drains will see `dropped` climb —
           draining is what frees slots. */
#define IOCTL_YGHV_WATCH \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x820, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_WATCH_OUT_HDR_QWORDS 2
/* Tier 4.2 (2026-10-17): time-dimension control (O(1) snapshot/rollback).
   32-byte input {ULONG op, pad, ULONG_PTR a, b, c}:
     op 0 track(gpa) / 1 untrack(gpa) / 2 snapshot(gpa)
     op 3 goto(gpa, version) / 4 current(gpa) */
#define IOCTL_YGHV_TIME \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x821, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_TIME_OUT_QWORDS 6
/* Tier 4.7 (2026-10-17): cache behaviour control (NPT PWT/PCD).
   Zero-length input = 6 qwords diagnostics.
   32-byte input {ULONG op, pad, ULONG_PTR a, b, c}: op 0 set(gpa,mode),
   1 set_alt(gpa,mode), 2 reset(gpa). */
#define IOCTL_YGHV_CACHE \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x822, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
/* Tier 4.4 (2026-10-17): instruction-level access control.
   Zero-length input = 6 qwords diagnostics.
   32-byte input {ULONG op, pad, ULONG_PTR gpa, rip, kinds}: op 0 add,
   1 remove, 2 drain(a = max qwords). */
/* Tier 4.5 (2026-10-17): frame ownership takeover.
   Zero-length input = 8 qwords diagnostics.
   32-byte input {ULONG op, pad, ULONG_PTR a, b, c}:
     op 0 reserve(gpa, len)   op 1 release(slot)
     op 2 read(slot, off)     -> out[0..] = bytes
     op 3 list                -> out[0] = count, then per item 5 qwords */
/* Tier 7.2/7.7 (2026-10-18): failure isolation / graceful degradation.
   Zero-length input = 8 qwords:
     total, degraded, fatal, last_reason, last_a, last_b, last_c, reserved
   A rising `fatal` means the machine needed a bugcheck; `total - fatal` is how
   many abnormal conditions the guest survived. */
/* Tier 7.1/7.3 (2026-10-18): self-hiding + integrity self-check.
   Zero-length input = 8 qwords: guarded, hidden, verified, changed, probes,
                                hide_fail, hash_fail, reserved
   32-byte input {ULONG op, pad, a, b, c}:
     op 0 add(a=gpa, b=hide, c=hash, tag in in[4..7])
     op 1 remove(a=gpa)
     op 2 verify all      -> out[0] = changed count
     op 3 list            -> out[0] = count, then 6 qwords per entry
     op 4 arm core        -> out[0] = pages guarded */
/* 9.338 (2026-10-18): direct physical-frame access.
   Tier 1/2 have always had this internally (yghv_pa_read / yghv_pa_write) but
   it was never exposed, so a test wanting to touch a frame had to misuse
   mem-write by handing it a PHYSICAL address as if it were a VA. That mistake
   is what crashed the machine twice:
     0x3B  Engine A faults because the "VA" is not mapped (13th)
     0x1A  the memory manager tries to make that bogus VA resident (14th)
   A dedicated entry point removes the need to abuse another one.

   32-byte input {ULONG op, pad, ULONG_PTR a(pa), b(value|len), c(off)}:
     op 0 = read  (a=pa, b=len, c=off)  -> out[0]=got, then the bytes
     op 1 = write (a=pa, b=u64 value)   -> out[0]=ok
     op 2 = write bytes from in[4..]   -> in[1]=pa, in[2]=len */
/* Tier 7.4/7.5/7.6 (2026-10-18): exit-latency self-measurement, escape attempts,
   and the current attack surface. 32-byte input {ULONG op, pad, a, b}:
     op 0 stats   -> 14 qwords: escapes[6], total, last_kind, last_detail,
                                 last_a, last_b, exits, worst_cycles, total_cycles
     op 1 surface -> out[0]=count, then 3 qwords per entry (name[2], is_ioctl)
     op 2 reset   */
#define IOCTL_YGHV_SENTRY \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x828, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_SENTRY_OUT_QWORDS 14
/* 9.371 (Tier 9.2): authenticated control commands.
   32-byte input {ULONG op, pad, ULONG_PTR a, b} plus optional bytes after it:
     op 0 stats  -> 10 qwords: mode, key_len, accepted, rejected, would_reject,
                                bad_seq, no_tag, last_seq, selftest, reserved
     op 1 set key <- in[1]=key_len, key bytes at in[4..]
     op 2 set mode <- in[1]=0 off / 1 monitor / 2 enforce
     op 3 reset stats
     op 4 sign   <- in[1]=seq, in[2]=args_len, args at in[4..]
                 -> out[0]=1, tag (32 bytes) at out[1..4]
        The DRIVER signs on request so a client never re-derives the signed
        layout. A client that signs a different layout than the driver verifies is
        indistinguishable from a wrong key - the worst possible error message - so
        the layout lives in exactly one place. */
#define IOCTL_YGHV_AUTH \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x829, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_AUTH_OUT_QWORDS 10
#define IOCTL_YGHV_PA \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x827, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define IOCTL_YGHV_GUARD \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x826, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_GUARD_OUT_QWORDS 8
#define IOCTL_YGHV_FAIL \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x825, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_FAIL_OUT_QWORDS 8
#define IOCTL_YGHV_FRAME \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x824, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_FRAME_OUT_QWORDS 8
#define IOCTL_YGHV_BP \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x823, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
/* Tier 4 prerequisite (2026-10-11): NPT remap experiment.
   Rewrites one GPA's NPT entry to point at a different SPA, then reports
   whether the guest actually observes the change.
   Input:  {ULONG gpa, ULONG spa, ULONG mode}   (mode 0 = main NPT, 1 = alt)
   Output: [0] status 1=ok, [1] previous spa, [2] new spa */
#define IOCTL_YGHV_NPT_REMAP \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x81E, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)

/*
 * 9.448: exit-coverage control and measurement.
 *
 * The 206 handler covers 35 exit codes; which of them are ARMED decides which can
 * ever fire. This IOCTL is the only way to change that set, and the only way to
 * read back which fired.
 *
 * Zero-length input -> read 4 qwords:
 *     [0] armed Misc1 mask
 *     [1] armed Misc2 mask
 *     [2] distinct exit codes observed so far (of 0x60..0x8D)
 *     [3] total exits counted (g_S206Exits)
 *
 * 16-byte input {ULONG op, pad, ULONG m1, ULONG m2}:
 *     op 0 = read back (same as zero-length)
 *     op 1 = set the extra Misc1/Misc2 intercept bits (OR'd into every VMCB)
 *     op 2 = clear the extra bits (back to the pre-9.448 set)
 *     op 3 = arm "quiet"  (HLT + PAUSE only: the two that prove idle exits work)
 *     op 4 = arm "full"   (every Misc1/Misc2 bit this tree defines)
 *
 * WHY NOT ARMED BY DEFAULT: HLT fires on every idle transition and PAUSE on every
 * spinlock contention path, so arming them turns idle time into exits. This tree
 * is already ~12x slower than the pre-9.445 build (12 cores sit in guest VMRUN
 * loops), so the default build keeps exactly the set it had.
 */
#define IOCTL_YGHV_EXITMASK \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x82A, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_EXITMASK_OUT_QWORDS 5

/*
 * 9.449 (Tier 1.5): Engine A vs Engine B, head to head, on the same range.
 *
 * The plan claimed B is faster on large ranges and on large pages. This is the
 * command that TIMES it instead of asserting it. See mem_bench.c for how the
 * comparison is kept fair: same bytes, same pattern, same budget, and A runs
 * second so B gets the colder cache.
 *
 * Input is the same shape as MEM_SCAN so a caller can reuse its request, plus a
 * per-engine page budget:
 *   {ULONG pid, ULONG pat_len, ULONG page_budget, ULONG flags,
 *    ULONG_PTR range_start, ULONG_PTR range_end, UCHAR pattern[64], UCHAR mask[64]}
 *
 * Output (10 QWORDs), field for field yghv_bench_result_t:
 *   [0] a_cycles  [1] a_reads  [2] a_bytes  [3] a_hits
 *   [4] b_cycles  [5] b_reads  [6] b_bytes  [7] b_hits
 *   [8] pages_visited  [9] speedup_x100  [10] walk_cr3
 *
 * a_hits and b_hits are SEPARATE on purpose. A functional disagreement between
 * the engines is a finding worth more than the timing, so it must be visible
 * rather than averaged into the performance number.
 *
 * speedup_x100 = a_cycles * 100 / b_cycles, integer hundredths so no float
 * crosses the IOCTL boundary. Above 100 means B was faster WITH the warmer cache
 * going to A.
 */
#define IOCTL_YGHV_MEM_BENCH \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x82B, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_BENCH_OUT_QWORDS 11

/*
 * 9.450 (Tier 7.1): ASK THE NPT WHAT A GUEST WOULD SEE AT ONE PHYSICAL PAGE.
 *
 * Tier 7.1's claim is "no guest scan can find our physical pages". Until this
 * command there was no way to READ that claim: npt_exclude_self existed, but
 * nothing could answer "is page X actually excluded right now". Re-hiding without
 * being able to check is how a page ends up in the guard table's DECOY mode while
 * the NPT entry still says present - a state that looks hidden from every
 * counter and is visible to the guest.
 *
 * Input:  {ULONG_PTR pa}   (the NPT is an identity map, so PA == GPA)
 * Output (6 QWORDs):
 *   [0] entry present    1 = the guest can reach this frame
 *   [1] permission       bit0 read, bit1 write, bit2 execute (0 = excluded)
 *   [2] frame mapped     should equal pa under identity mapping
 *   [3] large-page       1 = still a 2MB entry, so no split happened
 *   [4] host-visible     1 = WE can still reach it (our mapping intact)
 *   [5] state code       0 ok / 0x11 not identity mapped / 0x12 unknown
 *
 * The PAIR [0] and [4] is the point: guest-invisible AND host-visible is hidden
 * correctly. Guest-invisible and host-invisible means we locked ourselves out
 * (the 9.343 failure). Guest-VISIBLE means not hidden at all.
 */
#define IOCTL_YGHV_NPT_VIEW \
    YGHV_CTL_CODE(YGHV_IOCTL_DEVICE_TYPE, 0x82C, YGHV_METHOD_BUFFERED, YGHV_FILE_ANY_ACCESS)
#define YGHV_NPT_VIEW_OUT_QWORDS 6
typedef struct {
    ULONG     pid;
    ULONG     pat_len;
    ULONG     page_budget;
    ULONG     flags;
    ULONG_PTR range_start;
    ULONG_PTR range_end;
    UCHAR     pattern[64];
    UCHAR     mask[64];
} yghv_ioctl_bench_req_t;
typedef struct {
    ULONG_PTR gpa;
    ULONG_PTR spa;
    ULONG     mode;
    ULONG     _pad;
} yghv_ioctl_npt_remap_t;

#define YGHV_DEVICE_NAME_STRING     L"\\Device\\YuanGuardHV"
#define YGHV_DOS_DEVICE_NAME_STRING L"\\DosDevices\\YuanGuardHV"

typedef struct {
    ULONG pid;
} yghv_ioctl_set_target_t;

typedef struct {
    ULONG_PTR target_va;
} yghv_ioctl_va_t;

/* 206-C4: cross-process add/remove page — resolve the target slot by pid
   (the caller is the external controller, not the target process). */
typedef struct {
    ULONG pid;
    ULONG_PTR target_va;
} yghv_ioctl_va_pid_t;

/* 9.275 C6: fake-write shadow mode (0 = off, 1 = on). */
typedef struct {
    ULONG enable;
} yghv_ioctl_fake_mode_t;

typedef struct {
    ULONG active;
    ULONG pid;
    ULONG page_count;
} yghv_ioctl_state_t;

/*
 * Phase 1 (2026-10-11): page-table walk probe.
 *
 * Read:  {ULONG pid, ULONG_PTR va} -> resolves VA through the target's CR3
 *        by hand (CR3 -> PML4 -> PDPT -> PD -> PT) and reports what the walk
 *        found. No memory is read from the target — this only exercises the
 *        translation, which is the foundation of Engine B.
 *
 * Output layout (all UINT64 so it aligns):
 *   [0] status      1 = resolved, 0 = not present / bad cr3
 *   [1] cr3         the target's CR3 as used
 *   [2] va          echoed
 *   [3] pa          physical address (with page offset) or 0
 *   [4] page_base   page-aligned physical base
 *   [5] page_size   0x1000 / 0x200000 / 0x40000000
 *   [6] kind        1 = 4K, 2 = 2M, 3 = 1G
 *   [7] levels      2..4 (how deep the walk went)
 *   [8] attrs       bit0 present, 1 writable, 2 user, 3 accessed,
 *                   4 dirty, 5 nx, 6 global
 *   [9] stats.walks
 *  [10] stats.hit_4k
 *  [11] stats.hit_2m
 *  [12] stats.hit_1g
 *  [13] stats.not_present
 *  [14] stats.bad_cr3
 *  [15] stats.table_reads
 *
 * Write: {} (zero-length input) -> resets the walk statistics.
 */
typedef struct {
    ULONG     pid;
    ULONG     _pad;
    ULONG_PTR va;
} yghv_ioctl_pte_probe_t;

#define YGHV_PTE_PROBE_OUT_QWORDS 16

/*
 * Phase 3 (2026-10-11): typed cross-process read/write + engine selection.
 *
 * MEM_READ / MEM_WRITE share this request header. `count` is the number of
 * elements for fixed-size types; for STR_* and BYTES it is the byte count.
 * `engine` is 0 auto / 1 api / 2 pte.
 *
 * METHOD_BUFFERED shares one SystemBuffer between input and output, so the
 * reply overwrites the request as it is written — the driver must copy
 * everything it still needs out of the buffer before the first store.
 *
 * Read reply layout (QWORDs):
 *   [0] status      1 = ok
 *   [1] engine used 1 = api, 2 = pte
 *   [2] bytes transferred
 *   [3] type echoed
 *   [4] count echoed
 *   [5] data offset (bytes, from start of the buffer)
 *   ... payload bytes follow at that offset
 *
 * Write reply: same header, no payload.
 */
typedef struct {
    ULONG     pid;
    ULONG     type;
    ULONG_PTR va;
    ULONG     count;
    ULONG     engine;
    ULONG     flags;
    ULONG     _pad[2];
} yghv_ioctl_mem_req_t;

#define YGHV_MEM_HDR_QWORDS   8
#define YGHV_MEM_HDR_BYTES    (YGHV_MEM_HDR_QWORDS * 8)
#define YGHV_MEM_MAX_PAYLOAD  4096

/*
 * Engine control / diagnostics.
 *   Read: 16 qwords out.
 *     [0] current selection (0 auto, 1 api, 2 pte)
 *     [1] api_reads     [2] api_writes   [3] api_fail
 *     [4] pte_reads     [5] pte_writes   [6] pte_fail
 *     [7] fallbacks
 *     [8] auto_choices_api  [9] auto_choices_pte
 *     [10] pa reads     [11] pa reads_failed
 *     [12] pa writes    [13] pa writes_failed
 *     [14] pa bytes_read [15] pa map_failures
 *   Write: 4 bytes {ULONG sel} -> set engine; zero-length -> reset stats.
 */
/* 16 base + 3 Engine B write-failure counters (see control_device.c) */
#define YGHV_MEM_ENGINE_OUT_QWORDS 19

/*
 * Phase 4: signature scan.
 *
 * Input:
 *   {ULONG pid, ULONG pat_len, ULONG max_results, ULONG flags,
 *    ULONG_PTR range_start, ULONG_PTR range_end, ULONG64 align,
 *    UCHAR pattern[64], UCHAR mask[64]}
 *   flags: bit0 = writable only, bit1 = executable only
 *
 * Output (QWORDs):
 *   [0] status 1 = ok
 *   [1] hit count
 *   [2] pages read
 *   [3] pages skipped
 *   [4] bytes scanned
 *   [5..] hit VAs
 */
typedef struct {
    ULONG     pid;
    ULONG     pat_len;
    ULONG     max_results;
    ULONG     flags;
    ULONG_PTR range_start;
    ULONG_PTR range_end;
    ULONG64   align;
    UCHAR     pattern[64];
    UCHAR     mask[64];
} yghv_ioctl_scan_req_t;

#define YGHV_SCAN_OUT_HDR_QWORDS 5

#endif
