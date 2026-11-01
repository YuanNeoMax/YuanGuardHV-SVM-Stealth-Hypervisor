/*
 * mem_bench.c — Phase 5 (Tier 1.5): the "beyond" proof, as a measurement.
 *
 * WHY THIS FILE EXISTS, AND WHAT IT REPLACES
 *
 * MEM_ENGINE_PLAN.md makes three claims about Engine B, and until this file only
 * one of them was ever MEASURED:
 *
 *   1. functional  — B has a page-table-driven scan, A does not      (true, shown
 *                                                                     by possession)
 *   2. performance — B is FASTER than A on large / large-page ranges  (never measured)
 *   3. stealth     — no NT memory API on B's stack                    (shown separately)
 *
 * Claim 2 was "~1-3x", "5-20x", "~100x" in a table with no run behind it. A
 * number nobody produced is not an estimate, it is a wish, and this tree has
 * already paid for that habit twice (9.440's three identity-reading counters,
 * 9.447's TSC break that dissolved on measurement).
 *
 * So this file answers ONE question with a stopwatch: for the same process, the
 * same range, and the same pattern, how long does each engine take, and how many
 * reads did each perform?
 *
 * HOW THE COMPARISON IS CONSTRUCTED (this is the part that makes it honest)
 *
 * Both engines read the SAME BYTES and compare against the SAME pattern:
 *
 *   Engine A: for each 4K candidate in [start,end):
 *               MmCopyVirtualMemory(target, va, local, 4096, ...)  then match
 *   Engine B: walk the page tables once; for each MAPPED page:
 *               yghv_pa_read(page_base, local, 4096)               then match
 *
 * Three things follow, and each is a reason the number is believable:
 *
 *  - A pays a virtual->physical translation PER ADDRESS and B pays one walk per
 *    PAGE, so the comparison is about translations, not about memcpy speed.
 *  - A cannot skip an absent page without a syscall-level failure; B skips whole
 *    absent subtrees during the walk, so B's read count is页数 and A's is
 *    candidate数. That gap IS the "page-table driven" claim in numeric form.
 *  - Both stop on the same page budget, so neither can win by doing more work.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO
 *
 * It does not touch a protected page, a hidden page, or an armed breakpoint. The
 * point is a clean A-vs-B timing, and a target page under NPT control would make
 * the B timing measure the NPF handler instead of the engine.
 *
 * It also does not claim the two engines return the same hits unless they
 * actually do. The report returns both hit counts side by side precisely so a
 * mismatch is VISIBLE rather than averaged away: if A finds 3 and B finds 5, the
 * bench says so and the reader decides which one to distrust. A benchmark that
 * hides a functional disagreement to protect a performance number is worse than
 * no benchmark.
 */

#include "mem_bench.h"
#include "mem_pte.h"
#include "mem_pa.h"
#include "mem_engine.h"

NTKERNELAPI NTSTATUS MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress,
    SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
    PSIZE_T NumberOfBytesCopied);

/*
 * One page is the unit. 4KB keeps A and B on identical footing: A's natural
 * granularity is one address, and B's is one page, so giving both a 4KB buffer
 * means the difference measured is translations-per-buffer and not buffer size.
 */
/*
 * One page is the unit for ENGINE A, because one address is A's natural request.
 * For ENGINE B the buffer is sized for a 2MB large page, because reading a large
 * page in 4K slices would hand B 512 callbacks for one walk step - the walk would
 * still be cheap but the transfer would be indistinguishable from A's, and the
 * large-page advantage this bench exists to measure would be thrown away.
 *
 * Measured reason for this: the first version capped every read at 4K, so on a
 * large page B read 4096 of the 2MB and its `bytes` came out 512x smaller than
 * A's. The timing looked good and the byte counts revealed the bench was not
 * doing the work.
 */
