/*
 * mem_island.c — Phase 6: island-context physical access via the direct map.
 * See common/mem_island.h for why every other primitive is illegal here.
 */

#include "mem_island.h"
#include "mem_pa.h"

/*
 * The direct map is a fixed-offset window: kernel_va = base + pa.
 * Windows places it in the high half; on x64 the candidate region is
 * 0xFFFF800000000000..0xFFFFFFFFFFFFFFFF. We do not guess — we derive it from
 * a pair whose VA and PA we both know, then verify with a second pair.
 */
#define YGHV_DIRECTMAP_MIN   0xFFFF800000000000ULL
#define YGHV_DIRECTMAP_MAX   0xFFFFFFFFFFFFFFFFULL

static volatile LONG64 g_island_base;
static volatile LONG   g_island_up;

static volatile LONG64 g_island_reads;
static volatile LONG64 g_island_writes;
static volatile LONG64 g_island_refusals;
static volatile LONG64 g_island_probes;

int yghv_island_init(uint64_t probe_va_a, uint64_t probe_pa_a,
                     uint64_t probe_va_b, uint64_t probe_pa_b) {
    uint64_t base_a, base_b;

    if (!probe_va_a || !probe_va_b)
        return 0;

    /*
     * Derive the base from each pair independently. A correct pair gives
     * va - pa; two agreeing derivations is the validation (a wrong pair, or a
     * VA that is not direct-mapped, produces a different base).
     *
     * Both operands are page-aligned first: the low 12 bits belong to the
     * offset within the frame, not to the base.
     */
    base_a = (probe_va_a & ~(uint64_t)0xFFFULL) - (probe_pa_a & ~(uint64_t)0xFFFULL);
    base_b = (probe_va_b & ~(uint64_t)0xFFFULL) - (probe_pa_b & ~(uint64_t)0xFFFULL);

    InterlockedExchange64(&g_island_probes, 2);

    if (base_a != base_b)
        return 0;                       /* inconsistent: do not publish */
    if (base_a < YGHV_DIRECTMAP_MIN || base_a > YGHV_DIRECTMAP_MAX)
        return 0;                       /* not in the kernel half */

    InterlockedExchange64(&g_island_base, (LONG64)base_a);
    InterlockedExchange(&g_island_up, 1);
    return 1;
}

int yghv_island_ready(void) {
    return g_island_up && InterlockedCompareExchange64(&g_island_base, 0, 0) != 0;
}

uint64_t yghv_island_base(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_island_base, 0, 0);
}

/*
 * Bounds check before the load.
 *
 * A wrong base does not fault — it reads unrelated kernel memory and hands it
 * back as if it were the frame. That is worse than failing, so the range is
 * checked against the direct map's extent on every access, and a frame that
 * would land outside it is refused.
 */
static int yghv_island_addr(uint64_t pa, uint8_t **out) {
    uint64_t base = (uint64_t)InterlockedCompareExchange64(&g_island_base, 0, 0);
    uint64_t va;

    if (!g_island_up || !base)
        return 0;

    va = base + pa;
    /* Must stay inside the kernel half (canonical) — a wrapped or truncated
     * computation would land in user space, which is never the frame. */
    if (va < YGHV_DIRECTMAP_MIN)
        return 0;
    if (va < base)                      /* overflow */
        return 0;

    *out = (uint8_t *)(ULONG_PTR)va;
    return 1;
}

int yghv_island_read(uint64_t pa, void *dst, SIZE_T len) {
    uint8_t *src;

    if (!dst || !len || !pa) {
        InterlockedIncrement64(&g_island_refusals);
        return 0;
    }
    if (!yghv_island_addr(pa, &src)) {
        InterlockedIncrement64(&g_island_refusals);
        return 0;
    }

    /* Plain load — the whole point: no call, so legal at DISPATCH/GIF=0. */
    RtlCopyMemory(dst, src, len);
    InterlockedIncrement64(&g_island_reads);
    return 1;
}

int yghv_island_write(uint64_t pa, const void *src, SIZE_T len) {
    uint8_t *dst;

    if (!src || !len || !pa) {
        InterlockedIncrement64(&g_island_refusals);
        return 0;
    }
    if (!yghv_island_addr(pa, &dst)) {
        InterlockedIncrement64(&g_island_refusals);
        return 0;
    }

    RtlCopyMemory(dst, src, len);
    InterlockedIncrement64(&g_island_writes);
    return 1;
}

void yghv_island_stats_get(yghv_island_stats_t *out) {
    if (!out)
        return;
    out->base     = (uint64_t)InterlockedCompareExchange64(&g_island_base, 0, 0);
    out->reads    = (uint64_t)InterlockedCompareExchange64(&g_island_reads, 0, 0);
    out->writes   = (uint64_t)InterlockedCompareExchange64(&g_island_writes, 0, 0);
    out->refusals = (uint64_t)InterlockedCompareExchange64(&g_island_refusals, 0, 0);
    out->probes   = (uint64_t)InterlockedCompareExchange64(&g_island_probes, 0, 0);
}

void yghv_island_stats_reset(void) {
    InterlockedExchange64(&g_island_reads, 0);
    InterlockedExchange64(&g_island_writes, 0);
    InterlockedExchange64(&g_island_refusals, 0);
    InterlockedExchange64(&g_island_probes, 0);
}
