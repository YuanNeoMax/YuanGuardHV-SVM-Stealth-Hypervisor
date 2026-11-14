/*
 * yghv_sig.c -- masked byte-pattern search inside a kernel module image (9.589).
 *
 * WHY A SEPARATE MODULE FROM mem_scan
 *   mem_scan.c answers "find this pattern in a PROCESS's user address space":
 *   it walks page tables, skips absent subtrees, counts pages, and tolerates
 *   ranges measured in gigabytes. This answers a different question -- "find
 *   this pattern in a MODULE IMAGE" -- where the range is bounded by
 *   SizeOfImage, every page is resident, the answer must be unique, and the
 *   caller is usually running at DISPATCH_LEVEL inside a VM exit. Sharing one
 *   implementation would mean the user-space scanner carrying constraints it
 *   does not need and this one inheriting a page-walk it does not want.
 *
 * WHY UNIQUENESS IS ENFORCED, NOT ADVISED
 *   A signature that matches once is an address; one that matches twice is a
 *   coin flip that happens to work on this build. yghv_sig_find_unique()
 *   returns 0 unless EXACTLY ONE hit exists, and yghv_sig_scan() always
 *   reports the real count so "not found" and "ambiguous" never collapse into
 *   the same silent zero. The tree has already paid for that collapse once
 *   (mem_bp.c documents why -- 7 and -11 must stay distinct).
 *
 * MODULE RESOLUTION, IN ORDER OF HOW MUCH LAYOUT IT ASSUMES
 *   (1) by_reference: any exported routine in the target module, then
 *       RtlPcToFileHeader. Zero structure assumptions; preferred.
 *   (2) by_name: walk PsLoadedModuleList. Needs KLDR_DATA_TABLE_ENTRY offsets,
 *       which are hard coded here and VALIDATED at use -- a candidate whose
 *       links are not canonical, whose DllBase is unaligned, or whose name is
 *       not a plausible UNICODE_STRING is SKIPPED, never dereferenced.
 *   (3) self: our own function's address through RtlPcToFileHeader.
 *
 * THE SELF-TEST IS THE POINT
 *   There is no ground truth to compare a scanner against -- unless it is
 *   manufactured. yghv_offsets.c derives EPROCESS.UniqueProcessId from the
 *   instruction bytes of PsGetProcessId. Rebuilding those exact bytes from the
 *   derived offset and demanding the scanner find them at PsGetProcessId's own
 *   address closes the loop in both directions: a wrong offset fails the scan,
 *   and a broken scanner fails to find a pattern that provably exists.
 */
#include <ntddk.h>
#include <ntimage.h>          /* IMAGE_* and RtlImageNtHeader; ntddk.h does not pull it */
#include <ntstrsafe.h>        /* RtlStringCchCopyW */
#include <stdint.h>
#include "common/debug.h"

/*
 * RtlPcToFileHeader is exported by ntoskrnl but declared by NO header in the
 * 10.0.19041 WDK (grep -r over km/ shared/ finds nothing). Declaring it is the
 * documented prototype; the alternative -- walking PsLoadedModuleList to find
 * the image that contains an address -- is exactly the layout guess this
 * function exists to avoid.
 */
NTKERNELAPI PVOID NTAPI RtlPcToFileHeader(PVOID PcValue, PVOID *BaseOfImage);
#include "common/yghv_nt.h"
#include "common/yghv_sig.h"
#include "common/yghv_strtab.h"

/* Same KLDR_DATA_TABLE_ENTRY prefix loader_stealth.c uses. Duplicated rather
 * than shared because that file needs only the links and this one needs the
 * image extents; the two would drift apart inside one struct otherwise. */
typedef struct _YGHV_LDR_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    PVOID      ExceptionTable;
    ULONG      ExceptionTableSize;
    PVOID      GpValue;
    PVOID      NonPagedDebugInfo;
    PVOID      DllBase;
    PVOID      EntryPoint;
    ULONG      SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} YGHV_LDR_ENTRY, *PYGHV_LDR_ENTRY;

/* How far past the start of an image we are willing to believe SizeOfImage.
 * ntoskrnl is ~12 MB, win32k larger; 256 MB is generous, and the bound exists
 * so a corrupted header cannot send the scan into a stranger's memory. */
