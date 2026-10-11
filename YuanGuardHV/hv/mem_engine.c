/*
 * mem_engine.c — Phase 3: two-engine abstraction.
 *
 * Engine A (MmCopyVirtualMemory): the project's established path
 *   (protect.c:yghv_sync_write_page). Process semantics, CR3 switch
 *   internally, handles page faults and COW. Observable.
 *
 * Engine B (page-table walk + physical frame): no NT memory API on the
 *   path, no CR3 switch. Large-page aware. Cannot handle a page that is
 *   swapped out or not-present (the walk stops at the absent entry), so
 *   those requests fall back to A.
 *
 * Auto rules (see the plan): B for an ordinary user page when the walk
 * resolves; A for system space, cross-page reads, absent pages, and when
 * B is unavailable.
 */

#include "mem_engine.h"
#include "mem_pa.h"

NTKERNELAPI NTSTATUS MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress,
    SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
    PSIZE_T NumberOfBytesCopied);

static volatile LONG g_engine_sel;      /* 0 auto, 1 api, 2 pte */

static volatile LONG64 g_api_reads, g_api_writes, g_api_fail;
static volatile LONG64 g_pte_reads, g_pte_writes, g_pte_fail;
static volatile LONG64 g_fallbacks, g_auto_api, g_auto_pte;
static volatile LONG64 g_pte_wfail_irql;
static volatile LONG64 g_pte_wfail_walk;
static volatile LONG64 g_pte_wfail_pa;

/* ===================== Engine A: NT API ================================ */

static NTSTATUS api_read(PEPROCESS proc, uint64_t va, void *buf, SIZE_T len,
                         SIZE_T *done) {
    SIZE_T copied = 0;
    NTSTATUS st = MmCopyVirtualMemory(proc, (PVOID)(ULONG_PTR)va,
                                      IoGetCurrentProcess(), buf, len,
                                      KernelMode, &copied);
    if (done)
        *done = copied;
    if (!NT_SUCCESS(st)) {
        InterlockedIncrement64(&g_api_fail);
        return st;
    }
    InterlockedIncrement64(&g_api_reads);
    return STATUS_SUCCESS;
}

static NTSTATUS api_write(PEPROCESS proc, uint64_t va, const void *buf, SIZE_T len,
                          SIZE_T *done) {
    SIZE_T copied = 0;
    NTSTATUS st = MmCopyVirtualMemory(IoGetCurrentProcess(), (PVOID)buf,
                                      proc, (PVOID)(ULONG_PTR)va, len,
                                      KernelMode, &copied);
    if (done)
        *done = copied;
    if (!NT_SUCCESS(st)) {
        InterlockedIncrement64(&g_api_fail);
        return st;
    }
    InterlockedIncrement64(&g_api_writes);
    return STATUS_SUCCESS;
}

const yghv_mem_engine_ops_t g_yghv_engine_api = {
    "api", api_read, api_write
};

/* ===================== Engine B: page-table + physical ================= */

/*
 * Resolve and read/write one page at a time.
 *
 * A request may span pages, so loop; within a page the offset is carried
 * into the physical address. Each iteration re-walks, which is correct even
 * if the mapping changes between pages.
 *
 * Reads use MmCopyMemory (DISPATCH-safe). Writes go through MmMapIoSpace,
 * which needs PASSIVE_LEVEL — so an Engine B write from the island is not
 * allowed and returns STATUS_INVALID_DEVICE_STATE.
 */
static NTSTATUS pte_read(PEPROCESS proc, uint64_t va, void *buf, SIZE_T len,
                         SIZE_T *done) {
    uint64_t cr3;
    SIZE_T off = 0;
    uint8_t *dst = (uint8_t *)buf;

    if (done)
        *done = 0;
    if (!proc || !buf || !len)
        return STATUS_INVALID_PARAMETER;

    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);   /* KPROCESS.DTB */
    if (!cr3)
        return STATUS_INVALID_PARAMETER;

    while (off < len) {
        uint64_t cur = va + off;
        uint64_t page_left = 0x1000 - (cur & 0xFFF);
        SIZE_T chunk = (SIZE_T)((len - off) < page_left ? (len - off) : page_left);
        yghv_pte_info_t pi;

        RtlZeroMemory(&pi, sizeof(pi));
        if (!yghv_pte_walk(cr3, cur, &pi) || !pi.pa) {
            InterlockedIncrement64(&g_pte_fail);
            if (done)
                *done = off;
            return STATUS_NOT_FOUND;      /* not present -> caller may fall back */
        }

        if (!yghv_pa_read(pi.pa, dst + off, chunk)) {
            InterlockedIncrement64(&g_pte_fail);
            if (done)
                *done = off;
            return STATUS_UNSUCCESSFUL;
        }
        off += chunk;
    }

    InterlockedIncrement64(&g_pte_reads);
    if (done)
        *done = off;
    return STATUS_SUCCESS;
}

