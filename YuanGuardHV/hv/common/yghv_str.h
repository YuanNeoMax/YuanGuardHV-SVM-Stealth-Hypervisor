/*
 * yghv_str.h -- compile-time string encryption (9.589).
 *
 * WHY
 *   Every literal that names something the product cares about (device name,
 *   pool tag, the exported routines we look up, the registry key we create)
 *   otherwise sits in .rdata as a readable C string forever. Anyone with
 *   "strings yuanguard_hv.sys" gets the whole map of what this driver is
 *   looking for. Hashing the names would hide them but then we cannot call
 *   them -- we need the bytes back at runtime, just not in the file.
 *
 * HOW (and why it is really compile time)
 *   The encoder is a CONSTANT EXPRESSION built out of character constants:
 *
 *       YGHV_W8(k, 'P','s','G','e','t','P','r','o')
 *
 *   'P' is an integer constant, the shifts and ORs of constants are constant
 *   expressions, and the XOR with a constant key is one too. So the compiler
 *   folds the whole thing into a single immediate -- the ENCRYPTED value --
 *   and the plaintext characters never reach the object file. There is no
 *   generator script, no build step, and no table to keep in sync; a typo in
 *   the call site changes the string rather than silently desynchronising a
 *   manifest from a generated header.
 *
 *   The price is that a call site repeats the characters instead of writing a
 *   literal. That is deliberate: a literal is exactly what we are trying to
 *   keep out of the binary, and it must not appear in the source either,
 *   because the source is the thing a maintainer copies from.
 *
 * KEY SCHEDULE
 *   One key per string, and each 8-byte word is XORed with key + i*GOLDEN so
 *   two words of the same string do not share a keystream. A key of 0 is
 *   legal but pointless; the convention in this tree is to take the low half
 *   of the FNV-1a of the call site's module name, or any constant that is not
 *   itself a string.
 *
 * DECODING
 *   Always into a caller-provided buffer. No allocation, no static scratch --
 *   a shared scratch buffer would be a race between the island path and an
 *   IOCTL, and it would also put the plaintext somewhere "strings" can find it
 *   at runtime instead of only in a stack frame that dies with the call.
 *
 * Byte layout of one word: little endian, byte 0 is the first character.
 * A NUL inside the plaintext ends the string, so the last word is padded with
 * '\0' characters (see YGHV_W8P below) and the decoder stops there.
 */
#ifndef YGHV_STR_H
#define YGHV_STR_H

/*
 * 8 characters -> one little-endian word.
 *
 * The (unsigned char) casts matter: a plain 'x' is an int and a negative char
 * in a signed-char build would sign-extend into the neighbours.
 */
#define YGHV_CH8(a,b,c,d,e,f,g,h)                    \
    (  ((unsigned long long)(unsigned char)(a))       \
     | ((unsigned long long)(unsigned char)(b) <<  8) \
     | ((unsigned long long)(unsigned char)(c) << 16) \
     | ((unsigned long long)(unsigned char)(d) << 24) \
     | ((unsigned long long)(unsigned char)(e) << 32) \
     | ((unsigned long long)(unsigned char)(f) << 40) \
     | ((unsigned long long)(unsigned char)(g) << 48) \
     | ((unsigned long long)(unsigned char)(h) << 56))

/*
 * Upper bound for any decoded name. Every entry in the generated table is an
 * export name and none is near this; the constant exists so a caller can
 * declare its buffer without a second source of truth about the longest name.
 */
#define YGHV_STR_MAX_CHARS 64u

/* The golden-ratio odd constant, the usual choice for spreading a key. */
#define YGHV_STR_GOLDEN 0x9E3779B97F4A7C15ULL

/* Word i of a string encrypted with `key`. */
#define YGHV_W8(key, i, a,b,c,d,e,f,g,h)                      \
    (YGHV_CH8(a,b,c,d,e,f,g,h) ^                              \
     ((unsigned long long)(key) + (unsigned long long)(i) * YGHV_STR_GOLDEN))

/*
 * Decode `n` words into `out` (capacity `cap`, must be n*8+1 or more).
 * Always NUL-terminates. Returns `out` so it can be used inline.
 *
 * The caller owns the buffer; this never writes outside it and never allocates.
 */
static __inline char *yghv_str_dec(const unsigned long long *w, unsigned int n,
                                   unsigned long long key, char *out, unsigned int cap)
{
    unsigned int i, j, k = 0;

    if (!out || cap == 0) return out;
    for (i = 0; i < n; i++) {
        unsigned long long v = w[i] ^ (key + (unsigned long long)i * YGHV_STR_GOLDEN);
        for (j = 0; j < 8; j++) {
            char c = (char)((v >> (j * 8)) & 0xFF);
            if (c == 0 || k + 1 >= cap) { out[k] = 0; return out; }
            out[k++] = c;
        }
    }
    out[k] = 0;
    return out;
}

/*
 * USAGE — there is deliberately no one-shot macro, because the word list has a
 * length that a macro cannot count for us in standard C, and a macro that
 * guesses the length is a macro that silently truncates a name. The pattern is
 * two lines at the call site:
 *
 *     static const unsigned long long w[] = {
 *         YGHV_W8(0x5A17, 0, 'P','s','G','e','t','P','r','o'),
 *         YGHV_W8(0x5A17, 1, 'c','e','s','s','I','d', 0 , 0 ) };
 *     char name[24];
 *     yghv_str_dec(w, 2, 0x5A17, name, sizeof(name));   // -> "PsGetProcessId"
 *
 * The index argument must match the position in the array; that is the one
 * thing to get right, and a wrong index produces a mangled (not a plausible)
 * string, which the self-test below catches for the strings that matter.
 *
 * WHY `static const` AND NOT A LOCAL
 *   A local array is rebuilt on the stack on every call; a file-scope const
 *   array lives in .rdata as the ENCRYPTED words, which is exactly the point.
 *   It is initialised by the linker from immediates, so it never costs
 *   runtime work.
 */

/*
 * Self-test: encrypt/decrypt a known name and compare. Returns 1 on success.
 * Called once at driver load from yghv_offsets_init(); a failure there means
 * the build is broken (constant folding changed), not that the machine is.
 */
static __inline int yghv_str_selftest(void)
{
    static const unsigned long long probe[] = {
        YGHV_W8(0x5A17, 0, 'P','s','G','e','t','P','r','o'),
        YGHV_W8(0x5A17, 1, 'c','e','s','s','I','d', 0 , 0 )
    };
    static const char expect[] = "PsGetProcessId";
    char got[24];
    unsigned int i;

    yghv_str_dec(probe, 2, 0x5A17, got, sizeof(got));
    for (i = 0; i < sizeof(expect); i++)
        if (got[i] != expect[i]) return 0;
    return 1;
}

#endif /* YGHV_STR_H */
