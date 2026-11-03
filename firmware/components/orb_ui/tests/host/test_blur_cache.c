/* Exercise the production exact-backdrop cache through real blur draws.
 * Fixtures include padded rows and guard bytes. Allocation hooks deliberately
 * reenter diagnostics, fail and block; concurrent workers own separate layers.
 */
#define main original_blur_regression_main
#include "test_blur.c"
#undef main
#include "src/draw/sw/lv_draw_sw_blur_cache_private.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sched.h>

enum { BUDGET = 192 * 1024, ENTRY_LIMIT = 96 * 1024, THREADS = 8 };
static pthread_mutex_t s_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_alloc_cv = PTHREAD_COND_INITIALIZER;
static size_t s_live_bytes, s_peak_bytes, s_limit = BUDGET;
static unsigned s_allocations;
static int s_fail_next;
static bool s_pause_alloc, s_alloc_entered, s_resume_alloc;
static atomic_int s_thread_failures;

typedef union { size_t bytes; max_align_t alignment; } allocation_header_t;

static void * tracked_alloc(size_t size)
{
    /* If cache metadata is held while calling us, this deadlocks the test. */
    lv_draw_sw_blur_cache_stats_t stats;
    lv_draw_sw_blur_cache_get_stats(&stats);
    pthread_mutex_lock(&s_alloc_lock);
    s_allocations++;
    if(s_pause_alloc) {
        s_alloc_entered = true;
        pthread_cond_broadcast(&s_alloc_cv);
        while(!s_resume_alloc) pthread_cond_wait(&s_alloc_cv, &s_alloc_lock);
    }
    bool fail = s_fail_next > 0;
    if(fail) s_fail_next--;
    pthread_mutex_unlock(&s_alloc_lock);
    if(fail) return NULL;
    allocation_header_t * p = malloc(sizeof(*p) + size);
    if(!p) return NULL;
    p->bytes = size;
    pthread_mutex_lock(&s_alloc_lock);
    s_live_bytes += size;
    if(s_live_bytes > s_peak_bytes) s_peak_bytes = s_live_bytes;
    if(s_live_bytes > s_limit) atomic_fetch_add(&s_thread_failures, 1);
    pthread_mutex_unlock(&s_alloc_lock);
    return p + 1;
}

static void tracked_free(void * pointer)
{
    lv_draw_sw_blur_cache_stats_t stats;
    lv_draw_sw_blur_cache_get_stats(&stats);
    allocation_header_t * p = (allocation_header_t *)pointer - 1;
    pthread_mutex_lock(&s_alloc_lock);
    if(s_live_bytes < p->bytes) atomic_fetch_add(&s_thread_failures, 1);
    s_live_bytes -= p->bytes;
    pthread_mutex_unlock(&s_alloc_lock);
    free(p);
}

static lv_draw_sw_blur_cache_config_t config_for(size_t budget, size_t entry_limit)
{
    return (lv_draw_sw_blur_cache_config_t){
        .budget_bytes = budget, .max_entry_bytes = entry_limit,
        .alloc_cb = tracked_alloc, .free_cb = tracked_free
    };
}

static void configure(size_t budget, size_t entry_limit)
{
    lv_draw_sw_blur_cache_config_t config = config_for(budget, entry_limit);
    CHECK(lv_draw_sw_blur_cache_configure(&config) == LV_RESULT_OK);
    CHECK(s_live_bytes == 0);
    s_limit = budget;
    s_peak_bytes = 0;
}

static lv_draw_sw_blur_cache_stats_t stats(void)
{
    lv_draw_sw_blur_cache_stats_t s;
    lv_draw_sw_blur_cache_get_stats(&s);
    CHECK(s.bytes <= s.budget_bytes);
    CHECK(s.peak_bytes <= s.budget_bytes);
    CHECK(s.leased_entries == 0);
    return s;
}

typedef struct {
    fixture_t f;
    lv_draw_blur_dsc_t dsc;
    lv_area_t coords;
    uint8_t * expected;
} cache_case_t;

