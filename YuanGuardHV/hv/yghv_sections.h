/*
 * yghv_sections.h — Tier 7.1: the driver's own PE sections, as data.
 *
 * See yghv_sections.c for why this parser exists before any hiding code. Short
 * version: hiding the driver's data pages is possible, hiding its CODE pages is
 * not (no ALLOW loop, and every present=0 code page is a fetch fault we cannot
 * serve). So the hide has to be told which sections are executable - and a
 * parser written inside the hide path turns a parser bug into a bugcheck.
 */
#ifndef YGHV_SECTIONS_H
#define YGHV_SECTIONS_H

#include "svm_defs.h"
#include <ntddk.h>

typedef struct {
    char     name[12];   /* PE section name, NUL terminated by us */
    uint32_t va;         /* RVA from the image base */
    uint32_t size;       /* VirtualSize, or SizeOfRawData when VirtualSize is 0 */
    uint32_t executable; /* 1 = IMAGE_SCN_MEM_EXECUTE: must NOT be hidden */
    uint32_t writable;   /* 1 = IMAGE_SCN_MEM_WRITE */
} yghv_section_info_t;

/*
 * Walk the PE headers of an in-memory image and fill `out`.
 *
 * Returns 1 and sets *count_out when the headers parsed. Returns 0 on any
 * boundary violation - a short image, a bad MZ/PE magic, or a section table that
 * would run past the allocation - in which case the caller must treat every page
 * as NOT hideable rather than guess.
 *
 * Read only: no NPT writes, no allocation, no locks. Safe to call at PASSIVE.
 *
 * `flags_out` receives 1 for PE32, 2 for PE32+, 0 for an unrecognised magic, so a
 * caller can tell "parsed a 32-bit image" from "parsed nothing".
 */
int yghv_sections_report(uint8_t *image_base, uint32_t image_size,
                         yghv_section_info_t *out, uint32_t out_max,
                         uint32_t *count_out, uint32_t *flags_out);

#endif /* YGHV_SECTIONS_H */
