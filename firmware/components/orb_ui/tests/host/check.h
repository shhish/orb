#pragma once

#include <math.h>
#include <stdio.h>

static int s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) < (eps))

static int finish(const char *what)
{
    if (s_failed) {
        fprintf(stderr, "%s: %d check(s) failed\n", what, s_failed);
        return 1;
    }
    printf("%s: all tests passed\n", what);
    return 0;
}
