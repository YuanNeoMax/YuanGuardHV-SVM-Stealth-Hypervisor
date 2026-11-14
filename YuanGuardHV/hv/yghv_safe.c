/*
 * yghv_safe.c -- the implementation of the one guarded-dereference layer.
 * See common/yghv_safe.h for WHY this exists (two 0x50 bugchecks, both in code
 * that believed it had already validated the pointer).
 *
 * EVERY function here is NOINLINE. That is not a performance opinion: a __try
 * block that the compiler has inlined into its caller is a __try block whose
 * scope the optimizer decided, and this tree has bugchecked twice on exactly
 * that. The .map is the test -- if a function in this file stops appearing in
 * it, the guard has been dissolved and the build must not ship.
 */
#include "common/yghv_safe.h"
#include "common/debug.h"

/*
 * One range check, used by every reader.
 *
 * The end is computed with an explicit overflow test rather than by adding and
 * comparing: `p + len` can wrap, and a wrapped end is SMALLER than p, which
 * makes a naive `end >= start` test pass for a range that starts near 2^64.
 */
static __inline int yghv_safe_range_precheck(const void *p, unsigned long len)
{
    uint64_t start = (uint64_t)p;
    uint64_t end;

    if (len == 0) return 0;
    if (start < YGHV_SAFE_KERNEL_MIN) return 0;          /* not kernel space */

    end = start + (uint64_t)len - 1;
    if (end < start) return 0;                            /* wrapped */

    /*
     * 9.608 -- DO NOT REQUIRE THE LAST PAGE TO BE MAPPED WHEN THE READ FITS IN
     * ONE PAGE. Only the FIRST page is *needed*: an 8-byte read whose start is
     * in a live page reads live bytes as long as it does not cross a page
     * boundary. Demanding MmIsAddressValid(end) rejected perfectly readable
     * scalars whenever they happened to sit at the very end of a page, and that
     * is not hypothetical -- it silently broke the ActiveProcessLinks probe:
     *
     *   the probe walks PsInitialSystemProcess + 0x100 .. +0x8F8, and several of
     *   those candidates straddle a page boundary. Requiring both ends to be
     *   mapped rejected every one of them, the probe returned "no list found",
     *   YGHV_OFF_LINKS never got set (derived_mask came back 0x28 instead of
     *   0x2B), and IOCTL 0x838 op 0 returned STATUS_NOT_IMPLEMENTED, which the
     *   client shows as "Win32 error 1". The machine looked broken for a reason
     *   that had nothing to do with the machine.
     *
     * So: check the first page always; check the last page only when the range
     * genuinely spans more than one. The __try below is still the authority --
     * MmIsAddressValid is a hint, and a hint that says "no" too often is as
     * wrong as one that says "yes" too often.
     */
    if (!MmIsAddressValid((PVOID)start)) return 0;
    if ((start & ~0xFFFULL) != (end & ~0xFFFULL)) {
        /* The read crosses a page boundary: the far page must be live too. */
        if (!MmIsAddressValid((PVOID)end)) return 0;
    }
    return 1;
}

__declspec(noinline)
int yghv_safe_range_ok(const void *p, unsigned long len)
{
    return yghv_safe_range_precheck(p, len);
}

__declspec(noinline)
int yghv_safe_read(const void *src, void *dst, unsigned long len)
{
    if (!dst || len == 0) return 0;
    if (!yghv_safe_range_precheck(src, len)) return 0;

    __try {
        RtlCopyMemory(dst, src, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

__declspec(noinline)
int yghv_safe_read64(const void *src, uint64_t *out)
{
    uint64_t v = 0;

    if (!out) return 0;
    if (!yghv_safe_range_precheck(src, sizeof(v))) return 0;

    __try {
        v = *(const volatile uint64_t *)src;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    *out = v;
    return 1;
}

__declspec(noinline)
int yghv_safe_read32(const void *src, uint32_t *out)
{
    uint32_t v = 0;

    if (!out) return 0;
    if (!yghv_safe_range_precheck(src, sizeof(v))) return 0;

    __try {
        v = *(const volatile uint32_t *)src;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    *out = v;
    return 1;
}

__declspec(noinline)
int yghv_safe_read16(const void *src, uint16_t *out)
{
    uint16_t v = 0;

    if (!out) return 0;
    if (!yghv_safe_range_precheck(src, sizeof(v))) return 0;

    __try {
        v = *(const volatile uint16_t *)src;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    *out = v;
    return 1;
}

__declspec(noinline)
int yghv_safe_read8(const void *src, uint8_t *out)
{
    uint8_t v = 0;

    if (!out) return 0;
    if (!yghv_safe_range_precheck(src, sizeof(v))) return 0;

    __try {
        v = *(const volatile uint8_t *)src;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    *out = v;
    return 1;
}

__declspec(noinline)
int yghv_safe_write(void *dst, const void *src, unsigned long len)
{
    if (!dst || !src || len == 0) return 0;
    if (!yghv_safe_range_precheck(dst, len)) return 0;

    __try {
        RtlCopyMemory(dst, src, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}
