/*
 * yghv_pt.c — 9.593: CR3 decoding and page-table analysis.
 *
 * See common/yghv_pt.h for what this answers and why. The short version:
 * mem_pte.c translates one VA; this module answers questions about a whole
 * address space, and it decodes CR3 instead of passing the raw value around.
 *
 * BUILT ON mem_pte.c, NOT BESIDE IT
 *   Every table read here goes through yghv_pte_read_entry /
 *   yghv_pte_enum_range. Re-implementing the 4-level loop would mean a second
 *   place for the range-bounding bug recorded in mem_pte.c:211-218 (a 16-page
 *   scan that walked 100M entries because the inner loops started at index 0
 *   instead of at the range's index and wedged the driver in STOP_PENDING).
 *   One walker, one place to fix it.
 *
 * WHAT IS NOT HERE
 *   No writes. Page-table *redirection* already exists (mem_selfmap.c,
 *   npt_remap) and has its own invariants about flushing and about which CR3
 *   the CPU is currently using. Adding a second writer here would put those
 *   invariants in two files.
 */

#include <ntifs.h>
#include <stdint.h>
#include "common/yghv_pt.h"
#include "common/mem_pte.h"
#include "common/control_ioctl.h"
#include "debug.h"

/* PsLookupProcessByProcessId is in ntifs.h on this WDK, but the prototype is
 * absent under some include orders; declaring it here matches the pattern the
 * rest of the tree uses (protect.c:8, control_device.c:69). */
NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId, PEPROCESS *Process);

/* KPROCESS.DirectoryTableBase. Not in any WDK header, and it is the ONE
 * absolute offset this tree accepts (yghv_nt.h explains why the offset table
 * does not carry it): five existing call sites already use 0x028 —
 * control_device.c:500/551/569/922 and protect.c set_target. Staying with the
 * tree's existing convention beats introducing a sixth spelling of it. */
#define YGHV_PT_DTB_OFFSET 0x028u

static volatile LONG64 g_pt_dispatches;
static volatile LONG64 g_pt_walks;
static volatile LONG64 g_pt_scans;
static volatile LONG64 g_pt_bytes;
static volatile LONG64 g_pt_fails;
static volatile LONG64 g_pt_cr3_lookups;
static volatile LONG64 g_pt_truncated;

int yghv_pt_cr3_of(unsigned long pid, yghv_pt_cr3_t *out) {
    PEPROCESS proc = NULL;
    NTSTATUS st;

    if (!out)
        return 0;
    RtlZeroMemory(out, sizeof(*out));

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st) || !proc) {
        InterlockedIncrement64(&g_pt_fails);
        return 0;
    }

    out->raw     = *(volatile uint64_t *)((uint8_t *)proc + YGHV_PT_DTB_OFFSET);
    out->dtb     = out->raw & YGHV_CR3_DTB_MASK;
    out->pcid    = (uint32_t)(out->raw & YGHV_CR3_PCID_MASK);
    out->noflush = (uint32_t)((out->raw & YGHV_CR3_NOFLUSH) ? 1 : 0);

    ObDereferenceObject(proc);
    InterlockedIncrement64(&g_pt_cr3_lookups);

    /* A DTB of 0 means the offset is wrong for this build. Reporting it as a
     * successful lookup with a zero base would make every downstream walk
     * return "not mapped", which reads as "the process has no memory". */
    if (!out->dtb) {
        InterlockedIncrement64(&g_pt_fails);
        return 0;
    }
    return 1;
}

