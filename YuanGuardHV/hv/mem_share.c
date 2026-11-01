/*
 * mem_share.c — Tier 2.4: the zero-copy shared window.
 *
 * WHAT THIS IS, AND WHY IT IS THE SIMPLEST ONE IN THE WHOLE PLAN
 *
 * The plan describes Tier 2.4 as "one physical frame both sides operate on
 * directly - no copy, no syscall, no API". Every other tier needed new machinery.
 * This one needs NONE, because of a fact the tree already depends on: THE NPT IS
 * AN IDENTITY MAP.
 *
 *   GPA == SPA. A frame we allocate at physical address P is the SAME frame a
 *   guest reaches by touching address P.
 *
 * Measured before writing a line of this file (9.451): `npt-view 0xC6B6D000`
 * returned `guest present=1 perm=RWX`, i.e. a page this driver owns is directly
 * read/writable from the guest side with no mapping call at all.
 *
 * So the window is not a feature to build - it is a fact to name, pin, and
 * measure. What this file adds is the part that makes it USABLE:
 *
 *   - one page allocated and pinned, with its PA recorded
 *   - a handshake so a caller can tell "the guest wrote" from "the guest did not"
 *   - a sequence protocol that makes a single-slot exchange unambiguous
 *   - counters, because "zero copy" must be shown as zero copies performed
 *
 * THE PROTOCOL, AND WHY A SINGLE PAGE NEEDS ONE AT ALL
 *
 * A shared page has no natural synchronisation: the guest and the host are on
 * different CPUs with no common lock that the guest can take. The protocol here
 * is the smallest one that admits a correct reading:
 *
 *   byte 0   SEQUENCE   written LAST by the producer of a message
 *   byte 1   KIND       what the message is (caller-defined)
 *   byte 2   STATUS     host sets when it has consumed
 *   bytes 8..191   four qwords of argument
 *   bytes 192..511 one 320-byte payload area
 *
 * A consumer reads SEQ, reads the body, reads SEQ again; if SEQ changed, the
 * message was being written while it was read and the read is discarded. That is
 * a seqlock without the lock, which is the only form available when one side
 * cannot execute our code.
 *
 * WHY THE WINDOW IS NOT HIDDEN FROM THE GUEST
 *
 * Tier 7.1 hides pages from the guest. This page must NOT be hidden - its entire
 * purpose is that the guest can touch it. That is a real conflict between two
 * tiers and it is stated here rather than discovered later: the shared window is a
 * deliberate, single, guest-visible frame. `share-stats` reports its PA so it can
 * be audited against the 7.1 exclusion list.
 */

#include "mem_share.h"
#include "mem_pa.h"

/*
 * The page. Allocated once, at PASSIVE, and never freed until unload - the guest
 * may hold the address indefinitely and a freed frame would be aliased into
 * somebody else's allocation. That is the one lifetime this page has.
 */
static uint8_t  *g_share_va = NULL;
static uint64_t  g_share_pa = 0;

static volatile LONG   g_share_up = 0;
static volatile LONG64 g_share_host_writes = 0;
static volatile LONG64 g_share_host_reads = 0;
static volatile LONG64 g_share_seq_mismatch = 0;
static volatile LONG64 g_share_guest_writes_seen = 0;
static volatile LONG64 g_share_last_seq = 0;

/* ---- the wire layout, in one place so nothing else re-derives it --------- */
#define SH_OFF_SEQ     0u
#define SH_OFF_KIND    1u
#define SH_OFF_STATUS  2u
/*
 * 9.481: THE PAYLOAD LENGTH HAD NO FIELD, SO THE READER INVENTED ONE.
 *
 * The page carried SEQ/KIND/STATUS/ARGS/PAYLOAD and nothing else. `host_read`
 * then filled `*payload_len_out = max_payload` - the CALLER'S buffer size, not
 * the number of bytes the publisher actually wrote. A publisher that sent 10
 * bytes made the reader report "you have 4032 bytes of payload", i.e. the
 * consumer would go on to parse 4022 bytes of stale data from the previous
 * message as if they were part of this one.
 *
 * "Copy at most N bytes" and "here is how many there were" are two different
 * numbers and the page needs both. This slot is the second one. SH_MAX_PAYLOAD
 * is 4032, which does not fit in one byte, so the field is 16-bit and occupies
 * bytes 3..4 - still inside the reserved padding between STATUS and ARGS, so no
 * offset above it moves and an old reader/writer still finds KIND, STATUS, ARGS
 * and PAYLOAD exactly where they were.
 */