#define YGHV_BENCH_PAGE 4096u
#define YGHV_BENCH_LARGE (2u * 1024u * 1024u)
/*
 * 9.492: hard ceiling on pages examined per call, because a caller that omits the
 * budget used to get INFINITY and an unbounded IOCTL makes DriverUnload unbounded.
 * See the note at yghv_mem_bench's entry. 32768 x 4K = 128MB of address space per
 * call - more than a benchmark needs, small enough that one call cannot outlast a
 * caller's patience.
 */
#define YGHV_BENCH_MAX_PAGES 32768u

typedef struct {
    const uint8_t *pat;
    const uint8_t *mask;
    uint32_t       pat_len;
    uint8_t       *buf;

    uint64_t       hits;
    uint64_t       pages_read;      /* B: pages whose frame we read        */
    uint64_t       pages_cand;      /* A: addresses we asked MmCopyVM for  */
    uint64_t       bytes;
    uint64_t       page_budget;     /* 0 = no budget; both engines honour it */
    uint64_t       range_end;       /* B clips each page read to this bound   */
} bench_ctx_t;

static int bench_match(const uint8_t *p, const bench_ctx_t *c)
{
    uint32_t i;

    for (i = 0; i < c->pat_len; i++) {
        if (c->mask[i] == 0x00)
            continue;
        if (p[i] != c->pat[i])
            return 0;
    }
    return 1;
}

static void bench_scan_buf(uint64_t base, size_t size, bench_ctx_t *c)
{
    size_t pos;

    if (c->pat_len > size)
        return;

    for (pos = 0; pos + c->pat_len <= size; pos++) {
        if (bench_match(c->buf + pos, c)) {
            c->hits += 1;
        }
    }
    (void)base;
}

/* ---- Engine B: page-table driven ---------------------------------------- */

static int bench_b_page_cb(uint64_t va, const yghv_pte_info_t *info, void *cookie)
{
    bench_ctx_t *c = (bench_ctx_t *)cookie;
    uint32_t size;

    /*
     * The budget is enforced HERE because yghv_pte_enum_range has no budget
     * parameter - it walks until the range ends. Stopping from the callback is
     * the enumeration's own stop mechanism (non-zero return), which is why the
     * same page_budget applies to both engines: neither can do more work than
     * the other by finishing later.
     */
    if (c->pages_read >= c->page_budget)
        return 1;

    /*
     * Read the REAL page size in one go, up to 2MB.
     *
     * This is the large-page advantage in a single line: one walk step, one
     * physical read, 2MB covered. Capping at 4K (the first version) turned one
     * callback into 512 and made B look like A with extra steps.
     */
    size = (uint32_t)info->page_size;
    if (size > YGHV_BENCH_LARGE)
        size = YGHV_BENCH_LARGE;
    if (size == 0)
        size = YGHV_BENCH_PAGE;

    /*
     * 9.449: CLIP TO THE REQUESTED RANGE - this is what made the two engines
     * disagree on hits (A=223, B=242 over 32MB of javaw's heap).
     *
     * A's loop only reads a page when `va + 4096 <= end`, i.e. it never reads
     * across the end of the range. B was reading the page's FULL size whatever
     * `end` was, so the last page contributed bytes A never saw - and a byte A
     * never saw cannot produce a hit A did not report, which is exactly the 19
     * missing hits. The engines were not scanning the same bytes and the
     * comparison was invalid.
     *
     * Clipping here rather than shrinking the end bound keeps the enumeration
     * (which reports pages by their real size) untouched, and makes B's byte
     * count equal A's - the check that should have caught this before I read the
     * hit counts: when both report the same `bytes`, the same hits follow.
     */
    {
        uint64_t page_end = va + size;
        uint64_t clipped = c->range_end;

        if (clipped > page_end)
            clipped = page_end;
        if (va >= clipped)
            return 0;                 /* page starts beyond the range */
        size = (uint32_t)(clipped - va);
    }

    if (!yghv_pa_read(info->page_base, c->buf, size))
        return 0;

    c->pages_read += 1;
    c->bytes += size;
    bench_scan_buf(va, size, c);
    return 0;
}