int yghv_pt_tree(uint64_t cr3, uint64_t va, yghv_pt_tree_t *out) {
    uint64_t pml4_pa, pdpt_pa, pd_pa, pt_pa;
    uint64_t pml4e, pdpte, pde, pte;

    if (!out)
        return 0;
    RtlZeroMemory(out, sizeof(*out));

    pml4_pa = cr3 & YGHV_CR3_DTB_MASK;
    if (!pml4_pa) {
        InterlockedIncrement64(&g_pt_fails);
        return 0;
    }

    /* Level by level, keeping every entry and every table address: the point
     * of this function is to say WHICH level is absent, which a single
     * "pa == 0" answer cannot. */
    pml4e = yghv_pte_read_entry(pml4_pa, YGHV_PTE_INDEX_PML4(va));
    out->pml4e = pml4e;
    out->pml4_pa = pml4_pa;
    out->levels = 1;
    if (!(pml4e & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pt_walks);
        return 1;                       /* absent, but the walk itself worked */
    }

    pdpt_pa = pml4e & YGHV_PTE_ADDR_MASK;
    if (!pdpt_pa) {
        InterlockedIncrement64(&g_pt_fails);
        return 0;
    }
    pdpte = yghv_pte_read_entry(pdpt_pa, YGHV_PTE_INDEX_PDPT(va));
    out->pdpte = pdpte;
    out->pdpt_pa = pdpt_pa;
    out->levels = 2;
    if (!(pdpte & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pt_walks);
        return 1;
    }
    if (pdpte & YGHV_PTE_LARGE) {
        out->leaf = 3;
        out->pa = (pdpte & YGHV_PTE_ADDR_MASK_1G) | (va & (YGHV_PTE_SIZE_1G - 1));
        InterlockedIncrement64(&g_pt_walks);
        return 1;
    }

    pd_pa = pdpte & YGHV_PTE_ADDR_MASK;
    if (!pd_pa) {
        InterlockedIncrement64(&g_pt_fails);
        return 0;
    }
    pde = yghv_pte_read_entry(pd_pa, YGHV_PTE_INDEX_PD(va));
    out->pde = pde;
    out->pd_pa = pd_pa;
    out->levels = 3;
    if (!(pde & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pt_walks);
        return 1;
    }
    if (pde & YGHV_PTE_LARGE) {
        out->leaf = 2;
        out->pa = (pde & YGHV_PTE_ADDR_MASK_2M) | (va & (YGHV_PTE_SIZE_2M - 1));
        InterlockedIncrement64(&g_pt_walks);
        return 1;
    }

    pt_pa = pde & YGHV_PTE_ADDR_MASK;
    if (!pt_pa) {
        InterlockedIncrement64(&g_pt_fails);
        return 0;
    }
    pte = yghv_pte_read_entry(pt_pa, YGHV_PTE_INDEX_PT(va));
    out->pte = pte;
    out->pt_pa = pt_pa;
    out->levels = 4;
    if (!(pte & YGHV_PTE_PRESENT)) {
        InterlockedIncrement64(&g_pt_walks);
        return 1;
    }

    out->leaf = 1;
    out->pa = (pte & YGHV_PTE_ADDR_MASK) | (va & (YGHV_PTE_SIZE_4K - 1));

    /* Cross-check through mem_pte's walker. Two independent paths agreeing on
     * the same PA is worth one extra table read here and now, because the
     * failure this guards against — an offset that is right by coincidence —
     * is otherwise indistinguishable from success. */
    if (yghv_pte_va_to_pa(cr3, va) != out->pa) {
        InterlockedIncrement64(&g_pt_fails);
        LOG_ERROR("pt: tree/mem_pte disagree at va=0x%llx (tree=0x%llx)",
                  (unsigned long long)va, (unsigned long long)out->pa);
        out->pa = 0;
        out->leaf = 0;
        InterlockedIncrement64(&g_pt_walks);
        return 0;
    }

    InterlockedIncrement64(&g_pt_walks);
    return 1;
}

/* ------------------------------------------------------------------ range */

typedef struct {
    yghv_pt_map_t *map;
    uint64_t last_end;       /* end VA of the previous mapped page */
    uint64_t run_start;
    uint64_t run_bytes;
    uint64_t visited;
    uint64_t first_gap;
    int      have_prev;
} yghv_pt_scan_t;

static int yghv_pt_scan_cb(uint64_t va, const yghv_pte_info_t *info, void *cookie) {
    yghv_pt_scan_t *s = (yghv_pt_scan_t *)cookie;
    yghv_pt_map_t *m;

    if (!s || !info || !s->map)
        return 1;
    m = s->map;
    if (s->visited >= YGHV_PT_MAX_VISITS) {
        m->truncated = 1;
        InterlockedIncrement64(&g_pt_truncated);
        return 1;
    }
    s->visited++;

    m->pages++;
    m->bytes_mapped += info->page_size;
    if (info->page_size == YGHV_PTE_SIZE_1G)      m->pages_1g++;
    else if (info->page_size == YGHV_PTE_SIZE_2M) m->pages_2m++;
    else                                          m->pages_4k++;

    if (info->writable) m->writable++;
    if (info->nx == 0)  m->executable++;   /* NX clear = executable */
    if (info->user)     m->user++;

    /* Record the first hole: the gap between the end of the previous mapped
     * page and the start of this one. "Where is the first unmapped page" is
     * the question that tells a caller whether a range is densely packed or
     * has a hole at the top. */
    if (s->have_prev && s->last_end < va && m->first_gap_va == 0)
        m->first_gap_va = s->last_end;
    if (s->first_gap && m->first_gap_va == 0) {
        m->first_gap_va = s->first_gap;
    }

    /* Track the largest contiguous run of mapped bytes. A run is contiguous
     * when this page starts exactly where the previous one ended. */
    if (s->have_prev && s->last_end == va) {
        s->run_bytes += info->page_size;
    } else {
        s->run_start = va;
        s->run_bytes = info->page_size;
    }
    if (s->run_bytes > m->largest_run_bytes) {
        m->largest_run_bytes = s->run_bytes;
        m->largest_run_va = s->run_start;
    }

    s->last_end = va + info->page_size;
    s->have_prev = 1;
    return 0;
}

