/*
 * yghv_sig.h -- locating unexported kernel functions (9.589).
 *
 * WHY
 *   Half of what this driver needs is not exported: ObpLookupObjectName,
 *   MiResolveImageReferences, the internals behind PsSuspendProcess. They have
 *   no address in the export table and no documented prototype, so the only
 *   handle on them is their BYTES.
 *
 * WHAT THIS IS NOT
 *   It is not a generic "scan the whole address space" tool -- that already
 *   exists as yghv_mem_scan() and works on a PROCESS's user range. This module
 *   scans a MODULE IMAGE in kernel space, which is a different problem: the
 *   range is bounded by SizeOfImage, the pages are always resident, and the
 *   answer must be UNIQUE or it must be refused.
 *
 * THE RULE THIS MODULE ENFORCES
 *   A pattern that matches once is an address. A pattern that matches twice is
 *   a coin flip, and a coin flip that "works" on this build becomes a random
 *   kernel jump on the next one. So the lookup API returns a single address
 *   ONLY when exactly one hit exists; otherwise it returns 0 and the caller
 *   must fall back to a supported path. The hit count is always reported, so
 *   "not found" and "ambiguous" are distinguishable -- a distinction the tree
 *   has been bitten by before (see the note in mem_bp.c about -7 versus -11).
 *
 * SELF-TEST WITHOUT GROUND TRUTH
 *   The awkward part of any scanner is proving it works without already
 *   knowing the answer. This one can: yghv_offsets.c derives the offset of
 *   EPROCESS.UniqueProcessId from PsGetProcessId's own instruction bytes. From
 *   that offset the exact encoding of PsGetProcessId ("mov rax,[rcx+disp32];
 *   ret") can be REBUILT, and the scanner must then find exactly ONE hit in
 *   ntoskrnl -- the export itself. Ground truth comes from the export table,
 *   the pattern comes from the derived value, and the scanner has to connect
 *   them. yghv_sig_selftest() does that and is called from driver init.
 */
#ifndef YGHV_SIG_H
#define YGHV_SIG_H

#include <ntddk.h>
#include <stdint.h>

#define YGHV_SIG_MAX_HITS 16
#define YGHV_SIG_NAME_CHARS 64

/* How a module range was established. Reported so a caller can tell "the name
 * matched the loader list" from "we only know this address lives in it". */
#define YGHV_SIG_MOD_BY_REFERENCE 0x1u  /* base from RtlPcToFileHeader(export) */
#define YGHV_SIG_MOD_BY_NAME      0x2u  /* matched BaseDllName in the loader list */
#define YGHV_SIG_MOD_SELF         0x4u  /* our own driver image */

typedef struct _YGHV_SIG_MODULE {
    uint64_t base;
    uint32_t size;
    uint32_t flags;
    WCHAR    name[YGHV_SIG_NAME_CHARS];
} YGHV_SIG_MODULE;

/*
 * Module resolution.
 *
 * by_reference() is the one to prefer: give it the name of ANY exported
 * routine that lives in the module you want, and it gets the base from
 * RtlPcToFileHeader. That route involves no structure layout guess at all,
 * which is why it is the primitive the self-test uses.
 *
 * by_name() needs the loader list, which means it needs KLDR_DATA_TABLE_ENTRY
 * field offsets. Those are hard coded below and VALIDATED at use (pointer
 * canonicality, alignment, name length, image size) rather than trusted, and a
 * failed validation skips the entry instead of dereferencing it.
 */
int yghv_sig_module_by_reference(PCWSTR export_name, YGHV_SIG_MODULE *out);
int yghv_sig_module_by_name(PCWSTR module_name, YGHV_SIG_MODULE *out);
int yghv_sig_module_self(YGHV_SIG_MODULE *out);

/*
 * Scan one module image for a masked byte pattern.
 *
 * 'mask' may be NULL, meaning "every byte significant". A non-NULL mask marks
 * a wildcard with 0x00 in the corresponding slot, which is the convention
 * yghv_mem_scan() already uses -- two scanners in one tree disagreeing about
 * what a mask means is a defect waiting to be written.
 *
 * Returns the number of hits found, which may exceed 'max_hits' (the extra hits
 * are counted but not stored). 'hits' may be NULL if only the count matters.
 * A fault while reading the image is caught, counted, and reported as 0 hits.
 */
uint32_t yghv_sig_scan(const YGHV_SIG_MODULE *module,
                       const uint8_t *pattern, const uint8_t *mask,
                       uint32_t len, uint64_t *hits, uint32_t max_hits);

/*
 * The lookup form: returns the address only when the pattern is UNIQUE.
 *
 * out_hits (optional) always receives the real hit count, so the caller can
 * say "ambiguous, N hits" instead of "not found".
 */
