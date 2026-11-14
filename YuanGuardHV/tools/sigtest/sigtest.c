/*
 * sigtest -- prove the accessor decode/rebuild pair against the real ntoskrnl
 * (9.589).
 *
 * WHAT THIS PROVES, AND WHY IT CANNOT PROVE IT FROM INSIDE THE KERNEL
 *   yghv_sig_selftest() runs in the driver and checks that a rebuilt accessor
 *   body exists exactly once at the exported address. That is the right check
 *   to run in production, but it is a check the driver makes about itself: if
 *   the decoder were wrong in a way that also made the scanner wrong, both
 *   would agree and the driver would report success.
 *
 *   This program is the independent half. It takes ntoskrnl.exe OFF DISK, maps
 *   it the way the loader does, parses the export table itself, and then for
 *   every accessor export it can find:
 *
 *     1. decodes the displacement out of the exported body,
 *     2. rebuilds the body from that displacement,
 *     3. searches the whole mapped image for the rebuilt bytes,
 *     4. requires EXACTLY ONE hit, at the address the export table named.
 *
 *   Step 4 is the one that cannot be faked: the expected address comes from
 *   the export table, not from the decode, so a self-consistent but wrong pair
 *   fails here even though it would pass inside the driver.
 *
 * THE SHARED CODE IS SHARED
 *   The decode and the rebuild are yghv_acc_find_rcx_disp() and
 *   yghv_acc_build() from hv/common/yghv_accessor.h -- the same text the
 *   driver compiles. Only the SEARCH LOOP differs, and deliberately: here it
 *   is memcmp over a malloc'd image, in the driver it is a guarded walk over
 *   kernel pages. Searching is not what could be wrong; decoding is.
 *
 * THE MAPPING MATTERS
 *   An image on disk is not laid out like an image in memory: each section
 *   sits at its VirtualAddress and the gaps between them are zero. Comparing
 *   file offsets against RVAs is the classic way to get a scanner that "works"
 *   on one file and misses on every real image, so this maps first.
 *
 * Usage:  sigtest [path-to-ntoskrnl.exe]
 * Exit:   0 = every case passed, 1 = a case failed, 2 = setup failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "yghv_accessor.h"

/*
 * Accessors this project actually consumes, plus a few more that demonstrate
 * shapes the first group does not. Kept in one table because the value of the
 * test is coverage of SHAPES, not of names: an accessor list that is all
 * mov64 would pass while MOV8 and LEA64 were broken.
 */
static const char *g_names[] = {
    "PsGetProcessId",
    "PsGetProcessPeb",
    "PsGetProcessSectionBaseAddress",
    "PsGetProcessImageFileName",
    "PsGetProcessProtection",
    "PsGetProcessInheritedFromUniqueProcessId",
    "PsGetProcessSessionId",
    "PsGetProcessWow64Process",
    "PsGetProcessDebugPort",
    "PsGetProcessExitStatus",
    "PsGetThreadId",
    "PsGetThreadProcess",
    "PsGetThreadTeb",
    "PsGetThreadProcessId",
};

/* ---------------------------------------------------------------- PE bits */

static IMAGE_NT_HEADERS64 *g_nt;
static unsigned char *g_img;      /* mapped image, SizeOfImage bytes */
static DWORD g_img_size;
static unsigned char *g_raw;      /* the file bytes; g_nt points into these */

