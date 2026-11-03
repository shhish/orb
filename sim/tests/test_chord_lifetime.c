/* Exercise real immutable painter leases while cached rows are deleted and
 * queued tasks complete, fail, or are aborted. Compile this same source with
 * ASan/UBSan too: including orb_ui.c exposes the actual private lease path. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../firmware/components/orb_ui/orb_ui.c"
#include "sim_display.h"

static unsigned s_checks;
static uint32_t s_now;
static void *s_watch_source, *s_watch_pose;
static unsigned s_source_freed, s_pose_freed;
static bool s_fail_alloc;
static pthread_t s_ui_thread;
#ifdef ORB_CHORD_METADATA_OOM_TEST
extern int orb_chord_test_metadata_fail_after;
#endif

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "chord lifetime %s:%d: %s\n", __FILE__, __LINE__, #value); \
    exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void *allocate(size_t bytes)
{
    if (s_fail_alloc) { s_fail_alloc = false; return NULL; }
    return lv_malloc(bytes);
}
static void release(void *pointer)
{
    if (pointer == s_watch_source) ++s_source_freed;
    if (pointer == s_watch_pose) ++s_pose_freed;
    lv_free(pointer);
}

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static void layer_init(lv_layer_t *layer, const lv_area_t *area)
{
    lv_layer_init(layer);
    layer->buf_area = *area;
    layer->_clip_area = *area;
    layer->color_format = LV_COLOR_FORMAT_RGB565;
    layer->opa = LV_OPA_COVER;
    layer->draw_buf = lv_draw_buf_create(lv_area_get_width(area), lv_area_get_height(area),
                                        LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    CHECK(layer->draw_buf);
    lv_draw_buf_clear(layer->draw_buf, NULL);
}

static void layer_finish(lv_layer_t *layer)
{
    while (layer->draw_task_head) {
        lv_draw_dispatch_layer(NULL, layer);
        lv_draw_wait_for_finish();
    }
}

static void layer_delete(lv_layer_t *layer)
{
    lv_draw_buf_t *buffer = layer->draw_buf;
    lv_draw_layer_deinit(layer);
    lv_draw_buf_destroy(buffer);
}

static void cached_source_retirement(lv_display_t *display, unsigned termination)
{
    lv_obj_t *saved = lv_screen_active();
    lv_obj_t *screen = plain(NULL, 0, 0, 466, 466);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *list = plain(screen, 0, 0, 466, 466);
    lv_obj_t *row = plain(list, 24, 197, 418, 72);
    lv_obj_set_style_bg_color(row, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 36, 0);
    lv_obj_t *early = plain(row, 16, 16, 96, 40);
    lv_obj_set_style_bg_color(early, ORB_BLUE, 0);
    lv_obj_set_style_bg_opa(early, LV_OPA_COVER, 0);
    lv_obj_t *late = plain(row, 338, 20, 64, 32);
    lv_obj_set_style_bg_color(late, ORB_GREEN, 0);
    lv_obj_set_style_bg_opa(late, LV_OPA_COVER, 0);
    lv_screen_load(screen);
    CHECK(orb_round_list_apply(list, &(orb_round_list_config_t){
        .margin = 24, .fade = 38, .stepped = true, .chord_fit = true,
    }));
    s_now += 20;
    lv_refr_now(display);
    lv_draw_buf_t *reference = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    CHECK(reference);
    round_list_t *managed = event_data(list, round_list_event);
    CHECK(managed);
    round_chord_row_t *source = round_chord_find(managed, row);
    CHECK(source && source->draw_pose && !source->draw_pose_busy);
    lv_area_t clip = source->presented;
    lv_layer_t layer;
    layer_init(&layer, &clip);
    chord_565_prepare(source, &layer, &source->paint);
    chord_565_prepare_insets(source);
    s_watch_source = source;
    s_watch_pose = source->draw_pose;
    s_source_freed = s_pose_freed = 0;
#ifdef ORB_CHORD_METADATA_OOM_TEST
    if (termination == 3) orb_chord_test_metadata_fail_after = 1;
#endif
    CHECK(chord_pose_dispatch(source, &layer, &clip, source->paint.background));
    if (termination == 3) {
        CHECK(!source->draw_leases && !source->draw_pose_busy);
        CHECK(!layer.draw_task_head);
    } else {
        CHECK(source->draw_leases == 1 && source->draw_pose_busy);
        CHECK(layer.draw_task_head && layer.draw_task_head->next);
        lv_obj_delete(row);
        CHECK(source->retired);
        CHECK(!s_source_freed && !s_pose_freed);
        /* A later UI update may rewrite every mutable cache/geometry field. The
         * already queued native painter must still render its captured pose. */
        source->presented = (lv_area_t){0, 0, 0, 0};
        source->scale = 1;
        memset(&source->paint, 0, sizeof(source->paint));
        memset(source->insets, 0xff, (size_t)source->height * sizeof(*source->insets));
    }
    if (termination == 0 || termination == 3) {
        layer_finish(&layer);
        for (int y = clip.y1; y <= clip.y2; ++y) {
            const uint16_t *shown = (const uint16_t *)(layer.draw_buf->data +
                (y - clip.y1) * layer.draw_buf->header.stride);
            const lv_color32_t *expected = (const lv_color32_t *)(reference->data + y * reference->header.stride);
            for (int x = clip.x1; x <= clip.x2; ++x) {
                uint16_t color = lv_color_to_u16(lv_color_make(expected[x].red, expected[x].green, expected[x].blue));
                CHECK(shown[x - clip.x1] == color);
            }
        }
    } else if (termination == 2) {
        for (lv_draw_task_t *task = layer.draw_task_head; task; task = task->next)
            task->state = LV_DRAW_TASK_STATE_FAILED;
    }
    if (termination == 3) lv_obj_delete(row);
    layer_delete(&layer); /* Also retires queued/failed work without executing it. */
    CHECK(s_source_freed == 1 && s_pose_freed == 1);
    s_watch_source = s_watch_pose = NULL;
    lv_draw_buf_destroy(reference);
    lv_screen_load(saved);
    lv_obj_delete(screen);
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned entered, retired;
    bool timed_out;
} rendezvous_t;

