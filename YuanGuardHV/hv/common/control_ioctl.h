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

#endif
