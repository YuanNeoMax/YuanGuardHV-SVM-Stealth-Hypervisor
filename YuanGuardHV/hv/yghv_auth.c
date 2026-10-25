/*
 * yghv_auth.c - 9.371 (Tier 9.2). See common/yghv_auth.h for what this does and
 * does not claim.
 *
 * PAGE-EXACT BY CONSTRUCTION
 *
 * The key lives in a union that is exactly one page, so the linker cannot place
 * anything else on it (9.348 / 9.366 §28.1: DECLSPEC_ALIGN only aligns the start
 * and says nothing about the size). This matters more here than anywhere else in
 * the tree: a key page shared with a cold counter is a page whose hiding removes
 * the counter, and a key page whose tail carries a neighbour is a key that a
 * later heap-growth change could expose.
 */

#include "yghv_auth.h"

#define YGHV_AUTH_KEY_LEN_MAX YGHV_AUTH_KEY_MAX

typedef struct {
    uint8_t  key[YGHV_AUTH_KEY_LEN_MAX];
    uint32_t key_len;
    /* The sequence counter lives WITH the key: replay protection and the key are
     * one security decision, and separating them onto different pages would mean
     * a page sharing rule could split them. */
    uint64_t last_seq;
    uint32_t _pad;
} yghv_auth_page_t;

typedef union {
    yghv_auth_page_t s;
    uint8_t          page[4096];
} yghv_auth_region_t;

DECLSPEC_ALIGN(4096) static yghv_auth_region_t g_auth;
DECLSPEC_ALIGN(4096) static yghv_auth_region_t g_auth_pad;   /* separation */

_Static_assert(sizeof(yghv_auth_page_t) <= 4096, "auth state fits a page");
_Static_assert(sizeof(yghv_auth_region_t) == 4096, "auth region is exactly one page");

#define g_key     (g_auth.s.key)
#define g_keylen  (g_auth.s.key_len)
#define g_lastseq (g_auth.s.last_seq)

static volatile LONG   g_auth_mode;
static volatile LONG64 g_accepted;
static volatile LONG64 g_rejected;
static volatile LONG64 g_would;
static volatile LONG64 g_badseq;
static volatile LONG64 g_notag;

/* ============================================================================
 * key installation
 * ==========================================================================*/

int yghv_auth_set_key(const uint8_t *key, uint32_t key_len) {
    uint32_t i;

    if (KeAreAllApcsDisabled())
        return 0;                        /* PASSIVE only: the caller is the loader */

    if (key_len > YGHV_AUTH_KEY_LEN_MAX)
        return 0;
    if (key_len && !key)
        return 0;

    /* Zero the whole key field before writing, so a shorter key cannot leave a
     * longer one's tail behind - that tail would silently extend the MAC key. */
    for (i = 0; i < YGHV_AUTH_KEY_LEN_MAX; i++)
        g_key[i] = 0;
    for (i = 0; i < key_len; i++)
        g_key[i] = key[i];
    g_keylen = key_len;

    /* A NEW KEY RESTARTS THE SEQUENCE. Otherwise a client that re-keyed could be
     * locked out by a counter it never issued, and a client that kept its counter
     * across a re-key would be honoured for commands signed under a different
     * key epoch. Restarting makes the re-key a clean boundary. */
    g_lastseq = 0;

    return 1;
}

int yghv_auth_set_mode(uint32_t mode) {
    if (mode >= YGHV_AUTH_MAX)
        return 0;
    /* 9.371: ENFORCE with no key would refuse every command, including the one
     * that turns enforcement off. Refusing here is the only way out. */
    if (mode == YGHV_AUTH_ENFORCE && g_keylen == 0)
        return 0;
    InterlockedExchange(&g_auth_mode, (LONG)mode);
    return 1;
}

UINT32 yghv_auth_get_mode(void) {
    return (UINT32)InterlockedCompareExchange(&g_auth_mode, 0, 0);
}

/* ============================================================================
 * verification
 * ==========================================================================*/

uint32_t yghv_auth_signed_len(uint32_t args_len) {
    return YGHV_AUTH_SIGNED_HDR + args_len;
}

/*
 * Build the signed byte string. ISLAND-SAFE: fixed header into a small stack
 * buffer, then the caller's own argument bytes are hashed in place - the header
 * is the only thing copied, and it is YGHV_AUTH_SIGNED_HDR bytes.
 *
 * Why the caller's buffer is hashed directly rather than copied: args can be up
 * to the IOCTL buffer size, and this runs at DISPATCH on a path that must not
 * allocate. Hashing in place is both legal and cheaper.
 */
