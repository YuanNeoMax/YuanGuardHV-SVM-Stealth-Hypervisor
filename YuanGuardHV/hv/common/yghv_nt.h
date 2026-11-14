/*
 * yghv_nt.h -- native structure access for kernel objects (9.589).
 *
 * WHY THIS FILE EXISTS
 *   The WDK hands us PEPROCESS as an opaque pointer. Everything the feature
 *   set actually needs to READ or WRITE -- the protection byte, the process
 *   links, the PEB pointer, the thread list -- lives at offsets that the
 *   headers deliberately do not publish, and that change between Windows
 *   builds. Three ways to deal with that:
 *
 *     (a) copy a struct definition off the internet for one build. Works
 *         until the next Windows update, then silently reads the wrong field
 *         and corrupts memory instead of failing.
 *     (b) resolve every offset through a signature scan of ntoskrnl. Robust
 *         but each pattern is a maintenance liability and a scan can fail.
 *     (c) DERIVE what can be derived from exported accessors, and only hard
 *         code what cannot be. Then VALIDATE the derived answer against the
 *         exports before anyone uses it.
 *
 *   This file is (c). The derivation lives in yghv_offsets.c; this header only
 *   defines the vocabulary: the offset table, typed accessors built on it, and
 *   the small native structures whose layout is fixed by the ABI rather than
 *   by the build.
 *
 * WHY THERE IS NO "typedef struct _EPROCESS"
 *   Declaring a full EPROCESS is a claim about 0x8xx bytes of layout that this
 *   project cannot check at compile time and does not need. It needs six
 *   fields. A six-field accessor that indexes a table fails loudly when the
 *   table was never filled (offset 0 means "not derived", and every accessor
 *   reports that), whereas a full struct with the wrong padding reads a
 *   plausible value from the wrong place and never complains.
 *
 * THE OFFSET TABLE IS APPEND-ONLY
 *   Fields are added at the end so an older client that only knows the first
 *   N fields still reads a valid prefix. The magic and the version let a
 *   reader tell "this table was never initialised" from "this table was
 *   initialised by a build that knows fewer fields".
 */
#ifndef YGHV_NT_H
#define YGHV_NT_H

#include <ntddk.h>
#include <stdint.h>
#include "yghv_accessor.h"

#define YGHV_OFF_MAGIC   0x59474F46u   /* 'YGOF' */
#define YGHV_OFF_VERSION 2   /* 9.591: +ppid +exit_status +thread_list_entry +token */

/*
 * Offsets, in bytes, from the base of the object. 0 means NOT DERIVED, which
 * every accessor treats as a hard failure rather than "field at offset zero".
 */
typedef struct _YGHV_OFFSETS {
    uint32_t magic;
    uint32_t version;
    uint32_t build_number;      /* from RtlGetVersion; 0 if unknown */

    /* --- EPROCESS --- */
    uint16_t unique_process_id; /* ULONG_PTR; == PsGetProcessId() */
    uint16_t active_process_links; /* LIST_ENTRY, valid at PASSIVE */
    uint16_t process_lock;      /* EX_PUSH_LOCK guarding the links */
    uint16_t protection;        /* PS_PROTECTION, 1 byte */
    uint16_t peb;               /* PPEB; NULL for system processes */
    uint16_t section_base;      /* PVOID image base */
    uint16_t image_file_name;   /* CHAR[15] inside the object */
    uint16_t thread_list_head;  /* LIST_ENTRY of ETHREAD.ThreadListEntry */
    uint16_t process_flags;     /* ULONG of EPROCESS flags */

    /* --- ETHREAD --- */
    uint16_t thread_unique_id;  /* ULONG_PTR; == PsGetThreadId() */
    uint16_t thread_process;    /* PEPROCESS; == PsGetThreadProcess() */
    uint16_t thread_teb;        /* PVOID; == PsGetThreadTeb() */
    uint16_t thread_cid;        /* CLIENT_ID */

    /*
     * 9.591: two more EPROCESS fields, appended rather than inserted so the
     * prefix above keeps its offsets. Both are derived from single-instruction
     * accessors (measured on 19045):
     *
     *   PsGetProcessInheritedFromUniqueProcessId  48 8B 81 40 05 00 00 C3
     *   PsGetProcessExitStatus                    8B 81 D4 07 00 00 C3
     */
    uint16_t inherited_pid;     /* ULONG_PTR; parent pid */
    uint16_t exit_status;       /* ULONG; NTSTATUS once the process exits */

    /*
     * 9.591: the two coupled thread-list unknowns, filled by a structural
     * probe (yghv_proc.c) rather than by an export, because no export reads
     * either. Both are 0 until the probe succeeds, and the probe demands the
     * whole chain stay inside one process, so a wrong pair cannot pass.
     *
     *   thread_list_entry  ETHREAD:  LIST_ENTRY that is a node of that list
     *
     * Token is an EX_FAST_REF, found by matching PsReferencePrimaryToken()
     * against every aligned offset -- see the probe's comment for why a
     * self-comparison during init cannot validate it.
     */
    uint16_t thread_list_entry;
    uint16_t token;
    /* 16 more bytes than version 1 had; reserved keeps the tail aligned. */

    /* --- validation --- */
    uint32_t derived_mask;      /* YGHV_OFF_* bits that were actually derived */
    uint32_t validated_mask;    /* ... and then checked against an export */
    uint32_t failures;          /* derivations that produced a wrong answer */
    uint32_t reserved;
} yghv_offsets_t;

