/*
 * yghv_crypto.h — cryptographic hashing for the integrity monitor.
 *
 * Why not FNV-1a
 * -------------
 * Tier 7.3 first used FNV-1a over the object's bytes. That answers "did this
 * change by accident", which is not the question: a guest that wants to edit a
 * guarded table and stay undetected only has to make its edit hash-neutral, and
 * a 64-bit multiply-and-xor hash is trivial to steer. The hash has to be one
 * where finding a second preimage is hard even with full knowledge of the
 * algorithm and the current digest.
 *
 * SHA-256, implemented here rather than imported
 * ---------------------------------------------
 * The kernel image links no crypto library and this driver avoids pulling in
 * one (every dependency is attack surface, 7.6). SHA-256 is ~120 lines of
 * fixed arithmetic with published test vectors, so it is written out and
 * checked against them at build time.
 *
 * What this does and does not give
 * --------------------------------
 * It gives: a change cannot be hidden by arithmetic tinkering. An attacker who
 * controls the page content cannot find different bytes with the same digest.
 *
 * It does not give: protection against an attacker who can also rewrite the
 * STORED DIGEST. The digest lives in the same .bss the attacker is trying to
 * touch, and 9.320.1 showed that region cannot be hidden from a guest-mode
 * reader. So the digest is protected by DETECTION, not by secrecy: any edit to
 * the digest area itself shows up as a change in the digest's own page entry.
 */
#ifndef YGHV_CRYPTO_H
#define YGHV_CRYPTO_H

#include "svm_defs.h"
#include <ntddk.h>

#define YGHV_SHA256_DIGEST  32
#define YGHV_SHA256_BLOCK   64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;                 /* total bits processed */
    uint8_t  buf[YGHV_SHA256_BLOCK];
    uint32_t buflen;
} yghv_sha256_ctx_t;

void yghv_sha256_init(yghv_sha256_ctx_t *ctx);
void yghv_sha256_update(yghv_sha256_ctx_t *ctx, const void *data, uint32_t len);
void yghv_sha256_final(yghv_sha256_ctx_t *ctx, uint8_t out[YGHV_SHA256_DIGEST]);

/* One-shot over a buffer. */
void yghv_sha256(const void *data, uint32_t len,
                 uint8_t out[YGHV_SHA256_DIGEST]);

/* Constant-time-ish compare of two digests; returns 1 when equal. */
int yghv_sha256_equal(const uint8_t *a, const uint8_t *b);

/*
 * Self-test against the published vectors (empty string and "abc").
 * Returns 1 when both match. Called once at load — a hash that is silently
 * wrong is worse than no hash, because it reports "unchanged" forever.
 */
int yghv_crypto_selftest(void);
/* 9.343: same, but run once and cached (the answer cannot change). */
int yghv_crypto_selftest_cached(void);

/* ============================================================================
 * 9.371 (Tier 9.2): HMAC-SHA256, for authenticating control commands.
 * ==========================================================================*/

/*
 * Why HMAC and not "SHA256(key || message)"
 * ------------------------------------------
 * The naive concatenation is broken for SHA-256 when the key and the message are
 * both attacker-influenced and the hash can be length-extended: an attacker who
 * knows H(k||m) can compute H(k||m||padding||m2) without the key. HMAC closes
 * that by hashing the key twice, outside and inside, so the inner digest an
 * attacker would need to extend is never exposed.
 *
 * Why this exists at all
 * ----------------------
 * The device already restricts who can OPEN it (owner process, owner CR3, and
 * SeDebugPrivilege — control_device.c). Tier 9.2 asks the next question: given a
 * handle, can that caller COMMAND us? A signature over (sequence number ||
 * command || arguments) answers no, because the key never leaves the loader.
 *
 * Honest limit: this authenticates commands, it does not encrypt them. The IOCTL
 * buffer is METHOD_BUFFERED and its contents are visible to anyone who can see
 * the IRP. Encrypting the channel is a separate capability with a separate
 * failure mode; claiming this one covers it would be the same over-claim the
 * 35-VMEXIT sentence made.
 */
#define YGHV_HMAC_KEY_MAX  64
#define YGHV_HMAC_TAG      32

void yghv_hmac_sha256(const uint8_t *key, uint32_t key_len,
                      const void *msg, uint32_t msg_len,
                      uint8_t out[YGHV_HMAC_TAG]);

/*
 * Self-test against RFC 4231 test case 2:
 *   key  = "Jefe", data = "what do ya want for nothing?"
 *   tag  = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
 * A MAC that is silently wrong is worse than none: every command would verify.
 */
int yghv_hmac_selftest(void);
int yghv_hmac_selftest_cached(void);

#endif /* YGHV_CRYPTO_H */
