/*
 * mem_share.h — Tier 2.4: the zero-copy shared window.
 *
 * See mem_share.c for the design, the protocol, and the one conflict with Tier
 * 7.1 that is stated up front rather than discovered later.
 *
 * Short version: the NPT is an identity map, so a frame we allocate is the SAME
 * frame the guest reaches by touching its physical address. There is nothing to
 * copy and nothing to map - what is missing is only a handshake and a way to
 * measure that the exchange really happened.
 */
#ifndef YGHV_MEM_SHARE_H
#define YGHV_MEM_SHARE_H

#include "svm_defs.h"
#include <ntddk.h>

/* Wire layout offsets, mirrored in the client. */
#define YGHV_SHARE_OFF_SEQ      0u
#define YGHV_SHARE_OFF_KIND     1u
#define YGHV_SHARE_OFF_STATUS   2u
#define YGHV_SHARE_OFF_ARGS     8u
#define YGHV_SHARE_OFF_PAYLOAD  64u

#define YGHV_SHARE_ST_IDLE   0u
#define YGHV_SHARE_ST_TAKEN  1u
#define YGHV_SHARE_ST_DONE   2u
#define YGHV_SHARE_ST_ERROR  3u

typedef struct {
    uint64_t up;                 /* 1 = window allocated and pinned          */
    uint64_t pa;                 /* the physical address the GUEST must use   */
    uint64_t host_writes;        /* messages published by the host            */
    uint64_t host_reads;         /* guest messages consumed by the host       */
    uint64_t seq_mismatch;       /* reads discarded because SEQ moved (seqlock) */
    uint64_t guest_writes_seen;  /* distinct guest writes observed            */
    uint64_t last_seq;
    uint64_t page_size;
    uint64_t payload_max;
} yghv_share_stats_t;

int      yghv_share_init(void);
int      yghv_share_up(void);
uint64_t yghv_share_pa(void);

int yghv_share_host_write(uint32_t kind, const uint64_t *args, uint32_t nargs,
                          const void *payload, uint32_t payload_len, uint8_t *out_seq);

int yghv_share_host_read(uint8_t *expect_seq, uint32_t *kind, uint64_t *args,
                         uint32_t max_args, void *payload, uint32_t max_payload,
                         uint32_t *payload_len_out);

void yghv_share_stats_get(yghv_share_stats_t *out);
void yghv_share_stats_reset(void);

#endif /* YGHV_MEM_SHARE_H */