static NTSTATUS pte_write(PEPROCESS proc, uint64_t va, const void *buf, SIZE_T len,
                          SIZE_T *done) {
    uint64_t cr3;
    SIZE_T off = 0;
    const uint8_t *src = (const uint8_t *)buf;

    if (done)
        *done = 0;
    if (!proc || !buf || !len)
        return STATUS_INVALID_PARAMETER;

    /* MmMapLockedPagesSpecifyCache needs PASSIVE_LEVEL, NOT enabled APCs.
     *
     * The first version tested KeAreApcsDisabled() and rejected every write:
     * an IOCTL dispatch runs with APCs disabled (normal for a dispatch
     * routine), so that predicate is TRUE there even though the IRQL is
     * PASSIVE and mapping is perfectly legal — measured as B-wfail irql=1
     * with pte writes stuck at 0 and every write silently falling back to A.
     * KeGetCurrentIrql() is not usable either (this toolchain lowers it to
     * __readcr8, which the WDK km libs do not export for this target).
     *
     * KeAreAllApcsDisabled() is TRUE only when the IRQL is actually at or
     * above APC_LEVEL (raised), not merely when the thread has APCs disabled,
     * so it is the correct probe here. */
    if (KeAreAllApcsDisabled()) {
        InterlockedIncrement64(&g_pte_wfail_irql);
        return STATUS_INVALID_DEVICE_STATE;
    }

    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
    if (!cr3) {
        InterlockedIncrement64(&g_pte_wfail_walk);   /* no CR3 -> cannot walk */
        return STATUS_INVALID_PARAMETER;
    }

    while (off < len) {
        uint64_t cur = va + off;
        uint64_t page_left = 0x1000 - (cur & 0xFFF);
        SIZE_T chunk = (SIZE_T)((len - off) < page_left ? (len - off) : page_left);
        yghv_pte_info_t pi;

        RtlZeroMemory(&pi, sizeof(pi));
        if (!yghv_pte_walk(cr3, cur, &pi) || !pi.pa) {
            InterlockedIncrement64(&g_pte_wfail_walk);
            InterlockedIncrement64(&g_pte_fail);
            if (done)
                *done = off;
            return STATUS_NOT_FOUND;
        }

        if (!yghv_pa_write(pi.pa, src + off, chunk)) {
            InterlockedIncrement64(&g_pte_wfail_pa);
            InterlockedIncrement64(&g_pte_fail);
            if (done)
                *done = off;
            return STATUS_UNSUCCESSFUL;
        }
        off += chunk;
    }

    InterlockedIncrement64(&g_pte_writes);
    if (done)
        *done = off;
    return STATUS_SUCCESS;
}

const yghv_mem_engine_ops_t g_yghv_engine_pte = {
    "pte", pte_read, pte_write
};

/* ===================== selection ====================================== */

void yghv_mem_engine_set(yghv_engine_t e) {
    if (e > YGHV_ENGINE_PTE)
        e = YGHV_ENGINE_AUTO;
    InterlockedExchange(&g_engine_sel, (LONG)e);
}

yghv_engine_t yghv_mem_engine_get(void) {
    return (yghv_engine_t)InterlockedCompareExchange(&g_engine_sel, 0, 0);
}

/*
 * Auto rules:
 *   - explicit selection wins
 *   - system-space VA (top half) -> A (B has no advantage there)
 *   - a request crossing a page boundary -> A (one call vs several walks)
 *   - a page the walk cannot resolve -> A (it can fault the page in)
 *   - otherwise -> B
 */
yghv_engine_t yghv_mem_auto_pick(PEPROCESS proc, uint64_t va, SIZE_T len) {
    yghv_engine_t sel = yghv_mem_engine_get();
    uint64_t cr3;
    yghv_pte_info_t pi;

    if (sel == YGHV_ENGINE_API || sel == YGHV_ENGINE_PTE)
        return sel;

    if (!proc)
        return YGHV_ENGINE_API;

    /* Kernel VA: Engine B would have to walk the kernel CR3; no benefit. */
    if (va >= 0xFFFF800000000000ULL) {
        InterlockedIncrement64(&g_auto_api);
        return YGHV_ENGINE_API;
    }

    /* Crossing a page boundary: A does it in one call. */
    if (((va & 0xFFF) + len) > 0x1000) {
        InterlockedIncrement64(&g_auto_api);
        return YGHV_ENGINE_API;
    }

    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);
    RtlZeroMemory(&pi, sizeof(pi));
    if (!cr3 || !yghv_pte_walk(cr3, va, &pi) || !pi.pa) {
        /* Not present (swapped out / demand-zero): only A can fault it in. */
        InterlockedIncrement64(&g_auto_api);
        return YGHV_ENGINE_API;
    }

    InterlockedIncrement64(&g_auto_pte);
    return YGHV_ENGINE_PTE;
}

