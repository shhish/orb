#pragma once

#include <stdio.h>
#include <string.h>

extern int g_checks, g_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_STR(got, want)                                                                         \
    do {                                                                                             \
        const char *g_ = (got), *w_ = (want);                                                        \
        g_checks++;                                                                                  \
        if (!g_ || strcmp(g_, w_) != 0) {                                                            \
            g_failures++;                                                                            \
            fprintf(stderr, "%s:%d:\n  got  %s\n  want %s\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_); \
        }                                                                                            \
    } while (0)

void test_sas(void);
void test_proto(void);
void test_backoff(void);

void test_control(void);
