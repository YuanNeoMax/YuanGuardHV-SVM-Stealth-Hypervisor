/*
 * yghv_sections.c — Tier 7.1: report the driver's OWN PE sections, read-only.
 *
 * WHY THIS EXISTS BEFORE ANY HIDING CODE
 *
 * Tier 7.1's remaining item is "exclude the driver's own data pages from the guest
 * NPT". That needs to know which pages are DATA and which are CODE, because the
 * two cannot be treated the same way: a code page with present=0 stops our own
 * instruction fetch, and this tree has no ALLOW loop to serve it (9.450b). A data
 * page can be excluded; a code page cannot.
 *
 * The tree has no PE parser, and writing one inside a hiding routine means the
 * first bug in the parser shows up as a crash in the hide path - where a mistake
 * costs a bugcheck rather than a wrong answer.
 *
 * So the parser lands FIRST, on its own, answering only "what sections do I have
 * and which of them are executable". Once that answer is visible and correct,
 * the hide can consult it instead of re-deriving it. Same discipline as 9.450:
 * build the observation before the mechanism.
 *
 * It is deliberately read-only. Nothing here writes an NPT entry, allocates, or
 * takes a lock - so calling it cannot damage the state it is describing.
 */

#include "yghv_sections.h"

/*
 * Minimal PE structures, declared here rather than pulled from a header.
 *
 * ntddk.h does not expose IMAGE_NT_HEADERS for kernel use in this configuration,
 * and including winnt.h into a kernel translation unit brings its own conflicts
 * (the same reason loader_stealth.c re-declares KLDR_DATA_TABLE_ENTRY rather than
 * including ntifs.h). The layouts below follow the PE/COFF spec exactly; the
 * static asserts are what keeps that claim honest.
 */
typedef struct {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} YGHV_DOS_HEADER;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t SizeOfData;
} YGHV_DATA_DIRECTORY;

typedef struct {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t ImageBase;             /* PE32 only; PE32+ widens this */
} YGHV_OPTIONAL_HEADER32_HEAD;

typedef struct {
    uint32_t ImageBase;             /* PE32+ : 64-bit */
} YGHV_PE32PLUS_TAIL;

/*
 * 9.458: the two optional-header prefixes needed to reach the data directories.
 *
 * PE32 and PE32+ differ in the size of the base-of-code/ImageBase fields before
 * the directory array, so the offset is computed rather than assumed. Only the
 * fields up to SizeOfHeaders are declared here; everything after is reached by
 * arithmetic, which is why the layout is bounded at each step.
 */
typedef struct {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;            /* PE32 only; PE32+ has ImageBase here */
} YGHV_OPTIONAL_COMMON;

typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} YGHV_FILE_HEADER;

typedef struct {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} YGHV_SECTION_HEADER;

#define YGHV_MZ_MAGIC 0x5A4D
#define YGHV_PE_MAGIC 0x00004550
#define YGHV_PE32PLUS 0x20B
#define YGHV_PE32     0x10B

/* IMAGE_SCN_MEM_EXECUTE - the one bit that decides "can we hide this". */
#define YGHV_SCN_MEM_EXECUTE 0x20000000
#define YGHV_SCN_MEM_WRITE   0x80000000

/*
 * A hard cap on sections we will walk. A real image has a handful; a corrupt or
 * hostile NumberOfSections must not turn this report into a walk off the end of
 * the image, so the loop is bounded by BOTH the header count and this ceiling.
 */
#define YGHV_MAX_SECTIONS 32

