/* Count real production text work, rather than gating noisy host wall time.
 * CMake supplies the unmodified implementations with counter-only wrappers. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "label_work_probe.h"
#include "orb_fonts.h"
#include "orb_devtools_bench_power.h"
#include "orb_game_view.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_scroll_fixture.h"
#include "orb_ui.h"
#include "orb_workspace.h"
#include "sim_display.h"

#define COUNTERS(X) X(tasks) X(glyphs) X(lookups) X(misses) X(bitmaps) X(bitmap_pixels) \
    X(sizes) X(lines) X(widths) X(draw_ns) X(glyph_ns) X(bitmap_ns) X(line_ns)
#define DECLARE(name) atomic_uint_fast64_t name;
typedef struct { COUNTERS(DECLARE) } work_t;
#undef DECLARE
static work_t s_work;
static atomic_bool s_active;
static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static const char *s_mode;
static bool s_trace, s_cache;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "label work %s:%d: %s\n", s_mode, __LINE__, #value); exit(1); \
} } while (0)
#define ADD(name, value) atomic_fetch_add_explicit(&s_work.name, (value), memory_order_relaxed)

static uint64_t nanos(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

bool label_work_font_lookup(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                            uint32_t letter, uint32_t next)
{
    bool result = font->get_glyph_dsc(font, glyph, letter, next);
    if (atomic_load_explicit(&s_active, memory_order_relaxed)) {
        ADD(lookups, 1);
        if (!result) ADD(misses, 1);
    }
    return result;
}

bool lv_font_get_glyph_dsc_internal(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                                   uint32_t letter, uint32_t next)
{
    bool active = atomic_load_explicit(&s_active, memory_order_relaxed);
    uint64_t started = active ? nanos() : 0;
    bool result = label_work_glyph_impl(font, glyph, letter, next);
    if (active) { ADD(glyphs, 1); ADD(glyph_ns, nanos() - started); }
    return result;
}

const void *lv_font_get_glyph_bitmap_internal(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buffer)
{
    bool active = atomic_load_explicit(&s_active, memory_order_relaxed);
    uint64_t started = active ? nanos() : 0;
    const void *result = label_work_bitmap_impl(glyph, buffer);
    if (active) {
        ADD(bitmaps, 1); ADD(bitmap_pixels, (uint64_t)glyph->box_w * glyph->box_h);
        ADD(bitmap_ns, nanos() - started);
    }
    return result;
}

void lv_text_get_size_attributes(lv_point_t *size, const char *text, const lv_font_t *font,
                                 lv_text_attributes_t *attributes)
{
    if (atomic_load_explicit(&s_active, memory_order_relaxed)) {
        ADD(sizes, 1);
        if (s_trace) fprintf(stderr, "MEASURE mode=%s text='%s'\n", s_mode, text);
    }
    label_work_size_impl(size, text, font, attributes);
}

uint32_t lv_text_get_next_line(const char *text, uint32_t length, const lv_font_t *font,
                              int32_t *width, lv_text_attributes_t *attributes)
{
    bool active = atomic_load_explicit(&s_active, memory_order_relaxed);
    uint64_t started = active ? nanos() : 0;
    uint32_t result = label_work_line_impl(text, length, font, width, attributes);
    if (active) { ADD(lines, 1); ADD(line_ns, nanos() - started); }
    return result;
}

int32_t lv_text_get_line_width(const char *text, uint32_t length, const lv_font_t *font,
                              const lv_text_attributes_t *attributes)
{
    if (atomic_load_explicit(&s_active, memory_order_relaxed)) ADD(widths, 1);
    return label_work_width_impl(text, length, font, attributes);
}

void lv_draw_sw_label(lv_draw_task_t *task, const lv_draw_label_dsc_t *descriptor,
                      const lv_area_t *coords)
{
    bool active = atomic_load_explicit(&s_active, memory_order_relaxed);
    uint64_t started = active ? nanos() : 0;
    if (active) {
        ADD(tasks, 1);
        if (s_trace) fprintf(stderr,
            "LABEL mode=%s obj=%p text='%s' opa=%u area=%ld,%ld,%ld,%ld clip=%ld,%ld,%ld,%ld\n",
            s_mode, (void *)descriptor->base.obj, descriptor->text, descriptor->opa,
            (long)coords->x1, (long)coords->y1, (long)coords->x2, (long)coords->y2,
            (long)task->clip_area.x1, (long)task->clip_area.y1,
            (long)task->clip_area.x2, (long)task->clip_area.y2);
    }
    label_work_draw_impl(task, descriptor, coords);
    if (active) ADD(draw_ns, nanos() - started);
}

static uint32_t tick(void) { return s_now; }
static void advance(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 4) {
        s_now += 4;
        lv_timer_handler();
    }
    lv_refr_now(s_display);
    lv_draw_wait_for_finish();
}

static void begin(void)
{
    lv_draw_wait_for_finish();
#define ZERO(name) atomic_store_explicit(&s_work.name, 0, memory_order_relaxed);
    COUNTERS(ZERO)
#undef ZERO
    atomic_store(&s_active, true);
}

static void end(unsigned frames)
{
    lv_draw_wait_for_finish();
    atomic_store(&s_active, false);
    printf("LABELWORK mode=%s frames=%u", s_mode, frames);
#define PRINT(name) printf(" " #name "=%llu", (unsigned long long)atomic_load(&s_work.name));
    COUNTERS(PRINT)
#undef PRINT
    printf("\n");
}

static unsigned cache_labels(lv_obj_t *object)
{
    if (lv_obj_is_hidden(object)) return 0;
    unsigned count = 0;
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_obj_set_raster(object, true);
        lv_obj_raster_prepare(object);
        count++;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        count += cache_labels(lv_obj_get_child(object, i));
    return count;
}

static void cache_check(void)
{
    static uint8_t before[466 * 466 * 3], after[466 * 466 * 3];
    lv_refr_now(s_display); lv_draw_wait_for_finish();
    sim_display_rgb(before);
    unsigned labels = cache_labels(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display); lv_draw_wait_for_finish();
    sim_display_rgb(after);
    size_t changed = 0;
    for (size_t i = 0; i < sizeof(before); ++i) changed += before[i] != after[i];
    printf("LABELCACHE mode=%s labels=%u changed_channels=%zu\n", s_mode, labels, changed);
    CHECK(labels > 0);
    CHECK(changed == 0);
}

static void action(int code, float x, float y, void *context)
{
    (void)code; (void)x; (void)y; (void)context;
}

static void orbit(void)
{
    orb_game_state_t game;
    orb_game_init(&game, ORB_GAME_ORBIT, 0x4f5242u);
    orb_game_action(&game, ORB_GAME_START, 0, 0);
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), ORB_GAME_ORBIT, action, NULL);
    CHECK(view);
    for (unsigned frame = 0; frame < 180; ++frame) {
        if (frame == 120) { if (s_cache) cache_check(); begin(); }
        s_now += 17;
        orb_game_step(&game, 0, 0, true, false);
        orb_game_view_update(view, &game, 31, false, false);
        lv_timer_handler();
        lv_refr_now(s_display);
        lv_draw_wait_for_finish();
    }
    end(60);
    if (!s_cache) {
        /* Static score ink can be damaged by the moving rings, but one small
         * glyph must not grow into screen-sized label or layout work. */
        CHECK(atomic_load(&s_work.tasks) > 0 && atomic_load(&s_work.tasks) <= 74);
        CHECK(atomic_load(&s_work.glyphs) <= 222);
        CHECK(atomic_load(&s_work.bitmaps) > 0 && atomic_load(&s_work.bitmaps) <= 72);
        CHECK(atomic_load(&s_work.sizes) <= 2);
    }
    orb_game_view_destroy(view);
}

