/*
 * mem_pte.h — Phase 1: page-table walk library (Engine B foundation).
 *
 * Walks CR3 -> PML4 -> PDPT -> PD -> PT by hand, reading the table pages
 * through MmGetVirtualForPhysical. That API is correct HERE and only here:
 * page tables are SYSTEM pages, which the system already direct-maps, so
 * the lookup succeeds. (9.299 FIX-2: the same call on a *user* page returns
 * a bogus pointer and faulted the machine — see docs/KERNEL_API_CHECKLIST.md
 * section 3. Never use it for user memory; that is what mem_pa.c is for.)
 *
 * The walk itself is pure arithmetic + reads: no locks, no allocation, no
 * blocking, so it is safe at DISPATCH_LEVEL (the island context).
 *
 * 2026-10-11 Phase 1. Logic extracted from yghv_protect_guest_va_to_pa
 * (protect.c) and extended with large-page handling stats and attribute
 * queries.
 */
#ifndef YGHV_MEM_PTE_H
#define YGHV_MEM_PTE_H

/* svm_defs.h supplies the fixed-width integer types for this cross-compiler
 * build (it defines uint8_t..uint64_t when _STDINT is absent), plus the
 * _Static_assert shim. ntddk.h alone does not provide them under clang-cl. */
#include "svm_defs.h"
#include <ntddk.h>

/* --- x86-64 4-level paging constants ------------------------------------- */
#define YGHV_PTE_INDEX_PML4(va) (((va) >> 39) & 0x1FFULL)
#define YGHV_PTE_INDEX_PDPT(va) (((va) >> 30) & 0x1FFULL)
#define YGHV_PTE_INDEX_PD(va)   (((va) >> 21) & 0x1FFULL)
#define YGHV_PTE_INDEX_PT(va)   (((va) >> 12) & 0x1FFULL)

/* Physical address field of a table entry. Bits 12..51 (51-bit PA). */
#define YGHV_PTE_ADDR_MASK      0x000FFFFFFFFFF000ULL
/* 1GB large-page address field: bits 30..51 */
#define YGHV_PTE_ADDR_MASK_1G   0x000FFFFFC0000000ULL
/* 2MB large-page address field: bits 21..51 */
#define YGHV_PTE_ADDR_MASK_2M   0x000FFFFFFFE00000ULL

/* Entry flag bits (identical layout at every level for the low bits) */
#define YGHV_PTE_PRESENT        (1ULL << 0)
#define YGHV_PTE_WRITABLE       (1ULL << 1)
#define YGHV_PTE_USER           (1ULL << 2)
#define YGHV_PTE_WRITETHROUGH   (1ULL << 3)
#define YGHV_PTE_CACHEDISABLE   (1ULL << 4)
#define YGHV_PTE_ACCESSED       (1ULL << 5)
#define YGHV_PTE_DIRTY          (1ULL << 6)
#define YGHV_PTE_LARGE          (1ULL << 7)   /* PS bit (PDPT/PD only) */
#define YGHV_PTE_GLOBAL         (1ULL << 8)
#define YGHV_PTE_NX             (1ULL << 63)

/* Page sizes we can resolve */
#define YGHV_PTE_SIZE_4K        0x1000ULL
#define YGHV_PTE_SIZE_2M        0x200000ULL
#define YGHV_PTE_SIZE_1G        0x40000000ULL

typedef enum {
    YGHV_PTE_KIND_INVALID = 0,
    YGHV_PTE_KIND_4K      = 1,
    YGHV_PTE_KIND_2M      = 2,
    YGHV_PTE_KIND_1G      = 3,
} yghv_pte_kind_t;

/* Result of a full walk */
typedef struct {
    uint64_t         pa;          /* physical address (with page offset) */
    uint64_t         page_base;   /* pa with page offset cleared */
    uint64_t         page_size;   /* 4K / 2M / 1G */
    yghv_pte_kind_t  kind;
    uint64_t         entry;       /* the leaf entry that resolved it */
    uint32_t         levels;      /* how many tables we read (2/3/4) */
    /* Attributes assembled from the leaf entry */
    uint8_t          present;
    uint8_t          writable;
    uint8_t          user;
    uint8_t          accessed;
    uint8_t          dirty;
    uint8_t          nx;
    uint8_t          global;
} yghv_pte_info_t;

/* Walk statistics — for diagnostics and for proving large-page awareness */
typedef struct {
    uint64_t walks;
    uint64_t hit_4k;
    uint64_t hit_2m;
    uint64_t hit_1g;
    uint64_t not_present;
    uint64_t bad_cr3;
    uint64_t table_reads;   /* how many table entries we dereferenced */
} yghv_pte_stats_t;

/*
 * Read one 8-byte entry out of a page table.
 * Returns 0 if the table address is not system-mapped (treated as absent).
 * Safe at DISPATCH_LEVEL.
 */
uint64_t yghv_pte_read_entry(uint64_t table_pa, uint64_t index);

/*
 * Full walk. Fills *out (may be NULL) and returns the physical address
 * including the page offset, or 0 if any level is not present.
 * Safe at DISPATCH_LEVEL; no locks, no allocation.
 */
uint64_t yghv_pte_walk(uint64_t cr3, uint64_t va, yghv_pte_info_t *out);

/* Convenience: just the PA, no info struct. */
uint64_t yghv_pte_va_to_pa(uint64_t cr3, uint64_t va);

/* Query attributes only (returns 1 if mapped). */
int yghv_pte_query(uint64_t cr3, uint64_t va, yghv_pte_info_t *out);

/* Stats accessors (lock-free counters, safe from any context). */
void yghv_pte_stats_get(yghv_pte_stats_t *out);
void yghv_pte_stats_reset(void);

/*
 * Enumerate the present pages of an address range.
 *
 * Calls `cb` for each mapped page, passing the VA, its info, and the opaque
 * cookie. Large pages are reported ONCE with their real size (not expanded),
 * which is what makes scanning cheap: a 2MB page covers 512 candidates.
 *
 * Returns the number of pages visited. Stops early if cb returns non-zero.
 * This is the primitive the feature scan is built on.
 */
typedef int (*yghv_pte_enum_cb_t)(uint64_t va, const yghv_pte_info_t *info,
                                  void *cookie);

uint64_t yghv_pte_enum_range(uint64_t cr3, uint64_t start_va, uint64_t end_va,
                             yghv_pte_enum_cb_t cb, void *cookie);

/* User-space bounds on x64 Windows (canonical low half). */
#define YGHV_USER_VA_MIN   0x0000000000010000ULL
#define YGHV_USER_VA_MAX   0x00007FFFFFFEFFFFULL

#endif /* YGHV_MEM_PTE_H */
