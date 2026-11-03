/* Execute the actual immutable source with deterministic allocation/queue
 * failures. Both pixel kernels below compile from production source. */
#include "lvgl.h"
#include "orb_game_beads_draw.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"
#include "src/misc/lv_area_private.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks, s_allocations, s_frees, s_live, s_attempt, s_fail_enqueue;
static bool s_fail_allocation, s_auto_drain;
#define CHECK(c) do { ++s_checks; if (!(c)) { fprintf(stderr,"beads lease:%d: %s\n",__LINE__,#c); exit(1); } } while(0)

static void *source_calloc(size_t count, size_t bytes)
{
    CHECK(count == 1 && bytes < 6144);
    if (s_fail_allocation) return NULL;
    void *pointer = calloc(count, bytes);
    if (pointer) { ++s_allocations; ++s_live; }
    return pointer;
}
static void source_free(void *pointer)
{
    CHECK(pointer && s_live);
    ++s_frees; --s_live;
    free(pointer);
}

typedef struct {
    lv_layer_t *layer;
    lv_area_t clip;
    lv_draw_custom_execute_cb_t execute;
    lv_draw_custom_retire_cb_t retire;
    void *payload;
    lv_draw_task_t marker;
} queued_t;
static queued_t s_queue[2];

static void complete_queue(bool execute)
{
    for (unsigned i = 0; i < 2; ++i) {
        queued_t *q = &s_queue[i];
        if (!q->payload) continue;
        lv_area_t clip = q->layer->_clip_area;
        if (execute) q->execute(q->layer, &q->clip, q->payload);
        CHECK(lv_area_is_equal(&clip, &q->layer->_clip_area));
        q->retire(q->payload);
        q->payload = NULL;
        q->layer->draw_task_head = NULL;
    }
}
static bool test_enqueue(lv_layer_t *layer, const lv_area_t *clip,
                          lv_draw_custom_execute_cb_t execute,
                          lv_draw_custom_retire_cb_t retire, void *payload)
{
    if (++s_attempt == s_fail_enqueue) return false;
    for (unsigned i = 0; i < 2; ++i) if (!s_queue[i].payload) {
        s_queue[i] = (queued_t){.layer = layer, .clip = *clip, .execute = execute,
            .retire = retire, .payload = payload};
        layer->draw_task_head = &s_queue[i].marker;
        return true;
    }
    CHECK(false); return false;
}
static bool test_dispatch_layer(lv_display_t *display, lv_layer_t *layer)
{
    (void)display; (void)layer;
    if (s_auto_drain) complete_queue(true);
    return true;
}
static void test_dispatch(void) { if (s_auto_drain) complete_queue(true); }
static void test_wait(void) {}

#define calloc source_calloc
#define free source_free
#define lv_draw_custom test_enqueue
#define lv_draw_dispatch_layer test_dispatch_layer
#define lv_draw_dispatch test_dispatch
#define lv_draw_wait_for_finish test_wait
#include "../../firmware/components/orb_games/orb_game_beads_draw.c"
#undef calloc
#undef free
#undef lv_draw_custom
#undef lv_draw_dispatch_layer
#undef lv_draw_dispatch
#undef lv_draw_wait_for_finish

void orb_game_native_beads_reference(lv_layer_t *layer, const orb_game_native_beads_t *masks,
                                      size_t count, const int16_t *x, const int16_t *y,
                                      const uint8_t *opacity, const orb_game_sprite_t *sprites);

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
static lv_layer_t layer_create(lv_color_format_t format, int y)
{
    lv_layer_t layer = {0};
    lv_layer_init(&layer);
    layer.buf_area = (lv_area_t){0, y, 465, y + 83};
    layer._clip_area = (lv_area_t){7, y + 2, 457, y + 81};
    layer.color_format = format;
    layer.opa = LV_OPA_COVER;
    layer.draw_buf = lv_draw_buf_create(466, 84, format, LV_STRIDE_AUTO);
    CHECK(layer.draw_buf);
    for (unsigned i = 0; i < layer.draw_buf->data_size; ++i) layer.draw_buf->data[i] = (uint8_t)(i * 53 + y * 17);
    return layer;
}
static void layer_delete(lv_layer_t *layer)
{
    CHECK(!layer->draw_task_head);
    lv_draw_buf_destroy(layer->draw_buf);
}

