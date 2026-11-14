/*
 * yghv_accessor.h -- decode and rebuild the tiny "read a field of RCX" stubs
 * that ntoskrnl exports (9.589).
 *
 * WHY THIS IS A SEPARATE FILE WITH NO INCLUDES
 *   Two very different programs must execute EXACTLY these functions:
 *
 *     the driver   (kernel, live ntoskrnl, mmap'd image)
 *     tools/sigtest (user mode, ntoskrnl.exe read off disk, a byte buffer)
 *
 *   If the test carried its own copy of the decoder it would be testing the
 *   copy -- the class of "looks like a proof" defect this tree keeps finding
 *   (a duplicated predicate in a test is a test of the duplicate). So the
 *   logic lives here, depends on nothing but <stdint.h>, and both sides
 *   include this one file.
 *
 * WHAT AN ACCESSOR IS
 *   Microsoft publishes a set of exports whose entire body loads one field of
 *   the object in RCX and returns it. Their addresses are in the export table,
 *   so nothing has to be searched for; the DISPLACEMENT inside the load IS the
 *   field offset. Measured on this machine (build 19045):
 *
 *     48 8B 81 40 04 00 00 C3   PsGetProcessId              mov rax,[rcx+0x440]; ret
 *     48 8D 81 A8 05 00 00 C3   PsGetProcessImageFileName   lea rax,[rcx+0x5A8]; ret
 *     8A 81 7A 08 00 00 C3      PsGetProcessProtection      mov al,[rcx+0x87A];  ret
 *     8B 81 D4 07 00 00 C3      PsGetProcessExitStatus      mov eax,[rcx+0x7D4]; ret
 *
 *   Four shapes are demonstrated by those four exports. The fifth (disp8)
 *   exists for offsets the assembler can encode in one byte; it is here so a
 *   caller is not forced into the disp32 form when the offset is small.
 *
 *   A shape is NOT a guarantee that the body ends there. PsGetProcessWow64Process
 *   starts with a plain load and continues ("48 8B 81 80 05 00 00  48 85 C0 75
 *   02  C3"), so a rebuilt 8-byte body is not literally present at its address.
 *   That is why the self-tests check MEMBERSHIP of the exported address among
 *   the hits rather than demanding the byte string match the whole function.
 *
 * WHY BUILDING THE BYTES BACK MATTERS
 *   It is the only way to test a scanner without already knowing the answer.
 *   Derive the offset from an export's bytes, rebuild those bytes from the
 *   offset, then demand the scanner find them at the address the export table
 *   names. A wrong derivation makes the pattern absent; a broken scanner fails
 *   to find a pattern that provably exists. Neither half can pass on its own.
 */
#ifndef YGHV_ACCESSOR_H
#define YGHV_ACCESSOR_H

#include <stdint.h>

/*
 * The four observed encodings.
 *
 * No single one covers every accessor in a real ntoskrnl: PsGetProcessId uses
 * MOV64, PsGetProcessImageFileName uses LEA64, PsGetProcessProtection uses
 * MOV8. A self-test that rebuilds only the first would pass or fail depending
 * on which export it happened to pick, which is not a test.
 */
#define YGHV_ACC_MOV64   1u   /* 48 8B 81 dd dd dd dd C3  mov rax,[rcx+disp32]; ret */
#define YGHV_ACC_LEA64   2u   /* 48 8D 81 dd dd dd dd C3  lea rax,[rcx+disp32]; ret */
#define YGHV_ACC_MOV8    3u   /* 8A 81 dd dd dd dd C3     mov al,[rcx+disp32];  ret */
#define YGHV_ACC_MOV64_8 4u   /* 48 8B 41 dd C3           mov rax,[rcx+disp8];   ret */
#define YGHV_ACC_MOV32   5u   /* 8B 81 dd dd dd dd C3     mov eax,[rcx+disp32]; ret */

/* Offsets outside this band are not EPROCESS/ETHREAD fields. The floor is
 * 0x40 because every object starts with a header (dispatcher object, quota
 * block, push lock) and a field below it would be a header field, not a
 * property of the process; the ceiling is two pages because no field of
 * EPROCESS or ETHREAD reaches beyond that, and a displacement past it is far
 * more likely to be a misread instruction (a stack frame access, typically)
 * than a field of the object. */
#define YGHV_ACC_MIN_DISP 0x40u
#define YGHV_ACC_MAX_DISP 0x2000u