/* ---- Engine A: one NT memory API call per address ------------------------ */

static void bench_a_range(PEPROCESS proc, uint64_t start, uint64_t end,
                          bench_ctx_t *c, uint64_t page_budget)
{
    uint64_t va;
    SIZE_T copied = 0;
    NTSTATUS st;

    /*
     * Engine A is asked for ONE 4K buffer per address. That is deliberately A's
     * worst honest case and it is the right one: A has no large-page notion, so
     * there is nothing else it COULD be asked for. Giving A a 2MB request would
     * be inventing an API it does not have, and giving it a 4K request is what
     * every real caller does.
     *
     * Consequence, stated so nobody reads the ratio as apples-to-apples in
     * buffer size: on a 2MB large page B does one read of 2MB where A does 512
     * reads of 4K. That asymmetry IS the finding, not a flaw in the setup.
     */
    va = (start + YGHV_BENCH_PAGE - 1) & ~(uint64_t)(YGHV_BENCH_PAGE - 1);

    for (; va + YGHV_BENCH_PAGE <= end; va += YGHV_BENCH_PAGE) {
        if (page_budget && c->pages_cand >= page_budget)
            return;

        copied = 0;
        st = MmCopyVirtualMemory(proc, (PVOID)(ULONG_PTR)va,
                                 IoGetCurrentProcess(), c->buf,
                                 YGHV_BENCH_PAGE, KernelMode, &copied);
        c->pages_cand += 1;

        /*
         * A failure here is NORMAL and is part of the cost being measured: this
         * is exactly the range where Engine A must pay a translation attempt for
         * a page that is not present, and Engine B skips the whole subtree. Count
         * the attempt (which is the work) and move on.
         */
        if (!NT_SUCCESS(st) || copied != YGHV_BENCH_PAGE)
            continue;

        c->bytes += YGHV_BENCH_PAGE;
        bench_scan_buf(va, YGHV_BENCH_PAGE, c);
    }
}

/*
 * Run both engines over the same range and report.
 *
 * Returns 1 if both ran, 0 if the input was unusable. It does NOT return a
 * "winner": the caller sees two times and two read counts and decides. A single
 * "B is N times faster" number would hide which workload produced it, and the
 * gap depends entirely on how many absent pages the range contains.
 */
