/*
 * yghv_win.h -- window capture suppression (9.594).
 *
 * THE PROBLEM
 *   "Record the screen and the program's UI must not appear in the recording"
 *   is one flag on one window: SetWindowDisplayAffinity(hwnd,
 *   WDA_EXCLUDEFROMCAPTURE). The user-mode API is public, but a program that
 *   hides itself with it can be unhidden by anything with PROCESS_VM_WRITE, and
 *   a program that refuses the call at all can be forced from the kernel.
 *
 * WHERE THE REAL CODE LIVES
 *   win32k.sys exports 1318 __win32kstub_* entries and every one of them is
 *   "mov eax, <service number>; ret" -- calling a stub does nothing at all.
 *   The implementations are in win32kfull.sys (2357 exports, no stubs). So the
 *   module is resolved by name and the export by name inside its own table,
 *   which needs yghv_sig_export_of() rather than MmGetSystemRoutineAddress
 *   (the latter only searches ntoskrnl + hal).
 *
 * WHY THE CALL CAN WORK FROM A DRIVER AT ALL
 *   The win32k NtUser* implementations read the GUI state of the CALLING
 *   THREAD (ETHRED.Win32Thread). A system thread has none, and a call from one
 *   would either fail or corrupt a stranger's GUI state. This driver is called
 *   by DeviceIoControl, which is synchronous: the dispatch routine runs on the
 *   CLIENT'S thread. When the client issues the request from its UI thread that
 *   thread already owns a Win32Thread in the right session, so the call is
 *   legitimate and needs no thread juggling.
 *
 *   yghv_win_set() therefore reports YGHV_WIN_E_NO_WIN32THREAD instead of
 *   guessing when the calling thread has no GUI state. That is the one failure
 *   the caller can actually fix (call from the UI thread), so it gets its own
 *   code rather than being folded into "refused".
 *
 * WHAT IS VERIFIED RATHER THAN ASSUMED
 *   Set is followed by Get. The value read back must equal what was written,
 *   otherwise the caller is told YGHV_WIN_E_VERIFY -- the same rule
 *   YGHV_PROC_OP_PROT follows, and for the same reason: a write that "worked"
 *   and a write to the wrong place look identical without a read-back.
 */
#ifndef YGHV_WIN_H
#define YGHV_WIN_H

#include <ntddk.h>
#include <stdint.h>

/* Failure codes. Kept small and distinguishable on purpose: "the module is not
 * loaded" (no GUI session) and "the API refused this window" lead to different
 * actions, even though both are a zero from the API underneath. */
#define YGHV_WIN_E_NO_MODULE      1
#define YGHV_WIN_E_NO_EXPORT      2
#define YGHV_WIN_E_NO_GUI         3   /* no Win32Thread on the calling thread */
#define YGHV_WIN_E_BAD_ARG        4
#define YGHV_WIN_E_REFUSED        5   /* the win32k call returned FALSE */
#define YGHV_WIN_E_VERIFY         6   /* written value did not read back */

/* DisplayAffinity values (winuser.h, restated because um/ has no winuser.h). */
#define YGHV_WDA_NONE                0x00000000u
#define YGHV_WDA_MONITOR             0x00000001u
#define YGHV_WDA_EXCLUDEFROMCAPTURE  0x00000011u

/* Resolve win32kfull.sys / win32kbase.sys and the exports this module needs.
 * Returns the number of pieces resolved (0..4); the module is usable only when
 * all of them are. Called once from driver init -- the modules may not be
 * loaded yet if no interactive session exists, so a zero here is not fatal and
 * the first real request retries. */
int yghv_win_init(void);
int yghv_win_ready(void);

/* hwnd is passed as 64 bits because a HANDLE is sign-extended into the kernel;
 * the win32k exports take it in the low 32. */
int yghv_win_set(uint64_t hwnd, uint32_t affinity);          /* 0 or a YGHV_WIN_E_* */
int yghv_win_get(uint64_t hwnd, uint32_t *out_affinity);      /* 0 or a YGHV_WIN_E_* */

NTSTATUS yghv_win_dispatch(const void *in, void *out, ULONG out_len, ULONG *info);

/* [0] sets [1] gets [2] refused [3] verify_fail [4] no_gui [5] init_ok
 * [6] last affinity written [7] last hwnd */
void yghv_win_stats(uint64_t *out);

#endif /* YGHV_WIN_H */