/*
 * Find the first "[rcx + disp32]" load/lea/mov in a function prologue and
 * return its displacement.
 *
 * Accepted: optional REX (0x40-0x4F), then 0x8A/0x8B/0x8D, then a ModRM with
 * mod=10b and r/m=001b -- i.e. "memory operand, base RCX, 32-bit
 * displacement". The register field is free, so mov al / mov rax / lea rax all
 * match. disp8 forms are deliberately NOT decoded here: they cannot reach any
 * offset this tree cares about, and guessing at them would be inventing.
 *
 * 'max' bounds the scan so a function of a different shape cannot be read past
 * its end into its neighbour.
 */
static __inline int yghv_acc_find_rcx_disp(const unsigned char *code,
                                          unsigned int max,
                                          unsigned int *disp_out)
{
    unsigned int i;

    if (!code || max < 6 || !disp_out) return 0;

    for (i = 0; i + 6 <= max; i++) {
        unsigned int p = i, op, modrm, d;

        if (code[p] >= 0x40 && code[p] <= 0x4F) p++;
        if (p + 6 > max) break;

        op = code[p];
        if (op != 0x8A && op != 0x8B && op != 0x8D) continue;

        modrm = code[p + 1];
        if ((modrm & 0xC7) != 0x81) continue;

        d = (unsigned int)code[p + 2]
          | ((unsigned int)code[p + 3] << 8)
          | ((unsigned int)code[p + 4] << 16)
          | ((unsigned int)code[p + 5] << 24);

        if (d < YGHV_ACC_MIN_DISP || d > YGHV_ACC_MAX_DISP) continue;
        *disp_out = d;
        return 1;
    }
    return 0;
}

/*
 * Build the machine code of one accessor shape from an offset.
 *
 * Returns the byte count written (8, 8, 7 or 5), or 0 when the shape cannot
 * express the offset -- asking for disp 0x440 in the disp8 shape would produce
 * an instruction that certainly does not exist, and returning a pattern that
 * cannot match would turn "not found" into a silent false negative.
 */
static __inline unsigned int yghv_acc_build(unsigned int shape, unsigned int disp,
                                            unsigned char *out)
{
    if (!out) return 0;
    if (disp < YGHV_ACC_MIN_DISP || disp > YGHV_ACC_MAX_DISP) return 0;

    if (shape == YGHV_ACC_MOV64 || shape == YGHV_ACC_LEA64) {
        out[0] = 0x48;
        out[1] = (shape == YGHV_ACC_MOV64) ? 0x8B : 0x8D;
        out[2] = 0x81;
        out[3] = (unsigned char)(disp & 0xFF);
        out[4] = (unsigned char)((disp >> 8) & 0xFF);
        out[5] = (unsigned char)((disp >> 16) & 0xFF);
        out[6] = (unsigned char)((disp >> 24) & 0xFF);
        out[7] = 0xC3;
        return 8;
    }

    if (shape == YGHV_ACC_MOV32) {
        out[0] = 0x8B;
        out[1] = 0x81;
        out[2] = (unsigned char)(disp & 0xFF);
        out[3] = (unsigned char)((disp >> 8) & 0xFF);
        out[4] = (unsigned char)((disp >> 16) & 0xFF);
        out[5] = (unsigned char)((disp >> 24) & 0xFF);
        out[6] = 0xC3;
        return 7;
    }

    if (shape == YGHV_ACC_MOV8) {
        out[0] = 0x8A;
        out[1] = 0x81;
        out[2] = (unsigned char)(disp & 0xFF);
        out[3] = (unsigned char)((disp >> 8) & 0xFF);
        out[4] = (unsigned char)((disp >> 16) & 0xFF);
        out[5] = (unsigned char)((disp >> 24) & 0xFF);
        out[6] = 0xC3;
        return 7;
    }

    if (shape == YGHV_ACC_MOV64_8) {
        if (disp > 0x7F) return 0;
        out[0] = 0x48;
        out[1] = 0x8B;
        out[2] = 0x41;
        out[3] = (unsigned char)disp;
        out[4] = 0xC3;
        return 5;
    }

    return 0;
}

/* Human name of a shape, for logs. Returns a static string; no allocation. */
static __inline const char *yghv_acc_shape_name(unsigned int shape)
{
    switch (shape) {
    case YGHV_ACC_MOV64:   return "mov64 [rcx+disp32]";
    case YGHV_ACC_LEA64:   return "lea64 [rcx+disp32]";
    case YGHV_ACC_MOV8:    return "mov8  [rcx+disp32]";
    case YGHV_ACC_MOV64_8: return "mov64 [rcx+disp8]";
    case YGHV_ACC_MOV32:   return "mov32 [rcx+disp32]";
    default:               return "unknown";
    }
}

#endif /* YGHV_ACCESSOR_H */
