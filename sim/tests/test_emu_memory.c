/* Emulator allocation accounting, including the concurrent draw-worker path. */
#include "lvgl.h"

#include <pthread.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;

#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "emu memory: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static lv_mem_monitor_t monitor(void)
{
    lv_mem_monitor_t result;
    lv_mem_monitor(&result);
    return result;
}

static void *worker(void *context)
{
    unsigned char marker = (unsigned char)(uintptr_t)context;
    for (unsigned i = 0; i < 2000; ++i) {
        unsigned char *value = lv_malloc(64);
        if (!value || (uintptr_t)value % alignof(max_align_t)) return (void *)1;
        memset(value, marker, 64);
        unsigned char *resized = lv_realloc(value, 129);
        if (!resized) { lv_free(value); return (void *)1; }
        for (unsigned j = 0; j < 64; ++j) {
            if (resized[j] != marker) { lv_free(resized); return (void *)1; }
        }
        lv_free(resized);
    }
    return NULL;
}

int main(void)
{
    lv_init();
    lv_mem_monitor_t baseline = monitor();
    unsigned char *value = lv_malloc(37);
    CHECK(value != NULL);
    CHECK((uintptr_t)value % alignof(max_align_t) == 0);
    memset(value, 0x5a, 37);
    lv_mem_monitor_t current = monitor();
    CHECK(current.total_size == baseline.total_size + 37);
    CHECK(current.used_cnt == baseline.used_cnt + 1);
    CHECK(current.max_used >= current.total_size);
    CHECK(current.used_pct == 100);
    CHECK(current.free_size == 0 && current.free_cnt == 0 &&
          current.free_biggest_size == 0 && current.frag_pct == 0);

    unsigned char *resized = lv_realloc(value, 333);
    CHECK(resized != NULL);
    CHECK((uintptr_t)resized % alignof(max_align_t) == 0);
    for (unsigned i = 0; i < 37; ++i) CHECK(resized[i] == 0x5a);
    current = monitor();
    CHECK(current.total_size == baseline.total_size + 333);
    CHECK(current.used_cnt == baseline.used_cnt + 1);

    /* Overflow is rejected before calling malloc; a failed realloc leaves both
     * its source and accounting intact, independent of host overcommit policy. */
    CHECK(lv_malloc(SIZE_MAX) == NULL);
    CHECK(lv_realloc(resized, SIZE_MAX) == NULL);
    CHECK(monitor().total_size == current.total_size);
    CHECK(monitor().used_cnt == current.used_cnt);
    for (unsigned i = 0; i < 37; ++i) CHECK(resized[i] == 0x5a);

    value = lv_realloc(resized, 9);
    CHECK(value != NULL);
    CHECK(monitor().total_size == baseline.total_size + 9);
    for (unsigned i = 0; i < 9; ++i) CHECK(value[i] == 0x5a);
    lv_free(value);
    value = lv_realloc(NULL, 29);
    CHECK(value != NULL);
    CHECK(monitor().total_size == baseline.total_size + 29);
    value = lv_realloc(value, 0);
    CHECK(value != NULL); /* LVGL's zero-size sentinel is deliberately shared. */
    lv_free(value);
    lv_free(lv_malloc(0));
    lv_free(NULL);
    unsigned char *zeroed = lv_calloc(13, 7);
    CHECK(zeroed != NULL);
    for (unsigned i = 0; i < 91; ++i) CHECK(zeroed[i] == 0);
    lv_free(zeroed);
    CHECK(lv_calloc(SIZE_MAX, 2) == NULL);
    CHECK(monitor().total_size == baseline.total_size);
    CHECK(monitor().used_cnt == baseline.used_cnt);

    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1)) == 0);
    for (unsigned i = 0; i < 4; ++i) {
        void *result = NULL;
        CHECK(pthread_join(threads[i], &result) == 0);
        CHECK(result == NULL);
    }
    CHECK(monitor().total_size == baseline.total_size);
    CHECK(monitor().used_cnt == baseline.used_cnt);
    CHECK(monitor().max_used >= baseline.total_size + 333);
    CHECK(lv_mem_test() == LV_RESULT_OK);
    CHECK(lv_mem_add_pool(NULL, 0) == NULL);
    lv_mem_remove_pool(NULL);
    lv_deinit();
    printf("emu memory: %u checks passed, 8000 concurrent reallocations\n", s_checks);
    return 0;
}