#define SH_OFF_PLEN    3u        /* uint16 payload length, published with SEQ */
#define SH_OFF_ARGS    8u        /* four qwords: 8..39   */
#define SH_OFF_PAYLOAD 64u       /* 448 bytes: 64..511   */

#define SH_MAX_PAYLOAD (4096u - SH_OFF_PAYLOAD)

/* Status values the host writes back. Distinct from KIND on purpose: a caller
 * should never have to guess whether a value in byte 2 is "what I asked" or
 * "what happened". */
#define SH_ST_IDLE   0u
#define SH_ST_TAKEN  1u
#define SH_ST_DONE   2u
#define SH_ST_ERROR  3u

/*
 * Map the window. PASSIVE, once. Idempotent.
 *
 * Nonpaged is mandatory, not a preference: the guest touches this frame from
 * arbitrary context, and a pageable frame would be swapped out from under it -
 * there is no way for the guest to take a page fault on our behalf.
 */
int yghv_share_init(void)
{
    if (InterlockedCompareExchange(&g_share_up, 0, 0) != 0)
        return 1;

    if (g_share_va == NULL) {
        g_share_va = (uint8_t *)ExAllocatePoolWithTag(NonPagedPool, 0x1000, 'Yshr');
        if (g_share_va == NULL)
            return 0;
    }

    RtlZeroMemory(g_share_va, 0x1000);

    g_share_pa = (uint64_t)MmGetPhysicalAddress(g_share_va).QuadPart;
    if (g_share_pa == 0)
        return 0;

    /*
     * The PA must be 4K aligned or the guest's address arithmetic (which uses the
     * PA as the GPA) would land mid-page in somebody else's frame. MmGetPhysical
     * of a NonPagedPool allocation is page aligned, but assert it: a silent
     * misalignment here corrupts a random frame instead of failing.
     */
    if ((g_share_pa & 0xFFF) != 0)
        return 0;

    InterlockedExchange(&g_share_up, 1);
    return 1;
}

int  yghv_share_up(void)      { return (int)InterlockedCompareExchange(&g_share_up, 0, 0); }
uint64_t yghv_share_pa(void)  { return g_share_pa; }

/*
 * Host -> guest. Write the body FIRST, then bump SEQ. A consumer that reads SEQ,
 * reads the body, and reads SEQ again will see a mismatch if we are mid-write,
 * which is the whole point of the ordering.
 */
