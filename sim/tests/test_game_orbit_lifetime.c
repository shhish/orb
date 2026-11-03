/* Compile the actual source with queue/allocator boundaries replaced. This
 * exercises lifetime and both partial-enqueue paths without a shipping seam. */
#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_allocations, s_frees, s_live, s_checks, s_attempt, s_fail_enqueue;
static bool s_fail_allocation, s_auto_drain;

static void check(bool condition, const char *message)
{
    ++s_checks;
    if (!condition) { fprintf(stderr, "Orbit lifetime: %s\n", message); exit(1); }
}

static void *orbit_test_calloc(size_t count, size_t size)
{
    if (s_fail_allocation) return NULL;
    void *out = calloc(count, size);
    if (out) { ++s_allocations; ++s_live; }
    return out;
}

static void orbit_test_free(void *pointer)
{
    if (!pointer) return;
    ++s_frees;
    check(s_live > 0, "free without a live source/ring allocation");
    --s_live;
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
        queued_t *queued = &s_queue[i];
        if (!queued->payload) continue;
        if (execute) queued->execute(queued->layer, &queued->clip, queued->payload);
        queued->retire(queued->payload);
        queued->payload = NULL;
        queued->layer->draw_task_head = NULL;
    }
}

static bool orbit_test_enqueue(lv_layer_t *layer, const lv_area_t *clip,
                                lv_draw_custom_execute_cb_t execute,
                                lv_draw_custom_retire_cb_t retire, void *payload)
{
    ++s_attempt;
    if (s_attempt == s_fail_enqueue) return false;
    for (unsigned i = 0; i < 2; ++i) if (!s_queue[i].payload) {
        s_queue[i] = (queued_t){.layer = layer, .clip = *clip, .execute = execute,
            .retire = retire, .payload = payload};
        layer->draw_task_head = &s_queue[i].marker;
        return true;
    }
    check(false, "more than two outstanding worker slices");
    return false;
}

static bool orbit_test_dispatch_layer(lv_display_t *display, lv_layer_t *layer)
{
    (void)display;
    (void)layer;
    if (s_auto_drain) complete_queue(true);
    return true;
}

static void orbit_test_dispatch(void)
{
    if (s_auto_drain) complete_queue(true);
}

static void orbit_test_wait(void) {}

#define calloc orbit_test_calloc
#define free orbit_test_free
#define lv_draw_custom orbit_test_enqueue
#define lv_draw_dispatch_layer orbit_test_dispatch_layer
#define lv_draw_dispatch orbit_test_dispatch
#define lv_draw_wait_for_finish orbit_test_wait
#include "../../firmware/components/orb_games/orb_game_orbit.c"
#undef calloc
#undef free
#undef lv_draw_custom
#undef lv_draw_dispatch_layer
#undef lv_draw_dispatch
#undef lv_draw_wait_for_finish

static orb_game_orbit_t *create_source(void)
{
    orb_game_orbit_ring_t *rings[3];
    for (unsigned i = 0; i < 3; ++i) {
        rings[i] = orbit_test_calloc(1, sizeof(*rings[i]) + 2 * sizeof(rings[i]->rows[0]));
        check(rings[i] != NULL, "allocate immutable test ring");
        rings[i]->radius = 1;
        for (unsigned y = 0; y < 2; ++y) {
            rings[i]->rows[y].first = 1;
            rings[i]->rows[y].last = 0;
        }
    }
    orb_game_orbit_t *source = orb_game_orbit_create(rings);
    check(source != NULL, "allocate actual source");
    return source;
}

static void exercise_queue(lv_layer_t *layer, unsigned fail_enqueue, bool execute, unsigned expected_slices)
{
    orb_game_orbit_t *source = create_source();
    orb_game_sprite_t sprite = {.x = 233, .y = 60, .radius = 7,
        .shape = ORB_GAME_DISC, .colour = 0xff9230, .opacity = 127};
    s_attempt = 0;
    s_fail_enqueue = fail_enqueue;
    s_auto_drain = fail_enqueue != 0;
    check(orb_game_orbit_dispatch(source, layer, &sprite, 1, 1, 0x2c2c2e), "dispatch actual pose");
    check(s_attempt == expected_slices, "attempt only the selected number of slices");
    check(source->count == 1 && source->primitives[0].area.x1 == 226, "copy immutable primitive");
    if (!fail_enqueue) {
        if (expected_slices == 2)
            check(s_queue[0].clip.y2 + 1 == s_queue[1].clip.y1, "worker slices never overlap");
        else check(lv_area_is_equal(&s_queue[0].clip, &layer->_clip_area) && !s_queue[1].payload,
            "small whole-clip task covers exactly one immutable region");
        sprite.x = 99;
        sprite.opacity = 255;
        check(!orb_game_orbit_dispatch(source, layer, &sprite, 1, 2, 0x2c2c2e), "busy slot keeps serial fallback");
        check(source->primitives[0].area.x1 == 226, "busy pose survives input mutation");
    }
    unsigned outstanding = source->readers;
    orb_game_orbit_release(source); /* Simulate view deletion before task completion. */
    check(s_live == (outstanding ? 4u : 0u), "view deletion retains exactly leased source and rings");
    complete_queue(execute);
    check(s_live == 0, "completion/abort/failure retires all source and ring memory");
    check(layer->draw_task_head == NULL, "retirement leaves no queued task");
}

