/*
 * yghv_win.c -- window capture suppression from kernel mode (9.594).
 *
 * READ yghv_win.h FIRST: it explains why the real code is in win32kfull.sys,
 * why a __win32kstub_* call is a no-op, and why the call is only legitimate on
 * a thread that already owns GUI state.
 *
 * WHAT THIS FILE DOES NOT DO
 *   It does not enter a win32k critical section around the call. It does not
 *   have to: NtUserSetWindowDisplayAffinity's own first instructions acquire
 *   the user critical section itself (measured: "48 89 5c 24 08 ... ba 01 00 00
 *   00 33 c9 48 ff 15 .." -- the ff 15 is the call into the enter/leave pair).
 *   Wrapping it again would recurse into a lock that is not recursive and hang
 *   the machine, which is the classic way this goes wrong.
 */
#include <ntddk.h>
#include <stdint.h>
#include "common/yghv_win.h"
#include "common/control_ioctl.h"
#include "common/yghv_sig.h"
#include "debug.h"

/*
 * PsGetThreadWin32Thread / PsGetCurrentThreadWin32Thread are exported by
 * ntoskrnl but declared by no header in the 10.0.19041 WDK. Measured encodings:
 *   PsGetThreadWin32Thread         48 8b 81 c8 01 00 00 c3      -> [rcx+0x1C8]
 *   PsGetCurrentThreadWin32Thread  65 48 8b 04 25 88 01 00 00 48 8b 80 c8 01 ...
 * The second one confirms the same +0x1C8 field on the current thread, so the
 * declared prototype below is consistent with both.
 */
NTKERNELAPI PVOID PsGetCurrentThreadWin32Thread(void);
NTKERNELAPI PVOID PsGetThreadWin32Thread(PETHREAD Thread);

#define YGHV_WIN_TAG 'wPvG'

/* The exports this module calls. Each is resolved once and cached; a zero means
 * "not resolved yet", which triggers one retry per call rather than a fatal
 * state, because win32kfull.sys does not exist until an interactive session is
 * up and this driver may load before anyone logs in. */
/* INT32, not BOOL: ntdef's BOOL is a long and the win32k exports return a
 * BOOLEAN-sized truth value. The calling convention is left default -- on x64
 * there is exactly one, and naming it (NTAPI / __stdcall) only works when the
 * macro happens to be in scope. */
typedef int (*yghv_set_display_fn)(uint32_t hwnd, uint32_t affinity);
typedef int (*yghv_get_display_fn)(uint32_t hwnd, uint32_t *affinity);

static yghv_set_display_fn g_win_set;
static yghv_get_display_fn g_win_get;
static YGHV_SIG_MODULE     g_win_full;      /* win32kfull.sys */
static YGHV_SIG_MODULE     g_win_base;      /* win32kbase.sys */
static volatile LONG       g_win_init_done;

static volatile LONG64 g_win_sets;
static volatile LONG64 g_win_gets;
static volatile LONG64 g_win_refused;
static volatile LONG64 g_win_verify_fail;
static volatile LONG64 g_win_no_gui;
static volatile LONG64 g_win_init_ok;
static volatile LONG64 g_win_last_affinity;
static volatile LONG64 g_win_last_hwnd;

/* --------------------------------------------------------------- resolve */

int yghv_win_init(void)
{
    uint64_t addr = 0;
    uint32_t rva = 0;
    int have = 0;

    /* Module names are not in the encrypted string table on purpose: the table
     * exists so that "strings x.sys | grep PsGetProcess" finds nothing, and
     * win32kfull.sys / win32kbase.sys are what any triage expects a driver that
     * touches windows to name. Encrypting them buys nothing and would make the
     * one place that must be re-read on a new Windows build harder to find. */
    if (!yghv_sig_module_by_name(L"win32kfull.sys", &g_win_full)) {
        LOG_INFO("win: win32kfull.sys not present (no interactive session?)");
        return 0;
    }
    if (!yghv_sig_module_by_name(L"win32kbase.sys", &g_win_base)) {
        LOG_INFO("win: win32kbase.sys not present");
        /* Not fatal for the set/get path; counted only so the diagnostic op can
         * tell "no GUI" from "half a GUI". */
    }

    if (yghv_sig_export_of(&g_win_full, "NtUserSetWindowDisplayAffinity",
                           &addr, &rva)) {
        g_win_set = (yghv_set_display_fn)(ULONG_PTR)addr;
        have++;
    } else {
        LOG_ERROR("win: NtUserSetWindowDisplayAffinity not found in win32kfull");
    }

    addr = 0;
    rva = 0;
    if (yghv_sig_export_of(&g_win_full, "NtUserGetWindowDisplayAffinity",
                           &addr, &rva)) {
        g_win_get = (yghv_get_display_fn)(ULONG_PTR)addr;
        have++;
    } else {
        LOG_ERROR("win: NtUserGetWindowDisplayAffinity not found in win32kfull");
    }

    if (g_win_base.base) have++;   /* module resolved counts as a piece */
    if (have >= 2) InterlockedExchange64(&g_win_init_ok, 1);
    InterlockedExchange(&g_win_init_done, 1);

    LOG_INFO("win: init %d piece(s); full=%p size=0x%x base=%p size=0x%x",
             have, (void *)g_win_full.base, g_win_full.size,
             (void *)g_win_base.base, g_win_base.size);
    return have;
}

int yghv_win_ready(void)
{
    return (g_win_set && g_win_get) ? 1 : 0;
}