int yghv_share_host_write(uint32_t kind, const uint64_t *args, uint32_t nargs,
                          const void *payload, uint32_t payload_len, uint8_t *out_seq)
{
    if (!yghv_share_up())
        return 0;
    if (nargs > 4)
        return 0;
    if (payload_len > SH_MAX_PAYLOAD)
        return 0;
    if (payload_len && !payload)
        return 0;

    if (args && nargs) {
        RtlCopyMemory(g_share_va + SH_OFF_ARGS, args, nargs * sizeof(uint64_t));
    }
    if (payload_len) {
        RtlCopyMemory(g_share_va + SH_OFF_PAYLOAD, payload, payload_len);
    }

    g_share_va[SH_OFF_KIND] = (uint8_t)kind;
    g_share_va[SH_OFF_STATUS] = SH_ST_IDLE;
    /* 9.481: publish the ACTUAL payload length, not the caller's capacity - see
     * the note on SH_OFF_PLEN. 16-bit because SH_MAX_PAYLOAD is 4032; clamped
     * anyway so a future larger window cannot silently wrap this field. */
    {
        uint16_t plen = (payload_len > 0xFFFFu) ? 0xFFFFu : (uint16_t)payload_len;

        g_share_va[SH_OFF_PLEN]     = (uint8_t)(plen & 0xFF);
        g_share_va[SH_OFF_PLEN + 1] = (uint8_t)(plen >> 8);
    }

    /*
     * 9.478: A COMPILER BARRIER BEFORE PUBLISHING SEQ.
     *
     * The protocol is "write the body, THEN bump SEQ". x86 does not reorder
     * store-to-store in hardware, so this looks safe - but the COMPILER may move
     * an ordinary store across another ordinary store, and every write on this
     * page is a plain byte store. The compiler could publish SEQ before the
     * payload copy lands, and the consumer would then read a NEW sequence number
     * with an OLD body: exactly the torn read the seqlock exists to prevent.
     *
     * `volatile` does not help: it orders volatile accesses against each other,
     * not the RtlCopyMemory above against the SEQ store below (a call is only a
     * compiler barrier until it is inlined, and it can be).
     *
     * _ReadWriteBarrier() is the right primitive: compiler-only, which is all x86
     * needs for store-store ordering, and it emits no instruction.
     */
    _ReadWriteBarrier();

    /*
     * SEQ last, and every bump is a real change (0 is skipped so a zeroed page
     * cannot look like a newly published message).
     */
    {
        uint8_t s = g_share_va[SH_OFF_SEQ];

        s = (uint8_t)(s + 1);
        if (s == 0)
            s = 1;
        g_share_va[SH_OFF_SEQ] = s;
        if (out_seq)
            *out_seq = s;
    }

    InterlockedIncrement64(&g_share_host_writes);
    InterlockedExchange64(&g_share_last_seq, (LONG64)g_share_va[SH_OFF_SEQ]);
    return 1;
}

/*
 * Guest -> host, read side. The seqlock read: seq, body, seq again.
 *
 * Returns 1 and fills the caller's buffers when a message was read cleanly.
 * Returns 0 when the page is down or SEQ changed under us (the caller can retry).
 *
 * `expect_seq` is what the caller last saw. When it equals the current SEQ there
 * is nothing NEW to read, and that is reported as 0 rather than as an error -
 * "the guest has not written since I last looked" is the normal state.
 */