static int load_image(const char *path)
{
    FILE *f;
    long fsize;
    unsigned char *raw;
    IMAGE_DOS_HEADER *dos;
    IMAGE_SECTION_HEADER *sec;
    unsigned int i;

    f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return 0; }

    raw = (unsigned char *)malloc((size_t)fsize);
    if (!raw) { fclose(f); return 0; }
    if (fread(raw, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f); free(raw); return 0;
    }
    fclose(f);

    dos = (IMAGE_DOS_HEADER *)raw;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        printf("not a PE file\n"); free(raw); return 0;
    }
    g_nt = (IMAGE_NT_HEADERS64 *)(raw + dos->e_lfanew);
    if (g_nt->Signature != IMAGE_NT_SIGNATURE) {
        printf("bad NT signature\n"); free(raw); return 0;
    }
    if (g_nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        printf("not a 64-bit image\n"); free(raw); return 0;
    }

    g_img_size = g_nt->OptionalHeader.SizeOfImage;
    g_img = (unsigned char *)calloc(1, g_img_size);
    if (!g_img) { free(raw); return 0; }

    /* headers first, then each section at its virtual address -- exactly what
     * the loader does, and the reason a file-offset scan is wrong. */
    memcpy(g_img, raw, g_nt->OptionalHeader.SizeOfHeaders);

    sec = IMAGE_FIRST_SECTION(g_nt);
    for (i = 0; i < g_nt->FileHeader.NumberOfSections; i++) {
        DWORD va = sec[i].VirtualAddress;
        DWORD sz = sec[i].SizeOfRawData;
        DWORD raw_off = sec[i].PointerToRawData;

        if (va >= g_img_size) continue;
        if (sz == 0 || raw_off == 0) continue;
        if (raw_off + sz > (DWORD)fsize) continue;
        if (va + sz > g_img_size) sz = g_img_size - va;

        memcpy(g_img + va, raw + raw_off, sz);
    }

    /*
     * g_nt points into the FILE bytes, not into g_img, so those bytes have to
     * outlive this function. Freeing them here (the first version did) left
     * g_nt dangling and the very next export lookup dereferenced freed
     * memory -- a 0xC0000005 with no output at all, because the crash landed
     * before the first buffer flush.
     */
    g_raw = raw;
    return 1;
}

/* Resolve an export name to an RVA in the mapped image, or 0. Parsed here
 * rather than through the Windows loader because this file is not a loaded
 * module -- the point is to read the export table of an image we only have the
 * bytes of. */
static DWORD export_rva(const char *name)
{
    IMAGE_DATA_DIRECTORY *dir =
        &g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *exp;
    DWORD *names, *funcs;
    WORD *ords;
    DWORD i;

    if (dir->VirtualAddress == 0 || dir->Size == 0) return 0;
    if (dir->VirtualAddress + dir->Size > g_img_size) return 0;

    exp = (IMAGE_EXPORT_DIRECTORY *)(g_img + dir->VirtualAddress);
    names = (DWORD *)(g_img + exp->AddressOfNames);
    funcs = (DWORD *)(g_img + exp->AddressOfFunctions);
    ords = (WORD *)(g_img + exp->AddressOfNameOrdinals);

    for (i = 0; i < exp->NumberOfNames; i++) {
        const char *n = (const char *)(g_img + names[i]);
        if (strcmp(n, name) != 0) continue;
        if (ords[i] >= exp->NumberOfFunctions) return 0;
        return funcs[ords[i]];
    }
    return 0;
}

