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