uint64_t yghv_sig_find_unique(const YGHV_SIG_MODULE *module,
                              const uint8_t *pattern, const uint8_t *mask,
                              uint32_t len, uint32_t *out_hits);

/* Convenience: resolve the module from an export name, then find_unique. */
uint64_t yghv_sig_find_in_module_of(PCWSTR export_name,
                                    const uint8_t *pattern, const uint8_t *mask,
                                    uint32_t len, uint32_t *out_hits);

/*
 * Resolve an export by NAME inside an arbitrary module (9.594).
 *
 * MmGetSystemRoutineAddress only searches the kernel's own export table, which
 * covers ntoskrnl and hal and nothing else. win32kfull.sys / win32kbase.sys
 * keep their exports in their own tables, and the window-control work needs
 * those -- so the module is resolved first, then its export directory is
 * walked here.
 *
 * The mapping is AddressOfNames[i] -> AddressOfNameOrdinals[i] ->
 * AddressOfFunctions[ordinal]; a name index used directly as a function index
 * produces a plausible but wrong RVA. A forwarder (RVA inside the export
 * directory) is refused, not followed.
 *
 * Returns 1 and writes out_addr/out_rva (both optional) on success. Returns 0
 * for "no such export", "forwarder", and "unreadable image" alike; the shape
 * of the failure is in yghv_sig_stats().
 */
int yghv_sig_export_of(const YGHV_SIG_MODULE *module, const char *name,
                       uint64_t *out_addr, uint32_t *out_rva);

/*
 * Decode a relative branch at 'site'.
 *
 * Almost nothing interesting is found by its own bytes; it is found by the
 * CALL that reaches it. 0xE8 is call rel32 and 0xE9 is jmp rel32, both
 * "target = site + 5 + rel32". Returns 1 and writes *target on a match.
 */
int yghv_sig_branch_target(uint64_t site, uint64_t *target);

/*
 * Accessor shapes.
 *
 * Measured in ntoskrnl.exe on this machine, "read a field of RCX and return"
 * is written in four different ways and NO single one covers all of them:
 *
 *   48 8B 81 dd dd dd dd C3   mov rax,[rcx+disp32]; ret   PsGetProcessId (0x440)
 *   48 8D 81 dd dd dd dd C3   lea rax,[rcx+disp32]; ret   PsGetProcessImageFileName
 *   8A 81 dd dd dd dd C3      mov al,[rcx+disp32];  ret   PsGetProcessProtection
 *   48 8B 41 dd C3            mov rax,[rcx+disp8];   ret   (offsets below 0x80)
 *
 * Rebuilding only the first and asserting equality would make the self-test a
 * coin flip on which shape Microsoft happened to pick for the export it
 * chose. So the shape is explicit, and a caller that wants to locate a
 * specific accessor passes the shape it observed.
 */
/*
 * 9.591: these four are already defined by yghv_accessor.h, which yghv_nt.h
 * pulls in. A translation unit that includes both (yghv_proc.c does, because it
 * needs offsets and signatures) got eight -Wmacro-redefined warnings, which is
 * exactly the kind of noise that hides a real diagnostic. Guarded rather than
 * removed so a caller that includes only yghv_sig.h still compiles.
 */
#ifndef YGHV_ACC_MOV64
#define YGHV_ACC_MOV64   1   /* 48 8B 81 dd dd dd dd C3  (8 bytes) */
#endif
#ifndef YGHV_ACC_LEA64
#define YGHV_ACC_LEA64   2   /* 48 8D 81 dd dd dd dd C3  (8 bytes) */
#endif
#ifndef YGHV_ACC_MOV8
#define YGHV_ACC_MOV8    3   /* 8A 81 dd dd dd dd C3     (7 bytes) */
#endif
#ifndef YGHV_ACC_MOV64_8
#define YGHV_ACC_MOV64_8 4   /* 48 8B 41 dd C3           (5 bytes, disp < 0x80) */
#endif

/*
 * Build the machine code of one accessor shape from a derived offset.
 * Returns the number of bytes written into out8 (0 on a bad shape or when the
 * offset does not fit the shape), so the caller passes that count as the
 * pattern length rather than remembering a per-shape constant.
 */
uint32_t yghv_sig_build_accessor(uint32_t shape, uint32_t disp, uint8_t *out8);

/* The canonical Ps*Get* form. Kept because it is the common case and because
 * the self-test reads better as "a load of this offset" than as "shape 1". */
uint32_t yghv_sig_build_rcx_load(uint32_t disp, uint8_t *out8);

/* [0] scans [1] bytes [2] hits [3] faults [4] bad_module
 * [5] selftest_ok [6] selftest_fail [7] exports_found [8] forwarders_refused */
void yghv_sig_stats(uint64_t *out);
int  yghv_sig_selftest(void);

#endif /* YGHV_SIG_H */