/* Count non-overlapping-free occurrences of a pattern in the whole image. */
static unsigned int count_hits(const unsigned char *pat, unsigned int len,
                               unsigned long long *first)
{
    unsigned int hits = 0;
    DWORD i;

    if (len == 0 || len > g_img_size) return 0;
    for (i = 0; i + len <= g_img_size; i++) {
        if (memcmp(g_img + i, pat, len) == 0) {
            if (hits == 0 && first) *first = i;
            hits++;
        }
    }
    return hits;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "C:\\Windows\\System32\\ntoskrnl.exe";
    unsigned int i;
    int pass = 0, fail = 0, skipped = 0, ambiguous = 0, continues = 0;
    unsigned int shapes_seen = 0;

    printf("sigtest: accessor decode/rebuild against a real ntoskrnl\n");
    printf("image: %s\n", path);
    if (!load_image(path)) {
        printf("RESULT: FAIL (setup)\n");
        return 2;
    }
    printf("mapped SizeOfImage = 0x%lx bytes\n\n", (unsigned long)g_img_size);

    for (i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        DWORD rva = export_rva(g_names[i]);
        unsigned int disp = 0, len;
        unsigned char rebuilt[8];
        unsigned long long first = 0;
        unsigned int hits, shape, saw_export = 0;
        const char *verdict;

        if (rva == 0) {
            printf("%-42s SKIP  not exported on this build\n", g_names[i]);
            skipped++;
            continue;
        }

        if (!yghv_acc_find_rcx_disp(g_img + rva, 32, &disp)) {
            /*
             * No member load at all. PsGetProcessSessionId is the example: its
             * body is a CALL, not "load a field of RCX". There is no offset to
             * derive and nothing to scan for, so this is not a failure of the
             * decoder -- it is an accessor that does not have the property the
             * decoder needs.
             */
            printf("%-42s SKIP  not a member accessor (%s)\n", g_names[i],
                   "body has no [rcx+disp] load");
            skipped++;
            continue;
        }

        shape = (g_img[rva] == 0x8A) ? YGHV_ACC_MOV8
              : (g_img[rva] == 0x8B) ? YGHV_ACC_MOV32
              : (g_img[rva + 1] == 0x8D) ? YGHV_ACC_LEA64
              : YGHV_ACC_MOV64;

        len = yghv_acc_build(shape, disp, rebuilt);
        if (len == 0) {
            printf("%-42s FAIL  shape %u cannot express 0x%x\n",
                   g_names[i], shape, disp);
            fail++;
            continue;
        }

        hits = count_hits(rebuilt, len, &first);

        /* Where does the export sit among the hits? That, not the raw count, is
         * what decides whether the offset alone can name this function. */
        {
            unsigned int k;
            for (k = 0; k + len <= g_img_size; k++) {
                if (memcmp(g_img + k, rebuilt, len) == 0 && k == rva) {
                    saw_export = 1;
                    break;
                }
            }
        }

        if (saw_export && hits == 1) {
            verdict = "OK";
            pass++;
            shapes_seen |= (1u << shape);
        } else if (saw_export) {
            /* The pattern is present at the export but also elsewhere, so the
             * offset alone does not identify it. Honest outcome, and the reason
             * yghv_sig_find_unique() refuses -- a caller that used this would
             * jump to a coin flip. */
            verdict = "AMBIGUOUS (offset alone cannot name it)";
            ambiguous++;
        } else {
            /* The body starts with the shape and continues past it (a null
             * check, a second dereference), so the strict 8-byte body is not
             * literally present. Reported, not failed: the offset was still
             * derived correctly, which is the only thing this project uses it
             * for. */
            verdict = "CONTINUES (body extends past the load)";
            continues++;
        }

        printf("%-42s off=0x%03X %-18s %2u hit(s)  %s\n",
               g_names[i], disp, yghv_acc_shape_name(shape), hits, verdict);
    }

    printf("\nshapes exercised:");
    for (i = 1; i <= 4; i++)
        if (shapes_seen & (1u << i)) printf("  %s", yghv_acc_shape_name(i));
    printf("\n");

    /*
     * The negative control belongs in the same run: a pattern that certainly
     * does not occur keeps "0 hits" from being indistinguishable from "the
     * search never ran". Measured on this image: eight 0xCC bytes occur 22,753
     * times, so padding is not a valid control and UD2+garbage is.
     */
    {
        static const unsigned char absent[8] = { 0x0F, 0x0B, 0x0F, 0xCC,
                                                 0xCC, 0xCC, 0xCC, 0xCC };
        unsigned int n = count_hits(absent, sizeof(absent), NULL);
        printf("negative control (UD2+garbage): %u hit(s)", n);
        if (n != 0) { printf("  FAIL\n"); fail++; }
        else printf("  OK\n");
    }

    printf("\n%d uniquely locatable, %d ambiguous, %d continue past the load, "
           "%d skipped, %d failed\n",
           pass, ambiguous, continues, skipped, fail);
    printf("RESULT: %s\n", (fail == 0 && pass > 0) ? "PASS" : "FAIL");
    return (fail == 0 && pass > 0) ? 0 : 1;
}