static uint32_t random_value(uint32_t * seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}

static void case_init(cache_case_t * c, int width, int height, lv_color_format_t cf, unsigned variant)
{
    fixture_init(&c->f, width, height, cf, variant % 2 ? -13 : 11, variant % 3 ? 27 : -19);
    uint32_t seed = 1234567 + variant;
    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width * c->f.bytes; x++) pixel(&c->f, 0, y)[x] = random_value(&seed) >> 24;
    }
    c->dsc = descriptor(6 + variant % 25, variant % 5 ? 12 : LV_RADIUS_CIRCLE, 37 + variant % 217);
    c->dsc.tint = lv_color_hex(0x0a8cde + variant * 1231);
    c->dsc.quality = variant % 3;
    c->coords = c->f.layer.buf_area;
    if(variant % 2) { c->coords.x1 -= 5; c->coords.y2 += 9; }
    if(variant % 3) { c->f.task.clip_area.x1 += width / 4; c->f.task.clip_area.y2 -= height / 5; }
    c->f.task.opa = variant % 2 ? 177 : 255;
    save_before(&c->f);
    c->expected = malloc(c->f.size);
    CHECK(c->expected != NULL);
    if(!c->expected) exit(2);
    lv_draw_sw_blur(&c->f.task, &c->dsc, &c->coords);
    memcpy(c->expected, c->f.allocation, c->f.size);
    memcpy(c->f.allocation, c->f.before, c->f.size);
}

static bool case_run(cache_case_t * c)
{
    memcpy(c->f.allocation, c->f.before, c->f.size);
    lv_draw_sw_blur(&c->f.task, &c->dsc, &c->coords);
    return memcmp(c->f.allocation, c->expected, c->f.size) == 0;
}

static void case_free(cache_case_t * c)
{
    free(c->expected);
    fixture_free(&c->f);
}

static void refresh_reference(cache_case_t * c)
{
    configure(0, 0);
    memcpy(c->f.allocation, c->f.before, c->f.size);
    lv_draw_sw_blur(&c->f.task, &c->dsc, &c->coords);
    memcpy(c->expected, c->f.allocation, c->f.size);
    configure(BUDGET, ENTRY_LIMIT);
}

static void test_disabled_and_formats(void)
{
    CHECK(stats().budget_bytes == 0);
    const lv_color_format_t formats[] = {LV_COLOR_FORMAT_A8, LV_COLOR_FORMAT_RGB565,
        LV_COLOR_FORMAT_RGB565_SWAPPED, LV_COLOR_FORMAT_RGB888, LV_COLOR_FORMAT_ARGB8888};
    for(unsigned fmt = 0; fmt < sizeof(formats) / sizeof(formats[0]); fmt++) {
        for(unsigned variant = 0; variant < 36; variant++) {
            configure(0, 0);
            cache_case_t c;
            case_init(&c, 1 + variant * 2, 3 + variant, formats[fmt], variant);
            CHECK(case_run(&c));
            CHECK(stats().misses == 0);
            configure(BUDGET, ENTRY_LIMIT);
            CHECK(case_run(&c));
            CHECK(stats().misses == 1);
            CHECK(case_run(&c));
            CHECK(stats().hits == 1);
            CHECK(case_run(&c));
            CHECK(stats().hits == 2);
            case_free(&c);
        }
    }
    configure(0, 0);
}

static void test_source_mutation_and_identity(void)
{
    cache_case_t c, other;
    case_init(&c, 57, 39, LV_COLOR_FORMAT_ARGB8888, 0);
    case_init(&other, 57, 39, LV_COLOR_FORMAT_ARGB8888, 0);
    configure(BUDGET, ENTRY_LIMIT);
    CHECK(case_run(&c));
    CHECK(case_run(&other)); /* Different layer/task/buffer pointers, equal input. */
    CHECK(stats().hits == 1);
    for(unsigned i = 0; i < 96; i++) {
        size_t offset = GUARD + (i * 7 % c.f.height) * c.f.stride + (i * 17 % (c.f.width * c.f.bytes));
        c.f.before[offset] ^= 1;
        refresh_reference(&c);
        CHECK(case_run(&other)); /* Populate original input for the same key. */
        uint64_t misses = stats().misses;
        CHECK(case_run(&c));
        CHECK(stats().misses == misses + 1);
        CHECK(case_run(&c));
        CHECK(stats().hits == 1);
    }
    configure(0, 0);
    case_free(&other);
    case_free(&c);
}

