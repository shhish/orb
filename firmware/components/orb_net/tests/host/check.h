#pragma once

/* The host tests' checks: count, print the failing line, carry on. */

#include <stdio.h>
#include <string.h>

static int s_checks, s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            s_failed++;                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_STR(a, b)                                                 \
    do {                                                                \
        s_checks++;                                                     \
        if (strcmp((a), (b)) != 0) {                                    \
            s_failed++;                                                 \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                               \
    } while (0)

static inline int check_report(void)
{
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