typedef struct {
    rendezvous_t *rendezvous;
    uint16_t color;
} parallel_payload_t;

static void parallel_draw(lv_layer_t *layer, const lv_area_t *clip, const void *opaque)
{
    const parallel_payload_t *payload = opaque;
    rendezvous_t *r = payload->rendezvous;
    if (pthread_equal(pthread_self(), s_ui_thread)) abort();
    pthread_mutex_lock(&r->mutex);
    ++r->entered;
    pthread_cond_broadcast(&r->condition);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    while (r->entered < 2) {
        if (pthread_cond_timedwait(&r->condition, &r->mutex, &until) == ETIMEDOUT) {
            r->timed_out = true;
            break;
        }
    }
    pthread_mutex_unlock(&r->mutex);
    for (int y = clip->y1; y <= clip->y2; ++y) {
        uint16_t *out = lv_draw_layer_go_to_xy(layer, clip->x1 - layer->buf_area.x1, y - layer->buf_area.y1);
        for (int x = clip->x1; x <= clip->x2; ++x) out[x - clip->x1] = payload->color;
    }
}

static void parallel_retire(void *opaque)
{
    parallel_payload_t *payload = opaque;
    CHECK(pthread_equal(pthread_self(), s_ui_thread));
    ++payload->rendezvous->retired;
    free(payload);
}

static void parallel_tasks(void)
{
    rendezvous_t r = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    lv_area_t area = {0, 0, 31, 15};
    lv_layer_t layer;
    layer_init(&layer, &area);
    for (unsigned i = 0; i < 2; ++i) {
        parallel_payload_t *payload = malloc(sizeof(*payload));
        CHECK(payload);
        *payload = (parallel_payload_t){.rendezvous = &r, .color = i ? 0x07e0 : 0xf800};
        lv_area_t slice = {0, (int)i * 8, 31, (int)i * 8 + 7};
        CHECK(lv_draw_custom(&layer, &slice, parallel_draw, parallel_retire, payload));
    }
    layer_finish(&layer);
    CHECK(r.entered == 2 && r.retired == 2 && !r.timed_out);
    for (unsigned y = 0; y < 16; ++y) {
        const uint16_t *pixels = (const uint16_t *)(layer.draw_buf->data + y * layer.draw_buf->header.stride);
        for (unsigned x = 0; x < 32; ++x) CHECK(pixels[x] == (y < 8 ? 0xf800 : 0x07e0));
    }
    /* Empty clips return the payload lease to the creator without retirement. */
    lv_area_t outside = {40, 40, 50, 50};
    CHECK(!lv_draw_custom(&layer, &outside, parallel_draw, parallel_retire, NULL));
    CHECK(r.retired == 2);
    layer_delete(&layer);
    pthread_cond_destroy(&r.condition);
    pthread_mutex_destroy(&r.mutex);
}