static void test_semantic_keys(void)
{
    cache_case_t cases[11];
    for(unsigned i = 0; i < 11; i++) case_init(&cases[i], 37, 29, LV_COLOR_FORMAT_RGB565_SWAPPED, 0);
    cases[1].dsc.blur_radius++;
    cases[2].dsc.corner_radius = 7;
    cases[3].dsc.quality = LV_BLUR_QUALITY_PRECISION;
    cases[4].dsc.tint.red++;
    cases[5].dsc.tint_opa++;
    cases[6].f.task.opa--;
    cases[7].coords.x1--;
    cases[8].f.task.clip_area.x1++;
    cases[9].f.task.clip_area.y2--;
    cases[10].dsc.downsample_factor = 4;
    for(unsigned i = 0; i < 11; i++) {
        configure(0, 0);
        memcpy(cases[i].f.allocation, cases[i].f.before, cases[i].f.size);
        lv_draw_sw_blur(&cases[i].f.task, &cases[i].dsc, &cases[i].coords);
        memcpy(cases[i].expected, cases[i].f.allocation, cases[i].f.size);
    }
    configure(BUDGET, ENTRY_LIMIT);
    for(unsigned i = 0; i < 11; i++) {
        uint64_t misses = stats().misses;
        CHECK(case_run(&cases[i]));
        CHECK(stats().misses == misses + 1);
        CHECK(case_run(&cases[i]));
    }
    CHECK(stats().evictions >= 6);
    for(unsigned i = 0; i < 11; i++) case_free(&cases[i]);
    configure(0, 0);
}

static void test_eviction_failure_and_limits(void)
{
    cache_case_t small, medium, large;
    case_init(&small, 23, 11, LV_COLOR_FORMAT_RGB565, 0);
    case_init(&medium, 47, 23, LV_COLOR_FORMAT_RGB565, 0);
    case_init(&large, 250, 72, LV_COLOR_FORMAT_RGB565, 0);
    configure(5000, 5000);
    CHECK(case_run(&small));
    CHECK(case_run(&medium));
    CHECK(stats().evictions == 1);
    CHECK(case_run(&small));
    CHECK(stats().evictions == 2);
    CHECK(s_peak_bytes <= 5000);
    CHECK(case_run(&large));
    CHECK(stats().oversize_bypasses == 1);
    configure(BUDGET, ENTRY_LIMIT);
    s_fail_next = 1;
    CHECK(case_run(&small));
    CHECK(stats().allocation_failures == 1);
    CHECK(stats().bytes == 0);
    CHECK(case_run(&small));
    CHECK(case_run(&small));
    CHECK(stats().hits == 1);
    uint64_t hits = stats().hits;
    CHECK(lv_draw_sw_blur_cache_clear() == LV_RESULT_OK);
    CHECK(stats().bytes == 0 && stats().hits == hits);
    CHECK(case_run(&large));
    CHECK(stats().bytes == 250 * 72 * 4);
    configure(BUDGET, 0); /* Documented default per-entry bound. */
    cache_case_t oversize;
    configure(0, 0);
    case_init(&oversize, 466, 104, LV_COLOR_FORMAT_RGB565, 0);
    configure(BUDGET, 0);
    CHECK(case_run(&oversize));
    CHECK(stats().oversize_bypasses == 1 && stats().bytes == 0);
    lv_draw_sw_blur_cache_config_t bad = config_for(BUDGET, ENTRY_LIMIT);
    bad.free_cb = NULL;
    CHECK(lv_draw_sw_blur_cache_configure(&bad) == LV_RESULT_INVALID);
    CHECK(lv_draw_sw_blur_cache_configure(NULL) == LV_RESULT_INVALID);
    CHECK(stats().budget_bytes == BUDGET);
    configure(0, 0);
    case_free(&small); case_free(&medium); case_free(&large); case_free(&oversize);
}