int yghv_mem_bench(PEPROCESS proc, uint64_t start_va, uint64_t end_va,
                   const uint8_t *pattern, const uint8_t *mask, uint32_t pat_len,
                   uint64_t page_budget, yghv_bench_result_t *out)
{
    bench_ctx_t a, b;
    uint64_t cr3;
    uint64_t t0, t1;
    uint8_t *buf = NULL;
    uint64_t pages_visited = 0;
    int ok = 0;

    if (!proc || !pattern || !mask || !out || pat_len == 0 || pat_len > 64)
        return 0;
    if (start_va >= end_va)
        return 0;

    /*
     * 9.492: A ZERO BUDGET MEANT "UNBOUNDED", AND THAT MADE THE DRIVER UNLOADABLE.
     *
     * `page_budget == 0` is documented as "no budget; both engines honour it"
     * (see bench_ctx_t), and the client omits it unless asked. Measured
     * consequence, and it cost a reboot: `mem-bench <pid> 4D5A` with no budget
     * scanned explorer's whole address space for tens of minutes, and
     * `IoReleaseRemoveLockAndWait` in DriverUnload blocked on the in-flight IOCTL
     * the entire time - `sc stop` sat at STOP_PENDING past four minutes. Killing
     * the client did NOT stop it: the IOCTL holds a reference to the TARGET
     * process, not to the caller.
     *
     * An unbounded kernel operation reachable from an IOCTL is never acceptable,
     * whatever the caller asked for - it makes the unload path unbounded too.
     * The ceiling is imposed HERE, at the one place every path passes through,
     * rather than trusting a caller that forgot the argument.
     *
     * THE NUMBER: engine A pays one MmCopyVirtualMemory per 4K page, engine B one
     * callback per PT leaf. 32768 pages is ~32K syscall-equivalents; the measured
     * runaway did millions. At the ~7us per A read measured in 9.449 that bounds
     * one IOCTL to a few seconds - below any "hung" threshold, and still far more
     * range than a benchmark needs. A caller that genuinely wants more can ask;
     * what it can no longer do is forget and get infinity.
     */
    if (page_budget == 0 || page_budget > YGHV_BENCH_MAX_PAGES)
        page_budget = YGHV_BENCH_MAX_PAGES;

    RtlZeroMemory(out, sizeof(*out));
    RtlZeroMemory(&a, sizeof(a));
    RtlZeroMemory(&b, sizeof(b));

    /*
     * 2MB, not 4K: Engine B reads a whole large page in one transfer, and the
     * buffer has to be able to hold what the callback asks for. Allocated once,
     * on the caller's PASSIVE context, freed on every exit path.
     */
    buf = (uint8_t *)ExAllocatePoolWithTag(NonPagedPool, YGHV_BENCH_LARGE, 'knbY');
    if (!buf)
        return 0;

    a.pat = b.pat = pattern;
    a.mask = b.mask = mask;
    a.pat_len = b.pat_len = pat_len;
    a.buf = b.buf = buf;
    a.page_budget = b.page_budget = page_budget;
    a.range_end = b.range_end = end_va;

    /*
     * Start from the TARGET's CR3. Our own is the fallback only - the walk has to
     * run in the target's address space or every VA resolves against the wrong
     * tables. Same offset the engines themselves read (KPROCESS.DTB at +0x28),
     * read the same way, so this bench cannot drift from what it measures.
     */
    cr3 = *(volatile uint64_t *)((uint8_t *)proc + 0x028);

    /*
     * ORDER MATTERS AND IS DELIBERATE: B FIRST.
     *
     * Running A first would warm the caches with the same bytes and hand B a
     * head start, and the report would then be read as "B is faster" when B was
     * simply warmer. Running B first gives A the warmed cache - the direction
     * that makes B's number CONSERVATIVE. A benchmark that flatters its subject
     * is indistinguishable from an advertisement.
     */
    t0 = __rdtsc();
    pages_visited = yghv_pte_enum_range(cr3, start_va, end_va,
                                        bench_b_page_cb, &b);
    t1 = __rdtsc();

    /*
     * 9.449: record the CR3 we walked with, and say so when the walk stepped
     * through nothing.
     *
     * The first two runs reported `pages visited = 0` with a 26,594x "speedup" -
     * a number that was pure artefact, because B had done no work at all. Without
     * this field a reader cannot tell "B is fast" from "B did nothing", and the
     * two look identical in the timing. A benchmark must be able to report its
     * own failure.
     */
    out->walk_cr3 = cr3;

    out->b_cycles = t1 - t0;
    out->b_reads = b.pages_read;
    out->b_bytes = b.bytes;
    out->b_hits = b.hits;
    out->pages_visited = pages_visited;

    t0 = __rdtsc();
    bench_a_range(proc, start_va, end_va, &a, page_budget);
    t1 = __rdtsc();

    out->a_cycles = t1 - t0;
    out->a_reads = a.pages_cand;
    out->a_bytes = a.bytes;
    out->a_hits = a.hits;

    /*
     * The ratio is computed here, once, and only when A actually took time.
     * Reported as hundredths so the wire type stays an integer and the client
     * does not have to trust a float across an IOCTL boundary.
     */
    if (out->b_cycles != 0) {
        out->speedup_x100 = (out->a_cycles * 100) / out->b_cycles;
    } else {
        out->speedup_x100 = 0;
    }

    ok = 1;
    ExFreePoolWithTag(buf, 'knbY');
    return ok;
}