static void wake_scene(bool iris)
{
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo(); orb_home_load_demo();
    orb_model_set_connection(true, true); orb_home_set_connection(true, true);
    lv_obj_t *saved = lv_screen_active();
    orb_shell_t *shell = orb_shell_create(saved, NULL, NULL);
    CHECK(shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .clock_time = "10:09",
        .clock_date = "Wednesday, October 28"};
    orb_shell_update(shell, &status);
    for (unsigned frame = 0; frame < 100; ++frame) {
        s_now += 17; orb_shell_poll(shell); advance(0);
    }
    orb_shell_power_enable(shell, true);
    orb_shell_power(shell);
    s_now += 500; orb_shell_poll(shell); advance(0);
    orb_power_snapshot_t power = orb_shell_power_snapshot(shell);
    CHECK(power.state == ORB_POWER_ALWAYS_ON && power.redact);
    orb_power_input_result_t wake = orb_shell_power_wake(shell, ORB_POWER_WAKE_TOUCH);
    CHECK(wake.accepted && wake.consume_input);
    CHECK(orb_shell_power_snapshot(shell).moment == ORB_POWER_MOMENT_WAKE);
    s_now += 100; orb_shell_poll(shell); advance(0);
    if (!getenv("ORB_LABEL_WORK_FROZEN_WAKE")) {
        /* Firmware benchmark admission must finish this wake before it owns
         * the refresh loop and pauses ordinary shell polling. The raw contact
         * is consumed above; no contact is replayed into the destination. */
        for (unsigned elapsed = 0; elapsed < 2000; elapsed += 10) {
            s_now += 10; orb_shell_poll(shell); advance(0);
            if (orb_devtools_bench_power_settled(shell)) break;
        }
        CHECK(orb_devtools_bench_power_settled(shell));
    }
    if (iris) {
        CHECK(orb_shell_nav_bench(shell, "iris", 0, false));
        advance(0); begin();
        for (unsigned frame = 0; frame < 30; ++frame) {
            s_now += 17;
            CHECK(orb_shell_nav_bench(shell, "iris", frame * 17 + 1, false));
            lv_timer_handler(); advance(0);
        }
        end(30);
        CHECK(atomic_load(&s_work.tasks) == 0);
        /* First exposure may prepare nearby row sources after the wake;
         * public Face labels must still be absent from every iris band. */
        CHECK(atomic_load(&s_work.glyphs) <= 96);
        CHECK(atomic_load(&s_work.bitmaps) <= 22);
        CHECK(atomic_load(&s_work.sizes) == 0);
        CHECK(orb_shell_nav_bench(shell, "iris", 0, true));
    } else {
        lv_obj_t *screen = lv_obj_create(NULL);
        CHECK(screen); lv_screen_load(screen);
        orbit();
        lv_screen_load(saved); lv_obj_delete(screen);
    }
    orb_shell_destroy(shell);
}