static void * one_draw(void * pointer)
{
    if(!case_run(pointer)) atomic_fetch_add(&s_thread_failures, 1);
    return NULL;
}

static void test_busy_reconfiguration(void)
{
    cache_case_t c;
    case_init(&c, 59, 29, LV_COLOR_FORMAT_RGB565, 0);
    configure(BUDGET, ENTRY_LIMIT);
    pthread_mutex_lock(&s_alloc_lock);
    s_pause_alloc = true; s_resume_alloc = false; s_alloc_entered = false;
    pthread_mutex_unlock(&s_alloc_lock);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, one_draw, &c) == 0);
    pthread_mutex_lock(&s_alloc_lock);
    while(!s_alloc_entered) pthread_cond_wait(&s_alloc_cv, &s_alloc_lock);
    pthread_mutex_unlock(&s_alloc_lock);
    lv_draw_sw_blur_cache_stats_t current;
    lv_draw_sw_blur_cache_get_stats(&current);
    CHECK(current.leased_entries == 1 && current.bytes > 0);
    CHECK(lv_draw_sw_blur_cache_clear() == LV_RESULT_INVALID);
    lv_draw_sw_blur_cache_config_t off = {0};
    CHECK(lv_draw_sw_blur_cache_configure(&off) == LV_RESULT_INVALID);
    pthread_mutex_lock(&s_alloc_lock);
    s_resume_alloc = true; s_pause_alloc = false;
    pthread_cond_broadcast(&s_alloc_cv);
    pthread_mutex_unlock(&s_alloc_lock);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(stats().misses == 1);
    CHECK(case_run(&c));
    CHECK(stats().hits == 1);
    configure(0, 0);
    case_free(&c);
}

typedef struct { cache_case_t cases[2]; unsigned index; } worker_t;
static atomic_bool s_start;
static void * stress_draws(void * pointer)
{
    worker_t * worker = pointer;
    while(!atomic_load(&s_start)) sched_yield();
    for(unsigned i = 0; i < 1600; i++) {
        if(!case_run(&worker->cases[(i + worker->index) % 2])) atomic_fetch_add(&s_thread_failures, 1);
        if(i % 7 == 0) sched_yield();
    }
    return NULL;
}