int yghv_pt_map(uint64_t cr3, uint64_t start_va, uint64_t end_va,
                yghv_pt_map_t *out) {
    yghv_pt_scan_t s;

    if (!out || !cr3 || start_va >= end_va)
        return 0;
    if (end_va - start_va > YGHV_PT_MAX_SCAN_BYTES)
        return 0;

    RtlZeroMemory(out, sizeof(*out));
    out->start_va = start_va;
    out->end_va   = end_va;

    RtlZeroMemory(&s, sizeof(s));
    s.map = out;

    /* The very first page of the range: if it is unmapped, enum_range never
     * calls the callback for it and the gap would go unreported. */
    {
        yghv_pte_info_t pi;
        RtlZeroMemory(&pi, sizeof(pi));
        if (!yghv_pte_query(cr3, start_va, &pi))
            s.first_gap = start_va;
    }

    (void)yghv_pte_enum_range(cr3, start_va, end_va, yghv_pt_scan_cb, &s);

    /* A range whose tail is unmapped has a gap that no callback can report:
     * the last callback ends before end_va. */
    if (out->first_gap_va == 0 && s.have_prev && s.last_end < end_va)
        out->first_gap_va = s.last_end;
    if (out->first_gap_va == 0 && !s.have_prev)
        out->first_gap_va = start_va;

    InterlockedIncrement64(&g_pt_scans);
    InterlockedAdd64(&g_pt_bytes, (LONG64)(end_va - start_va));
    return 1;
}

typedef struct {
    yghv_pt_page_t *arr;
    uint32_t max;
    uint32_t n;
    uint32_t truncated;
} yghv_pt_list_t;

static int yghv_pt_list_cb(uint64_t va, const yghv_pte_info_t *info, void *cookie) {
    yghv_pt_list_t *l = (yghv_pt_list_t *)cookie;

    if (!l || !info)
        return 1;
    if (l->n >= l->max) {
        l->truncated = 1;
        return 1;
    }
    l->arr[l->n].va   = va;
    l->arr[l->n].pa   = info->pa;
    l->arr[l->n].size = info->page_size;
    l->arr[l->n].flags = ((uint32_t)(info->writable ? 1u : 0u))
                       | ((uint32_t)(info->user ? 2u : 0u))
                       | ((uint32_t)(info->nx ? 4u : 0u))
                       | ((uint32_t)(info->accessed ? 8u : 0u));
    l->arr[l->n]._pad = 0;
    l->n++;
    return 0;
}

uint32_t yghv_pt_pages(uint64_t cr3, uint64_t start_va, uint64_t end_va,
                       yghv_pt_page_t *out, uint32_t max, uint32_t *truncated) {
    yghv_pt_list_t l;

    if (truncated)
        *truncated = 0;
    if (!out || !max || !cr3 || start_va >= end_va)
        return 0;
    if (end_va - start_va > YGHV_PT_MAX_SCAN_BYTES)
        return 0;

    RtlZeroMemory(&l, sizeof(l));
    l.arr = out;
    l.max = max;

    (void)yghv_pte_enum_range(cr3, start_va, end_va, yghv_pt_list_cb, &l);
    if (truncated)
        *truncated = l.truncated;
    if (l.truncated)
        InterlockedIncrement64(&g_pt_truncated);
    return l.n;
}

/* ------------------------------------------------------------------ IOCTL */

