/*
 * yghv_inject.h -- code injection primitives (9.592).
 *
 * WHAT THIS IS
 *   Five primitives, and nothing above them. Every "injection technique" is a
 *   composition of these:
 *
 *     alloc   reserve + commit memory in the target, with a chose protection
 *     write   copy bytes into that memory (delegates to the memory engine)
 *     thread  start a thread at a given VA with one argument
 *     apc     queue a user-mode APC onto one of the target's existing threads
 *     free    release the reservation again
 *
 *   A DLL injection is alloc + write(path) + thread(LoadLibraryW, path). A
 *   shellcode injection is alloc + write(bytes) + thread(bytes). Neither is a
 *   separate kernel feature, and writing them as separate features is how a
 *   driver ends up with five copies of the same allocation bug.
 *
 * WHY THE FUNCTION ADDRESS IS A PARAMETER
 *   The kernel does NOT resolve user-mode exports. Walking the target's PEB to
 *   find kernel32!LoadLibraryW needs three more structure offsets (PEB.Ldr,
 *   LDR_DATA_TABLE_ENTRY.DllBase, IMAGE_EXPORT_DIRECTORY), each of which is a
 *   build-specific guess that fails silently when wrong. The client already
 *   runs in user mode where GetProcAddress is exact, so it passes the address
 *   in. One number crosses the boundary instead of three guesses.
 *
 * THE RULE, SAME AS EVERYWHERE ELSE IN THIS TREE
 *   Fail closed with a distinct status. A target that cannot be opened, a
 *   thread id that does not belong to the process, a size above the cap -- each
 *   returns its own code and writes nothing. Nothing here retries silently.
 */

#ifndef YGHV_INJECT_H
#define YGHV_INJECT_H

#include <ntddk.h>
#include <stdint.h>

/* Ceilings. These are policy: a driver that will map 1 GB into any process on
 * request is a weapon, and one that will not is still a tool. */
#define YGHV_INJ_MAX_ALLOC   (1024u * 1024u)     /* one megabyte per call */
#define YGHV_INJ_MAX_WRITE   (64u * 1024u)       /* one chunk of payload */
#define YGHV_INJ_MAX_PAYLOAD 56u                 /* inline bytes in the IOCTL */

/* Status codes this module returns beyond the NTSTATUS set. 0 means success. */
#define YGHV_INJ_OK          0
#define YGHV_INJ_E_NO_TARGET 1   /* PsLookupProcessByProcessId failed */
#define YGHV_INJ_E_NO_HANDLE 2   /* ObOpenObjectByPointer refused (PP/PPL) */
#define YGHV_INJ_E_SIZE      3   /* zero or above the cap */
#define YGHV_INJ_E_ALLOC     4   /* ZwAllocateVirtualMemory failed */
#define YGHV_INJ_E_WRITE     5   /* the memory engine could not deliver */
#define YGHV_INJ_E_THREAD    6   /* RtlCreateUserThread failed */
#define YGHV_INJ_E_TID       7   /* thread id does not belong to this process */
#define YGHV_INJ_E_APC       8   /* KeInsertQueueApc refused */
#define YGHV_INJ_E_FREE      9   /* ZwFreeVirtualMemory failed */
#define YGHV_INJ_E_OFFSETS   10  /* the offset table is not ready */

#ifdef __cplusplus
extern "C" {
#endif

/* Open the target with the rights the requested operation needs.
 * Returns an NTSTATUS; on success *handle must be closed with ZwClose. */
typedef struct {
    void *handle;      /* HANDLE */
    void *process;     /* PEPROCESS, dereferenced already */
    uint64_t base;     /* image base, for reporting only */
} yghv_inj_target_t;

/* All five primitives take the pid directly: the open is cheap and doing it
 * inside keeps the handle lifetime inside one call -- a handle held across
 * calls is a handle that leaks when a client disconnects mid-sequence. */

int yghv_inj_alloc(unsigned long pid, uint64_t *va_inout, uint64_t size,
                   unsigned long protect);

int yghv_inj_write(unsigned long pid, uint64_t va, const void *buf,
                   unsigned long len, unsigned long *written);

int yghv_inj_free(unsigned long pid, uint64_t va, uint64_t size);

int yghv_inj_thread(unsigned long pid, uint64_t start_va, uint64_t arg,
                    unsigned long flags, unsigned long *tid_out);

int yghv_inj_apc(unsigned long pid, unsigned long tid, uint64_t routine_va,
                 uint64_t arg, unsigned long *queued);

/* Diagnostics: [0] allocs [1] writes [2] threads [3] apcs [4] frees
 *              [5] fails  [6] bytes written [7] apcs still in flight */
void yghv_inj_stats(uint64_t *out);

/*
 * IOCTL arm. Kept in this file rather than in control_device.c because the
 * buffer aliasing rules it obeys (request header and reply header share one
 * METHOD_BUFFERED buffer, and the WRITE payload sits between them) are facts
 * about this module's request layout, not about the device.
 */
NTSTATUS yghv_inject_dispatch(const void *in, const void *payload, ULONG payload_len,
                              void *out, ULONG out_len, ULONG *info);

/* Release every KAPC still queued and not yet freed by its kernel routine.
 * Called from the unload path: a KAPC is non-paged pool owned by the system,
 * and the kernel routine that frees it lives in an image that is about to
 * disappear. */
void yghv_inj_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* YGHV_INJECT_H */
