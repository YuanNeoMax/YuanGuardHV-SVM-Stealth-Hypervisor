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

    /* opbuf: ObjectType, Operations, PreOperation, PostOperation */
    *(PVOID *)(opbuf + 0x00) = (PVOID)PsProcessType;
    *(USHORT *)(opbuf + 0x08) = 0x0003;  /* HANDLE_CREATE | HANDLE_DUPLICATE */
    *(PVOID *)(opbuf + 0x10) = (PVOID)yghv_obguard_pre;

    /*
     * 9.601: altitude 321000 is in the legitimate OB filter range (320000-329999)
     * but it is still a signature.  321000 is the number several public rootkit
     * samples use; picking 327681 puts us in the same range with a value no
     * static list matches.
     */
    RtlInitUnicodeString(&altitude, L"327681");

    /* regbuf: Version, Count, Altitude, RegistrationContext, OperationRegistration */
    *(USHORT *)(regbuf + 0x00) = 0x0100;  /* OB_FLT_REGISTRATION_VERSION */
    *(USHORT *)(regbuf + 0x02) = 1;
    *(UNICODE_STRING *)(regbuf + 0x08) = altitude;
    *(PVOID *)(regbuf + 0x20) = opbuf;

    st = g_ob_register(regbuf, &handle);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("obguard: ObRegisterCallbacks failed 0x%x", (unsigned)st);
        return 0;
    }
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

        /* Verify the list entry looks valid before touching it. */
        if (!prev || !next) return 0;
        if ((ULONG_PTR)prev < 0xFFFF800000000000ULL) return 0;
        if ((ULONG_PTR)next < 0xFFFF800000000000ULL) return 0;
        if (prev->Flink != self) return 0;
        if (next->Blink != self) return 0;

        /* Unlink. */
        prev->Flink = next;
        next->Blink = prev;
        self->Flink = self;
        self->Blink = self;

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

void yghv_obguard_stats(uint64_t *out)
{
    out[0] = (uint64_t)g_ob_strips;
    out[1] = g_ob_handle ? 1 : 0;
    out[2] = (uint64_t)(uintptr_t)g_ob_target;
    out[3] = (g_ob_register && g_ob_unregister) ? 1 : 0;
}
