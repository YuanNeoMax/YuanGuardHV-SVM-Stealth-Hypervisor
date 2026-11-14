/*
 * yghv_pt.h — 9.593: CR3 decoding and page-table analysis as a service.
 *
 * WHY THIS EXISTS
 *   mem_pte.c already walks CR3 -> PML4 -> PDPT -> PD -> PT and returns a
 *   physical address. What it does NOT do is answer the questions a page
 *   table analyst actually asks:
 *
 *     "what is this process's CR3, and is that the raw CR3 or the DTB?"
 *     "which of these four levels is the one that is missing?"
 *     "how much of this range is mapped, and in what page sizes?"
 *     "where are the holes?"
 *
 *   Every one of those was, until now, a hand-written loop inside one IOCTL
 *   case (IOCTL_YGHV_PTE_PROBE, control_device.c), which means it could not
 *   be reused and could not be tested on its own.
 *
 * WHAT "CR3 DECRYPTION" MEANS HERE
 *   Not encryption: decoding. A raw CR3 on a PCID-capable machine is not a
 *   page-table base — the low 12 bits carry the PCID, bit 63 is the no-flush
 *   hint, and the DTB is only CR3 & ~0xFFF. Passing the raw value to a walk
 *   works by accident (the walk masks it anyway) but printing it as "the
 *   page table base" is wrong by up to 4095, and comparing two processes'
 *   CR3s for equality is wrong whenever their PCIDs differ.
 *
 * IRQL
 *   The walk itself is pure arithmetic plus MmCopyMemory(PHYSICAL) reads, so
 *   it is safe at DISPATCH_LEVEL. This module only calls it from IOCTL
 *   context (PASSIVE), and takes no locks.
 */
#ifndef YGHV_PT_H
#define YGHV_PT_H

#include <ntddk.h>
#include <stdint.h>

/* Raw CR3 layout on a PCID-capable x64 CPU. */
#define YGHV_CR3_DTB_MASK      0x000FFFFFFFFFF000ULL
#define YGHV_CR3_PCID_MASK     0x0000000000000FFFULL
#define YGHV_CR3_NOFLUSH       (1ULL << 63)

/* A range scan is bounded so one IOCTL cannot run for minutes. */
#define YGHV_PT_MAX_SCAN_BYTES (1024ull * 1024ull * 1024ull)   /* 1 GB   */
#define YGHV_PT_MAX_VISITS     65536u                          /* pages  */
#define YGHV_PT_MAX_LIST       64u                             /* output */

typedef struct {
    uint64_t raw;        /* exactly what KPROCESS.DirectoryTableBase held */
    uint64_t dtb;        /* raw & YGHV_CR3_DTB_MASK                        */
    uint32_t pcid;       /* raw & YGHV_CR3_PCID_MASK                       */
    uint32_t noflush;    /* bit 63                                         */
} yghv_pt_cr3_t;

/* Resolve a process's CR3 by pid. Returns 1 on success. */
int yghv_pt_cr3_of(unsigned long pid, yghv_pt_cr3_t *out);

/*
 * One level of the walk, as raw entries plus the physical address each entry
 * was read from. A missing level reports entry == 0 and stops; the caller can
 * tell "absent" (present bit clear) from "unreadable table" (pa == 0) because
 * the two produce different counters.
 */
typedef struct {
    uint64_t pml4e;  uint64_t pml4_pa;
    uint64_t pdpte;  uint64_t pdpt_pa;
    uint64_t pde;    uint64_t pd_pa;
    uint64_t pte;    uint64_t pt_pa;
    uint32_t levels;       /* how many tables were actually read (0..4) */
    uint32_t leaf;         /* 0 = none, 3 = 1G, 2 = 2M, 1 = 4K          */
    uint64_t pa;           /* resolved PA (0 when not mapped)           */
} yghv_pt_tree_t;

int yghv_pt_tree(uint64_t cr3, uint64_t va, yghv_pt_tree_t *out);

/* Aggregate shape of a VA range. */
typedef struct {
    uint64_t start_va, end_va;
    uint64_t pages;         /* mapped pages visited (large pages count 1) */
    uint64_t bytes_mapped;  /* sum of page sizes                          */
    uint64_t pages_4k, pages_2m, pages_1g;
    uint64_t writable, executable, user;   /* page counts, not bytes     */
    uint64_t first_gap_va;  /* first unmapped VA in range, 0 if none      */
    uint64_t largest_run_va;    /* start of the largest contiguous run    */
    uint64_t largest_run_bytes;
    uint32_t truncated;     /* hit YGHV_PT_MAX_VISITS                     */
    uint32_t _pad;
} yghv_pt_map_t;

int yghv_pt_map(uint64_t cr3, uint64_t start_va, uint64_t end_va,
                yghv_pt_map_t *out);

/* List mapped page starts (one entry per page, large pages NOT expanded). */
typedef struct {
    uint64_t va;
    uint64_t pa;
    uint64_t size;
    uint32_t flags;   /* bit0 writable bit1 user bit2 nx bit3 accessed */
    uint32_t _pad;
} yghv_pt_page_t;

/* Returns how many entries were written (<= max). */
uint32_t yghv_pt_pages(uint64_t cr3, uint64_t start_va, uint64_t end_va,
                       yghv_pt_page_t *out, uint32_t max, uint32_t *truncated);

/* IOCTL entry point: op dispatch lives in yghv_pt.c, not in control_device.c. */
NTSTATUS yghv_pt_dispatch(const void *in, void *out, ULONG out_len, ULONG *info);

/* Counters: [0] dispatches [1] walks [2] scans [3] bytes scanned
 *           [4] failures  [5] cr3 lookups [6] truncated scans [7] reserved */
void yghv_pt_stats(uint64_t *out);

#endif /* YGHV_PT_H */
