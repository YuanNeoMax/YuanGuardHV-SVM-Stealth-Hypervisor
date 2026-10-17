/*
 * mem_pa.c — Phase 2: physical-frame read/write.
 *
 * READ uses MmCopyMemory(PHYSICAL). Verified in this project (206c25 sync
 * guard). Needs no mapping, reports failure by status, safe at
 * DISPATCH_LEVEL — the same call mem_pte.c uses for table pages, for the
 * reason in docs/KERNEL_API_CHECKLIST.md section 3 (MmGetVirtualForPhysical
 * is not reliable for arbitrary physical pages).
 *
 * WRITE has no MmCopyMemory equivalent. The first attempt used MmMapIoSpace,
 * which is WRONG here: that API is for device MMIO, and on a normal RAM page
 * it failed — so every Engine B write silently fell back to the API path
 * (visible as fallbacks=N with pte writes staying 0). The correct primitive
 * for writing a RAM page is an MDL whose PFN we set by hand: describe the
 * page, map it, write, unmap.
 *
 * PASSIVE_LEVEL only: MmMapLockedPagesSpecifyCache may block.
 */

#include "mem_pa.h"
#include "mem_island.h"

static volatile LONG64 g_pa_reads;
static volatile LONG64 g_pa_reads_fail;
static volatile LONG64 g_pa_writes;
static volatile LONG64 g_pa_writes_fail;
static volatile LONG64 g_pa_bytes_r;
static volatile LONG64 g_pa_bytes_w;
static volatile LONG64 g_pa_map_fail;

int yghv_pa_read(uint64_t pa, void *dst, SIZE_T len) {
    MM_COPY_ADDRESS src;
    SIZE_T done = 0;
    NTSTATUS st;

    if (!dst || !len || !pa)
        return 0;

    src.PhysicalAddress.QuadPart = (LONGLONG)pa;
    st = MmCopyMemory(dst, src, len, MM_COPY_MEMORY_PHYSICAL, &done);
    if (!NT_SUCCESS(st) || done != len) {
        InterlockedIncrement64(&g_pa_reads_fail);
        return 0;
    }

    InterlockedIncrement64(&g_pa_reads);
    InterlockedExchangeAdd64(&g_pa_bytes_r, (LONG64)len);
    return 1;
}

/*
 * Write to a physical RAM page.
 *
 * MmMapIoSpace is for device MMIO and failed on ordinary RAM, so use an MDL
 * instead: describe the page, put the real PFN in by hand, map it cached,
 * write, unmap. This is the standard "map an arbitrary physical page" trick
 * and it needs no process attach.
 *
 * An MDL built this way covers one page, so loop page by page.
 */
typedef struct _YGHV_MDL_SMALL {
    MDL       Mdl;
    ULONG_PTR Pfn[1];
} YGHV_MDL_SMALL;

int yghv_pa_write(uint64_t pa, const void *src, SIZE_T len) {
    YGHV_MDL_SMALL mdl;
    SIZE_T off = 0;

    if (!src || !len || !pa)
        return 0;

    while (off < len) {
        uint64_t cur = pa + off;
        uint64_t page_left = 0x1000 - (cur & 0xFFF);
        SIZE_T chunk = (len - off) < page_left ? (len - off) : page_left;
        uint64_t page_base = cur & ~(uint64_t)0xFFF;
        PVOID mapped;

        /*
         * 9.323 REVERT: back to the MDL path.
         *
         * 9.321 tried the kernel direct map (kernel_va = base + pa), which is
         * correct for LOW frames — Phase 6 validated it on KUSER_SHARED_DATA
         * at PA 0x575F000. But the direct map does NOT cover every physical
         * frame: writing a target frame at PA 0x355C17000 (13.34 GB) faulted
         * 0x50 PAGE_FAULT_IN_NONPAGED_AREA at base+PA. Verified by the stack
         *   yghv_pa_write+0x6A -> memcpy+0x17
         * with the faulting address exactly base + that PA.
         *
         * So the direct map is only usable when the frame is known to be
         * covered. Rather than add a fragile range check, revert to the MDL
         * path — it is the one Phase 2-7 validated end to end.
         */
        RtlZeroMemory(&mdl, sizeof(mdl));
        /* Give MmInitializeMdl a page-aligned "virtual" address so it builds
         * a one-page descriptor; we then replace the PFN it derived with the
         * real one for this physical page. */
        MmInitializeMdl(&mdl.Mdl, (PVOID)page_base, 0x1000);
        mdl.Pfn[0] = (ULONG_PTR)(page_base >> 12);
        mdl.Mdl.ByteOffset = (CSHORT)(cur & 0xFFF);
        mdl.Mdl.ByteCount  = (ULONG)chunk;
        mdl.Mdl.MdlFlags  |= MDL_PAGES_LOCKED;

        mapped = MmMapLockedPagesSpecifyCache(&mdl.Mdl, KernelMode,
                                              MmCached, NULL, FALSE,
                                              NormalPagePriority);
        if (!mapped) {
            InterlockedIncrement64(&g_pa_map_fail);
            InterlockedIncrement64(&g_pa_writes_fail);
            return 0;
        }

        RtlCopyMemory(mapped, (const uint8_t *)src + off, chunk);
        MmUnmapLockedPages(mapped, &mdl.Mdl);

        off += chunk;
    }

    InterlockedIncrement64(&g_pa_writes);
    InterlockedExchangeAdd64(&g_pa_bytes_w, (LONG64)len);
    return 1;
}

int yghv_pa_read_u64(uint64_t pa, uint64_t *out) {
    uint64_t v = 0;
    if (!out)
        return 0;
    if (!yghv_pa_read(pa, &v, sizeof(v)))
        return 0;
    *out = v;
    return 1;
}

int yghv_pa_write_u64(uint64_t pa, uint64_t value) {
    return yghv_pa_write(pa, &value, sizeof(value));
}

void yghv_pa_stats_get(yghv_pa_stats_t *out) {
    if (!out)
        return;
    out->reads         = (uint64_t)InterlockedCompareExchange64(&g_pa_reads, 0, 0);
    out->reads_failed  = (uint64_t)InterlockedCompareExchange64(&g_pa_reads_fail, 0, 0);
    out->writes        = (uint64_t)InterlockedCompareExchange64(&g_pa_writes, 0, 0);
    out->writes_failed = (uint64_t)InterlockedCompareExchange64(&g_pa_writes_fail, 0, 0);
    out->bytes_read    = (uint64_t)InterlockedCompareExchange64(&g_pa_bytes_r, 0, 0);
    out->bytes_written = (uint64_t)InterlockedCompareExchange64(&g_pa_bytes_w, 0, 0);
    out->map_failures  = (uint64_t)InterlockedCompareExchange64(&g_pa_map_fail, 0, 0);
}

void yghv_pa_stats_reset(void) {
    InterlockedExchange64(&g_pa_reads, 0);
    InterlockedExchange64(&g_pa_reads_fail, 0);
    InterlockedExchange64(&g_pa_writes, 0);
    InterlockedExchange64(&g_pa_writes_fail, 0);
    InterlockedExchange64(&g_pa_bytes_r, 0);
    InterlockedExchange64(&g_pa_bytes_w, 0);
    InterlockedExchange64(&g_pa_map_fail, 0);
}
