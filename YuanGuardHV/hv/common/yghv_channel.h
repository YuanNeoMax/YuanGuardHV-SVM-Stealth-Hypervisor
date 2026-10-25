/*
 * yghv_channel.h - 9.375 (Tier 9.1): a control channel with NO DEVICE OBJECT.
 *
 * THE PROBLEM
 *
 * Today every command needs a handle to \\.\YuanGuardHV. That handle is:
 *   - discoverable   (the name is in the object manager and in symlinks)
 *   - enumerable     (a driver list shows us)
 *   - auditable      (an IOCTL is an IRP, and IRPs can be traced)
 * Tier 9.1 asks for a channel where none of those three are true: an operator
 * with no handle can still command the hypervisor.
 *
 * WHY CPUID AND WHAT IT COSTS
 *
 * CPUID is a user-mode instruction that already traps to us (INTERCEPT_CPUID is
 * one of the five armed groups). So a guest can call it from any ring and we get
 * a VMEXIT without any object existing. Four 32-bit registers is not much
 * bandwidth, so the leaf is a DOORBELL, not a mailbox:
 *
 *   caller puts a request in its OWN memory, passes the guest VA in registers,
 *   the driver resolves that VA to a physical frame and reads it directly.
 *
 * That is the whole trick: the buffer never has to be shared up front, because
 * the hypervisor can already read any guest VA (yghv_protect_guest_va_to_pa is
 * the same function the INT3 path uses). Nothing is allocated at load, nothing
 * appears in the object manager.
 *
 * WHY THIS IS SEPARATE FROM THE AUTH LAYER
 *
 * 9.371 added a key and a monotonic sequence to the DEVICE channel. This channel
 * needs the same two things for the same reasons - a doorbell that anyone can
 * ring is worse than a handle, not better - so it reuses yghv_auth_check()
 * rather than reimplementing it. Enforcement is per-channel, because a driver
 * that is too strict here would be unusable for bring-up while the device path
 * still works.
 */

#ifndef YGHV_CHANNEL_H
#define YGHV_CHANNEL_H

/*
 * NO KERNEL INCLUDES HERE ON PURPOSE.
 *
 * svm_simplevm206.cpp includes <ntifs.h>, and pulling <ntddk.h> in after it
 * redefines PEPROCESS/PETHREAD (measured: C2371 twice). This header only needs
 * fixed-width integers and the auth tag length, so it takes neither.
 */
#include "svm_defs.h"

/* 32 = YGHV_AUTH_TAG / YGHV_HMAC_TAG. Repeated as a literal rather than included,
 * because yghv_auth.h pulls <ntddk.h> and that collides with the <ntifs.h> the
 * including translation unit already has (C2371 on PEPROCESS). A static assert in
 * yghv_channel.c pins the two together, so the duplication cannot drift. */
#define YGHV_CH_TAG_LEN 32

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The doorbell leaf. Chosen in the same 0x4141414x block as CPUID_UNLOAD_SIMPLE_SVM
 * (41) and CPUID_FAKE_VPD (42) - this tree's private range - so it cannot collide
 * with a real Intel or AMD leaf. 0x41414143 = 'CBA A' little-endian.
 */
#define YGHV_CPUID_CHANNEL  0x41414143

/*
 * 9.375g: A CPUID LEAF CANNOT CARRY A 64-BIT POINTER, SO THE ADDRESS IS SPLIT.
 *
 * CPUID's only inputs are EAX (leaf) and ECX (sub-leaf). EBX and EDX are written
 * by the hardware, so anything a caller leaves in them is gone before our handler
 * runs. Four attempts to pass the request VA in EBX/EDX failed for exactly this
 * reason (measured every time as status=4, EDX=1 "no VA supplied", with a stub
 * that was by then correct).
 *
 * That leaves 64 bits between two registers, and one of them has to identify the
 * leaf. The split used here:
 *
 *   EAX = YGHV_CH_BASE | ((va >> 32) & 0xFFFF)    high 16 bits of the VA
 *   ECX = va & 0xFFFFFFFF                          low 32 bits of the VA
 *
 * A canonical x64 user VA is 47 bits, so 16 + 32 = 48 covers it with room. The
 * leaf is matched by MASK, not equality, which is why this costs one comparison
 * and no extra state: no registration table, no shared page, nothing that would
 * have to exist before the device is removed.
 */
