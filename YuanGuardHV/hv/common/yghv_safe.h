/*
 * yghv_safe.h -- the ONE place a pointer of unknown validity is dereferenced.
 *
 * 9.608. THIS FILE EXISTS BECAUSE OF TWO BUGCHECKS, both 0x50
 * PAGE_FAULT_IN_NONPAGED_AREA, both inside code that was CERTAIN it had
 * validated the pointer first:
 *
 *   #1  yghv_offsets_init     read ffff9e804c7d1ec0   (ActiveProcessLinks probe)
 *   #2  yghv_sig_readable_image read ffff84a988200000 (module image header read)
 *
 * Both addresses are CANONICAL and one is PAGE-ALIGNED. Both passed every test
 * their caller made, because the test was:
 *
 *     if (p >= 0xFFFF800000000000ULL) { ... dereference ... }
 *
 * "In the upper half of the address space" is not "mapped". x64 kernel space is
 * 128 TB of mostly EMPTY address space with a few windows that have PTEs; a
 * value that is merely canonical says nothing at all about whether a page backs
 * it. Nineteen call sites in this tree made that comparison and then read
 * through the pointer.
 *
 * WHY THIS IS A HEADER AND NOT THREE NOINLINE FUNCTIONS IN ONE .c
 *   The first fix marked three helpers __declspec(noinline) so their __try
 *   blocks would survive compilation. That was necessary and it worked -- the
 *   .map grew symbols for all three -- and the very next boot bugchecked in a
 *   FOURTH function that had the same flaw and no __try at all. Patching one
 *   function at a time is how you spend a boot per defect. The rule has to be
 *   structural: if a pointer's validity is unknown, it goes through THIS file.
 *
 * THE LAYERING, and why all three parts are required
 *   1. canonical   -- rejects user-space and non-canonical values with no OS call.
 *   2. MmIsAddressValid -- asks the memory manager whether a PTE exists. This is
 *      the question a range comparison cannot answer. It is documented as a
 *      RACEABLE hint: a page can be unmapped the instant after it returns TRUE.
 *   3. __try/__except -- the authoritative answer. (2) exists so that the common
 *      "canonical garbage" case does not need an exception; (3) exists because
 *      (2) cannot promise anything.
 *
 *   NOINLINE on every function here. A __try block inside an inlined callee is
 *   at the optimizer's discretion, and this tree has already paid for that
 *   twice. The cost is a call; the alternative is a bugcheck.
 *
 * IRQL: MmIsAddressValid is callable at DISPATCH_LEVEL or below and must not be
 * used on pageable memory at raised IRQL. Every caller in this tree runs at
 * PASSIVE (DriverEntry, IOCTL dispatch, unload). If a VM-exit path ever needs
 * this, it must pass the raw pointer to a PASSIVE deferral instead -- a
 * bugcheck from inside a VM exit is not survivable.
 */
#ifndef YGHV_SAFE_H
#define YGHV_SAFE_H

#include <ntddk.h>
#include <stdint.h>

/* The one boundary. Named so that "canonical" is defined in exactly one place. */
#define YGHV_SAFE_KERNEL_MIN 0xFFFF800000000000ULL

/*
 * Is this value a kernel pointer worth attempting to read?
 *
 * This is a FILTER, not a guarantee -- it is deliberately cheap and it does NOT
 * call into the memory manager. Use yghv_safe_read64/mem for the actual read;
 * use this only to skip obviously impossible candidates before doing real work.
 */
static __inline int yghv_safe_is_kernel_ptr(uint64_t p)
{
    return p >= YGHV_SAFE_KERNEL_MIN;
}

/*
 * Guarded read of N bytes. Returns 1 on success, 0 if the range is not
 * readable. *out is written only on success.
 *
 * The whole range is checked, not just its first byte: a 15-byte image name
 * that starts in a mapped page and ends in an unmapped one is exactly the case
 * a first-byte-only check lets through.
 */
__declspec(noinline)
int yghv_safe_read(const void *src, void *dst, unsigned long len);

/* Guarded 8-byte scalar read. */
__declspec(noinline)
int yghv_safe_read64(const void *src, uint64_t *out);

/* Guarded 4-byte scalar read. */
__declspec(noinline)
int yghv_safe_read32(const void *src, uint32_t *out);
__declspec(noinline)
int yghv_safe_read16(const void *src, uint16_t *out);
__declspec(noinline)
int yghv_safe_read8(const void *src, uint8_t *out);

/*
 * Guarded write of N bytes. NOT a convenience wrapper -- a write to an
 * unvalidated kernel pointer corrupts whatever is there, and unlike a read
 * there is no exception boundary that makes it survivable in general.
 * Provided for the probe paths that need it; never use it on data you have not
 * established the layout of.
 */
__declspec(noinline)
int yghv_safe_write(void *dst, const void *src, unsigned long len);

/*
 * Is the whole [p, p+len) range mapped right now?
 *
 * Answers the question yghv_safe_read *would* answer, without copying. Use it
 * when you need the answer before choosing a path, not the bytes.
 */
__declspec(noinline)
int yghv_safe_range_ok(const void *p, unsigned long len);

#endif /* YGHV_SAFE_H */