/* A clipped, oversized logical child starts beyond the shared X table. Its
 * native painter must retain the exact local-map fallback; ASan surrounds the
 * deliberately exact-sized 466-entry allocation with red zones. */
static void pose_map_guard(void)
{
    static uint16_t pixels[800 * 4];
    for (unsigned i = 0; i < sizeof(pixels) / sizeof(pixels[0]); ++i) pixels[i] = i * 17 + 123;
    uint16_t insets[4] = {0};
    round_chord_row_t row = {.width = 800, .height = 4, .current_width = 800,
        .scale = 211, .presented = {0, 0, 465, 2}, .insets = insets};
    lv_draw_buf_init(&row.source, 800, 4, LV_COLOR_FORMAT_RGB565, 1600, pixels, sizeof(pixels));
    round_chord_child_t child = {.x = -256, .width = 1100, .height = 4};
    chord_565_t paint = {.opacity = 255};
    for (unsigned i = 0; i < 64; ++i) {
        paint.green[i] = i << 5;
        if (i < 32) { paint.red[i] = i << 11; paint.blue[i] = i; }
    }
    lv_area_t area = {0, 0, 465, 2}, clip = {300, 0, 465, 2};
    lv_layer_t layer;
    layer_init(&layer, &area);
    chord_blit565(&row, &child, &layer, &clip, 1100, 0, &paint);
    size_t bytes = layer.draw_buf->data_size;
    uint8_t *reference = malloc(bytes);
    uint16_t *map = malloc(ORB_SCREEN_D * sizeof(*map));
    CHECK(reference && map);
    memcpy(reference, layer.draw_buf->data, bytes);
    for (unsigned i = 0; i < ORB_SCREEN_D; ++i) map[i] = i * 256 / row.scale;
    row.draw_map = map;
    lv_draw_buf_clear(layer.draw_buf, NULL);
    chord_blit565(&row, &child, &layer, &clip, 1100, 0, &paint);
    CHECK(!memcmp(reference, layer.draw_buf->data, bytes));
    free(map);
    free(reference);
    layer_delete(&layer);
}

/* Map reuse is keyed only by scale. A sentinel beyond this tiny clip proves
 * same-scale translation reuses the fully retired slot; a new scale must
 * regenerate every entry, including the sentinel, with the exact quotient. */
static void pose_map_reuse(void)
{
    size_t bytes = sizeof(round_chord_row_t) + 72 * sizeof(uint16_t);
    round_chord_row_t *source = allocate(bytes);
    CHECK(source);
    memset(source, 0, bytes);
    source->insets = (uint16_t *)source->children;
    source->width = source->current_width = 466;
    source->height = 72;
    source->scale = 211;
    source->presented = (lv_area_t){0, 0, 383, 58};
    chord_pose_prepare(source);
    CHECK(source->draw_pose && !source->draw_pose->draw_map_scale);
    lv_area_t area = {0, 0, 31, 15};
    lv_layer_t layer;
    layer_init(&layer, &area);
    CHECK(chord_pose_dispatch(source, &layer, &area, 0x1234));
    const uint16_t *map = source->draw_pose->draw_map;
    for (unsigned i = 0; i < ORB_SCREEN_D; ++i) CHECK(map[i] == i * 256 / 211);
    layer_finish(&layer);
    CHECK(!source->draw_pose_busy && source->draw_pose->draw_map_scale == 211);
    uint16_t *owned_map = (uint16_t *)map;
    owned_map[ORB_SCREEN_D - 1] = 0xbeef;
    source->presented.x1 = 1;
    source->presented.x2 = 384;
    lv_area_t translated_clip = {1, 0, 31, 15};
    CHECK(chord_pose_dispatch(source, &layer, &translated_clip, 0x1234));
    CHECK(source->draw_pose->draw_map == map && map[ORB_SCREEN_D - 1] == 0xbeef);
    layer_finish(&layer);
    source->scale = 173;
    source->presented.x2 = 314;
    source->presented.y2 = 47;
    CHECK(chord_pose_dispatch(source, &layer, &translated_clip, 0x1234));
    CHECK(source->draw_pose->draw_map == map && source->draw_pose->draw_map_scale == 173);
    for (unsigned i = 0; i < ORB_SCREEN_D; ++i) CHECK(map[i] == i * 256 / 173);
    layer_finish(&layer);
    chord_row_release(source);
    layer_delete(&layer);
}

