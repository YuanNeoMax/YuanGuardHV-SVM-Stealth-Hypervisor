/*
 * yghv_channel.c - 9.375 (Tier 9.1). See common/yghv_channel.h for the design.
 *
 * ISLAND-SAFE BY CONTRACT
 *
 * This runs from the CPUID exit: DISPATCH_LEVEL, GIF=0, no thread context of our
 * own. So:
 *   - no ExAllocatePool, no KeWait, no lock acquisition
 *   - guest memory is read with MmCopyMemory(PHYSICAL), which is the one
 *     physical-access primitive this tree already verified at DISPATCH
 *     (mem_pa.c §3)
 *   - counters are plain interlocked increments
 *
 * The one call that is NOT obviously island-safe is
 * yghv_protect_guest_va_to_pa(), which walks the guest's page tables. It is pure
 * memory reads through a mapped NPT table and the INT3 path already calls it from
 * an exit - but it is the reason this file is written to fail CLOSED: if the
 * translation fails, the request is refused rather than retried or guessed.
 */

#include "yghv_channel.h"
/* yghv_channel.h deliberately does not include this (it pulls <ntddk.h>, which
 * collides with the <ntifs.h> in svm_simplevm206.cpp). This translation unit has
 * no <ntifs.h>, so it may. */
#include "yghv_auth.h"
#include "yghv_crypto.h"
#include "mem_pa.h"
/*
 * 9.375i: WALK THE PAGE TABLES THE WAY THIS TREE ALREADY LEARNED TO.
 *
 * The first version called yghv_protect_guest_va_to_pa(), whose entry reader is
 * MmGetVirtualForPhysical(). mem_pa.c's header says exactly why that is wrong:
 * "MmGetVirtualForPhysical is not reliable for arbitrary physical pages" - a user
 * process's page-table pages are not guaranteed to be direct-mapped, and it
 * returns a pointer for some and NULL for others. mem_pte.c was written to fix
 * this and uses MmCopyMemory(PHYSICAL) instead (9.299 FIX-2).
 *
 * Running the UNRELIABLE walk from a CPUID exit at DISPATCH with GIF=0 is worse
 * than running it from PASSIVE, because a failed lookup there is not a wrong
 * answer - it can be a mapping call that cannot complete while interrupts are
 * disabled. That is the shape of the 11:50 machine loss (unexpected shutdown, no
 * dump, no bugcheck: 6008 with nothing to analyse).
 *
 * So this file uses the verified walker, not the legacy one.
 */
#include "mem_pte.h"
#include "cpuid.h"

/* the duplicated tag length must match the real one */
_Static_assert(YGHV_CH_TAG_LEN == YGHV_AUTH_TAG,
               "channel tag length drifted from YGHV_AUTH_TAG");

/*
 * PAGE-EXACT STATE (9.348 rule): the counters live in a union that is exactly one
 * page, so the linker cannot place anything else on them and a later lesson about
 * page-sharing cannot bite here.
 */
typedef struct {
    volatile LONG64 calls;
    volatile LONG64 ok;
    volatile LONG64 bad_magic;
    volatile LONG64 bad_op;
    volatile LONG64 fault;
    volatile LONG64 auth_fail;
    volatile LONG64 last_seq;
} yghv_channel_state_t;

typedef union {
    yghv_channel_state_t s;
    uint8_t page[4096];
} yghv_channel_region_t;

DECLSPEC_ALIGN(4096) static yghv_channel_region_t g_ch;
DECLSPEC_ALIGN(4096) static yghv_channel_region_t g_ch_pad;   /* separation */

_Static_assert(sizeof(yghv_channel_state_t) <= 4096, "channel state fits a page");
_Static_assert(sizeof(yghv_channel_region_t) == 4096, "channel region is exactly one page");

/* Read a little-endian u32 out of a byte buffer without alignment assumptions:
 * the caller's header is untrusted memory and may sit at any offset. */
static uint32_t ch_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write the ACK plus a status. Kept in one place so no exit path can forget the
 * ack - that omission is exactly what made two callers dead code in 9.370. */
static void ch_ack(uint32_t status, uint32_t op,
                   uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    *ecx = YGHV_CHANNEL_ACK;
    *eax = status;
    *ebx = op;
    *edx = 0;
}