static void test_threaded_stress(void)
{
    worker_t workers[THREADS];
    pthread_t threads[THREADS];
    for(unsigned i = 0; i < THREADS; i++) {
        workers[i].index = i;
        for(unsigned j = 0; j < 2; j++) {
            /* Two same-key source variants and several differently sized
             * entries force leased hits, invalidation, growth and eviction. */
            case_init(&workers[i].cases[j], 33 + i % 3 * 13, 19 + i % 3 * 7,
                      i % 2 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565, i % 3);
            if(j) {
                cache_case_t * c = &workers[i].cases[j];
                c->f.before[GUARD + c->f.stride * 3 + 12] ^= 77;
                memcpy(c->f.allocation, c->f.before, c->f.size);
                lv_draw_sw_blur(&c->f.task, &c->dsc, &c->coords);
                memcpy(c->expected, c->f.allocation, c->f.size);
            }
        }
    }
    configure(18000, 12000);
    atomic_store(&s_start, false);
    for(unsigned i = 0; i < THREADS; i++) CHECK(pthread_create(&threads[i], NULL, stress_draws, &workers[i]) == 0);
    atomic_store(&s_start, true);
    /* Clear/configure are valid only between leases; rejection preserves the
     * cache. Successful resets must not invalidate another worker's storage. */
    unsigned rejected = 0;
    for(unsigned i = 0; i < 800; i++) {
        lv_result_t r = i % 2 ? lv_draw_sw_blur_cache_clear() :
            lv_draw_sw_blur_cache_configure(&(lv_draw_sw_blur_cache_config_t){
                .budget_bytes = 18000, .max_entry_bytes = 12000,
                .alloc_cb = tracked_alloc, .free_cb = tracked_free});
        rejected += r == LV_RESULT_INVALID;
        sched_yield();
    }
    for(unsigned i = 0; i < THREADS; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(atomic_load(&s_thread_failures) == 0);
    /* A successful configure after the last worker legitimately zeros its
     * counters. Verify post-race service deterministically instead of relying
     * on the scheduler leaving a worker active after the last reset. Lease
     * rejection itself is covered by test_busy_reconfiguration above. */
    CHECK(case_run(&workers[0].cases[0]));
    lv_draw_sw_blur_cache_stats_t final = stats();
    CHECK(final.hits + final.misses + final.busy_bypasses > 0);
    CHECK(s_peak_bytes <= 18000);
    printf("thread stress: hits=%llu misses=%llu busy=%llu evictions=%llu rejected=%u peak=%zu\n",
           (unsigned long long)final.hits, (unsigned long long)final.misses,
           (unsigned long long)final.busy_bypasses, (unsigned long long)final.evictions,
           rejected, s_peak_bytes);
    configure(0, 0);
    for(unsigned i = 0; i < THREADS; i++) for(unsigned j = 0; j < 2; j++) case_free(&workers[i].cases[j]);
}

static void test_failed_render_and_busy_slots(void)
{
    cache_case_t cases[5];
    for(unsigned i = 0; i < 5; i++) case_init(&cases[i], 17, 13, LV_COLOR_FORMAT_RGB565, 0);
    configure(BUDGET, ENTRY_LIMIT);
    lv_blur_cache_entry_t * leases[5];
    for(unsigned i = 0; i < 5; i++) {
        fixture_t * f = &cases[i].f;
        lv_blur_cache_result_t result = lv_draw_sw_blur_cache_begin(&f->task, &cases[i].dsc,
            &cases[i].coords, &f->layer.buf_area, f->buf.data, f->stride, f->bytes, &leases[i]);
        CHECK(result == (i == 4 ? LV_BLUR_CACHE_BYPASS : LV_BLUR_CACHE_MISS));
    }
    for(unsigned i = 0; i < 5; i++) {
        lv_draw_sw_blur_cache_finish(leases[i], cases[i].f.buf.data, cases[i].f.stride, false);
    }
    CHECK(stats().busy_bypasses == 1);
    CHECK(case_run(&cases[0]));
    CHECK(stats().hits == 0); /* Aborted render must not publish stale output. */
    CHECK(case_run(&cases[0]));
    CHECK(stats().hits == 1);
    configure(0, 0);
    for(unsigned i = 0; i < 5; i++) case_free(&cases[i]);
}

int main(void)
{
    lv_init();
    test_disabled_and_formats();
    test_source_mutation_and_identity();
    test_semantic_keys();
    test_eviction_failure_and_limits();
    test_busy_reconfiguration();
    test_failed_render_and_busy_slots();
    test_threaded_stress();
    /* Default allocators and repeated init/deinit exercise lifecycle cleanup. */
    lv_draw_sw_blur_cache_config_t standard = {.budget_bytes = BUDGET};
    CHECK(lv_draw_sw_blur_cache_configure(&standard) == LV_RESULT_OK);
    lv_deinit();
    for(unsigned i = 0; i < 3; i++) {
        lv_init();
        configure(0, 0);
        cache_case_t c;
        case_init(&c, 37, 19, LV_COLOR_FORMAT_RGB565, 0);
        configure(BUDGET, ENTRY_LIMIT);
        CHECK(case_run(&c));
        case_free(&c);
        lv_deinit();
        CHECK(s_live_bytes == 0);
    }
    CHECK(atomic_load(&s_thread_failures) == 0);
    printf("blur cache: %d checks, %d failures, %u allocations\n", s_checks, s_failed, s_allocations);
    return s_failed ? 1 : 0;
}
