/*
 * yghv_auth.h - 9.371 (Tier 9.2): authenticated control commands.
 *
 * THE QUESTION THIS ANSWERS
 *
 * The control device already decides WHO may open it: the owner process, the
 * owner CR3, and SeDebugPrivilege (control_device.c, P0 hardening). That is an
 * ACL. Tier 9.2 asks the next question - given a handle, may that caller
 * COMMAND us?
 *
 * An ACL cannot answer that, because a handle can be inherited, duplicated, or
 * obtained by a process that then gets compromised. A signature over the command
 * and its arguments answers it, because the key never leaves the caller's own
 * memory and never enters the kernel image.
 *
 * THREE PARTS, AND WHY EACH IS NEEDED
 *
 *   signature   HMAC-SHA256 over (sequence || command || arguments). Without it,
 *               a handle is authority. With it, the key is authority.
 *   sequence    a monotonic counter the driver enforces strictly increasing.
 *               Without it a captured command replays forever; the signature
 *               alone cannot tell one occurrence from another.
 *   key         injected by the loader, never stored in the image and never
 *               printed. An unauthenticated driver stays usable for bring-up;
 *               see "enforcement" below.
 *
 * WHAT THIS DOES NOT DO
 *
 * It does not encrypt: METHOD_BUFFERED contents are visible to anything that can
 * see the IRP. It does not prove the caller is benign, only that the caller
 * knows the key. It does not protect against a compromise that can read the
 * caller's key from the caller's own memory.
 */
#ifndef YGHV_AUTH_H
#define YGHV_AUTH_H

#include "svm_defs.h"
#include <ntddk.h>
#include "yghv_crypto.h"

#define YGHV_AUTH_KEY_MAX   64
#define YGHV_AUTH_TAG       YGHV_HMAC_TAG     /* 32 */

/* Enforcement levels, advanced deliberately rather than all at once. */
typedef enum {
    YGHV_AUTH_OFF       = 0,   /* bring-up: no key, no checks (default)      */
    YGHV_AUTH_MONITOR   = 1,   /* verify and COUNT failures, but allow       */
    YGHV_AUTH_ENFORCE   = 2,   /* verify and REFUSE on failure              */
    YGHV_AUTH_MAX       = 3
} yghv_auth_mode_t;

/*
 * Install the shared key. PASSIVE. `key_len` of 0 clears it.
 *
 * The key is copied into non-paged, page-exact storage (see yghv_auth.c) and is
 * never returned by any IOCTL - the only readout is a byte COUNT, so a caller can
 * see whether a key is installed without learning anything about it.
 */
int yghv_auth_set_key(const uint8_t *key, uint32_t key_len);

/* Advance / query enforcement. Going to ENFORCE with no key installed is
 * REFUSED: that would lock every command out, including the one that would turn
 * it back off. */
int  yghv_auth_set_mode(uint32_t mode);
UINT32 yghv_auth_get_mode(void);

/*
 * Verify one command. ISLAND-SAFE (no allocation, no locks, no blocking): this
 * sits on the IOCTL dispatch path, which can be entered at DISPATCH_LEVEL.
 *
 * `tag` is the 32 bytes the caller supplied. Returns:
 *   1  accepted (either mode OFF, or the tag verified)
 *   0  rejected (mode ENFORCE and the tag did not verify)
 *  -1  would be rejected (mode MONITOR and the tag did not verify) - caller
 *      proceeds, but the failure is counted
 */
int yghv_auth_check(uint64_t seq, uint32_t code,
                    const void *args, uint32_t args_len,
                    const uint8_t tag[YGHV_AUTH_TAG]);

/*
 * The exact bytes that get signed, exposed so a client can produce them without
 * re-deriving the layout and getting it subtly wrong (a client that signs a
 * different layout than the driver verifies is indistinguishable from a wrong
 * key, which is the worst possible error message).
 *
 * Layout, little-endian, fixed width so no field can shift:
 *   [0..7]   sequence number (u64)
 *   [8..11]  IOCTL function number (u32, the CTL_CODE Function field)
 *   [12..15] argument byte count (u32)
 *   [16..]   the argument bytes
 */
#define YGHV_AUTH_SIGNED_HDR 16
uint32_t yghv_auth_signed_len(uint32_t args_len);

/*
 * Produce the tag for a command WITHOUT verifying one. PASSIVE-safe and
 * island-safe (it only reads the key), but it is exposed to the control plane
 * because the client must not re-derive the layout - see the note on IOCTL op 4.
 */
void yghv_auth_sign(uint64_t seq, uint32_t code,
                    const void *args, uint32_t args_len,
                    uint8_t out[YGHV_AUTH_TAG]);

typedef struct {
    uint32_t mode;
    uint32_t key_len;          /* how many bytes of key are installed (0 = none) */
    uint64_t accepted;         /* commands whose tag verified */
    uint64_t rejected;         /* commands refused (ENFORCE) */
    uint64_t would_reject;     /* would have been refused (MONITOR) */
    uint64_t bad_seq;          /* sequence not strictly increasing */
    uint64_t no_tag;           /* command arrived with no tag at all */
    uint64_t last_seq;         /* highest sequence accepted */
    uint64_t selftest;         /* 1 = HMAC vectors match */
} yghv_auth_stats_t;

void yghv_auth_stats_get(yghv_auth_stats_t *out);
void yghv_auth_reset_stats(void);

#endif /* YGHV_AUTH_H */