#define YGHV_SIG_MAX_IMAGE (256u * 1024u * 1024u)

static volatile LONG64 g_sig_scans;
static volatile LONG64 g_sig_bytes;
static volatile LONG64 g_sig_hits;
static volatile LONG64 g_sig_faults;
static volatile LONG64 g_sig_bad_module;
static volatile LONG64 g_sig_selftest_ok;
static volatile LONG64 g_sig_selftest_fail;
static volatile LONG64 g_sig_exports;      /* successful name lookups (9.594) */
static volatile LONG64 g_sig_forwarders;   /* forwarder exports we refused (9.594) */

/* ---------------------------------------------------------------- helpers */

static int yghv_sig_canonical(uint64_t p)
{
    /* Kernel space on x64 is the upper canonical half. Anything else is not a
     * kernel pointer, and following it would either fault or read user memory
     * the caller did not ask about. */
    return p >= 0xFFFF800000000000ULL;
}

static int yghv_sig_readable_image(uint64_t base, uint32_t size, uint32_t *out_size)
{
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DOS_HEADER *dos;

    if (!yghv_sig_canonical(base)) return 0;
    if ((base & 0xFFF) != 0) return 0;

    __try {
        dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return 0;
        nt = (IMAGE_NT_HEADERS64 *)(base + (uint32_t)dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
        if (nt->OptionalHeader.SizeOfImage < 0x1000) return 0;
        if (nt->OptionalHeader.SizeOfImage > YGHV_SIG_MAX_IMAGE) return 0;
        if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(&g_sig_faults);
        return 0;
    }

    *out_size = nt->OptionalHeader.SizeOfImage;
    (void)size;
    return 1;
}

/* ----------------------------------------------------------------- modules */

int yghv_sig_module_by_reference(PCWSTR export_name, YGHV_SIG_MODULE *out)
{
    UNICODE_STRING name;
    PVOID fn = NULL;
    PVOID base = NULL;
    uint32_t size = 0;

    if (!out || !export_name) return 0;
    RtlZeroMemory(out, sizeof(*out));

    RtlInitUnicodeString(&name, export_name);
    fn = MmGetSystemRoutineAddress(&name);
    if (!fn) return 0;

    /* RtlPcToFileHeader maps a code address back to the image that owns it.
     * It reads the loader's own structures, so this path still depends on the
     * loader being intact -- but on none of our offsets. */
    if (!RtlPcToFileHeader(fn, &base) || !base) return 0;
    if (!yghv_sig_readable_image((uint64_t)base, 0, &size)) {
        InterlockedIncrement64(&g_sig_bad_module);
        return 0;
    }

    out->base = (uint64_t)base;
    out->size = size;
    out->flags = YGHV_SIG_MOD_BY_REFERENCE;
    (void)RtlStringCchCopyW(out->name, YGHV_SIG_NAME_CHARS, export_name);
    out->name[YGHV_SIG_NAME_CHARS - 1] = 0;
    return 1;
}

int yghv_sig_module_by_name(PCWSTR module_name, YGHV_SIG_MODULE *out)
{
    UNICODE_STRING funcName;
    UNICODE_STRING want;
    PLIST_ENTRY head;
    PLIST_ENTRY le;
    uint32_t guard = 0;

    if (!out || !module_name) return 0;
    RtlZeroMemory(out, sizeof(*out));

    {
        WCHAR wide[YGHV_STR_MAX_CHARS];

        if (!yghv_str_get_w(YGHV_SID_PSLOADEDMODULELIST, (unsigned short *)wide,
                            YGHV_STR_MAX_CHARS))
            return 0;
        RtlInitUnicodeString(&funcName, wide);
    }
    head = (PLIST_ENTRY)MmGetSystemRoutineAddress(&funcName);
    if (!head) return 0;

    RtlInitUnicodeString(&want, module_name);
    if (!want.Buffer || want.Length == 0) return 0;

    __try {
        if (!yghv_sig_canonical((uint64_t)head)) return 0;

        for (le = head->Flink; le != head; le = le->Flink) {
            PYGHV_LDR_ENTRY e;

            /* A corrupted or circular list must end the walk, not hang the
             * driver. 4096 modules is far past any real system. */
            if (++guard > 4096) break;
            if (!yghv_sig_canonical((uint64_t)le)) break;

            e = CONTAINING_RECORD(le, YGHV_LDR_ENTRY, InLoadOrderLinks);

            /* Validate before trusting: these offsets are ours, not the
             * header's, so a wrong one lands on garbage and must be caught
             * here rather than by a fault three instructions later. */
            if (!e->BaseDllName.Buffer || e->BaseDllName.Length == 0) continue;
            if ((e->BaseDllName.Length & 1) != 0) continue;
            if (e->BaseDllName.Length > 256) continue;
            if (!yghv_sig_canonical((uint64_t)e->BaseDllName.Buffer)) continue;
            if (e->BaseDllName.Length != want.Length) continue;
            if (RtlCompareMemory(e->BaseDllName.Buffer, want.Buffer,
                                 want.Length) != want.Length) continue;

            if (!yghv_sig_readable_image((uint64_t)e->DllBase, 0, &out->size)) {
                continue;
            }
            out->base = (uint64_t)e->DllBase;
            out->flags = YGHV_SIG_MOD_BY_NAME;
            (void)RtlStringCchCopyW(out->name, YGHV_SIG_NAME_CHARS,
                                    e->BaseDllName.Buffer);
            out->name[YGHV_SIG_NAME_CHARS - 1] = 0;
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(&g_sig_faults);
        return 0;
    }
    return 0;
}

int yghv_sig_module_self(YGHV_SIG_MODULE *out)
{
    PVOID base = NULL;
    uint32_t size = 0;

    if (!out) return 0;
    RtlZeroMemory(out, sizeof(*out));

    if (!RtlPcToFileHeader((PVOID)(ULONG_PTR)&yghv_sig_module_self, &base) || !base) {
        return 0;
    }
    if (!yghv_sig_readable_image((uint64_t)base, 0, &size)) {
        InterlockedIncrement64(&g_sig_bad_module);
        return 0;
    }
    out->base = (uint64_t)base;
    out->size = size;
    out->flags = YGHV_SIG_MOD_SELF;
    (void)RtlStringCchCopyW(out->name, YGHV_SIG_NAME_CHARS, L"self");
    return 1;
}

/* ---------------------------------------------------------------- exports */

/*
 * Resolve an export by NAME inside an arbitrary module.
 *
 * Why this cannot be MmGetSystemRoutineAddress: that call only searches the
 * kernel's own export table (ntoskrnl + hal). win32kfull.sys, win32kbase.sys
 * and every other driver keep their exports in THEIR OWN table, and the whole
 * window-control work (9.594) needs exactly those. So the module is resolved
 * first (by_reference / by_name) and this walks that module's export
 * directory directly.
 *
 * The trap that has bitten this tree before: AddressOfFunctions is indexed by
 * ORDINAL, not by name index. Walking AddressOfNames and using that counter to
 * index AddressOfFunctions yields plausible-looking but wrong RVAs -- they land
 * in .text, they are executable, and they are not the function. The mapping is
 * AddressOfNames[i] -> AddressOfNameOrdinals[i] -> AddressOfFunctions[ordinal].
 *
 * A forwarded export (a function whose RVA points back inside the export
 * directory) is refused rather than followed: a forwarder is a name of a name,
 * and resolving it means parsing yet another module's table. Reporting zero is
 * honest; returning the forwarder's own RVA as if it were code is not.
 */
int yghv_sig_export_of(const YGHV_SIG_MODULE *module, const char *name,
                       uint64_t *out_addr, uint32_t *out_rva)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_EXPORT_DIRECTORY *ed;
    uint32_t exp_rva = 0;
    uint32_t exp_size = 0;
    int found = 0;

    if (out_addr) *out_addr = 0;
    if (out_rva) *out_rva = 0;
    if (!module || !name || !name[0]) return 0;
    if (!yghv_sig_canonical(module->base) || module->size < 0x1000) return 0;

    __try {
        dos = (IMAGE_DOS_HEADER *)module->base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return 0;
        nt = (IMAGE_NT_HEADERS64 *)(module->base + (uint32_t)dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

        exp_rva  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        exp_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        if (exp_rva == 0 || exp_size < sizeof(IMAGE_EXPORT_DIRECTORY)) return 0;
        if (exp_rva >= module->size) return 0;

        ed = (IMAGE_EXPORT_DIRECTORY *)(module->base + exp_rva);
        /* NumberOfNames is a 32-bit field read from an image; bound it before
         * it becomes a loop count. A real module never has 64K exports. */
        if (ed->NumberOfNames == 0 || ed->NumberOfNames > 0x10000) return 0;
        if (ed->AddressOfNames == 0 || ed->AddressOfNameOrdinals == 0 ||
            ed->AddressOfFunctions == 0) return 0;

        {
            const uint32_t *names = (const uint32_t *)(module->base + ed->AddressOfNames);
            const uint16_t *ords  = (const uint16_t *)(module->base + ed->AddressOfNameOrdinals);
            const uint32_t *funcs = (const uint32_t *)(module->base + ed->AddressOfFunctions);
            uint32_t i;

            for (i = 0; i < ed->NumberOfNames; i++) {
                const char *cand = (const char *)(module->base + names[i]);
                uint32_t j = 0;

                /* Bounded compare, not strcmp: the name comes from an image we
                 * do not own and may not be terminated inside that image. */
                while (j < 96 && cand[j] && cand[j] == name[j]) j++;
                if (j == 0 || cand[j] != name[j]) continue;
                if (cand[j] != 0) continue;

                {
                    uint16_t ord = ords[i];
                    uint32_t rva;

                    if (ord >= ed->NumberOfFunctions) break;
                    rva = funcs[ord];
                    if (rva == 0) break;

                    /* Forwarder: the RVA points back into the export directory
                     * itself. Refuse instead of handing back a data address. */
                    if (rva >= exp_rva && rva < exp_rva + exp_size) {
                        InterlockedIncrement64(&g_sig_forwarders);
                        break;
                    }
                    if (rva >= module->size) break;

                    if (out_rva)  *out_rva = rva;
                    if (out_addr) *out_addr = module->base + rva;
                    found = 1;
                }
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(&g_sig_faults);
        return 0;
    }

    if (found) InterlockedIncrement64(&g_sig_exports);
    return found;
}

/* ---------------------------------------------------------------- scan */

uint32_t yghv_sig_scan(const YGHV_SIG_MODULE *module,
                       const uint8_t *pattern, const uint8_t *mask,
                       uint32_t len, uint64_t *hits, uint32_t max_hits)
{
    uint32_t found = 0;
    uint64_t p;
    uint64_t end;

    if (!module || !pattern || len == 0 || len > 256) return 0;
    if (module->size < len) return 0;
    if (!yghv_sig_canonical(module->base)) return 0;

    InterlockedIncrement64(&g_sig_scans);

    /* Scan the whole image, including the PE headers: a pattern that also
     * matches in a header is genuinely ambiguous and the caller must know. */
    end = module->base + (uint64_t)module->size - len;

    __try {
        for (p = module->base; p <= end; p++) {
            const uint8_t *c = (const uint8_t *)p;
            uint32_t i;
            int ok = 1;

            /* First byte check before the loop: it rejects ~255/256 of
             * positions for the cost of one comparison. A caller that
             * wildcards byte 0 gives that up and pays the full comparison at
             * every position -- legal, but not free, which is why the
             * convention in mem_engine.h is to leave it significant. */
            if ((!mask || mask[0] != 0) && c[0] != pattern[0]) continue;

            for (i = 1; i < len; i++) {
                if (mask && mask[i] == 0) continue;
                if (c[i] != pattern[i]) { ok = 0; break; }
            }
            if (!ok) continue;

            if (found < max_hits && hits) hits[found] = p;
            found++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A hole in the image (a discardable section that was never loaded,
         * for instance) ends the scan. Reporting the hits found so far is
         * honest; pretending the scan covered everything is not. */
        InterlockedIncrement64(&g_sig_faults);
    }

    InterlockedAdd64(&g_sig_bytes, (LONG64)module->size);
    InterlockedAdd64(&g_sig_hits, (LONG64)found);
    return found;
}

uint64_t yghv_sig_find_unique(const YGHV_SIG_MODULE *module,
                              const uint8_t *pattern, const uint8_t *mask,
                              uint32_t len, uint32_t *out_hits)
{
    uint64_t hit[2];
    uint32_t n;

    if (out_hits) *out_hits = 0;
    n = yghv_sig_scan(module, pattern, mask, len, hit, 2);
    if (out_hits) *out_hits = n;
    if (n != 1) return 0;
    return hit[0];
}

uint64_t yghv_sig_find_in_module_of(PCWSTR export_name,
                                    const uint8_t *pattern, const uint8_t *mask,
                                    uint32_t len, uint32_t *out_hits)
{
    YGHV_SIG_MODULE mod;

    if (out_hits) *out_hits = 0;
    if (!yghv_sig_module_by_reference(export_name, &mod)) return 0;
    return yghv_sig_find_unique(&mod, pattern, mask, len, out_hits);
}

int yghv_sig_branch_target(uint64_t site, uint64_t *target)
{
    const uint8_t *c = (const uint8_t *)site;
    int32_t rel;

    if (!target || !yghv_sig_canonical(site)) return 0;

    __try {
        if (c[0] != 0xE8 && c[0] != 0xE9) return 0;
        rel = (int32_t)((uint32_t)c[1] | ((uint32_t)c[2] << 8) |
                        ((uint32_t)c[3] << 16) | ((uint32_t)c[4] << 24));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(&g_sig_faults);
        return 0;
    }

    *target = site + 5 + (int64_t)rel;
    return 1;
}

uint32_t yghv_sig_build_accessor(uint32_t shape, uint32_t disp, uint8_t *out8)
{
    /* Thin wrapper so callers that include only yghv_sig.h still get the
     * builder. The logic lives in yghv_accessor.h because the user-mode
     * self-test must run the same bytes-in/bytes-out code as the driver. */
    return yghv_acc_build(shape, disp, out8);
}

uint32_t yghv_sig_build_rcx_load(uint32_t disp, uint8_t *out8)
{
    return yghv_acc_build(YGHV_ACC_MOV64, disp, out8);
}

int yghv_sig_selftest(void)
{
    /*
     * TWO exports, TWO shapes, and one negative control -- because a single
     * case proves less than it looks. PsGetProcessId (mov rax,[rcx+disp32]) and
     * PsGetProcessProtection (mov al,[rcx+disp32]) are encoded differently, so
     * a builder that only knows one encoding passes one check and fails the
     * other instead of appearing to work.
     *
     * The offsets come from yghv_offsets.c, which derived them from these very
     * functions' bytes. Rebuilding the bytes from the derived offset and
     * demanding the scanner find them at the address the EXPORT TABLE names
     * closes the loop both ways: bad derivation -> pattern absent -> fail;
     * broken scanner -> present pattern not found -> fail.
     */
    static const struct { int sid; uint32_t shape; UINT16 *off; }
        cases[] = {
            { YGHV_SID_PSGETPROCESSID,        YGHV_ACC_MOV64, &g_yghv_off.unique_process_id },
            { YGHV_SID_PSGETPROCESSPROTECTION, YGHV_ACC_MOV8,  &g_yghv_off.protection }
        };
    static const uint8_t absent[8] = { 0x0F, 0x0B, 0x0F, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    YGHV_SIG_MODULE mod;
    uint32_t pass = 0;
    uint32_t i;

    if (!YGHV_OFF_OK(YGHV_OFF_PID) || g_yghv_off.unique_process_id == 0) {
        InterlockedIncrement64(&g_sig_selftest_fail);
        LOG_ERROR("sig selftest: pid offset not derived, scanner unverified");
        return 0;
    }

    for (i = 0; i < RTL_NUMBER_OF(cases); i++) {
        UNICODE_STRING name;
        WCHAR caseName[YGHV_STR_MAX_CHARS];
        PVOID expected;
        uint8_t pat[8];
        uint32_t plen, hits = 0;
        uint64_t hit;

        if (!yghv_str_get_w(cases[i].sid, (unsigned short *)caseName,
                            YGHV_STR_MAX_CHARS)) {
            InterlockedIncrement64(&g_sig_selftest_fail);
            LOG_ERROR("sig selftest: table entry %d does not decode", cases[i].sid);
            return 0;
        }

        if (*cases[i].off == 0) {
            /* Not derived on this build (PsGetProcessProtection is absent
             * before 1607). Skipping is correct; counting it as a pass would
             * not be. */
            LOG_INFO("sig selftest: %ws offset not derived, case skipped",
                     caseName);
            continue;
        }

        plen = yghv_sig_build_accessor(cases[i].shape, *cases[i].off, pat);
        if (plen == 0) {
            InterlockedIncrement64(&g_sig_selftest_fail);
            LOG_ERROR("sig selftest: shape %u cannot express offset 0x%x",
                      cases[i].shape, *cases[i].off);
            return 0;
        }

        if (!yghv_sig_module_by_reference(caseName, &mod)) {
            InterlockedIncrement64(&g_sig_selftest_fail);
            LOG_ERROR("sig selftest: could not resolve the module of %ws",
                      caseName);
            return 0;
        }

        RtlInitUnicodeString(&name, caseName);
        expected = MmGetSystemRoutineAddress(&name);
        if (!expected) {
            InterlockedIncrement64(&g_sig_selftest_fail);
            return 0;
        }

        hit = yghv_sig_find_unique(&mod, pat, NULL, plen, &hits);
        if (hits != 1 || hit != (uint64_t)expected) {
            InterlockedIncrement64(&g_sig_selftest_fail);
            LOG_ERROR("sig selftest: %ws shape %u offset 0x%x -> %u hit(s), "
                      "addr 0x%llx, expected 0x%llx",
                      caseName, cases[i].shape, *cases[i].off, hits, hit,
                      (uint64_t)expected);
            return 0;
        }
        pass++;
    }

    if (pass == 0) {
        InterlockedIncrement64(&g_sig_selftest_fail);
        LOG_ERROR("sig selftest: no case ran, so nothing was verified");
        return 0;
    }

    /*
     * Negative control. Every module contains plenty of 0xCC padding, so a
     * run of CC bytes is NOT absent and would make this control vacuous --
     * measured on this machine: 22,753 occurrences of eight 0xCC bytes in
     * ntoskrnl. UD2 followed by garbage is absent (measured: 0).
     */
    {
        WCHAR ctlName[YGHV_STR_MAX_CHARS];
        int ctlOk;

        ctlOk = yghv_str_get_w(YGHV_SID_PSGETPROCESSID,
                               (unsigned short *)ctlName, YGHV_STR_MAX_CHARS);
        if (!ctlOk || !yghv_sig_module_by_reference(ctlName, &mod)) {
            InterlockedIncrement64(&g_sig_selftest_fail);
            return 0;
        }
    }
    if (yghv_sig_scan(&mod, absent, NULL, sizeof(absent), NULL, 0) != 0) {
        InterlockedIncrement64(&g_sig_selftest_fail);
        LOG_ERROR("sig selftest: the negative control matched, so a zero hit "
                  "count proves nothing");
        return 0;
    }

    InterlockedIncrement64(&g_sig_selftest_ok);
    LOG_INFO("sig selftest: %u accessor shape(s) rebuilt and located uniquely; "
             "absent pattern returned 0 hits (%u byte image)",
             pass, mod.size);
    return 1;
}

void yghv_sig_stats(uint64_t *out)
{
    if (!out) return;
    out[0] = (uint64_t)InterlockedCompareExchange64(&g_sig_scans, 0, 0);
    out[1] = (uint64_t)InterlockedCompareExchange64(&g_sig_bytes, 0, 0);
    out[2] = (uint64_t)InterlockedCompareExchange64(&g_sig_hits, 0, 0);
    out[3] = (uint64_t)InterlockedCompareExchange64(&g_sig_faults, 0, 0);
    out[4] = (uint64_t)InterlockedCompareExchange64(&g_sig_bad_module, 0, 0);
    out[5] = (uint64_t)InterlockedCompareExchange64(&g_sig_selftest_ok, 0, 0);
    out[6] = (uint64_t)InterlockedCompareExchange64(&g_sig_selftest_fail, 0, 0);
    out[7] = (uint64_t)InterlockedCompareExchange64(&g_sig_exports, 0, 0);
    out[8] = (uint64_t)InterlockedCompareExchange64(&g_sig_forwarders, 0, 0);
}