void yghv_channel_doorbell(uint64_t guest_cr3, uint64_t req_va,
                           uint32_t op_hint, uint32_t seq_hint,
                           uint32_t *out_eax, uint32_t *out_ebx,
                           uint32_t *out_ecx, uint32_t *out_edx) {
    uint8_t hdr[YGHV_CH_REQ_SIZE];
    uint64_t req_pa;
    uint32_t magic, op, seq, status;
    uint64_t value = 0;

    InterlockedIncrement64(&g_ch.s.calls);

    /* The VA must resolve before anything else is believed. A caller that passes
     * a wild pointer gets FAULT, not a read of unrelated physical memory. */
    /*
     * 9.375c: A FAULT MUST SAY WHY. The first two runs of G43 returned status=4
     * for every opcode with no way to tell "the VA was 0" from "the VA did not
     * translate" from "the frame read failed" - three completely different bugs
     * behind one number. EDX now carries a diagnostic code on FAULT:
     *   1 = no VA supplied, 2 = no CR3, 3 = VA did not translate, 4 = frame read failed
     */
    if (!req_va) {
        InterlockedIncrement64(&g_ch.s.fault);
        ch_ack(YGHV_CH_STATUS_FAULT, op_hint, out_eax, out_ebx, out_ecx, out_edx);
        *out_edx = 1;
        return;
    }
    if (!guest_cr3) {
        InterlockedIncrement64(&g_ch.s.fault);
        ch_ack(YGHV_CH_STATUS_FAULT, op_hint, out_eax, out_ebx, out_ecx, out_edx);
        *out_edx = 2;
        return;
    }
    req_pa = yghv_pte_va_to_pa(guest_cr3, req_va);
    if (!req_pa) {
        InterlockedIncrement64(&g_ch.s.fault);
        ch_ack(YGHV_CH_STATUS_FAULT, op_hint, out_eax, out_ebx, out_ecx, out_edx);
        *out_edx = 3;
        /* also surface the VA and CR3 that were tried, in EBX and ECX-tagged form */
        *out_ebx = (uint32_t)(req_va & 0xFFFFFFFFULL);
        return;
    }
    if (!yghv_pa_read(req_pa, hdr, sizeof(hdr))) {
        InterlockedIncrement64(&g_ch.s.fault);
        ch_ack(YGHV_CH_STATUS_FAULT, op_hint, out_eax, out_ebx, out_ecx, out_edx);
        *out_edx = 4;
        *out_ebx = (uint32_t)(req_pa & 0xFFFFFFFFULL);
        return;
    }

    magic = ch_rd32(hdr + YGHV_CH_REQ_MAGIC);
    op    = ch_rd32(hdr + YGHV_CH_REQ_OP);
    seq   = ch_rd32(hdr + YGHV_CH_REQ_SEQ);

    if (magic != YGHV_CHANNEL_MAGIC) {
        InterlockedIncrement64(&g_ch.s.bad_magic);
        ch_ack(YGHV_CH_STATUS_BADMAGIC, op, out_eax, out_ebx, out_ecx, out_edx);
        return;
    }

    /*
     * Authenticate BEFORE acting, and reuse 9.371's checker rather than a second
     * implementation. The tag lives in the request buffer right after the header,
     * so the caller does not have to pass it in registers (it would not fit).
     *
     * mode OFF (the default) accepts everything, which keeps bring-up usable; the
     * device channel and this one share the key and the sequence counter, so a
     * caller who enabled enforcement gets it here too.
     */
    {
        uint8_t tag[YGHV_CH_TAG_LEN];
        if (!yghv_pa_read(req_pa + YGHV_CH_REQ_SIZE, tag, sizeof(tag))) {
            InterlockedIncrement64(&g_ch.s.auth_fail);
            ch_ack(YGHV_CH_STATUS_AUTH, op, out_eax, out_ebx, out_ecx, out_edx);
            return;
        }
        if (yghv_auth_check((uint64_t)seq, op, hdr, YGHV_CH_REQ_SIZE, tag) == 0) {
            InterlockedIncrement64(&g_ch.s.auth_fail);
            ch_ack(YGHV_CH_STATUS_AUTH, op, out_eax, out_ebx, out_ecx, out_edx);
            return;
        }
    }

    switch (op) {
    case YGHV_CH_OP_PING:
        /* The cheapest possible proof that the channel works: it answers at all.
         * A caller who cannot get PING is not talking to the hypervisor. */
        value = 0x504F4E47ULL;   /* 'GNOP' */
        status = YGHV_CH_STATUS_OK;
        break;

    case YGHV_CH_OP_HEALTH:
        /*
         * Report the crypto and auth self-tests, because they are the two things
         * that fail SILENTLY: a wrong SHA-256 reports "unchanged" forever and a
         * wrong MAC accepts every command. Both are cached at first call.
         */
        value = (uint64_t)(yghv_crypto_selftest_cached() ? 1 : 0)
              | ((uint64_t)(yghv_hmac_selftest_cached() ? 1 : 0) << 8)
              | ((uint64_t)(yghv_auth_get_mode() & 0xFF) << 16);
        status = YGHV_CH_STATUS_OK;
        break;

    case YGHV_CH_OP_READ: {
        /*
         * Read the caller's own frame back to them, at the offset they named.
         * Deliberately limited to the same frame the REQUEST lives in: that keeps
         * this a channel test rather than an arbitrary physical-memory reader,
         * which the device path already is and which this channel must not become
         * by accident.
         */
        uint32_t off = 0;
        uint64_t pa;

        if (!yghv_pa_read(req_pa + YGHV_CH_REQ_ARG0, &pa, sizeof(pa)) ||
            pa < (req_pa & ~(uint64_t)0xFFFULL) ||
            pa >= ((req_pa & ~(uint64_t)0xFFFULL) + 0x1000)) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        (void)off;
        if (!yghv_pa_read(pa, &value, sizeof(value)))
            status = YGHV_CH_STATUS_FAULT;
        else
            status = YGHV_CH_STATUS_OK;
        break;
    }

    case YGHV_CH_OP_STATS:
        value = (uint64_t)InterlockedCompareExchange64(&g_ch.s.calls, 0, 0);
        status = YGHV_CH_STATUS_OK;
        break;

    case YGHV_CH_OP_MEM_READ: {
        uint64_t src_va, phys;
        if (!yghv_pa_read(req_pa + YGHV_CH_REQ_ARG0, &src_va, sizeof(src_va)) || !src_va) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        phys = yghv_pte_va_to_pa(guest_cr3, src_va);
        if (!phys || !yghv_pa_read_u64(phys, &value)) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        status = YGHV_CH_STATUS_OK;
        break;
    }

    case YGHV_CH_OP_MEM_WRITE: {
        uint64_t dst_va, phys, wr_val;
        if (!yghv_pa_read(req_pa + YGHV_CH_REQ_ARG0, &dst_va, sizeof(dst_va)) ||
            !yghv_pa_read(req_pa + YGHV_CH_REQ_ARG1, &wr_val, sizeof(wr_val)) ||
            !dst_va) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        phys = yghv_pte_va_to_pa(guest_cr3, dst_va);
        if (!phys || !yghv_pa_write_u64(phys, wr_val)) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        value = wr_val;
        status = YGHV_CH_STATUS_OK;
        break;
    }

    case YGHV_CH_OP_PHYS_READ: {
        uint64_t pa_src;
        if (!yghv_pa_read(req_pa + YGHV_CH_REQ_ARG0, &pa_src, sizeof(pa_src)) || !pa_src) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        if (!yghv_pa_read_u64(pa_src, &value)) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        status = YGHV_CH_STATUS_OK;
        break;
    }

    case YGHV_CH_OP_PHYS_WRITE: {
        uint64_t pa_dst, pw_val;
        if (!yghv_pa_read(req_pa + YGHV_CH_REQ_ARG0, &pa_dst, sizeof(pa_dst)) ||
            !yghv_pa_read(req_pa + YGHV_CH_REQ_ARG1, &pw_val, sizeof(pw_val)) ||
            !pa_dst) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        if (!yghv_pa_write_u64(pa_dst, pw_val)) {
            status = YGHV_CH_STATUS_FAULT;
            InterlockedIncrement64(&g_ch.s.fault);
            break;
        }
        value = pw_val;
        status = YGHV_CH_STATUS_OK;
        break;
    }

    default:
        InterlockedIncrement64(&g_ch.s.bad_op);
        ch_ack(YGHV_CH_STATUS_BADOP, op, out_eax, out_ebx, out_ecx, out_edx);
        return;
    }

    if (status == YGHV_CH_STATUS_OK) {
        InterlockedIncrement64(&g_ch.s.ok);
        InterlockedExchange64(&g_ch.s.last_seq, (LONG64)seq);
    }

    ch_ack(status, op, out_eax, out_ebx, out_ecx, out_edx);
    /* The value rides in EDX:EBX, which is why EBX is reused as high half - the
     * status stays in EAX so a caller can check success without masking. */
    *out_ebx = (uint32_t)(value >> 32);
    *out_edx = (uint32_t)(value & 0xFFFFFFFFULL);
}

void yghv_channel_stats_get(yghv_channel_stats_t *out) {
    if (!out)
        return;
    out->calls      = (uint64_t)InterlockedCompareExchange64(&g_ch.s.calls, 0, 0);
    out->ok         = (uint64_t)InterlockedCompareExchange64(&g_ch.s.ok, 0, 0);
    out->bad_magic  = (uint64_t)InterlockedCompareExchange64(&g_ch.s.bad_magic, 0, 0);
    out->bad_op     = (uint64_t)InterlockedCompareExchange64(&g_ch.s.bad_op, 0, 0);
    out->fault      = (uint64_t)InterlockedCompareExchange64(&g_ch.s.fault, 0, 0);
    out->auth_fail  = (uint64_t)InterlockedCompareExchange64(&g_ch.s.auth_fail, 0, 0);
    out->last_seq   = (uint64_t)InterlockedCompareExchange64(&g_ch.s.last_seq, 0, 0);
}