static void list(void)
{
    CHECK(orb_model_init() && orb_home_init());
    orb_scroll_fixture_t *fixture = orb_scroll_fixture_create(lv_screen_active(), ORB_SCROLL_AGENTS);
    CHECK(fixture);
    advance(1200);
    bool prepared = false;
    for (unsigned attempt = 0; attempt < 5 && !prepared; ++attempt) {
        prepared = orb_scroll_fixture_prepare(fixture);
        advance(100);
    }
    CHECK(prepared);
    begin();
    for (unsigned elapsed = 0; elapsed <= 1200; elapsed += 16) {
        orb_scroll_fixture_tick(fixture, elapsed);
        advance(16);
    }
    end(76);
    CHECK(orb_scroll_fixture_gestures(fixture) == 1);
    orb_scroll_fixture_diagnostics_t diagnostics;
    orb_scroll_fixture_diagnostics(fixture, &diagnostics);
    CHECK(diagnostics.offset_changes > 0);
    /* Newly exposed rows may prepare once; warm moving rows must remain
     * compositor copies and must never remeasure text at every scroll step. */
    CHECK(atomic_load(&s_work.tasks) <= 3);
    CHECK(atomic_load(&s_work.glyphs) <= 640);
    CHECK(atomic_load(&s_work.bitmaps) <= 35);
    CHECK(atomic_load(&s_work.sizes) <= 3);
    CHECK(atomic_load(&s_work.lines) <= 15);
    orb_scroll_fixture_destroy(fixture);
}

static void face(bool iris)
{
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo(); orb_home_load_demo();
    orb_model_set_connection(true, true); orb_home_set_connection(true, true);
    orb_workspace_t *workspace = orb_workspace_create(lv_screen_active(), NULL, NULL, NULL);
    CHECK(workspace);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .clock_time = "10:09",
        .clock_date = "Wednesday, October 28"};
    orb_workspace_update(workspace, &status);
    advance(1600);
    if (s_cache) cache_check();
    if (iris) {
        CHECK(orb_workspace_nav_bench(workspace, "iris", 0, false));
        lv_refr_now(s_display); lv_draw_wait_for_finish();
    }
    begin();
    unsigned frames = iris ? 30 : 60;
    for (unsigned frame = 0; frame < frames; ++frame) {
        if (iris) {
            s_now += 17;
            CHECK(orb_workspace_nav_bench(workspace, "iris", frame * 17 + 1, false));
            orb_workspace_poll(workspace);
            lv_timer_handler();
        } else lv_obj_invalidate(lv_screen_active());
        lv_refr_now(s_display);
        lv_draw_wait_for_finish();
    }
    end(frames);
    if (iris) {
        /* The compositor owns immutable snapshots after cold preparation. A
         * moving iris must never re-enter labels or remeasure their text. */
        CHECK(atomic_load(&s_work.tasks) == 0);
        CHECK(atomic_load(&s_work.glyphs) == 0);
        CHECK(atomic_load(&s_work.bitmaps) == 0);
        CHECK(atomic_load(&s_work.sizes) == 0);
        CHECK(orb_workspace_nav_bench(workspace, "iris", 0, true));
    } else if (!s_cache) {
        CHECK(atomic_load(&s_work.tasks) > 0 && atomic_load(&s_work.tasks) <= 780);
        CHECK(atomic_load(&s_work.glyphs) <= 6600);
        /* A glyph crossing a band is blended in each clip, but glyph ink
         * wholly outside that clip must not be decoded. */
        CHECK(atomic_load(&s_work.bitmaps) <= 2400);
        CHECK(atomic_load(&s_work.sizes) == 0);
    }
    orb_workspace_destroy(workspace);
}

int main(int argc, char **argv)
{
    s_mode = argc > 1 ? argv[1] : "orbit";
    CHECK(argc <= 2);
    s_trace = getenv("ORB_LABEL_WORK_TRACE") != NULL;
    s_cache = getenv("ORB_LABEL_WORK_CACHE") != NULL;
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL);
    CHECK(orb_fonts_preload_default() == ORB_FONTS_OK);
    if (!strcmp(s_mode, "orbit")) orbit();
    else if (!strcmp(s_mode, "wake-orbit")) wake_scene(false);
    else if (!strcmp(s_mode, "wake-iris")) wake_scene(true);
    else if (!strcmp(s_mode, "list")) list();
    else if (!strcmp(s_mode, "face")) face(false);
    else if (!strcmp(s_mode, "iris")) face(true);
    else CHECK(false);
    printf("label work %s: %u checks passed\n", s_mode, s_checks);
    return EXIT_SUCCESS;
}
