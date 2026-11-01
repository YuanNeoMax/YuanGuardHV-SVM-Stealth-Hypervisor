#ifndef YGHV_NPT_H
#define YGHV_NPT_H
#include "svm_defs.h"
typedef union { uint64_t all; struct { uint64_t present:1; uint64_t writable:1; uint64_t user:1; uint64_t pwt:1; uint64_t pcd:1; uint64_t accessed:1; uint64_t dirty:1; uint64_t large_page:1; uint64_t global:1; uint64_t _avail_9_11:3; uint64_t pfn:40; uint64_t _rsvd_52_58:7; uint64_t _avail_59_61:3; uint64_t _ignored:1; uint64_t nx:1; }; } npt_entry_t;
_Static_assert(sizeof(npt_entry_t) == 8, "NPT size");
#define NPT_ENTRIES_PER_TABLE 512
#define NPT_PML4_INDEX(a) (((a)>>39)&0x1FF)
#define NPT_PDPT_INDEX(a) (((a)>>30)&0x1FF)
#define NPT_PD_INDEX(a) (((a)>>21)&0x1FF)
#define NPT_PT_INDEX(a) (((a)>>12)&0x1FF)
#define NPT_PAGE_OFFSET(a) ((a)&0xFFF)
/*
 * 9.489: how much address space ONE PML4 entry covers (512 GB).
 *
 * `npt_identity_map_range` refuses an end address above this, because the manager's
 * teardown can only free PD pages recorded for p4==0 - see the comment at that
 * check. A named constant rather than an inline 1ULL<<39 so the check and the index
 * macro cannot drift apart.
 */
#define NPT_PML4_ENTRY_SPAN (1ULL << 39)
#define NPT_LARGE_PAGE_FLAGS ((1ULL<<0)|(1ULL<<1)|(1ULL<<2)|(1ULL<<5)|(1ULL<<7))
#define NPT_4K_PAGE_FLAGS ((1ULL<<0)|(1ULL<<1)|(1ULL<<2)|(1ULL<<5))
#define NPT_PFN_4K(e)  (((e) >> 12) & 0xFFFFFFFFFULL)
#define NPT_PFN_2MB(e) (((e) >> 21) & 0xFFFFFFFFFULL)
#define NPT_PERM_PRESENT  (1ULL<<0)
#define NPT_PERM_WRITABLE (1ULL<<1)
#define NPT_PERM_NX       (1ULL<<63)
/* 9.280 (C6 v2): the mgr tracks DIRECT VAs for every table it allocates.
 * MmGetVirtualForPhysical is empirically unreliable on this build (selective
 * NULL / wrong aliases, the 9.271-9.273 saga), so table access must never go
 * through it. The identity map (0..512GB) only uses p4==0, so pdpt_va/pd_va
 * indexed by the PML4/PDPT index cover every table; split PTs are recorded in
 * pt_reg. npt_cleanup frees them. */
typedef struct { uint64_t pa; npt_entry_t *va; } npt_pt_reg_t;
typedef struct {
    uint64_t pml4_pa;
    npt_entry_t *pml4_va;
    uint64_t total_mapped_pages;
    uint64_t total_mapped_2mb_pages;
    npt_entry_t *pdpt_va[512];   /* PDPT page VA per PML4 index (p4<512) */
    npt_entry_t *pd_va[512];     /* PD page VA per PDPT index (p4==0) */
    npt_pt_reg_t pt_reg[256];    /* split PT pages (pa -> va) */
    uint32_t pt_reg_count;
} npt_mgr_t;
int npt_init(npt_mgr_t*m,uint64_t x); int npt_identity_map_range(npt_mgr_t*m,uint64_t s,uint64_t e);
int npt_set_page_perm(npt_mgr_t*m,uint64_t g,uint64_t f); int npt_set_page_perm_range(npt_mgr_t*m,uint64_t g,uint64_t s,uint64_t f);
uint64_t npt_translate(npt_mgr_t*m,uint64_t g); int npt_cleanup(npt_mgr_t*m);
uint64_t npt_read_entry(npt_mgr_t*m,uint64_t g);
int npt_write_entry(npt_mgr_t*m,uint64_t g,uint64_t v);
int npt_split_2mb_to_4kb(npt_mgr_t*m,uint64_t g);
int npt_map_page(npt_mgr_t*m,uint64_t g,uint64_t spa,uint64_t flags);
int npt_get_entry(npt_mgr_t*m,uint64_t g,uint64_t*out);   /* 9.329: read a leaf entry back */
int npt_exclude_pa(npt_mgr_t*m,uint64_t g);
int npt_exclude_range(npt_mgr_t*m,uint64_t g,uint64_t s);
int npt_exclude_self(npt_mgr_t*m);
#endif