/* Which fields were derived vs. taken from a table. */
#define YGHV_OFF_PID       0x00000001u
#define YGHV_OFF_LINKS     0x00000002u
#define YGHV_OFF_PROTECT   0x00000004u
#define YGHV_OFF_PEB       0x00000008u
#define YGHV_OFF_BASE      0x00000010u
#define YGHV_OFF_NAME      0x00000020u
#define YGHV_OFF_THREADS   0x00000040u
#define YGHV_OFF_TID       0x00000080u
#define YGHV_OFF_TPROC     0x00000100u
#define YGHV_OFF_TEB       0x00000200u
#define YGHV_OFF_PPID      0x00000400u
#define YGHV_OFF_XSTAT     0x00000800u
#define YGHV_OFF_TLIST     0x00001000u
#define YGHV_OFF_TOKEN     0x00002000u

extern yghv_offsets_t g_yghv_off;

/*
 * Accessors. Every one of them checks the offset is non-zero first: a call
 * made before yghv_offsets_init() returns 0 / NULL rather than dereferencing
 * the object's header as if it were the field.
 *
 * They are macros rather than functions because they must work on a PEPROCESS
 * the caller already holds a reference to, and a function would need to
 * re-validate that reference on every call.
 */
#define YGHV_FIELD_PTR(obj, off, type) \
    (((off) != 0) ? ((type *)((PUCHAR)(obj) + (off))) : ((type *)NULL))

#define YGHV_OFF_OK(bit) \
    ((g_yghv_off.magic == YGHV_OFF_MAGIC) && ((g_yghv_off.derived_mask & (bit)) != 0))

static __inline ULONG_PTR yghv_proc_id(PEPROCESS p) {
    ULONG_PTR *f = YGHV_FIELD_PTR(p, g_yghv_off.unique_process_id, ULONG_PTR);
    return f ? *f : 0;
}
static __inline PEPROCESS yghv_proc_from_links(PLIST_ENTRY le) {
    if (g_yghv_off.active_process_links == 0) return NULL;
    return (PEPROCESS)((PUCHAR)le - g_yghv_off.active_process_links);
}
static __inline PLIST_ENTRY yghv_proc_links(PEPROCESS p) {
    return YGHV_FIELD_PTR(p, g_yghv_off.active_process_links, LIST_ENTRY);
}
static __inline UCHAR *yghv_proc_protection(PEPROCESS p) {
    return YGHV_FIELD_PTR(p, g_yghv_off.protection, UCHAR);
}
static __inline PVOID *yghv_proc_peb_slot(PEPROCESS p) {
    return YGHV_FIELD_PTR(p, g_yghv_off.peb, PVOID);
}
static __inline PVOID *yghv_proc_base_slot(PEPROCESS p) {
    return YGHV_FIELD_PTR(p, g_yghv_off.section_base, PVOID);
}
static __inline char *yghv_proc_image_name(PEPROCESS p) {
    return YGHV_FIELD_PTR(p, g_yghv_off.image_file_name, char);
}
static __inline ULONG_PTR yghv_thread_id(PETHREAD t) {
    ULONG_PTR *f = YGHV_FIELD_PTR(t, g_yghv_off.thread_unique_id, ULONG_PTR);
    return f ? *f : 0;
}
static __inline PEPROCESS yghv_thread_process(PETHREAD t) {
    PEPROCESS *f = YGHV_FIELD_PTR(t, g_yghv_off.thread_process, PEPROCESS);
    return f ? *f : NULL;
}
static __inline ULONG_PTR yghv_proc_inherited_pid(PEPROCESS p) {
    ULONG_PTR *f = YGHV_FIELD_PTR(p, g_yghv_off.inherited_pid, ULONG_PTR);
    return f ? *f : 0;
}
static __inline ULONG yghv_proc_exit_status(PEPROCESS p) {
    ULONG *f = YGHV_FIELD_PTR(p, g_yghv_off.exit_status, ULONG);
    return f ? *f : 0;
}

