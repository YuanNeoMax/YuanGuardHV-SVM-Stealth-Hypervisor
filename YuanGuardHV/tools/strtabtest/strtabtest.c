/*
 * strtabtest: decode the generated string table and print it.
 *
 * WHY USER MODE
 *   The table is consumed by the driver, but checking it from inside the
 *   driver would mean a test that runs only after the driver loads, on a
 *   machine where it may not load at all. The decode path is a static inline
 *   over a const array, so the same text compiles here and there: this program
 *   exercises exactly the bytes the driver will use.
 *
 * WHAT IT PROVES
 *   That each entry decodes to a name that (a) is printable ASCII, (b) starts
 *   with "Ps", and (c) is one of the names the generator was given. A wrong
 *   key or a wrong word index fails here, and the driver would then be looking
 *   up a name that does not exist.
 */
#include <stdio.h>
#include <string.h>
#include "yghv_strtab.h"

/* Kept in the same order as the generator list; the test compares the decoded
 * text against these, so a reordering in one place and not the other fails. */
static const char *const g_expect[YGHV_SID_COUNT] = {
    "PsGetCurrentProcessId",
    "PsGetProcessId",
    "PsGetProcessImageFileName",
    "PsGetProcessPeb",
    "PsGetProcessProtection",
    "PsGetProcessSectionBaseAddress",
    "PsGetThreadId",
    "PsGetThreadProcess",
    "PsGetThreadTeb",
    "PsInitialSystemProcess",
    "PsLoadedModuleList",
};

int main(void)
{
    int i, bad = 0;

    printf("strtabtest: %d entries\n", (int)YGHV_SID_COUNT);

    for (i = 0; i < (int)YGHV_SID_COUNT; i++) {
        char narrow[YGHV_STR_MAX_CHARS];
        unsigned short wide[YGHV_STR_MAX_CHARS];
        int j, wide_ok = 1;

        if (!yghv_str_get(i, narrow, sizeof(narrow))) {
            printf("  [%2d] DECODE FAILED\n", i);
            bad++;
            continue;
        }
        if (!yghv_str_get_w(i, wide, YGHV_STR_MAX_CHARS)) {
            printf("  [%2d] WIDE DECODE FAILED\n", i);
            bad++;
            continue;
        }
        for (j = 0; narrow[j]; j++)
            if (wide[j] != (unsigned short)(unsigned char)narrow[j]) wide_ok = 0;
        if (wide[j] != 0) wide_ok = 0;

        if (strcmp(narrow, g_expect[i]) != 0) {
            printf("  [%2d] MISMATCH got \"%s\" want \"%s\"\n",
                   i, narrow, g_expect[i]);
            bad++;
            continue;
        }
        if (!wide_ok) {
            printf("  [%2d] WIDE MISMATCH for \"%s\"\n", i, narrow);
            bad++;
            continue;
        }
        printf("  [%2d] %s\n", i, narrow);
    }

    if (!yghv_strtab_selftest()) {
        printf("selftest(): FAILED\n");
        bad++;
    }

    printf("%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS");
    return bad ? 1 : 0;
}