static void pixel_cases(void)
{
    static const lv_color_format_t formats[] = {LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_RGB565_SWAPPED,
        LV_COLOR_FORMAT_ARGB8888, LV_COLOR_FORMAT_XRGB8888};
    static const uint8_t opacities[] = {0, 1, 2, 3, 16, 63, 127, 128, 191, 252, 253, 255};
    lv_opa_t body[ORB_GAME_NATIVE_BODY_D * ORB_GAME_NATIVE_BODY_D];
    int16_t x[ORB_GAME_BEAD_COUNT], y[ORB_GAME_BEAD_COUNT];
    uint8_t opacity[ORB_GAME_BEAD_COUNT];
    orb_game_sprite_t sprites[ORB_GAME_BEAD_COUNT] = {0};
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
        for (unsigned frame = 0; frame < 18; ++frame) {
            circle_mask(body, ORB_GAME_BEAD_DRAW_RADIUS);
            orb_game_native_beads_t masks;
            orb_game_native_beads_prepare(&masks, body);
            orb_game_beads_draw_t *source = orb_game_beads_draw_create(&masks);
            CHECK(source && sizeof(*source) < 6144);
            lv_layer_t actual = layer_create(formats[f], frame % 3 * 84);
            lv_layer_t expected = layer_create(formats[f], frame % 3 * 84);
            for (unsigned i = 0; i < ORB_GAME_BEAD_COUNT; ++i) {
                x[i] = (int16_t)((i * 53 + frame * 17) % 489 - 12);
                y[i] = (int16_t)(actual.buf_area.y1 + (int)(i * 11 + frame * 3) % 109 - 12);
                opacity[i] = opacities[(i + frame) % sizeof(opacities)];
                sprites[i].colour = (i * 0x173219u + frame * 0x4381u) & 0xffffffu;
            }
            orb_game_native_beads_reference(&expected, &masks, ORB_GAME_BEAD_COUNT, x, y, opacity, sprites);
            s_attempt = 0; s_fail_enqueue = frame % 3; s_auto_drain = s_fail_enqueue != 0;
            CHECK(orb_game_beads_draw_dispatch(source, &actual, ORB_GAME_BEAD_COUNT, x, y,
                opacity, sprites, frame));
            CHECK(s_attempt == 2);
            /* Every caller-owned byte can change or disappear after enqueue. */
            memset(x, 0, sizeof(x)); memset(y, 0, sizeof(y)); memset(opacity, 0, sizeof(opacity));
            memset(sprites, 0, sizeof(sprites)); memset(body, 0, sizeof(body));
            orb_game_beads_draw_release(source);
            complete_queue(true);
            CHECK(!s_live);
            CHECK(actual.draw_buf->data_size == expected.draw_buf->data_size);
            for (unsigned i = 0; i < actual.draw_buf->data_size; ++i)
                CHECK(actual.draw_buf->data[i] == expected.draw_buf->data[i]);
            layer_delete(&actual); layer_delete(&expected);
        }
    }
}