/*
 * ACCESSOR DECODE lives in yghv_accessor.h, which includes nothing but
 * <stdint.h> so the user-mode test program (tools/sigtest) and this driver
 * execute the same code instead of two copies of it. What was here was a
 * second implementation of that decoder; keeping both would have meant the
 * self-test proved things about the copy.
 *
 * yghv_off_from_export() in yghv_offsets.c is the only user: it calls
 * yghv_acc_find_rcx_disp() on the body of an exported accessor and takes the
 * displacement as the field offset.
 */

/*
 * PS_PROTECTION. Fixed by the ABI (one byte, 3-bit type, 1-bit audit, 4-bit
 * signer) and therefore safe to declare, unlike EPROCESS.
 *
 * Type 0 = none, 1 = light (PPL), 2 = protected (PP). Setting type without
 * a matching signer produces a process the kernel will refuse to signal or
 * open with full access, which is the point; setting type 2 on a process
 * without a valid signature makes the kernel treat every access as denied
 * and can make the machine unrecoverable without a reboot. The caller is
 * expected to know which it wants.
 */
typedef union _YGHV_PS_PROTECTION {
    UCHAR Level;
    struct {
        UCHAR Type   : 3;
        UCHAR Audit  : 1;
        UCHAR Signer : 4;
    } s;
} YGHV_PS_PROTECTION;

#define YGHV_PROT_NONE   0
#define YGHV_PROT_LIGHT  1
#define YGHV_PROT_FULL   2

/* Signer values (Win10 1607+). 0 = none, 1 = Authenticode, 2 = CodeGen,
 * 3 = Antimalware, 4 = Lsa, 5 = Windows, 6 = WinTcb, 7 = WinSystem. */
#define YGHV_SIGNER_NONE        0
#define YGHV_SIGNER_AUTHENTICODE 1
#define YGHV_SIGNER_CODEGEN     2
#define YGHV_SIGNER_ANTIMALWARE 3
#define YGHV_SIGNER_LSA         4
#define YGHV_SIGNER_WINDOWS     5
#define YGHV_SIGNER_WINTCB      6
#define YGHV_SIGNER_WINSYSTEM   7

/*
 * Handle rights used when downgrading a handle. ACCESS_SYSTEM_SECURITY and
 * WRITE_DAC are the two that separate "can look" from "can change who else
 * can look"; the rest of the generic bits are expanded by the object manager
 * at open time, so a downgrade rewrites the granted mask rather than the
 * requested one.
 */
/*
 * 9.591: the WDK kernel headers ship exactly two process access rights,
 * PROCESS_ALL_ACCESS and PROCESS_DUP_HANDLE. Everything else is only in the
 * user-mode winnt.h, which a driver never includes - so YGHV_PROC_READ_RIGHTS
 * below spelled two identifiers that did not exist, and any file that actually
 * used it failed to compile. Define the three this driver needs, at their
 * documented values, and guard each so a future header that does export them
 * wins.
 */
#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE                  0x0001
#endif
#ifndef PROCESS_VM_READ
#define PROCESS_VM_READ                    0x0010
#endif
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION  0x1000
#endif
#ifndef PROCESS_SUSPEND_RESUME
#define PROCESS_SUSPEND_RESUME             0x0800
#endif

#define YGHV_PROC_READ_RIGHTS  (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ)
#define YGHV_PROC_FULL_RIGHTS  (PROCESS_ALL_ACCESS)

NTSTATUS yghv_offsets_init(void);
void     yghv_offsets_stats(uint64_t *out);   /* [0] derived [1] validated [2] failures [3] build */
PEPROCESS yghv_offsets_system_process(void);
int      yghv_offsets_ready(void);

#endif /* YGHV_NT_H */