NTSTATUS yghv_pt_dispatch(const void *in, void *out, ULONG out_len, ULONG *info) {
    const yghv_ioctl_pt_req_t *req;
    yghv_ioctl_pt_req_t r;
    uint64_t *o;
    ULONG ops_info = YGHV_PT_HDR_QWORDS * sizeof(uint64_t);

    if (!in || !out || !info)
        return STATUS_INVALID_PARAMETER;
    if (out_len < YGHV_PT_HDR_QWORDS * sizeof(uint64_t))
        return STATUS_BUFFER_TOO_SMALL;

    /* METHOD_BUFFERED: in and out are the SAME buffer (9.591 lesson). Copy the
     * request out first, then clear the reply area. */
    RtlCopyMemory(&r, in, sizeof(r));
    o = (uint64_t *)out;
    RtlZeroMemory(out, out_len);
    *info = ops_info;
    InterlockedIncrement64(&g_pt_dispatches);

    switch (r.op) {
    case YGHV_PT_OP_CR3: {
        yghv_pt_cr3_t c;
        if (!yghv_pt_cr3_of(r.pid, &c)) {
            o[0] = 0;
            return STATUS_SUCCESS;
        }
        o[0] = 1;
        o[1] = c.raw;
        o[2] = c.dtb;
        o[3] = ((uint64_t)c.pcid) | ((uint64_t)c.noflush << 32);
        return STATUS_SUCCESS;
    }

    case YGHV_PT_OP_TREE: {
        yghv_pt_cr3_t c;
        yghv_pt_tree_t t;
        int ok;
        if (!yghv_pt_cr3_of(r.pid, &c)) {
            o[0] = 0;
            return STATUS_SUCCESS;
        }
        ok = yghv_pt_tree(c.dtb, (uint64_t)r.va, &t);
        o[0]  = ok ? 1 : 0;
        o[1]  = c.dtb;
        o[2]  = (uint64_t)r.va;
        o[3]  = t.pa;
        o[4]  = t.pml4e;  o[5]  = t.pml4_pa;
        o[6]  = t.pdpte;  o[7]  = t.pdpt_pa;
        o[8]  = t.pde;    o[9]  = t.pd_pa;
        o[10] = t.pte;    o[11] = t.pt_pa;
        o[12] = (uint64_t)t.levels;
        o[13] = (uint64_t)t.leaf;
        return STATUS_SUCCESS;
    }

    case YGHV_PT_OP_MAP: {
        yghv_pt_cr3_t c;
        yghv_pt_map_t m;
        if (!yghv_pt_cr3_of(r.pid, &c)) {
            o[0] = 0;
            return STATUS_SUCCESS;
        }
        if (!yghv_pt_map(c.dtb, (uint64_t)r.va, (uint64_t)r.end, &m)) {
            o[0] = 0;
            return STATUS_SUCCESS;
        }
        o[0]  = 1;
        o[1]  = m.pages;
        o[2]  = m.bytes_mapped;
        o[3]  = m.pages_4k;
        o[4]  = m.pages_2m;
        o[5]  = m.pages_1g;
        o[6]  = m.writable;
        o[7]  = m.executable;
        o[8]  = m.user;
        o[9]  = m.first_gap_va;
        o[10] = m.largest_run_va;
        o[11] = m.largest_run_bytes;
        o[12] = (uint64_t)m.truncated;
        return STATUS_SUCCESS;
    }

    case YGHV_PT_OP_PAGES: {
        yghv_pt_cr3_t c;
        uint32_t want, got, trunc = 0;
        yghv_pt_page_t *arr;
        ULONG cap;

        if (!yghv_pt_cr3_of(r.pid, &c)) {
            o[0] = 0;
            return STATUS_SUCCESS;
        }

        /* Rows are 4 qwords. The reply must fit both the header and the rows,
         * so the ceiling comes from out_len rather than from a constant that
         * could drift away from the client's request size. */
        cap = (out_len / sizeof(uint64_t)) - YGHV_PT_HDR_QWORDS;
        want = r.max ? r.max : YGHV_PT_MAX_LIST;
        if (want > cap)
            want = cap;
        if (want == 0)
            return STATUS_BUFFER_TOO_SMALL;

        arr = (yghv_pt_page_t *)(void *)((uint8_t *)out
              + YGHV_PT_HDR_QWORDS * sizeof(uint64_t));
        got = yghv_pt_pages(c.dtb, (uint64_t)r.va, (uint64_t)r.end,
                            arr, want, &trunc);

        o[0] = 1;
        o[1] = (uint64_t)got;
        o[2] = (uint64_t)trunc;
        o[3] = c.dtb;
        *info = (YGHV_PT_HDR_QWORDS + (ULONG)got * YGHV_PT_ROW_QWORDS)
                * sizeof(uint64_t);
        return STATUS_SUCCESS;
    }

    case YGHV_PT_OP_STATS: {
        yghv_pt_stats(o);
        return STATUS_SUCCESS;
    }

    default:
        return STATUS_INVALID_PARAMETER;
    }
}

void yghv_pt_stats(uint64_t *out) {
    if (!out)
        return;
    out[0] = (uint64_t)g_pt_dispatches;
    out[1] = (uint64_t)g_pt_walks;
    out[2] = (uint64_t)g_pt_scans;
    out[3] = (uint64_t)g_pt_bytes;
    out[4] = (uint64_t)g_pt_fails;
    out[5] = (uint64_t)g_pt_cr3_lookups;
    out[6] = (uint64_t)g_pt_truncated;
    out[7] = 0;
}