/* ===================== unified entry ================================== */

NTSTATUS yghv_mem_read(PEPROCESS proc, uint64_t va, void *buf, SIZE_T len,
                       SIZE_T *done, yghv_engine_t *used) {
    yghv_engine_t e = yghv_mem_auto_pick(proc, va, len);
    NTSTATUS st;

    if (e == YGHV_ENGINE_PTE) {
        st = g_yghv_engine_pte.read(proc, va, buf, len, done);
        if (NT_SUCCESS(st)) {
            if (used) *used = YGHV_ENGINE_PTE;
            return st;
        }
        /* B could not do it — A handles faults and COW. */
        InterlockedIncrement64(&g_fallbacks);
    }

    st = g_yghv_engine_api.read(proc, va, buf, len, done);
    if (used) *used = YGHV_ENGINE_API;
    return st;
}

NTSTATUS yghv_mem_write(PEPROCESS proc, uint64_t va, const void *buf, SIZE_T len,
                        SIZE_T *done, yghv_engine_t *used) {
    yghv_engine_t e = yghv_mem_auto_pick(proc, va, len);
    NTSTATUS st;

    if (e == YGHV_ENGINE_PTE) {
        st = g_yghv_engine_pte.write(proc, va, buf, len, done);
        if (NT_SUCCESS(st)) {
            if (used) *used = YGHV_ENGINE_PTE;
            return st;
        }
        InterlockedIncrement64(&g_fallbacks);
    }

    st = g_yghv_engine_api.write(proc, va, buf, len, done);
    if (used) *used = YGHV_ENGINE_API;
    return st;
}

/* ===================== typed sizes ==================================== */

uint32_t yghv_mem_type_size(uint32_t type) {
    switch (type) {
    case YGHV_T_U8:  case YGHV_T_I8:  return 1;
    case YGHV_T_U16: case YGHV_T_I16: return 2;
    case YGHV_T_U32: case YGHV_T_I32: case YGHV_T_F32: return 4;
    case YGHV_T_U64: case YGHV_T_I64: case YGHV_T_F64: return 8;
    case YGHV_T_VEC3: return 12;
    case YGHV_T_MAT4: return 64;
    default: return 0;   /* strings and bytes are caller-sized */
    }
}

void yghv_mem_stats_get(yghv_mem_stats_t *out) {
    if (!out)
        return;
    out->api_reads   = (uint64_t)InterlockedCompareExchange64(&g_api_reads, 0, 0);
    out->api_writes  = (uint64_t)InterlockedCompareExchange64(&g_api_writes, 0, 0);
    out->api_fail    = (uint64_t)InterlockedCompareExchange64(&g_api_fail, 0, 0);
    out->pte_reads   = (uint64_t)InterlockedCompareExchange64(&g_pte_reads, 0, 0);
    out->pte_writes  = (uint64_t)InterlockedCompareExchange64(&g_pte_writes, 0, 0);
    out->pte_fail    = (uint64_t)InterlockedCompareExchange64(&g_pte_fail, 0, 0);
    out->fallbacks   = (uint64_t)InterlockedCompareExchange64(&g_fallbacks, 0, 0);
    out->auto_choices_api = (uint64_t)InterlockedCompareExchange64(&g_auto_api, 0, 0);
    out->auto_choices_pte = (uint64_t)InterlockedCompareExchange64(&g_auto_pte, 0, 0);
    out->pte_wfail_irql = (uint64_t)InterlockedCompareExchange64(&g_pte_wfail_irql, 0, 0);
    out->pte_wfail_walk = (uint64_t)InterlockedCompareExchange64(&g_pte_wfail_walk, 0, 0);
    out->pte_wfail_pa   = (uint64_t)InterlockedCompareExchange64(&g_pte_wfail_pa, 0, 0);
}

void yghv_mem_stats_reset(void) {
    InterlockedExchange64(&g_api_reads, 0);
    InterlockedExchange64(&g_api_writes, 0);
    InterlockedExchange64(&g_api_fail, 0);
    InterlockedExchange64(&g_pte_reads, 0);
    InterlockedExchange64(&g_pte_writes, 0);
    InterlockedExchange64(&g_pte_fail, 0);
    InterlockedExchange64(&g_fallbacks, 0);
    InterlockedExchange64(&g_auto_api, 0);
    InterlockedExchange64(&g_auto_pte, 0);
    InterlockedExchange64(&g_pte_wfail_irql, 0);
    InterlockedExchange64(&g_pte_wfail_walk, 0);
    InterlockedExchange64(&g_pte_wfail_pa, 0);
}
