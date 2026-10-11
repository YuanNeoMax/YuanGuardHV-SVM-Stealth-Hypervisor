/*
 * mem_selfmap.c — Phase 2b: self-built physical mapping window.
 * See common/mem_selfmap.h for the design and the honest limits.
 *
 * The window is one reserved VA whose leaf PTE we rewrite by hand. Once it
 * points at a frame, reading/writing that frame is a plain memory access —
 * no MmCopyMemory, no MDL, no API on the access path.
 */

#include "mem_selfmap.h"
#include "mem_pa.h"

/*
 * INVLPG and CR3 read, done with inline asm.
 *
 * Declaring them as NTKERNELAPI made the linker look for __imp___invlpg /
 * __imp___readcr3, which the WDK km libs do not export for this target. The
 * project already uses inline asm for the same reason elsewhere
 * (svm_simplevm206_x64.asm), so do the same here.
 */
static void yghv_invlpg(void *addr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(addr) : "memory");
}

static uint64_t yghv_readcr3(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

#define YGHV_SELFMAP_PAGES  1
#define YGHV_SELFMAP_SIZE   0x1000

static PVOID    g_window_va;      /* reserved mapping address */
static PMDL     g_window_mdl;     /* locked MDL backing the window */
static PVOID    g_window_locked;  /* VA returned by the map call */
static uint64_t g_window_entry_pa;/* PA of the leaf PTE we rewrite */
static uint64_t g_window_target;  /* frame currently aliased (0 = none) */
static volatile LONG g_window_up;

static volatile LONG64 g_sm_inits;
static volatile LONG64 g_sm_retargets;
static volatile LONG64 g_sm_reads;
static volatile LONG64 g_sm_writes;
static volatile LONG64 g_sm_fail;
static volatile LONG64 g_sm_flushes;

/*
 * Flush one TLB entry for our own address space.
 *
 * INVLPG works here: it invalidates a GVA->GPA translation in the CURRENT
 * address space. What 9.297/9.298 falsified was the NPT (GPA->HPA) layer,
 * which is a different structure and a different flush. Do not conflate the
 * two — that mistake is what made the earlier round's experiments ambiguous.
 */
static void yghv_selfmap_flush(uint64_t va) {
    yghv_invlpg((void *)(ULONG_PTR)va);
    InterlockedIncrement64(&g_sm_flushes);
}

/*
 * Rewrite the window's leaf entry so its PFN is the target frame's.
 *
 * The entry keeps its own flags (present/writable/nx) — only the address
 * field is replaced. Writing through mem_pa keeps the access primitive the
 * same one the rest of Engine B uses.
 */
static int yghv_selfmap_retarget(uint64_t target_pa) {
    uint64_t entry;
    uint64_t new_entry;
    uint64_t frame = target_pa & ~(uint64_t)0xFFFULL;

    if (!g_window_entry_pa)
        return 0;

    if (!yghv_pa_read(g_window_entry_pa, &entry, sizeof(entry)))
        return 0;

    /* Preserve every flag bit; replace only bits 12..51 with the new PFN. */
    new_entry = (entry & ~YGHV_PTE_ADDR_MASK) | (frame & YGHV_PTE_ADDR_MASK);

    if (!yghv_pa_write(g_window_entry_pa, &new_entry, sizeof(new_entry)))
        return 0;

    yghv_selfmap_flush((uint64_t)(ULONG_PTR)g_window_va);
    g_window_target = frame;
    InterlockedIncrement64(&g_sm_retargets);
    return 1;
}

int yghv_selfmap_init(void) {
    /*
     * DISABLED — see docs/YUANMOD_HANDOFF_CURRENT.md 9.307.
     *
     * The original implementation crashed the machine three times with
     * 0x1A / 0x1240 (PFN database corrupt). Three independent API errors:
     *
     *   1. MmBuildMdlForNonPagedPool was called on a *reserved mapping*
     *      address. That routine assumes the buffer is in nonpaged pool and
     *      derives the PFN with MmGetPhysicalAddress; a reserved mapping
     *      address has no backing page, so the PFN it wrote was garbage.
     *      (The documented partner for MmAllocateMappingAddress is
     *      MmProbeAndLockPages + MmMapLockedPagesWithReservedMapping.)
     *   2. That garbage PFN was then installed into a PTE by
     *      MmMapLockedPagesSpecifyCache.
     *   3. yghv_selfmap_retarget additionally rewrote the leaf PTE by hand,
     *      which bypasses the memory manager's PFN reference counting.
     *
     * The deeper conclusion is that the goal itself is unreachable: a window
     * that reaches an arbitrary physical frame with NO api on the access path
     * requires editing PTEs by hand, and editing PTEs by hand breaks the PFN
     * reference counts that the memory manager owns. The two requirements are
     * mutually exclusive unless a driver reimplements that bookkeeping, which
     * is an internal contract and out of scope here.
     *
     * What already works without it: Engine B (page-table walk + physical
     * frame via MmCopyMemory / an MDL) carries no process semantics — no
     * MmCopyVirtualMemory, no CR3 switch — which is the observable-attack-
     * surface reduction Phase 2b was meant to add. Phase 1-5 deliver that and
     * passed 10/10.
     *
     * The code is kept as a record. init() therefore always fails, which
     * makes the engine fall back to plain Engine B (see yghv_mem_auto_pick).
     */
    InterlockedIncrement64(&g_sm_fail);
    return 0;
}

static int yghv_selfmap_init_unsafe_do_not_call(void) {
    yghv_pte_info_t pi;
    uint64_t cr3;

    if (g_window_up)
        return 1;

    /* 1. Reserve a page-aligned VA. A "mapping address" is reserved by the
     *    memory manager and deliberately has no backing store, which is
     *    exactly what we want: we supply the backing ourselves. */
    g_window_va = MmAllocateMappingAddress(YGHV_SELFMAP_SIZE, 'PMSY');
    if (!g_window_va) {
        InterlockedIncrement64(&g_sm_fail);
        return 0;
    }

    /* 2. Lock an MDL on it and map it so the PTE exists and is writable.
     *    MmMapLockedPagesSpecifyCache fills in a real, owned PTE. */
    g_window_mdl = IoAllocateMdl(g_window_va, YGHV_SELFMAP_SIZE, FALSE, FALSE, NULL);
    if (!g_window_mdl) {
        MmFreeMappingAddress(g_window_va, 'PMSY');
        g_window_va = NULL;
        InterlockedIncrement64(&g_sm_fail);
        return 0;
    }
    MmInitializeMdl(g_window_mdl, g_window_va, YGHV_SELFMAP_SIZE);
    MmBuildMdlForNonPagedPool(g_window_mdl);

    g_window_locked = MmMapLockedPagesSpecifyCache(g_window_mdl, KernelMode,
                                                   MmCached, NULL, FALSE,
                                                   NormalPagePriority);
    if (!g_window_locked) {
        IoFreeMdl(g_window_mdl);
        g_window_mdl = NULL;
        MmFreeMappingAddress(g_window_va, 'PMSY');
        g_window_va = NULL;
        InterlockedIncrement64(&g_sm_fail);
        return 0;
    }

    /* 3. Walk our OWN address space for the window VA to find the leaf entry
     *    we are allowed to repoint. */
    cr3 = yghv_readcr3();
    RtlZeroMemory(&pi, sizeof(pi));
    if (!yghv_pte_walk(cr3, (uint64_t)(ULONG_PTR)g_window_va, &pi) || !pi.entry_pa) {
        MmUnmapLockedPages(g_window_locked, g_window_mdl);
        IoFreeMdl(g_window_mdl);
        g_window_mdl = NULL;
        MmFreeMappingAddress(g_window_va, 'PMSY');
        g_window_va = NULL;
        InterlockedIncrement64(&g_sm_fail);
        return 0;
    }
    g_window_entry_pa = pi.entry_pa;
    g_window_target   = pi.page_base;

    InterlockedExchange(&g_window_up, 1);
    InterlockedIncrement64(&g_sm_inits);
    return 1;
}

void yghv_selfmap_stop(void) {
    if (!InterlockedExchange(&g_window_up, 0))
        return;

    if (g_window_locked && g_window_mdl) {
        MmUnmapLockedPages(g_window_locked, g_window_mdl);
        g_window_locked = NULL;
    }
    if (g_window_mdl) {
        IoFreeMdl(g_window_mdl);
        g_window_mdl = NULL;
    }
    if (g_window_va) {
        MmFreeMappingAddress(g_window_va, 'PMSY');
        g_window_va = NULL;
    }
    g_window_entry_pa = 0;
    g_window_target   = 0;
}

int yghv_selfmap_ready(void) {
    return g_window_up && g_window_locked && g_window_entry_pa;
}

uint64_t yghv_selfmap_window_va(void) {
    return (uint64_t)(ULONG_PTR)g_window_va;
}

void *yghv_selfmap_map(uint64_t target_pa, uint32_t offset) {
    uint64_t frame;

    if (!yghv_selfmap_ready() || offset >= YGHV_SELFMAP_SIZE)
        return NULL;

    frame = target_pa & ~(uint64_t)0xFFFULL;
    if (!frame) {
        InterlockedIncrement64(&g_sm_fail);
        return NULL;
    }

    /* Never point the window at itself: rewriting our own PTE through the
     * window would alias the entry we are writing. */
    if (frame == g_window_target && g_window_entry_pa >= frame &&
        g_window_entry_pa < frame + YGHV_SELFMAP_SIZE) {
        InterlockedIncrement64(&g_sm_fail);
        return NULL;
    }

    if (frame != g_window_target) {
        if (!yghv_selfmap_retarget(frame))
            return NULL;
    }

    return (uint8_t *)g_window_locked + offset;
}

int yghv_selfmap_read(uint64_t pa, void *dst, SIZE_T len) {
    SIZE_T off = 0;

    if (!dst || !len || !pa)
        return 0;

    while (off < len) {
        uint64_t cur = pa + off;
        uint64_t left = YGHV_SELFMAP_SIZE - (cur & 0xFFF);
        SIZE_T chunk = (len - off) < left ? (len - off) : (SIZE_T)left;
        void *p = yghv_selfmap_map(cur, (uint32_t)(cur & 0xFFF));

        if (!p) {
            InterlockedIncrement64(&g_sm_fail);
            return 0;
        }
        RtlCopyMemory((uint8_t *)dst + off, p, chunk);
        off += chunk;
    }
    InterlockedIncrement64(&g_sm_reads);
    return 1;
}

int yghv_selfmap_write(uint64_t pa, const void *src, SIZE_T len) {
    SIZE_T off = 0;

    if (!src || !len || !pa)
        return 0;

    while (off < len) {
        uint64_t cur = pa + off;
        uint64_t left = YGHV_SELFMAP_SIZE - (cur & 0xFFF);
        SIZE_T chunk = (len - off) < left ? (len - off) : (SIZE_T)left;
        void *p = yghv_selfmap_map(cur, (uint32_t)(cur & 0xFFF));

        if (!p) {
            InterlockedIncrement64(&g_sm_fail);
            return 0;
        }
        RtlCopyMemory(p, (const uint8_t *)src + off, chunk);
        off += chunk;
    }
    InterlockedIncrement64(&g_sm_writes);
    return 1;
}

void yghv_selfmap_stats_get(yghv_selfmap_stats_t *out) {
    if (!out)
        return;
    out->inits     = (uint64_t)InterlockedCompareExchange64(&g_sm_inits, 0, 0);
    out->retargets = (uint64_t)InterlockedCompareExchange64(&g_sm_retargets, 0, 0);
    out->reads     = (uint64_t)InterlockedCompareExchange64(&g_sm_reads, 0, 0);
    out->writes    = (uint64_t)InterlockedCompareExchange64(&g_sm_writes, 0, 0);
    out->failures  = (uint64_t)InterlockedCompareExchange64(&g_sm_fail, 0, 0);
    out->tlb_flushes = (uint64_t)InterlockedCompareExchange64(&g_sm_flushes, 0, 0);
}

void yghv_selfmap_stats_reset(void) {
    InterlockedExchange64(&g_sm_inits, 0);
    InterlockedExchange64(&g_sm_retargets, 0);
    InterlockedExchange64(&g_sm_reads, 0);
    InterlockedExchange64(&g_sm_writes, 0);
    InterlockedExchange64(&g_sm_fail, 0);
    InterlockedExchange64(&g_sm_flushes, 0);
}
