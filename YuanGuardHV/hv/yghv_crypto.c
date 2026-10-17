/*
 * yghv_crypto.c — SHA-256 for the integrity monitor. See common/yghv_crypto.h.
 *
 * Written out rather than linked in: the driver pulls no crypto library, and
 * every dependency is attack surface (Tier 7.6). The algorithm is fixed
 * arithmetic with published test vectors, which yghv_crypto_selftest() checks
 * at load.
 */

#include "yghv_crypto.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void yghv_sha256_block(yghv_sha256_ctx_t *ctx, const uint8_t *p) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void yghv_sha256_init(yghv_sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void yghv_sha256_update(yghv_sha256_ctx_t *ctx, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;

    ctx->bitlen += (uint64_t)len * 8;

    while (len > 0) {
        uint32_t take = YGHV_SHA256_BLOCK - ctx->buflen;
        if (take > len)
            take = len;
        RtlCopyMemory(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == YGHV_SHA256_BLOCK) {
            yghv_sha256_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void yghv_sha256_final(yghv_sha256_ctx_t *ctx, uint8_t out[YGHV_SHA256_DIGEST]) {
    uint64_t bits = ctx->bitlen;
    int i;

    /* Pad: 0x80, zeros, then the length as a big-endian 64-bit value. */
    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > YGHV_SHA256_BLOCK - 8) {
        while (ctx->buflen < YGHV_SHA256_BLOCK)
            ctx->buf[ctx->buflen++] = 0;
        yghv_sha256_block(ctx, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < YGHV_SHA256_BLOCK - 8)
        ctx->buf[ctx->buflen++] = 0;
    for (i = 0; i < 8; i++)
        ctx->buf[YGHV_SHA256_BLOCK - 8 + i] = (uint8_t)(bits >> (56 - i * 8));
    yghv_sha256_block(ctx, ctx->buf);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void yghv_sha256(const void *data, uint32_t len,
                 uint8_t out[YGHV_SHA256_DIGEST]) {
    yghv_sha256_ctx_t ctx;
    yghv_sha256_init(&ctx);
    yghv_sha256_update(&ctx, data, len);
    yghv_sha256_final(&ctx, out);
}

int yghv_sha256_equal(const uint8_t *a, const uint8_t *b) {
    uint8_t d = 0;
    int i;
    for (i = 0; i < YGHV_SHA256_DIGEST; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

int yghv_crypto_selftest(void) {
    /* Published vectors (FIPS 180-4 / NIST examples). */
    static const uint8_t want_empty[YGHV_SHA256_DIGEST] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    static const uint8_t want_abc[YGHV_SHA256_DIGEST] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    uint8_t got[YGHV_SHA256_DIGEST];

    yghv_sha256("", 0, got);
    if (!yghv_sha256_equal(got, want_empty))
        return 0;

    yghv_sha256("abc", 3, got);
    if (!yghv_sha256_equal(got, want_abc))
        return 0;

    /*
     * One multi-block case, to exercise the padding-into-a-new-block path: a
     * 56-byte string is the boundary where buflen > BLOCK-8 fires in
     * yghv_sha256_final, so it takes the extra-block branch.
     *
     * 9.343: the first version of this vector was mis-transcribed and the
     * self-test reported FAIL against a correct implementation. That is the
     * self-test doing its job — it is checked against the algorithm, not
     * against my typing — and it is why the digest is verified at load rather
     * than assumed. The value below is the published NIST vector, confirmed by
     * running the same code standalone.
     */
    {
        static const char *s =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        static const uint8_t want[YGHV_SHA256_DIGEST] = {
            0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
            0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1
        };
        yghv_sha256(s, 56, got);
        if (!yghv_sha256_equal(got, want))
            return 0;
    }

    return 1;
}

/*
 * 9.343: remember the self-test outcome from load.
 *
 * Running three SHA-256 passes on every status query is wasteful, and the answer
 * cannot change after load — the code is fixed. Cache it once.
 */
static volatile LONG g_crypto_ok = -1;   /* -1 = not yet run */

int yghv_crypto_selftest_cached(void) {
    LONG v = InterlockedCompareExchange(&g_crypto_ok, 0, 0);
    if (v >= 0)
        return (int)v;
    v = yghv_crypto_selftest() ? 1 : 0;
    InterlockedExchange(&g_crypto_ok, v);
    return (int)v;
}