int yghv_sections_report(uint8_t *image_base, uint32_t image_size,
                         yghv_section_info_t *out, uint32_t out_max,
                         uint32_t *count_out, uint32_t *flags_out)
{
    YGHV_DOS_HEADER *dos;
    YGHV_FILE_HEADER *fh;
    YGHV_SECTION_HEADER *sh;
    uint8_t *nt;
    uint32_t nsec, i;
    uint32_t opt_size;
    uint16_t magic;

    if (!image_base || !image_size || !out || !count_out || !out_max)
        return 0;

    *count_out = 0;
    if (flags_out)
        *flags_out = 0;

    /*
     * Bounds first, every time: a truncated or bad image must produce "cannot
     * read" rather than a page fault inside a reporting command. This is the
     * whole reason the parser is its own file.
     */
    if (image_size < sizeof(YGHV_DOS_HEADER))
        return 0;

    dos = (YGHV_DOS_HEADER *)image_base;
    if (dos->e_magic != YGHV_MZ_MAGIC)
        return 0;

    if (dos->e_lfanew == 0 ||
        (uint32_t)dos->e_lfanew + 4 + sizeof(YGHV_FILE_HEADER) >= image_size)
        return 0;

    nt = image_base + dos->e_lfanew;
    if (*(uint32_t *)nt != YGHV_PE_MAGIC)
        return 0;

    fh = (YGHV_FILE_HEADER *)(nt + 4);
    opt_size = fh->SizeOfOptionalHeader;

    if (opt_size < 2)
        return 0;

    /* The optional header is where the PE32/PE32+ magic lives; the section table
     * starts right after it, so an absurd SizeOfOptionalHeader is another bound. */
    if ((uint32_t)((uint8_t *)fh + sizeof(YGHV_FILE_HEADER) - image_base) + opt_size
            >= image_size)
        return 0;

    magic = *(uint16_t *)((uint8_t *)fh + sizeof(YGHV_FILE_HEADER));
    if (flags_out) {
        *flags_out = (magic == YGHV_PE32PLUS) ? 2u
                   : (magic == YGHV_PE32)     ? 1u
                                              : 0u;
    }
    if (magic != YGHV_PE32 && magic != YGHV_PE32PLUS)
        return 0;

    sh = (YGHV_SECTION_HEADER *)((uint8_t *)fh + sizeof(YGHV_FILE_HEADER) + opt_size);

    nsec = fh->NumberOfSections;
    if (nsec > YGHV_MAX_SECTIONS)
        nsec = YGHV_MAX_SECTIONS;

    for (i = 0; i < nsec && i < out_max; i++) {
        /*
         * Each section header must be inside the image, or the walk stops here
         * rather than reading whatever follows the allocation.
         */
        if ((uint32_t)((uint8_t *)&sh[i] - image_base) + sizeof(YGHV_SECTION_HEADER)
                > image_size)
            break;

        RtlZeroMemory(&out[i], sizeof(out[i]));

        {
            uint32_t n;
            for (n = 0; n < 8 && sh[i].Name[n]; n++) {
                out[i].name[n] = (char)sh[i].Name[n];
            }
            out[i].name[n < 8 ? n : 7] = 0;
        }

        out[i].va = sh[i].VirtualAddress;
        out[i].size = sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData;
        out[i].executable = (sh[i].Characteristics & YGHV_SCN_MEM_EXECUTE) ? 1 : 0;
        out[i].writable = (sh[i].Characteristics & YGHV_SCN_MEM_WRITE) ? 1 : 0;
    }

    /*
     * 9.458: MARK THE SECTION THAT CONTAINS THE IMPORT ADDRESS TABLE.
     *
     * Measured, and it cost a bugcheck: hiding .rdata hides the IAT, because the
     * IAT lives at RVA 0x28000 - the first page of .rdata. After that hide, `call
     * [IAT slot]` read 0 and jumped to address 0: 0x3B C0000005 with rip = 0 at
     * `call IoAcquireRemoveLockEx`, an instruction that was itself correct.
     *
     * The directory is read here rather than assumed, because a linker is free to
     * place the IAT anywhere. Bounds are checked at each hop: a short optional
     * header or a directory index past the header must produce "unknown", not a
     * read off the end of the allocation.
     */
    {
        uint32_t dir_off;
        uint32_t iat_rva = 0;
        uint32_t iat_size = 0;

        /*
         * DataDirectory[0] = export, [1] = import, [12] = IAT. Its offset differs
         * by magic: PE32 puts BaseOfData (4 bytes) before ImageBase (4); PE32+
         * drops BaseOfData and uses an 8-byte ImageBase.
         */
        dir_off = (magic == YGHV_PE32PLUS) ? 112u : 96u;

        if (opt_size >= dir_off + 13u * 8u &&
            (uint32_t)((uint8_t *)fh + sizeof(YGHV_FILE_HEADER) - image_base) +
                dir_off + 13u * 8u <= image_size) {

            uint8_t *dd = (uint8_t *)fh + sizeof(YGHV_FILE_HEADER) + dir_off;

            /* entry 12: RVA and size */
            iat_rva  = *(uint32_t *)(dd + 12u * 8u);
            iat_size = *(uint32_t *)(dd + 12u * 8u + 4u);

            if (iat_rva) {
                uint32_t j;

                for (j = 0; j < i; j++) {
                    uint32_t start = out[j].va;
                    uint32_t end = out[j].va + out[j].size;

                    /* An IAT that straddles two sections marks both; refusing one
                     * of the two would still break the calls through the other. */
                    if (iat_rva >= start && iat_rva < end) {
                        out[j].has_iat = 1;
                    } else if (iat_size &&
                               iat_rva + iat_size > start && iat_rva < end) {
                        out[j].has_iat = 1;
                    }
                }
            }
        }
    }

    *count_out = i;
    return 1;
}