static void bounded_pose_allocations(void)
{
    round_chord_row_t *row = allocate(sizeof(*row));
    CHECK(row);
    memset(row, 0, sizeof(*row));
    row->height = 72;
    size_t before = s_chord_pose_bytes;
    s_fail_alloc = true;
    chord_pose_prepare(row);
    CHECK(!row->draw_pose && s_chord_pose_bytes == before);
    s_chord_pose_bytes = ORB_ROUND_LIST_DRAW_POSE_BUDGET;
    chord_pose_prepare(row);
    CHECK(!row->draw_pose && s_chord_pose_bytes == ORB_ROUND_LIST_DRAW_POSE_BUDGET);
    s_chord_pose_bytes = before;
    release(row);
    /* Six ordinary four-child/72px rows exceed the visible row count of the
     * standard 320 px list viewport. The shared map must not exhaust their
     * optional pose slots or increase the existing 16 KiB global cap. */
    round_chord_row_t *visible[6];
    CHECK(before == 0);
    for (unsigned i = 0; i < 6; ++i) {
        size_t bytes = sizeof(*row) + 4 * sizeof(row->children[0]) + 72 * sizeof(uint16_t);
        visible[i] = allocate(bytes);
        CHECK(visible[i]);
        memset(visible[i], 0, bytes);
        visible[i]->count = 4;
        visible[i]->height = 72;
        chord_pose_prepare(visible[i]);
        CHECK(visible[i]->draw_pose);
        CHECK(s_chord_pose_bytes <= ORB_ROUND_LIST_DRAW_POSE_BUDGET);
    }
    printf("six visible row poses: bytes=%zu cap=%u\n", s_chord_pose_bytes, ORB_ROUND_LIST_DRAW_POSE_BUDGET);
    for (unsigned i = 0; i < 6; ++i) chord_row_release(visible[i]);
    CHECK(s_chord_pose_bytes == before);
}

static void pose_preparation_reuse(void)
{
    size_t bytes = sizeof(round_chord_row_t) + sizeof(round_chord_child_t) + 72 * sizeof(uint16_t);
    round_chord_row_t *source = allocate(bytes);
    CHECK(source);
    memset(source, 0, bytes);
    source->count = 1;
    source->insets = (uint16_t *)(source->children + 1);
    source->width = source->current_width = 466;
    source->height = 72;
    source->scale = source->inset_scale = 256;
    source->presented = (lv_area_t){0, 0, 465, 71};
    source->children[0] = (round_chord_child_t){.width = 23, .hidden = true};
    source->paint = (chord_565_t){.opacity = 255, .valid = true};
    source->paint.palette[255] = 0x5678;
    chord_pose_prepare(source);
    CHECK(source->draw_pose);
    lv_area_t area = {2, 2, 31, 15};
    lv_layer_t layer;
    layer_init(&layer, &area);
    CHECK(chord_pose_dispatch(source, &layer, &area, 0x1234));
    layer_finish(&layer);
    round_chord_row_t *pose = source->draw_pose;
    CHECK(pose->children[0].draw_width == 23);
    pose->children[0].draw_width = 17;
    pose->paint.palette[255] = 0xabcdefu & UINT16_MAX;
    pose->insets[71] = 0xbeef;
    CHECK(chord_pose_dispatch(source, &layer, &area, 0x1234));
    CHECK(pose->children[0].draw_width == 17);
    CHECK(pose->paint.palette[255] == (0xabcdefu & UINT16_MAX));
    CHECK(pose->insets[71] == 0xbeef);
    layer_finish(&layer);
    ++source->presented.y1;
    ++source->presented.y2;
    CHECK(chord_pose_dispatch(source, &layer, &area, 0x1234));
    CHECK(pose->children[0].draw_width == 23);
    layer_finish(&layer);
    source->paint.opacity = 137;
    ++source->inset_radius;
    CHECK(chord_pose_dispatch(source, &layer, &area, 0x1234));
    CHECK(pose->paint.palette[255] == 0x5678 && pose->paint.opacity == 137);
    CHECK(!pose->insets[71]);
    layer_finish(&layer);
    layer_delete(&layer);
    chord_row_release(source);
}