static void reuse_and_fallback(lv_layer_t *layer)
{
    orb_game_orbit_t *source = create_source();
    orb_game_sprite_t sprite = {.x = 233, .y = 60, .radius = 8,
        .shape = ORB_GAME_STAR, .colour = 0xffd600, .opacity = 252};
    s_attempt = s_fail_enqueue = 0;
    s_auto_drain = false;
    check(orb_game_orbit_dispatch(source, layer, &sprite, 1, 7, 0x2c2c2e), "prepare star opcodes");
    check(source->count == 6, "star retains centre fill plus five native triangles");
    complete_queue(true);
    unsigned allocations = s_allocations;
    sprite.x = 120;
    check(orb_game_orbit_dispatch(source, layer, &sprite, 1, 7, 0x2c2c2e), "reuse same-revision pose");
    check(source->primitives[0].area.x1 == 229, "same-revision pose is immutable");
    complete_queue(false);
    check(orb_game_orbit_dispatch(source, layer, &sprite, 1, 8, 0x2c2c2e), "prepare next revision after retirement");
    check(source->primitives[0].area.x1 == 116, "new revision captures new geometry");
    complete_queue(true);
    check(s_allocations == allocations, "motion and retirement allocate no pose storage");
    sprite.shape = ORB_GAME_GATE;
    check(!orb_game_orbit_dispatch(source, layer, &sprite, 1, 9, 0x2c2c2e), "unsupported primitive keeps serial path");
    check(!orb_game_orbit_dispatch(source, layer, &sprite, ORBIT_SPRITES + 1, 10, 0x2c2c2e), "oversized pose keeps bounded fallback");
    orb_game_orbit_release(source);
    check(s_live == 0, "source reuse/fallback frees every allocation");
}

static void granularity_cases(lv_layer_t *layer)
{
    const lv_area_t clips[] = {{214, 52, 245, 67}, {0, 0, 465, 14}, {0, 0, 464, 15},
        {0, 0, 465, 15}, {0, 0, 465, 83}};
    const unsigned expected[] = {1, 1, 1, 2, 2};
    for (unsigned i = 0; i < sizeof(clips) / sizeof(clips[0]); ++i) {
        layer->_clip_area = clips[i];
        for (unsigned fail = 0; fail <= 2; ++fail) {
            exercise_queue(layer, fail, false, expected[i]);
            exercise_queue(layer, fail, true, expected[i]);
        }
    }
    layer->_clip_area = layer->buf_area;
}

int main(void)
{
    lv_init();
    lv_draw_buf_t *buffer = lv_draw_buf_create(466, 84, LV_COLOR_FORMAT_RGB565_SWAPPED, LV_STRIDE_AUTO);
    check(buffer != NULL, "allocate real native band");
    lv_layer_t layer = {.draw_buf = buffer, .color_format = LV_COLOR_FORMAT_RGB565_SWAPPED,
        .buf_area = {0, 0, 465, 83}, ._clip_area = {0, 0, 465, 83}, .opa = LV_OPA_COVER};
    orb_game_orbit_ring_t *empty[3] = {0};
    s_fail_allocation = true;
    check(orb_game_orbit_create(empty) == NULL, "optional source allocation failure is bounded");
    s_fail_allocation = false;
    for (unsigned repeat = 0; repeat < 128; ++repeat) {
        for (unsigned fail = 0; fail <= 2; ++fail) {
            exercise_queue(&layer, fail, false, 2); /* aborted/FAILED task cleanup */
            exercise_queue(&layer, fail, true, 2); /* ordinary completion */
        }
        reuse_and_fallback(&layer);
    }
    granularity_cases(&layer);
    check(s_allocations == s_frees && !s_live, "all games source/ring ownership balances");
    lv_draw_buf_destroy(buffer);
    lv_deinit();
    printf("Orbit lifetime: %u checks; complete/abort/source deletion/OOM/partial enqueue/reuse\n", s_checks);
    return 0;
}