static void yghv_auth_tag(uint64_t seq, uint32_t code,
                          const void *args, uint32_t args_len,
                          uint8_t out[YGHV_AUTH_TAG]) {
    uint8_t hdr[YGHV_AUTH_SIGNED_HDR];
    yghv_sha256_ctx_t ctx;
    uint8_t kpad[YGHV_SHA256_BLOCK];
    uint8_t ipad[YGHV_SHA256_BLOCK];
    uint8_t inner[YGHV_SHA256_DIGEST];
    uint32_t i;

    hdr[0]  = (uint8_t)(seq);
    hdr[1]  = (uint8_t)(seq >> 8);
    hdr[2]  = (uint8_t)(seq >> 16);
    hdr[3]  = (uint8_t)(seq >> 24);
    hdr[4]  = (uint8_t)(seq >> 32);
    hdr[5]  = (uint8_t)(seq >> 40);
    hdr[6]  = (uint8_t)(seq >> 48);
    hdr[7]  = (uint8_t)(seq >> 56);
    hdr[8]  = (uint8_t)(code);
    hdr[9]  = (uint8_t)(code >> 8);
    hdr[10] = (uint8_t)(code >> 16);
    hdr[11] = (uint8_t)(code >> 24);
    hdr[12] = (uint8_t)(args_len);
    hdr[13] = (uint8_t)(args_len >> 8);
    hdr[14] = (uint8_t)(args_len >> 16);
    hdr[15] = (uint8_t)(args_len >> 24);

    /* Inline HMAC over (hdr || args), so the caller's buffer is never copied. */
    for (i = 0; i < YGHV_SHA256_BLOCK; i++)
        kpad[i] = 0;
    for (i = 0; i < g_keylen && i < YGHV_SHA256_BLOCK; i++)
        kpad[i] = g_key[i];

    for (i = 0; i < YGHV_SHA256_BLOCK; i++)
        ipad[i] = (uint8_t)(kpad[i] ^ 0x36);
    yghv_sha256_init(&ctx);
    yghv_sha256_update(&ctx, ipad, YGHV_SHA256_BLOCK);
    yghv_sha256_update(&ctx, hdr, YGHV_AUTH_SIGNED_HDR);
    if (args && args_len)
        yghv_sha256_update(&ctx, args, args_len);
    yghv_sha256_final(&ctx, inner);

    for (i = 0; i < YGHV_SHA256_BLOCK; i++)
        kpad[i] = (uint8_t)(kpad[i] ^ 0x36 ^ 0x5c);   /* ipad -> opad */
    yghv_sha256_init(&ctx);
    yghv_sha256_update(&ctx, kpad, YGHV_SHA256_BLOCK);
    yghv_sha256_update(&ctx, inner, YGHV_SHA256_DIGEST);
    yghv_sha256_final(&ctx, out);

    for (i = 0; i < YGHV_SHA256_BLOCK; i++) {
        kpad[i] = 0;
        ipad[i] = 0;
    }
}

/* 9.371: the same tag computation, exported for IOCTL op 4. */
void yghv_auth_sign(uint64_t seq, uint32_t code,
                    const void *args, uint32_t args_len,
                    uint8_t out[YGHV_AUTH_TAG]) {
    yghv_auth_tag(seq, code, args, args_len, out);
}

int yghv_auth_check(uint64_t seq, uint32_t code,
                    const void *args, uint32_t args_len,
                    const uint8_t tag[YGHV_AUTH_TAG]) {
    UINT32 mode = yghv_auth_get_mode();
    uint8_t want[YGHV_AUTH_TAG];

    if (mode == YGHV_AUTH_OFF)
        return 1;

    if (!tag) {
        InterlockedIncrement64(&g_notag);
        InterlockedIncrement64(&g_would);
        return (mode == YGHV_AUTH_ENFORCE) ? 0 : -1;
    }

    /* The sequence must be STRICTLY greater than the last accepted one. Equal is
     * a replay, smaller is a replay or a stale client; both are refused. This is
     * checked before the MAC because it is cheap and because a replayed command
     * carries a valid MAC and would otherwise pass. */
    if (seq <= g_lastseq && g_lastseq != 0) {
        InterlockedIncrement64(&g_badseq);
        InterlockedIncrement64(&g_would);
        return (mode == YGHV_AUTH_ENFORCE) ? 0 : -1;
    }

    yghv_auth_tag(seq, code, args, args_len, want);

    /* Constant-time compare: a fast-exit compare leaks how many leading bytes an
     * attacker guessed correctly, which is enough to forge a tag byte by byte. */
    {
        uint8_t diff = 0;
        uint32_t i;
        for (i = 0; i < YGHV_AUTH_TAG; i++)
            diff |= (uint8_t)(want[i] ^ tag[i]);
        if (diff != 0) {
            InterlockedIncrement64(&g_would);
            if (mode == YGHV_AUTH_ENFORCE) {
                InterlockedIncrement64(&g_rejected);
                return 0;
            }
            return -1;
        }
    }

    /* Accepted: advance the counter only now, so a failed command cannot consume
     * a sequence number and desynchronise the client. */
    g_lastseq = seq;
    InterlockedIncrement64(&g_accepted);
    return 1;
}

/* ============================================================================
 * stats
 * ==========================================================================*/

void yghv_auth_stats_get(yghv_auth_stats_t *out) {
    if (!out)
        return;
    out->mode         = yghv_auth_get_mode();
    out->key_len      = g_keylen;
    out->accepted     = (uint64_t)InterlockedCompareExchange64(&g_accepted, 0, 0);
    out->rejected     = (uint64_t)InterlockedCompareExchange64(&g_rejected, 0, 0);
    out->would_reject = (uint64_t)InterlockedCompareExchange64(&g_would, 0, 0);
    out->bad_seq      = (uint64_t)InterlockedCompareExchange64(&g_badseq, 0, 0);
    out->no_tag       = (uint64_t)InterlockedCompareExchange64(&g_notag, 0, 0);
    out->last_seq     = g_lastseq;
    out->selftest     = (uint64_t)yghv_hmac_selftest_cached();
}

void yghv_auth_reset_stats(void) {
    InterlockedExchange64(&g_accepted, 0);
    InterlockedExchange64(&g_rejected, 0);
    InterlockedExchange64(&g_would, 0);
    InterlockedExchange64(&g_badseq, 0);
    InterlockedExchange64(&g_notag, 0);
}