/* The one precondition that cannot be faked: the calling thread must own GUI
 * state, because win32k looks the hwnd up in THIS thread's desktop. A system
 * thread has none, and calling anyway either fails or touches a stranger's
 * desktop. Returning NO_GUI lets the caller fix it by calling from its UI
 * thread, which is a thing the caller can actually do. */
static int yghv_win_have_gui(void)
{
    if (PsGetCurrentThreadWin32Thread() != NULL) return 1;
    InterlockedIncrement64(&g_win_no_gui);
    return 0;
}

/* -------------------------------------------------------------------- ops */

int yghv_win_set(uint64_t hwnd, uint32_t affinity)
{
    uint32_t got = 0xFFFFFFFFu;
    int ok;

    if (!yghv_win_ready() && yghv_win_init() < 2) return YGHV_WIN_E_NO_MODULE;
    if (hwnd == 0 || hwnd > 0xFFFFFFFFULL) return YGHV_WIN_E_BAD_ARG;
    if (!yghv_win_have_gui()) return YGHV_WIN_E_NO_GUI;

    /* Called at PASSIVE_LEVEL from the dispatch routine, on the client's own
     * thread -- see the header. */
    ok = g_win_set((uint32_t)hwnd, affinity);
    if (!ok) {
        InterlockedIncrement64(&g_win_refused);
        return YGHV_WIN_E_REFUSED;
    }

    InterlockedIncrement64(&g_win_sets);
    InterlockedExchange64(&g_win_last_affinity, (LONG64)affinity);
    InterlockedExchange64(&g_win_last_hwnd, (LONG64)hwnd);

    /* Read back. A set that silently did nothing (wrong hwnd, different
     * desktop, a policy layer eating the call) is indistinguishable from a
     * successful one until something actually captures the screen, and by then
     * nobody is looking at a return code. */
    if (!g_win_get((uint32_t)hwnd, &got) || got != affinity) {
        InterlockedIncrement64(&g_win_verify_fail);
        LOG_ERROR("win: set hwnd=0x%x want=0x%x readback=0x%x",
                  (unsigned)hwnd, affinity, got);
        return YGHV_WIN_E_VERIFY;
    }
    return 0;
}

int yghv_win_get(uint64_t hwnd, uint32_t *out_affinity)
{
    uint32_t got = 0;

    if (out_affinity) *out_affinity = 0;
    if (!out_affinity) return YGHV_WIN_E_BAD_ARG;
    if (!yghv_win_ready() && yghv_win_init() < 2) return YGHV_WIN_E_NO_MODULE;
    if (hwnd == 0 || hwnd > 0xFFFFFFFFULL) return YGHV_WIN_E_BAD_ARG;
    if (!yghv_win_have_gui()) return YGHV_WIN_E_NO_GUI;

    if (!g_win_get((uint32_t)hwnd, &got)) {
        InterlockedIncrement64(&g_win_refused);
        return YGHV_WIN_E_REFUSED;
    }
    InterlockedIncrement64(&g_win_gets);
    *out_affinity = got;
    return 0;
}

/* ------------------------------------------------------------- dispatch */

/*
 * Answer layout (qwords):
 *   [0] 1 on success, 0 on failure
 *   [1] YGHV_WIN_E_* code, or 0
 *   [2] affinity (get, or the echo of a successful set)
 *   [3] hwnd
 *   [4..7] reserved
 */
NTSTATUS yghv_win_dispatch(const void *in, void *out, ULONG out_len, ULONG *info)
{
    const yghv_ioctl_win_req_t *req = (const yghv_ioctl_win_req_t *)in;
    UINT64 *o = (UINT64 *)out;
    int rc;
    uint32_t affinity = 0;

    if (!in || !out) return STATUS_INVALID_PARAMETER;
    if (out_len < YGHV_WIN_HDR_QWORDS * sizeof(UINT64)) return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(o, YGHV_WIN_HDR_QWORDS * sizeof(UINT64));
    *info = YGHV_WIN_HDR_QWORDS * sizeof(UINT64);

    switch (req->op) {
    case YGHV_WIN_OP_SET:
        rc = yghv_win_set(req->hwnd, req->affinity);
        if (rc == 0) {
            o[0] = 1;
            o[2] = req->affinity;
        } else {
            o[1] = (UINT64)rc;
        }
        o[3] = req->hwnd;
        break;

    case YGHV_WIN_OP_GET:
        rc = yghv_win_get(req->hwnd, &affinity);
        if (rc == 0) {
            o[0] = 1;
            o[2] = affinity;
        } else {
            o[1] = (UINT64)rc;
        }
        o[3] = req->hwnd;
        break;

    case YGHV_WIN_OP_STATS:
        yghv_win_stats(o);
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

void yghv_win_stats(uint64_t *out)
{
    if (!out) return;
    out[0] = (uint64_t)InterlockedCompareExchange64(&g_win_sets, 0, 0);
    out[1] = (uint64_t)InterlockedCompareExchange64(&g_win_gets, 0, 0);
    out[2] = (uint64_t)InterlockedCompareExchange64(&g_win_refused, 0, 0);
    out[3] = (uint64_t)InterlockedCompareExchange64(&g_win_verify_fail, 0, 0);
    out[4] = (uint64_t)InterlockedCompareExchange64(&g_win_no_gui, 0, 0);
    out[5] = (uint64_t)InterlockedCompareExchange64(&g_win_init_ok, 0, 0);
    out[6] = (uint64_t)InterlockedCompareExchange64(&g_win_last_affinity, 0, 0);
    out[7] = (uint64_t)InterlockedCompareExchange64(&g_win_last_hwnd, 0, 0);
}
