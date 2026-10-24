/*
 * yghv_sentry.h — Tier 7.4 / 7.5 / 7.6.
 *
 * These three are one subject from three sides: how the hypervisor behaves when
 * someone is looking at it, or attacking it.
 *
 * 7.4 ANTI-DETECTION RESPONSE
 *     Time-based detection is the one probe a guest can always run on itself:
 *     rdtsc around a hypervisor call, or cpuidex leaf 1 / 0x40000000. The cost
 *     of a VMEXIT is what leaks. What this does is keep a measurement of our own
 *     exit latency and expose it, so a regression that makes us slow — and
 *     therefore loud — is visible before someone else notices it.
 *     Honest limit: this measures and reports. Hiding the cost needs TSC
 *     scaling, which Tier 5 owns (5.1) and which is a much larger change.
 *
 * 7.5 ESCAPE DETECTION
 *     The guest is the attacker of the thing that runs it. Every value that
 *     reaches an emulation path is untrusted: MSR indices, port numbers, IOIO
 *     addresses, CR values, VMCB fields the guest can reach. This records every
 *     rejected input with enough context to reconstruct what was attempted,
 *     rather than silently clamping and hoping.
 *
 * 7.6 ATTACK SURFACE MINIMISATION
 *     The cheapest defence is not having the feature. This keeps an inventory of
 *     what is currently INTERCEPTED and what is currently EXPOSED through the
 *     IOCTL surface, so the answer to "what can be attacked" is a list that can
 *     be read, not a claim that can be made. Anything not needed can be dropped
 *     from the intercept mask and the answer gets shorter.
 */
#ifndef YGHV_SENTRY_H
#define YGHV_SENTRY_H

#include "svm_defs.h"
#include <ntddk.h>

/* ---- 7.5 escape attempts ---- */

typedef enum {
    YGHV_ESC_MSR_INDEX = 0,   /* guest wrote/read an MSR we do not emulate */
    YGHV_ESC_IO_PORT   = 1,   /* I/O port outside the emulated range */
    YGHV_ESC_CR_VALUE  = 2,   /* CR/EFER value that would break us */
    YGHV_ESC_VMCB_FIELD= 3,   /* guest reached a VMCB field it should not */
    YGHV_ESC_IOCTL_ARG = 4,   /* user-mode IOCTL argument out of range */
    YGHV_ESC_GPA_GUARD = 5,   /* access to a page we hid */
    YGHV_ESC_MAX       = 6
} yghv_esc_kind_t;

/*
 * Record one rejected input. ISLAND-SAFE (no locks, no allocation): called from
 * VMEXIT handlers. `detail` is kind-specific (the MSR index, the port, the CR
 * number, ...).
 */
void yghv_sentry_escape(yghv_esc_kind_t kind, uint64_t detail, uint64_t a,
                        uint64_t b);

/* ---- 7.4 detection-response / self-measurement ---- */

/*
 * ISLAND-SAFE: note how long an exit took. Call it from the VMEXIT prologue and
 * epilogue with rdtsc values; the module keeps a running worst case and a
 * count, so an exit handler that got slow shows up as a number.
 */
void yghv_sentry_exit_begin(uint64_t tsc);
void yghv_sentry_exit_end(uint64_t tsc);

/* Jitter estimate a guest would see: max observed exit cycles. */
uint64_t yghv_sentry_exit_worst(void);
uint64_t yghv_sentry_exit_count(void);

/* ---- 7.6 surface inventory ---- */

/*
 * Record that one intercept is currently armed. `name` is a short label
 * ("MSR", "IOIO", "CR3", ...). Idempotent by name: the inventory is a set, not
 * a log, so it describes the CURRENT surface rather than its history.
 */
int yghv_sentry_surface_add(const char *name);
int yghv_sentry_surface_remove(const char *name);

/* Which IOCTL codes the device accepts right now. */
int yghv_sentry_surface_add_ioctl(uint32_t code);

/*
 * 9.366: fill the inventory from the mask.
 *
 * 9.368 (A2) REMOVED the "arm_intercepts" entry point that did this from a
 * hand-written list of nine names: the list and the mask were two statements in
 * two files, so adding an intercept meant remembering to add a name, and
 * forgetting produced an inventory that under-reported the surface while still
 * looking complete. The mask is the truth, so the module that owns the VMCB
 * scans it and calls surface_add() once per armed bit.
 *
 * What remains here is storage plus the "has anything filled it yet" flag, so
 * this file keeps no opinion about what a VMCB is.
 */
int yghv_sentry_surface_armed(void);

/* The filler sets this when it finishes, so a second core does not re-register
 * the same names (each add() would otherwise bump the hit count as if the
 * intercept had been exercised). */
void yghv_sentry_surface_mark_armed(void);

typedef struct {
    uint64_t escapes[YGHV_ESC_MAX];   /* attempts per kind */
    uint64_t escapes_total;
    uint64_t last_kind;
    uint64_t last_detail;
    uint64_t last_a;
    uint64_t last_b;
} yghv_sentry_escape_stats_t;

typedef struct {
    uint64_t exits;          /* how many exits measured */
    uint64_t worst_cycles;   /* slowest one, in TSC ticks */
    uint64_t total_cycles;
} yghv_sentry_timing_t;

#define YGHV_SENTRY_MAX_SURFACE 64
#define YGHV_SENTRY_NAME_LEN    16

typedef struct {
    char     name[YGHV_SENTRY_NAME_LEN];
    uint64_t count;
    uint32_t is_ioctl;
    uint32_t active;
} yghv_sentry_surface_entry_t;

void yghv_sentry_escape_stats_get(yghv_sentry_escape_stats_t *out);
void yghv_sentry_timing_get(yghv_sentry_timing_t *out);
uint32_t yghv_sentry_surface_list(yghv_sentry_surface_entry_t *out, uint32_t max);
void yghv_sentry_reset(void);

const char *yghv_sentry_esc_name(uint64_t kind);

#endif /* YGHV_SENTRY_H */