static void round_diagnostic_scopes(lv_display_t *display)
{
    uint32_t events = lv_display_get_event_count(display);
    orb_ui_scroll_diagnostics_begin();
    CHECK(s_scroll_diagnostics.refresh_phase_available);
    CHECK(lv_display_get_event_count(display) == events + 1);
    scroll_stage_record(orb_ui_scroll_diagnostics_clock() - 100, &s_scroll_diagnostics.hash);
    lv_display_send_event(display, LV_EVENT_REFR_START, NULL);
    CHECK(s_scroll_in_refresh);
    scroll_stage_record(orb_ui_scroll_diagnostics_clock() - 100, &s_scroll_diagnostics.hash);
    lv_display_send_event(display, LV_EVENT_REFR_READY, NULL);
    CHECK(!s_scroll_in_refresh);
    orb_ui_scroll_diagnostics_t diagnostics;
    orb_ui_scroll_diagnostics_snapshot(&diagnostics);
    CHECK(diagnostics.hash.calls == 2 && diagnostics.hash.max_us >= 100);
    CHECK(diagnostics.hash.outside_us >= 100 && diagnostics.hash.us >= diagnostics.hash.outside_us + 100);
    orb_ui_scroll_diagnostics_begin(); /* Repeated begin retires its old observer. */
    CHECK(lv_display_get_event_count(display) == events + 1);
    CHECK(!s_scroll_diagnostics.hash.calls);
    orb_ui_scroll_diagnostics_end();
    CHECK(lv_display_get_event_count(display) == events);
    CHECK(orb_ui_scroll_diagnostics_clock() == UINT64_MAX);
    lv_display_t *temporary = lv_display_create(1, 1);
    CHECK(temporary);
    lv_display_set_default(temporary);
    orb_ui_scroll_diagnostics_begin();
    CHECK(s_scroll_display == temporary);
    lv_display_delete(temporary);
    CHECK(!s_scroll_display && !s_scroll_in_refresh);
    orb_ui_scroll_diagnostics_end();
    lv_display_set_default(display);
}

static void source_pack_scratch_oom(void)
{
    size_t bytes = sizeof(round_chord_row_t) + sizeof(round_chord_child_t);
    round_chord_row_t *source = allocate(bytes);
    CHECK(source);
    memset(source, 0, bytes);
    source->count = 1;
    source->width = 20;
    source->height = 4;
    source->source_bg = 1;
    source->bytes = 20 * 4 * sizeof(uint16_t);
    source->pixels = allocate(source->bytes);
    CHECK(source->pixels);
    uint16_t *pixels = source->pixels;
    for (unsigned i = 0; i < 80; ++i) pixels[i] = i == 31 ? 2 : 1;
    source->children[0] = (round_chord_child_t){.width = 20, .height = 4};
    lv_draw_buf_init(&source->source, 20, 4, LV_COLOR_FORMAT_RGB565, 40, pixels, source->bytes);
    s_fail_alloc = true; /* Fail only the bounded transient palette/hash table. */
    CHECK(orb_round_source_pack(source));
    CHECK(!s_fail_alloc && !source->sparse && source->pixels == pixels && source->bytes == 160);
    for (unsigned i = 0; i < 80; ++i) CHECK(pixels[i] == (i == 31 ? 2 : 1));
    CHECK(orb_round_source_pack(source));
    CHECK(source->sparse && source->bytes < 160 && !source->source.data);
    for (int y = 0; y < 4; ++y) {
        chord_source_cursor_t line = chord_source_line(source, NULL, y);
        for (int x = 0; x < 20; ++x) CHECK(chord_source_pixel(source, &line, x) == (y * 20 + x == 31 ? 2 : 1));
    }
    chord_row_release(source);
}


int main(void)
{
    s_ui_thread = pthread_self();
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    CHECK(display);
    orb_ui_init(&(orb_ui_config_t){.alloc = allocate, .free = release});
    round_diagnostic_scopes(display);
    parallel_tasks();
    bounded_pose_allocations();
    pose_map_guard();
    pose_map_reuse();
    pose_preparation_reuse();
    source_pack_scratch_oom();
    for (unsigned mode = 0; mode < 3; ++mode) cached_source_retirement(display, mode);
#ifdef ORB_CHORD_METADATA_OOM_TEST
    cached_source_retirement(display, 3);
#endif
    CHECK(s_chord_pose_bytes == 0);
    lv_deinit();
    printf("chord lifetime: %u checks passed\n", s_checks);
    return 0;
}
