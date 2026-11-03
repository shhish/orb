/* Actual LVGL CUSTOM tasks, including two-worker execution and abort cleanup. */
#include "lvgl.h"
#include "orb_game_beads_draw.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"
#include "src/misc/lv_area_private.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned s_checks, s_live, s_attempt, s_fail_enqueue;
static pthread_t s_ui;
#define CHECK(c) do { ++s_checks; if (!(c)) { fprintf(stderr,"beads tasks:%d: %s\n",__LINE__,#c); exit(1); } } while(0)
static void *source_calloc(size_t count, size_t bytes)
{
    void *pointer = calloc(count, bytes);
    if (pointer) ++s_live;
    return pointer;
}
static void source_free(void *pointer)
{
    CHECK(pointer && s_live && pthread_equal(pthread_self(), s_ui));
    --s_live; free(pointer);
}

typedef struct {
    lv_draw_custom_execute_cb_t execute;
    lv_draw_custom_retire_cb_t retire;
    void *payload;
} task_context_t;
static task_context_t s_contexts[2];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_condition = PTHREAD_COND_INITIALIZER;
static unsigned s_entered;
static bool s_parallel, s_timed_out, s_ran_on_ui;

static void execute_task(lv_layer_t *layer, const lv_area_t *clip, const void *payload)
{
    const task_context_t *context = payload;
    pthread_mutex_lock(&s_mutex);
    s_ran_on_ui |= pthread_equal(pthread_self(), s_ui);
    ++s_entered;
    pthread_cond_broadcast(&s_condition);
    if (s_parallel) {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 5;
        while (s_entered < 2 && !s_timed_out)
            if (pthread_cond_timedwait(&s_condition, &s_mutex, &until) == ETIMEDOUT) s_timed_out = true;
    }
    pthread_mutex_unlock(&s_mutex);
    context->execute(layer, clip, context->payload);
}
static void retire_task(void *payload)
{
    task_context_t *context = payload;
    CHECK(pthread_equal(pthread_self(), s_ui) && context->payload);
    context->retire(context->payload);
    context->payload = NULL;
}
static bool test_enqueue(lv_layer_t *layer, const lv_area_t *clip,
                          lv_draw_custom_execute_cb_t execute,
                          lv_draw_custom_retire_cb_t retire, void *payload)
{
    if (++s_attempt == s_fail_enqueue) return false;
    for (unsigned i = 0; i < 2; ++i) if (!s_contexts[i].payload) {
        s_contexts[i] = (task_context_t){execute, retire, payload};
        if (lv_draw_custom(layer, clip, execute_task, retire_task, &s_contexts[i])) return true;
        s_contexts[i].payload = NULL;
        return false;
    }
    CHECK(false); return false;
}
#define calloc source_calloc
#define free source_free
#define lv_draw_custom test_enqueue
#include "../../firmware/components/orb_games/orb_game_beads_draw.c"
#undef calloc
#undef free
#undef lv_draw_custom

void orb_game_native_beads_reference(lv_layer_t *, const orb_game_native_beads_t *, size_t,
                                      const int16_t *, const int16_t *, const uint8_t *,
                                      const orb_game_sprite_t *);

static void circle_mask(lv_opa_t *pixels, unsigned radius)
{
    unsigned diameter = radius * 2 + 1;
    lv_area_t area = {0, 0, (int32_t)diameter - 1, (int32_t)diameter - 1};
    lv_draw_sw_mask_radius_param_t mask;
    lv_draw_sw_mask_radius_init(&mask, &area, (int32_t)radius, false);
    void *masks[] = {&mask, NULL};
    memset(pixels, 255, diameter * diameter);
    for (unsigned y = 0; y < diameter; ++y)
        (void)lv_draw_sw_mask_apply(masks, pixels + y * diameter, 0, (int32_t)y, (int32_t)diameter);
    lv_draw_sw_mask_free_param(&mask);
}
static void finish(lv_layer_t *layer)
{
    while (layer->draw_task_head) {
        lv_draw_dispatch_layer(NULL, layer);
        lv_draw_wait_for_finish();
    }
}
static lv_layer_t layer_create(lv_color_format_t format)
{
    lv_layer_t layer = {0}; lv_layer_init(&layer);
    layer.buf_area = layer._clip_area = (lv_area_t){0, 0, 465, 465};
    layer.color_format = format; layer.opa = LV_OPA_COVER;
    layer.draw_buf = lv_draw_buf_create(466, 466, format, LV_STRIDE_AUTO); CHECK(layer.draw_buf);
    for (unsigned i = 0; i < layer.draw_buf->data_size; ++i) layer.draw_buf->data[i] = (uint8_t)(i * 39);
    return layer;
}
static void exercise(lv_color_format_t format, unsigned fail_enqueue, unsigned termination)
{
    lv_opa_t body[ORB_GAME_NATIVE_BODY_D * ORB_GAME_NATIVE_BODY_D];
    circle_mask(body, ORB_GAME_BEAD_DRAW_RADIUS);
    orb_game_native_beads_t masks; orb_game_native_beads_prepare(&masks, body);
    int16_t x[64], y[64]; uint8_t opacity[64]; orb_game_sprite_t sprites[64] = {0};
    for (unsigned i = 0; i < 64; ++i) {
        x[i] = (int16_t)(i * 17 % 466); y[i] = (int16_t)(i * 17 % 466);
        opacity[i] = (uint8_t)(i * 31); sprites[i].colour = i * 0x638123u & 0xffffffu;
    }
    lv_layer_t layer = layer_create(format), expected = layer_create(format);
    orb_game_native_beads_reference(&expected, &masks, 64, x, y, opacity, sprites);
    orb_game_beads_draw_t *source = orb_game_beads_draw_create(&masks); CHECK(source && s_live == 1);
    s_attempt = 0; s_fail_enqueue = fail_enqueue;
    s_parallel = fail_enqueue == 0 && termination == 0;
    s_entered = 0; s_timed_out = s_ran_on_ui = false;
    CHECK(orb_game_beads_draw_dispatch(source, &layer, 64, x, y, opacity, sprites, 1));
    CHECK(s_attempt == 2);
    memset(sprites, 0, sizeof(sprites)); memset(x, 0, sizeof(x)); memset(body, 0, sizeof(body));
    orb_game_beads_draw_release(source);
    if (termination == 0) {
        finish(&layer);
        CHECK(!s_timed_out && !s_ran_on_ui);
        if (!fail_enqueue) CHECK(s_entered == 2);
        for (unsigned i = 0; i < layer.draw_buf->data_size; ++i)
            CHECK(layer.draw_buf->data[i] == expected.draw_buf->data[i]);
    } else if (termination == 2) {
        for (lv_draw_task_t *task = layer.draw_task_head; task; task = task->next)
            task->state = LV_DRAW_TASK_STATE_FAILED;
    }
    lv_draw_layer_deinit(&layer);
    CHECK(!s_live && !s_contexts[0].payload && !s_contexts[1].payload);
    lv_draw_buf_destroy(layer.draw_buf); lv_draw_buf_destroy(expected.draw_buf);
}
int main(void)
{
    s_ui = pthread_self(); lv_init();
    for (unsigned mode = 0; mode < 2; ++mode) {
        lv_color_format_t format = mode ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB565_SWAPPED;
        for (unsigned failure = 0; failure < 3; ++failure) exercise(format, failure, 0);
        exercise(format, 0, 1); exercise(format, 0, 2);
    }
    printf("Beads actual CUSTOM: %u checks; two-worker rendezvous, exact full native pixels, enqueue OOM, view delete, real abort/FAILED retirement pass\n", s_checks);
    return 0;
}
