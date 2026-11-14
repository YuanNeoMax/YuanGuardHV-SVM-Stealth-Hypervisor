/*
 * yghv_obguard.h -- handle privilege downgrade (9.597).
 *
 * ObRegisterCallbacks-based: intercepts handle creation to a guarded process
 * and strips PROCESS_TERMINATE | PROCESS_VM_READ | PROCESS_VM_WRITE |
 * PROCESS_SUSPEND_RESUME | PROCESS_DUP_HANDLE from the requested access.
 *
 * Uses byte-buffer struct construction + runtime symbol resolution to avoid
 * WDK prototype conflicts.
 */

#ifndef YGHV_OB_GUARD_H_
#define YGHV_OB_GUARD_H_

#include <ntddk.h>
#include <stdint.h>

/* Resolve ObRegisterCallbacks / ObUnRegisterCallbacks at runtime. Returns 1 if both resolved. */
int yghv_obguard_init(void);

/* Enable handle guarding for a specific process. Returns 1 on success. */
int yghv_obguard_enable(PEPROCESS target);

/* Disable handle guarding. Safe to call when not enabled. */
void yghv_obguard_disable(void);

/* Fill 4 qwords: [0] strips [1] enabled [2] target [3] resolved */
void yghv_obguard_stats(uint64_t *out);

/* 9.608: the NTSTATUS from the last registration attempt, so op10 can say WHY
 * it is not protecting instead of answering a bare 0. 0xC0000022 means the
 * object manager refused the registration itself -- every input was verified
 * correct before that conclusion was reached. */
uint64_t yghv_obguard_last_status(void);

#endif /* YGHV_OB_GUARD_H_ */
