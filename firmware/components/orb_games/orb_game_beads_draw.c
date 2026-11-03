#include "orb_game_beads_draw.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"

#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#include "esp_timer.h"
#else
#include <time.h>
#endif

typedef struct {
    orb_game_beads_draw_t *source;
    uint64_t elapsed_us;
    bool executed, queued;
} beads_slice_t;

struct orb_game_beads_draw {
    unsigned references, readers;
    size_t count;
    uint64_t revision;
    bool prepared;
    beads_slice_t slices[2];
    orb_game_beads_draw_stats_t stats;
    orb_game_native_beads_t masks;
    lv_opa_t body[ORB_GAME_NATIVE_BODY_D * ORB_GAME_NATIVE_BODY_D];
    int16_t x[ORB_GAME_BEAD_COUNT], y[ORB_GAME_BEAD_COUNT];
    uint8_t opacity[ORB_GAME_BEAD_COUNT];
    uint32_t colours[ORB_GAME_BEAD_COUNT];
};

orb_game_beads_draw_t *orb_game_beads_draw_create(const orb_game_native_beads_t *masks)
{
    if (!masks || !masks->body.pixels || masks->body.diameter != ORB_GAME_NATIVE_BODY_D) return NULL;
#ifdef ESP_PLATFORM
    orb_game_beads_draw_t *source = orb_memory_calloc(ORB_MEMORY_GAMES, 1, sizeof(*source));
#else
    orb_game_beads_draw_t *source = calloc(1, sizeof(*source));
#endif
    if (!source) return NULL;
    source->references = 1;
    for (unsigned i = 0; i < 2; ++i) source->slices[i].source = source;
    memcpy(source->body, masks->body.pixels, sizeof(source->body));
    orb_game_native_beads_prepare(&source->masks, source->body);
    return source;
}

void orb_game_beads_draw_release(orb_game_beads_draw_t *source)
{
    if (!source || --source->references) return;
#ifdef ESP_PLATFORM
    orb_memory_free(source);
#else
    free(source);
#endif
}

static bool pose_prepare(orb_game_beads_draw_t *source, size_t count,
                           const int16_t *x, const int16_t *y, const uint8_t *opacity,
                           const orb_game_sprite_t *sprites, uint64_t revision)
{
    if (count > ORB_GAME_BEAD_COUNT || (count && (!x || !y || !opacity || !sprites))) return false;
    source->count = count;
    if (count) {
        memcpy(source->x, x, count * sizeof(*x));
        memcpy(source->y, y, count * sizeof(*y));
        memcpy(source->opacity, opacity, count * sizeof(*opacity));
        for (size_t i = 0; i < count; ++i) source->colours[i] = sprites[i].colour;
    }
    source->revision = revision;
    source->prepared = true;
    return true;
}

static uint64_t beads_draw_now(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
#endif
}

static void pose_draw(lv_layer_t *layer, const lv_area_t *clip, const void *payload)
{
    beads_slice_t *slice = (beads_slice_t *)payload;
    const orb_game_beads_draw_t *source = slice->source;
    uint64_t started = beads_draw_now();
    /* Copy only stable buffer metadata: the UI may still enqueue tasks or
     * change the shared clip. Native/blend need no mutable layer fields. */
    lv_layer_t target = {.draw_buf = layer->draw_buf, .buf_area = layer->buf_area,
        .color_format = layer->color_format, ._clip_area = *clip};
    orb_game_native_beads_colours(&target, &source->masks, source->count,
        source->x, source->y, source->opacity, source->colours, sizeof(*source->colours));
    slice->elapsed_us = beads_draw_now() - started;
    slice->executed = true;
}

static void slice_report(beads_slice_t *slice)
{
    if (!slice->executed) return;
    orb_game_beads_draw_stats_t *stats = &slice->source->stats;
    if (slice->queued) {
        ++stats->custom_calls;
        stats->custom_us += slice->elapsed_us;
        if (slice->elapsed_us > stats->custom_max_us) stats->custom_max_us = slice->elapsed_us;
    } else {
        ++stats->sync_calls;
        stats->sync_us += slice->elapsed_us;
    }
    slice->executed = false;
}

static void pose_retire(void *payload)
{
    beads_slice_t *slice = payload;
    orb_game_beads_draw_t *source = slice->source;
    /* Cleanup is the UI publication fence: workers only write their own
     * slice payload; stats never read a concurrently executing slice. */
    slice_report(slice);
    --source->readers;
    orb_game_beads_draw_release(source);
}

static void draw_drain(lv_layer_t *layer)
{
    while (layer->draw_task_head) {
        lv_draw_dispatch_layer(layer->display, layer);
        lv_draw_dispatch();
        lv_draw_wait_for_finish();
    }
}

bool orb_game_beads_draw_dispatch(orb_game_beads_draw_t *source, lv_layer_t *layer,
                                   size_t count, const int16_t *x, const int16_t *y,
                                   const uint8_t *opacity, const orb_game_sprite_t *sprites,
                                   uint64_t revision)
{
    if (!source || !layer) return false;
    if (source->readers) {
        lv_draw_wait_for_finish();
        lv_draw_dispatch();
        if (source->readers) return false;
    }
    if ((!source->prepared || source->revision != revision) &&
        !pose_prepare(source, count, x, y, opacity, sprites, revision)) return false;
    lv_area_t clip;
    if (!lv_area_intersect(&clip, &layer->_clip_area, &layer->buf_area)) return true;
    lv_area_t slices[2] = {clip, clip};
    slices[0].y2 = (clip.y1 + clip.y2) / 2;
    slices[1].y1 = slices[0].y2 + 1;
    ++source->references; /* Creator protects partial enqueue/fallback. */
    for (unsigned i = 0; i < 2; ++i) {
        if (slices[i].y1 > slices[i].y2) continue;
        ++source->references;
        ++source->readers;
        beads_slice_t *slice = &source->slices[i];
        slice->executed = false;
        slice->queued = true;
        if (!lv_draw_custom(layer, &slices[i], pose_draw, pose_retire, slice)) {
            slice->queued = false;
            pose_retire(slice); /* Failed enqueue transferred no ownership. */
            draw_drain(layer);
            pose_draw(layer, &slices[i], slice);
            slice_report(slice);
        }
    }
    orb_game_beads_draw_release(source);
    return true;
}

void orb_game_beads_draw_stats(const orb_game_beads_draw_t *source, orb_game_beads_draw_stats_t *out)
{
    if (out) *out = source ? source->stats : (orb_game_beads_draw_stats_t){0};
}