#define YGHV_CH_BASE_MASK   0xFFFF0000u
#define YGHV_CH_BASE        0x41410000u

/* Build the two register values for a given request VA. Exposed so the client
 * cannot get the split wrong - a client that splits differently looks exactly like
 * a caller passing a wild pointer. */
static unsigned int yghv_ch_leaf(unsigned long long va) {
    return YGHV_CH_BASE | (unsigned int)((va >> 32) & 0xFFFFu);
}
static unsigned int yghv_ch_sub(unsigned long long va) {
    return (unsigned int)(va & 0xFFFFFFFFu);
}

/*
 * The ack. Same lesson as 9.370: a caller cannot distinguish "the driver answered"
 * from "the hardware answered an unimplemented leaf" unless the driver writes
 * something the hardware would not. ECX carries it, and the value is chosen so a
 * default CPUID result (which zeroes or returns feature bits) cannot produce it.
 */
#define YGHV_CHANNEL_ACK    0x43484759   /* 'Y G H C' */

/* Leaf magic in the request header, so a stray pointer cannot be mistaken for a
 * request: the first word must match exactly. */
#define YGHV_CHANNEL_MAGIC  0x59474856u  /* 'V H G Y' */

/* Opcodes. Kept deliberately small; each one is a capability the device channel
 * already has, so this channel can be verified by comparison rather than by trust. */
#define YGHV_CH_OP_PING     0
#define YGHV_CH_OP_HEALTH   1
#define YGHV_CH_OP_READ     2   /* read the caller's own memory back (sanity) */
#define YGHV_CH_OP_STATS    3   /* hypervisor counters, no side effects */

#define YGHV_CH_REQ_MAGIC   0
#define YGHV_CH_REQ_OP      4
#define YGHV_CH_REQ_SEQ     8
#define YGHV_CH_REQ_ARG0    16
#define YGHV_CH_REQ_ARG1    24
#define YGHV_CH_REQ_SIZE    32

#define YGHV_CH_RSP_MAGIC   0
#define YGHV_CH_RSP_OP      4
#define YGHV_CH_RSP_STATUS  8
#define YGHV_CH_RSP_VALUE   16
#define YGHV_CH_RSP_SIZE    32

#define YGHV_CH_STATUS_OK       0
#define YGHV_CH_STATUS_BADMAGIC 1
#define YGHV_CH_STATUS_BADOP    2
#define YGHV_CH_STATUS_AUTH     3
#define YGHV_CH_STATUS_FAULT    4   /* the caller's VA did not resolve */

/*
 * Handle one doorbell. Called from the CPUID exit path, which runs at DISPATCH
 * with GIF=0, so this is ISLAND-SAFE by contract: no allocation, no locks, no
 * blocking call. `guest_cr3` is the caller's address space, used only to resolve
 * the request VA to a frame.
 *
 * On return, *out_lo / *out_hi are the two 32-bit registers the CPUID exit will
 * hand back. They always carry the ACK, so "no answer" is never ambiguous.
 */
void yghv_channel_doorbell(uint64_t guest_cr3, uint64_t req_va,
                           uint32_t op_hint, uint32_t seq_hint,
                           uint32_t *out_eax, uint32_t *out_ebx,
                           uint32_t *out_ecx, uint32_t *out_edx);

typedef struct {
    uint64_t calls;        /* doorbells answered */
    uint64_t ok;
    uint64_t bad_magic;
    uint64_t bad_op;
    uint64_t fault;        /* VA did not resolve to guest memory */
    uint64_t auth_fail;
    uint64_t last_seq;
} yghv_channel_stats_t;

void yghv_channel_stats_get(yghv_channel_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* YGHV_CHANNEL_H */