static void lifetime_cases(void)
{
    lv_opa_t body[ORB_GAME_NATIVE_BODY_D * ORB_GAME_NATIVE_BODY_D];
    circle_mask(body, ORB_GAME_BEAD_DRAW_RADIUS);
    orb_game_native_beads_t masks; orb_game_native_beads_prepare(&masks, body);
    CHECK(!orb_game_beads_draw_create(NULL));
    s_fail_allocation = true; CHECK(!orb_game_beads_draw_create(&masks)); s_fail_allocation = false;
    lv_layer_t layer = layer_create(LV_COLOR_FORMAT_RGB565_SWAPPED, 0);
    int16_t x = 31, y = 40; uint8_t opacity = 255;
    orb_game_sprite_t sprite = {.colour = 0xffb3c3};
    for (unsigned termination = 0; termination < 3; ++termination) {
        orb_game_beads_draw_t *source = orb_game_beads_draw_create(&masks); CHECK(source);
        s_attempt = s_fail_enqueue = 0; s_auto_drain = false;
        CHECK(orb_game_beads_draw_dispatch(source, &layer, 1, &x, &y, &opacity, &sprite, 1));
        CHECK(source->references == 3 && source->readers == 2);
        CHECK(s_queue[0].clip.y2 + 1 == s_queue[1].clip.y1);
        x = 99;
        CHECK(!orb_game_beads_draw_dispatch(source, &layer, 1, &x, &y, &opacity, &sprite, 2));
        CHECK(source->x[0] != x);
        orb_game_beads_draw_release(source); CHECK(s_live == 1);
        complete_queue(termination == 0); CHECK(!s_live);
        x = 31;
    }
    orb_game_beads_draw_t *source = orb_game_beads_draw_create(&masks); CHECK(source);
    unsigned allocations = s_allocations;
    for (unsigned i = 1; i <= 100; ++i) {
        CHECK(orb_game_beads_draw_dispatch(source, &layer, 1, &x, &y, &opacity, &sprite, i));
        complete_queue(true);
        x = 99;
        CHECK(orb_game_beads_draw_dispatch(source, &layer, 1, &x, &y, &opacity, &sprite, i));
        CHECK(source->x[0] == 31); complete_queue(false); x = 31;
    }
    CHECK(s_allocations == allocations);
    CHECK(!orb_game_beads_draw_dispatch(source, &layer, ORB_GAME_BEAD_COUNT + 1, &x, &y, &opacity, &sprite, 101));
    CHECK(!orb_game_beads_draw_dispatch(source, &layer, 1, NULL, &y, &opacity, &sprite, 101));
    orb_game_beads_draw_release(source); CHECK(!s_live);
    layer_delete(&layer);
}

static void retired_statistics(void)
{
    lv_opa_t body[ORB_GAME_NATIVE_BODY_D * ORB_GAME_NATIVE_BODY_D];
    circle_mask(body, ORB_GAME_BEAD_DRAW_RADIUS);
    orb_game_native_beads_t masks; orb_game_native_beads_prepare(&masks, body);
    orb_game_beads_draw_t *source = orb_game_beads_draw_create(&masks); CHECK(source);
    lv_layer_t layer = layer_create(LV_COLOR_FORMAT_RGB565_SWAPPED, 0);
    int16_t x = 31, y = 40; uint8_t opacity = 255;
    orb_game_sprite_t sprite = {.colour = 0xffb3c3};
    orb_game_beads_draw_stats_t stats;
    s_attempt = s_fail_enqueue = 0; s_auto_drain = false;
    CHECK(orb_game_beads_draw_dispatch(source, &layer, 1, &x, &y, &opacity, &sprite, 1));
    orb_game_beads_draw_stats(source, &stats);
    CHECK(!stats.custom_calls && !stats.sync_calls);
    complete_queue(true); orb_game_beads_draw_stats(source, &stats);
    CHECK(stats.custom_calls == 2 && !stats.sync_calls && stats.custom_max_us <= stats.custom_us);
    s_attempt = 0; s_fail_enqueue = 1; s_auto_drain = true;
    CHECK(orb_game_beads_draw_dispatch(source, &layer, 1, &x, &y, &opacity, &sprite, 2));
    complete_queue(true); orb_game_beads_draw_stats(source, &stats);
    CHECK(stats.custom_calls == 3 && stats.sync_calls == 1);
    s_attempt = s_fail_enqueue = 0; s_auto_drain = false;
    CHECK(orb_game_beads_draw_dispatch(source, &layer, 1, &x, &y, &opacity, &sprite, 3));
    complete_queue(false); orb_game_beads_draw_stats(source, &stats);
    CHECK(stats.custom_calls == 3 && stats.sync_calls == 1);
    orb_game_beads_draw_release(source); layer_delete(&layer);
    orb_game_beads_draw_stats(NULL, &stats);
    CHECK(!stats.custom_calls && !stats.sync_calls);
}

int main(void)
{
    lv_init();
    pixel_cases(); lifetime_cases(); retired_statistics();
    CHECK(s_allocations == s_frees && !s_live);
    printf("Beads immutable batch: %u checks; %zu-byte source; full native reference, clipped two slices, OOM, reuse, delete, completion/abort/FAILED cleanup pass\n", s_checks, sizeof(orb_game_beads_draw_t));
    return 0;
}
