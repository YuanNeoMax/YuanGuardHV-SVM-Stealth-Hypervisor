/*
 * mem_engine.h — Phase 3: two-engine abstraction + typed memory access.
 *
 * Engine A: MmCopyVirtualMemory (the NT path). Has process semantics, so
 *           memory-monitoring tools can see it, and it switches CR3.
 * Engine B: hand-rolled page-table walk + physical frame access. No NT
 *           memory API on the path, no CR3 switch, large-page aware.
 *
 * Both implement the same interface so callers do not care which is used.
 * The auto selector picks per the rules in yghv_mem_auto_pick().
 */
#ifndef YGHV_MEM_ENGINE_H
#define YGHV_MEM_ENGINE_H

#include "svm_defs.h"
#include <ntddk.h>
#include "mem_pte.h"

typedef enum {
    YGHV_ENGINE_AUTO = 0,
    YGHV_ENGINE_API  = 1,   /* A: MmCopyVirtualMemory */
    YGHV_ENGINE_PTE  = 2,   /* B: page-table walk + physical frame */
} yghv_engine_t;

/* Typed access — the wire type ids are shared with the client. */
typedef enum {
    YGHV_T_U8 = 0, YGHV_T_U16, YGHV_T_U32, YGHV_T_U64,
    YGHV_T_I8, YGHV_T_I16, YGHV_T_I32, YGHV_T_I64,
    YGHV_T_F32, YGHV_T_F64,
    YGHV_T_STR_ASCII, YGHV_T_STR_UTF16, YGHV_T_STR_UTF8,
    YGHV_T_BYTES,
    YGHV_T_VEC3,    /* float[3] */
    YGHV_T_MAT4,    /* float[16] */
    YGHV_T_MAX
} yghv_mem_type_t;

/* Fixed size of each type in bytes; strings and bytes are variable. */
uint32_t yghv_mem_type_size(uint32_t type);

/* --- the interface both engines implement --------------------------------- */
typedef struct {
    const char *name;
    /* Read len bytes from (process, va). *done receives the byte count. */
    NTSTATUS (*read)(PEPROCESS proc, uint64_t va, void *buf, SIZE_T len, SIZE_T *done);
    /* Write len bytes to (process, va). */
    NTSTATUS (*write)(PEPROCESS proc, uint64_t va, const void *buf, SIZE_T len, SIZE_T *done);
} yghv_mem_engine_ops_t;

extern const yghv_mem_engine_ops_t g_yghv_engine_api;   /* Engine A */
extern const yghv_mem_engine_ops_t g_yghv_engine_pte;   /* Engine B */

/* Current selection (0 = auto). */
void     yghv_mem_engine_set(yghv_engine_t e);
yghv_engine_t yghv_mem_engine_get(void);

/* Resolve the engine actually used for this request (applies auto rules). */
yghv_engine_t yghv_mem_auto_pick(PEPROCESS proc, uint64_t va, SIZE_T len);

/* Unified entry points — pick the engine, fall back to A on B's failure. */
NTSTATUS yghv_mem_read(PEPROCESS proc, uint64_t va, void *buf, SIZE_T len,
                       SIZE_T *done, yghv_engine_t *used);
NTSTATUS yghv_mem_write(PEPROCESS proc, uint64_t va, const void *buf, SIZE_T len,
                        SIZE_T *done, yghv_engine_t *used);

/* Diagnostics */
typedef struct {
    uint64_t api_reads,   api_writes,   api_fail;
    uint64_t pte_reads,   pte_writes,   pte_fail;
    uint64_t fallbacks;         /* B failed -> A used */
    uint64_t auto_choices_api, auto_choices_pte;
    /* Engine B write failure breakdown. Without this, a blocked Engine B
     * write is indistinguishable from "B was never tried" — it just shows up
     * as a fallback to A (this hid a wrong mapping API for a whole round). */
    uint64_t pte_wfail_irql;    /* rejected: IRQL too high to map */
    uint64_t pte_wfail_walk;    /* walk could not resolve the VA */
    uint64_t pte_wfail_pa;      /* MDL map or copy failed */
} yghv_mem_stats_t;

void yghv_mem_stats_get(yghv_mem_stats_t *out);
void yghv_mem_stats_reset(void);

/* --- Phase 4: signature scan (mem_scan.c) --------------------------------
 * Walks the page tables once, skips absent subtrees, and reads one page at a
 * time — so the read count is proportional to PAGES, not to addresses.
 * `mask[i] == 0` marks a wildcard byte. `flags`: bit0 writable, bit1 exec.
 * Returns the number of hits written into `out`. */
uint32_t yghv_mem_scan(PEPROCESS proc, uint64_t start_va, uint64_t end_va,
                       const uint8_t *pattern, const uint8_t *mask,
                       uint32_t pat_len, uint64_t align, uint32_t flags,
                       uint64_t *out, uint32_t out_max,
                       uint64_t *pages_read, uint64_t *pages_skipped,
                       uint64_t *bytes_scanned);

#endif /* YGHV_MEM_ENGINE_H */