int yghv_share_host_read(uint8_t *expect_seq, uint32_t *kind, uint64_t *args,
                         uint32_t max_args, void *payload, uint32_t max_payload,
                         uint32_t *payload_len_out)
{
    uint8_t s1, s2, k;
    uint32_t nargs;

    if (!yghv_share_up())
        return 0;

    s1 = g_share_va[SH_OFF_SEQ];
    if (expect_seq && s1 == *expect_seq)
        return 0;                       /* nothing new */

    k = g_share_va[SH_OFF_KIND];

    nargs = max_args > 4 ? 4 : max_args;
    if (args && nargs) {
        RtlCopyMemory(args, g_share_va + SH_OFF_ARGS, nargs * sizeof(uint64_t));
    }
    if (payload && max_payload) {
        RtlCopyMemory(payload, g_share_va + SH_OFF_PAYLOAD, max_payload);
    }
    /*
     * 9.481: REPORT THE PUBLISHED LENGTH, NOT `max_payload`.
     *
     * This used to be `*payload_len_out = max_payload`, i.e. it answered "how
     * much room did you give me" when the caller asked "how much did I get". A
     * 10-byte message made the consumer parse 4022 bytes of the PREVIOUS
     * message's leftovers as part of this one.
     *
     * The value is clamped to max_payload because the publisher may have written
     * more than this caller can hold - the copy above was already clipped, so
     * reporting the larger number would describe bytes the caller does not
     * have.
     */
    if (payload_len_out) {
        uint16_t plen = (uint16_t)(g_share_va[SH_OFF_PLEN] |
                                  ((uint16_t)g_share_va[SH_OFF_PLEN + 1] << 8));

        *payload_len_out = (plen > max_payload) ? max_payload : (uint32_t)plen;
    }

    /*
     * 9.478: THE SAME BARRIER ON THE READ SIDE, IN THE OTHER DIRECTION.
     *
     * The seqlock read is "SEQ, body, SEQ again". Without a compiler barrier the
     * compiler may HOIST the second SEQ read above the body copies - the two
     * reads then observe the same value almost always, and the retry-on-change
     * check becomes decoration while torn bodies pass through it.
     *
     * This barrier is what makes the second read actually happen after the copies
     * rather than wherever the optimiser preferred.
     */
    _ReadWriteBarrier();

    /*
     * The second read is the entire synchronisation. If it differs, the guest was
     * writing while we copied and the body we hold is a mix of two messages.
     */
    s2 = g_share_va[SH_OFF_SEQ];
    if (s2 != s1) {
        InterlockedIncrement64(&g_share_seq_mismatch);
        return 0;
    }

    g_share_va[SH_OFF_STATUS] = SH_ST_TAKEN;

    if (expect_seq)
        *expect_seq = s1;

    InterlockedIncrement64(&g_share_host_reads);
    InterlockedIncrement64(&g_share_guest_writes_seen);
    if (kind)
        *kind = k;
    return 1;
}

void yghv_share_stats_get(yghv_share_stats_t *out)
{
    if (!out)
        return;
    out->up = (uint64_t)InterlockedCompareExchange(&g_share_up, 0, 0);
    out->pa = g_share_pa;
    out->host_writes = (uint64_t)InterlockedCompareExchange64(&g_share_host_writes, 0, 0);
    out->host_reads = (uint64_t)InterlockedCompareExchange64(&g_share_host_reads, 0, 0);
    out->seq_mismatch = (uint64_t)InterlockedCompareExchange64(&g_share_seq_mismatch, 0, 0);
    out->guest_writes_seen = (uint64_t)InterlockedCompareExchange64(&g_share_guest_writes_seen, 0, 0);
    out->last_seq = (uint64_t)InterlockedCompareExchange64(&g_share_last_seq, 0, 0);
    out->page_size = 0x1000;
    out->payload_max = SH_MAX_PAYLOAD;
}

void yghv_share_stats_reset(void)
{
    InterlockedExchange64(&g_share_host_writes, 0);
    InterlockedExchange64(&g_share_host_reads, 0);
    InterlockedExchange64(&g_share_seq_mismatch, 0);
    InterlockedExchange64(&g_share_guest_writes_seen, 0);
}

/*
 * 9.487: THE SHARED PAGE IS RELEASED ON UNLOAD - it was not, and that is a leak
 * of one non-paged frame per load.
 *
 * `yghv_share_init` allocates the frame once (the `g_share_va == NULL` guard stops
 * a second allocation on a re-init), so a long-running session does not grow.
 * But nothing ever freed it: unload, reload, unload, reload leaked 4 KB of
 * NON-PAGED pool each cycle. Non-paged pool is the one resource a driver cannot
 * ask the OS to reclaim, and this driver also pins frames for protection - so a
 * slow leak here competes directly with the mechanism the project exists for.
 *
 * Order matters: drop `up` FIRST so no caller can enter `host_write`/`host_read`
 * between the free and the pointer clearing, then free, then clear both the
 * pointer and the recorded PA. A stale `g_share_pa` would let a caller still
 * believe there is a window at a frame that now belongs to somebody else.
 */
void yghv_share_down(void)
{
    InterlockedExchange(&g_share_up, 0);

    if (g_share_va) {
        ExFreePoolWithTag(g_share_va, 'Yshr');
        g_share_va = NULL;
    }
    g_share_pa = 0;
}
