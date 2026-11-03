/* Production surface fixtures must leave real models and queues untouched,
 * drive offscreen rows into view, and stop redrawing after their fling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "orb_scroll_fixture.h"
#include "orb_devtools_scroll_fixture.h"
#include "orb_model.h"
#include "orb_home.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_frames;
static bool s_perf_active;
static uint64_t s_perf_ns[512];
static unsigned s_perf_count;
static uint8_t s_partial[466 * 466 * 3], s_full[466 * 466 * 3];
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static uint32_t tick(void) { return s_now; }
typedef struct { unsigned down, up, scroll; uint32_t previous; bool pressed; } reports_t;
static void observed(orb_scroll_event_t event, int32_t x, int32_t y, void *ctx)
{
    reports_t *reports = ctx;
    if (event == ORB_SCROLL_OFFSET) { reports->scroll++; return; }
    CHECK(x >= 0 && x < 466 && y >= 0 && y < 466);
    if (event == ORB_SCROLL_INPUT) {
        if (reports->pressed) CHECK(s_now - reports->previous >= ORB_SCROLL_REPORT_MS);
        reports->down++; reports->previous = s_now; reports->pressed = true;
    } else { CHECK(reports->pressed); reports->up++; reports->pressed = false; }
}
static void rendered(lv_event_t *e) { LV_UNUSED(e); s_frames++; }
static uint64_t wall_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static void advance(unsigned ms)
{
    for (unsigned n = 0; n < ms;) {
        unsigned step = LV_MIN(4, ms - n); s_now += step; n += step;
        unsigned frames = s_frames;
        uint64_t start = s_perf_active ? wall_ns() : 0;
        lv_timer_handler();
        if (s_perf_active && s_frames != frames) {
            CHECK(s_perf_count < sizeof(s_perf_ns) / sizeof(s_perf_ns[0]));
            s_perf_ns[s_perf_count++] = wall_ns() - start;
        }
    }
}

static int compare_ns(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static void report_handler_perf(const char *surface, unsigned mode)
{
    CHECK(s_perf_count > 0);
    qsort(s_perf_ns, s_perf_count, sizeof(s_perf_ns[0]), compare_ns);
    printf("scroll_handler surface=%s mode=%u samples=%u median_us=%.2f p95_us=%.2f max_us=%.2f\n",
           surface, mode, s_perf_count, s_perf_ns[s_perf_count / 2] / 1000.0,
           s_perf_ns[(s_perf_count - 1) * 95 / 100] / 1000.0,
           s_perf_ns[s_perf_count - 1] / 1000.0);
}
static lv_point_t s_touch;
static bool s_pressed;
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{ LV_UNUSED(input); data->point = s_touch; data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static void tap(lv_indev_t *input, int x, int y)
{
    s_touch = (lv_point_t){x, y}; s_pressed = true; lv_indev_read(input); advance(40);
    s_pressed = false; lv_indev_read(input); advance(40);
}
static void save(const char *directory, const char *name, unsigned mode, const char *phase)
{
    if (!directory) return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/scroll-%s-%u-%s.png", directory, name, mode, phase);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    CHECK(sim_display_save_png(path));
}

static lv_obj_t *find_list(lv_obj_t *parent)
{
    orb_round_list_cache_stats_t cache;
    if (orb_round_list_cache_stats(parent, &cache)) return parent;
    if (lv_obj_get_child_count(parent) > 2 && lv_obj_get_raster(lv_obj_get_child(parent, 0))) return parent;
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t *list = find_list(lv_obj_get_child(parent, i));
        if (list) return list;
    }
    return NULL;
}

static void check_pixels(lv_display_t *display, lv_obj_t *screen)
{
    lv_refr_now(display);
    sim_display_rgb(s_partial);
    lv_obj_invalidate(screen);
    lv_refr_now(display);
    sim_display_rgb(s_full);
    CHECK(!memcmp(s_partial, s_full, sizeof(s_full)));
}

typedef struct { unsigned begin, end, scroll; } owner_events_t;
static void owner_event(lv_event_t *event)
{
    owner_events_t *events = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_BEGIN) events->begin++;
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) events->end++;
    if (lv_event_get_code(event) == LV_EVENT_SCROLL) events->scroll++;
}

typedef struct { unsigned calls; lv_obj_t *replacement; bool replace; } owner_lifetime_t;
static void owner_deleted_by_callback(lv_event_t *event)
{
    owner_lifetime_t *lifetime = lv_event_get_user_data(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    lv_obj_t *parent = lv_obj_get_parent(obj);
    lifetime->calls++;
    lv_obj_delete(obj);
    /* A replacement may reuse an allocator address; the stopped owner's
     * lifetime must remain over regardless of object-validity lookups. */
    if (lifetime->replace) lifetime->replacement = lv_obj_create(parent);
}

static void owner_snapped_by_callback(lv_event_t *event)
{
    unsigned *calls = lv_event_get_user_data(event);
    if ((*calls)++) return;
    lv_obj_scroll_to_y(lv_event_get_current_target_obj(event), 0, LV_ANIM_OFF);
}

static lv_obj_t *lifetime_list(void)
{
    lv_obj_t *list = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 120);
    lv_obj_set_size(list, 466, 240);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 83, i * 96);
        lv_obj_set_size(row, 300, 96);
    }
    CHECK(orb_round_list_apply(list, NULL));
    lv_obj_update_layout(list);
    return list;
}

static void check_owner_callback_lifetimes(void)
{
    const lv_event_code_t phases[] = {LV_EVENT_SCROLL_END, LV_EVENT_SCROLL_BEGIN,
                                     LV_EVENT_SCROLL, LV_EVENT_SCROLL_END};
    for (unsigned phase = 0; phase < 4; phase++) {
        lv_obj_t *list = lifetime_list();
        owner_lifetime_t lifetime = {.replace = phase == 0};
        if (phase == 0) {
            /* Cancelling native momentum synchronously emits SCROLL_END. */
            lv_obj_scroll_to_y(list, 40, LV_ANIM_ON);
            advance(40);
            CHECK(lv_obj_is_scrolling(list));
        } else if (phase == 3) CHECK(orb_round_list_scroll_to(list, 8));
        lv_obj_add_event_cb(list, owner_deleted_by_callback, phases[phase], &lifetime);
        if (phase == 3) orb_round_list_scroll_finish(list);
        else CHECK(orb_round_list_scroll_to(list, phase == 0 ? 48 : 20));
        CHECK(lifetime.calls == 1);
        if (lifetime.replace) {
            CHECK(lifetime.replacement != NULL);
            lv_obj_delete(lifetime.replacement);
        }
        advance(8);
    }
    lv_obj_t *list = lifetime_list();
    lv_obj_scroll_to_y(list, 40, LV_ANIM_ON);
    advance(40);
    unsigned snaps = 0;
    lv_obj_add_event_cb(list, owner_snapped_by_callback, LV_EVENT_SCROLL_END, &snaps);
    CHECK(orb_round_list_scroll_to(list, 47));
    CHECK(snaps > 0 && lv_obj_get_scroll_y(list) == 47);
    orb_round_list_scroll_finish(list);
    lv_obj_delete(list);
}

static void check_continuous_owner(lv_display_t *display)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 120);
    lv_obj_set_size(list, 466, 240);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    CHECK(!orb_round_list_scroll_to(list, 10));
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 83, i * 96);
        lv_obj_set_size(row, 300, 96);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x606060 + i * 0x202020), 0);
        lv_obj_set_style_bg_opa(row, 255, 0);
    }
    CHECK(orb_round_list_apply(list, &(orb_round_list_config_t){.margin = 24, .fade = 38}));
    lv_obj_raster_prepare(screen);
    lv_refr_now(display);
    owner_events_t events = {0};
    lv_obj_add_event_cb(list, owner_event, LV_EVENT_ALL, &events);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    for (int y = 4; y <= 40; y += 4) {
        CHECK(orb_round_list_scroll_to(list, y));
        CHECK(lv_obj_get_scroll_y(list) == y);
        CHECK(orb_round_list_scroll_active(list));
        CHECK(events.begin == 1 && events.end == 0);
        check_pixels(display, screen);
    }
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);
    CHECK(events.scroll == 10);
    orb_round_list_scroll_finish(list);
    CHECK(!orb_round_list_scroll_active(list) && events.end == 1);
    orb_round_list_scroll_finish(list);
    CHECK(events.end == 1);
    CHECK(orb_round_list_scroll_to(list, 40));
    CHECK(events.begin == 1 && !orb_round_list_scroll_active(list));
    CHECK(orb_round_list_scroll_to(list, 100000));
    CHECK(lv_obj_get_scroll_bottom(list) == 0);
    CHECK(events.begin == 2 && events.end == 1);
    CHECK(orb_round_list_scroll_to(list, -100000));
    CHECK(lv_obj_get_scroll_y(list) == 0);
    orb_round_list_scroll_finish(list);
    CHECK(events.begin == 2 && events.end == 2);
    check_pixels(display, screen);
    lv_obj_delete(list);
    CHECK(!orb_round_list_scroll_to(NULL, 10));
    CHECK(!orb_round_list_scroll_active(NULL));
    orb_round_list_scroll_finish(NULL);
}

static void check_unchanged_rows(lv_display_t *display, lv_obj_t *screen, bool stepped)
{
    lv_obj_t *list = find_list(screen); CHECK(list);
    /* Re-delivered scroll/layout notifications must not restart layout or
     * rebuild retained text when the viewport and readability are unchanged. */
    for (unsigned pressed = 0; pressed < 2; pressed++) {
        lv_obj_t *row = lv_obj_get_child(list, 0);
        if (pressed) lv_obj_add_state(row, LV_STATE_PRESSED);
        advance(1000);
        /* A SCROLL event truthfully reveals the rim. Prime that one chrome change
         * before checking repeated notifications at the same content position. */
        lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
        lv_refr_now(display);
        lv_obj_raster_stats_t before, after;
        lv_obj_raster_get_stats(&before);
        unsigned frames = s_frames;
        orb_ui_scroll_diagnostics_begin();
        for (unsigned i = 0; i < 10; i++) {
            lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
            lv_obj_send_event(list, LV_EVENT_LAYOUT_CHANGED, NULL);
            lv_refr_now(display);
        }
        orb_ui_scroll_diagnostics_t diagnostics;
        orb_ui_scroll_diagnostics_snapshot(&diagnostics);
        orb_ui_scroll_diagnostics_end();
        lv_obj_raster_get_stats(&after);
        CHECK(diagnostics.readable_calls == (stepped ? 0 : 20));
        CHECK(diagnostics.style_writes == 0 && diagnostics.font_changes == 0);
        CHECK(after.builds == before.builds);
        CHECK(s_frames == frames);
        if (pressed) lv_obj_remove_state(row, LV_STATE_PRESSED);
    }
}

#if defined(CONFIG_ORB_RENDER_ROWS_AT_SIZE) && CONFIG_ORB_RENDER_ROWS_AT_SIZE
static void check_warm_native_scroll(lv_display_t *display, lv_obj_t *screen,
                                     const char *directory, const char *name, unsigned mode)
{
    lv_obj_t *list = find_list(screen); CHECK(list);
    /* Production idle slices prepare the local viewport and its neighbours;
     * this test must not manually prime sources that the board never sees. */
    advance(200);
    CHECK(lv_obj_raster_rows_ready(lv_obj_get_child(list, 0)));
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    orb_ui_scroll_diagnostics_begin();
    for(unsigned repeat = 0; repeat < 3; repeat++) {
        for(int position = 0; position <= 80; position += 8) {
            CHECK(orb_round_list_scroll_to(list, position));
            lv_refr_now(display);
        }
        if(!repeat) save(directory, name, mode, "warm-preview");
        for(int position = 80; position >= 0; position -= 8) {
            CHECK(orb_round_list_scroll_to(list, position));
            lv_refr_now(display);
        }
    }
    orb_ui_scroll_diagnostics_t diagnostics;
    orb_ui_scroll_diagnostics_snapshot(&diagnostics);
    orb_ui_scroll_diagnostics_end();
    lv_obj_raster_get_stats(&after);
    CHECK(diagnostics.font_changes == 0 && diagnostics.style_writes == 0);
    CHECK(after.builds == before.builds);
    orb_round_list_scroll_finish(list);
}
#endif

int main(int argc, char **argv)
{
    CHECK(argc <= 2);
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create(); CHECK(display);
    lv_display_add_event_cb(display, rendered, LV_EVENT_RENDER_READY, NULL);
    orb_ui_init(NULL);
    lv_indev_t *input = lv_indev_create(); CHECK(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(input, read_pointer);
    lv_indev_set_display(input, display);
    CHECK(orb_model_init()); CHECK(orb_home_init());
    check_continuous_owner(display);
    check_owner_callback_lifetimes();
    orb_model_load_demo(); orb_home_load_demo();
    CHECK(orb_model_voice_focus("retained-owner-focus", "retained-owner-card"));
    orb_model_snapshot_t *before = calloc(1, sizeof(*before)), *after = calloc(1, sizeof(*after));
    orb_home_snapshot_t *home_before = calloc(1, sizeof(*home_before)), *home_after = calloc(1, sizeof(*home_after));
    CHECK(before && after && home_before && home_after);
    CHECK(orb_model_snapshot(before)); CHECK(orb_home_snapshot(home_before));
    lv_obj_t *saved = lv_screen_active();
    orb_scroll_surface_t parsed;
    CHECK(!orb_scroll_surface_parse("unknown", &parsed));
    CHECK(!orb_scroll_surface_parse(NULL, &parsed));
    CHECK(orb_scroll_fixture_viewport_pixels(NULL) == 0);
    for (unsigned mode = 0; mode < 4; mode++) {
        CHECK(orb_fonts_set_content_size(mode & 1 ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency((mode & 2) != 0);
        for (unsigned surface = 0; surface < ORB_SCROLL_SURFACE_COUNT; surface++) {
            const char *name = orb_scroll_surface_name(surface);
            CHECK(orb_scroll_surface_parse(name, &parsed) && parsed == surface);
            lv_obj_t *screen = lv_obj_create(NULL);
            lv_obj_remove_style_all(screen);
            lv_obj_set_size(screen, 466, 466);
            lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(screen, 255, 0);
            orb_scroll_fixture_t *fixture = orb_devtools_scroll_fixture_create(screen, surface);
            CHECK(fixture);
            lv_screen_load(screen); advance(1000);
            bool prepared = false;
            for (unsigned attempts = 0; attempts < 5 && !prepared; attempts++) {
                prepared = orb_scroll_fixture_prepare(fixture); advance(100);
            }
            CHECK(prepared);
#if defined(CONFIG_ORB_RENDER_ROWS_AT_SIZE) && CONFIG_ORB_RENDER_ROWS_AT_SIZE
            if (surface == ORB_SCROLL_AGENTS || surface == ORB_SCROLL_SETTINGS || surface == ORB_SCROLL_NOTIFICATIONS) {
                lv_obj_raster_rows_stats_t rows;
                lv_obj_raster_rows_get_stats(&rows);
                if (!rows.rows) fprintf(stderr, "no row admission: surface=%s mode=%u\n", name, mode);
                CHECK(rows.rows > 0); /* Actual transparent production rows were admitted. */
            }
#endif
            uint32_t viewport = orb_scroll_fixture_viewport_pixels(fixture);
            CHECK(viewport > 0 && viewport <= 466u * 466u);
#if defined(CONFIG_ORB_RENDER_ROWS_AT_SIZE) && CONFIG_ORB_RENDER_ROWS_AT_SIZE
            if(surface == ORB_SCROLL_AGENTS || surface == ORB_SCROLL_SETTINGS || surface == ORB_SCROLL_NOTIFICATIONS)
                check_warm_native_scroll(display, screen, argc == 2 ? argv[1] : NULL, name, mode);
#endif
            save(argc == 2 ? argv[1] : NULL, name, mode, "rest");
            /* Physical input on navigation and Home's room rows cannot turn
             * a private fixture into a live request. */
            tap(input, 101, 101); tap(input, 233, 210); tap(input, 187, 195);
            advance(500);
            reports_t reports = {0};
            orb_scroll_fixture_observe(fixture, observed, &reports);
            unsigned frames = s_frames;
            orb_ui_scroll_diagnostics_begin();
            s_perf_count = 0; s_perf_active = true;
            for (unsigned ms = 0; ms <= 650; ms += 4) {
                orb_scroll_fixture_tick(fixture, ms); advance(4);
            }
            s_perf_active = false;
            report_handler_perf(name, mode);
            CHECK(reports.down == 15 && reports.up == 1 && !reports.pressed);
            orb_scroll_fixture_diagnostics_t diagnostics;
            orb_scroll_fixture_diagnostics(fixture, &diagnostics);
            CHECK(diagnostics.reports == 15 && diagnostics.releases == 1 && diagnostics.skipped_reports == 0);
            CHECK(diagnostics.report_gap_max_ms == ORB_SCROLL_REPORT_MS);
            orb_ui_scroll_diagnostics_t ui_diagnostics;
            orb_ui_scroll_diagnostics_snapshot(&ui_diagnostics);
            printf("scroll_perf surface=%s mode=%u frames=%u calls=%llu round_us=%llu round_max_us=%llu rows=%llu readable_calls=%llu style_writes=%llu font_changes=%llu\n",
                   name, mode, s_frames - frames,
                   (unsigned long long)ui_diagnostics.round_calls,
                   (unsigned long long)ui_diagnostics.round_us,
                   (unsigned long long)ui_diagnostics.round_max_us,
                   (unsigned long long)ui_diagnostics.round_rows,
                   (unsigned long long)ui_diagnostics.readable_calls,
                   (unsigned long long)ui_diagnostics.style_writes,
                   (unsigned long long)ui_diagnostics.font_changes);
            CHECK(diagnostics.offset_changes > 0 && diagnostics.scroll_events >= diagnostics.offset_changes);
            CHECK(ui_diagnostics.round_calls > 0 && ui_diagnostics.round_rows > 0);
            if (surface == ORB_SCROLL_AGENTS || surface == ORB_SCROLL_SETTINGS || surface == ORB_SCROLL_NOTIFICATIONS) {
                /* Design1.0 translates two cached presentation sizes. No
                 * text sizing/style writes are permitted during scrolling. */
                CHECK(ui_diagnostics.readable_calls == 0 && ui_diagnostics.style_writes == 0);
                CHECK(ui_diagnostics.redundant_style_writes == 0);
            }
            orb_ui_scroll_diagnostics_end();
            orb_ui_scroll_diagnostics_t frozen;
            advance(4);
            orb_ui_scroll_diagnostics_snapshot(&frozen);
            CHECK(!memcmp(&frozen, &ui_diagnostics, sizeof(frozen)));
            CHECK(orb_scroll_fixture_gestures(fixture) == 1);
            if (s_frames == frames) fprintf(stderr, "no motion surface=%s mode=%u position=%ld\n", name, mode,
                                            (long)orb_scroll_fixture_position(fixture));
            CHECK(s_frames > frames);
            CHECK(orb_scroll_fixture_position(fixture) > 0);
            save(argc == 2 ? argv[1] : NULL, name, mode, "scrolled");
            check_pixels(display, screen);
            orb_scroll_fixture_tick(fixture, 1200); advance(900);
            CHECK(!orb_scroll_fixture_moving(fixture));
            frames = s_frames;
            advance(1000);
            if (s_frames != frames) fprintf(stderr, "idle surface=%s mode=%u frames=%u\n", name, mode, s_frames - frames);
            CHECK(s_frames == frames);
            if (surface == ORB_SCROLL_AGENTS || surface == ORB_SCROLL_SETTINGS || surface == ORB_SCROLL_NOTIFICATIONS)
                check_unchanged_rows(display, screen, true);
            int32_t settled_position = orb_scroll_fixture_position(fixture);
            lv_obj_t *settled_list = find_list(screen);
            int32_t extent = settled_list ? lv_obj_get_scroll_y(settled_list) +
                lv_obj_get_scroll_bottom(settled_list) : 0;
            orb_scroll_fixture_tick(fixture, 1600); advance(300);
            CHECK(orb_scroll_fixture_gestures(fixture) == 2);
            CHECK(reports.down == 16);
            orb_scroll_fixture_tick(fixture, 2250); advance(900);
            CHECK(reports.down == 16 && reports.up == 2); /* No catch-up reports. */
            orb_scroll_fixture_diagnostics(fixture, &diagnostics);
            CHECK(diagnostics.reports == 16 && diagnostics.releases == 2 && diagnostics.skipped_reports == 14);
            /* The fixture reverses at a settled boundary. Kit72px rows plus
             * their12px gap can reach the bottom on the first fling, so the
             * next sparse stroke legitimately travels back. Compare against
             * settled geometry, retaining direction and bounds checks instead
             * of assuming every sparse stroke must increase its offset. */
            if ((mode & 1) && surface == ORB_SCROLL_AGENTS) {
                CHECK(settled_list && extent > 0);
                int32_t position = orb_scroll_fixture_position(fixture);
                CHECK(position >= 0 && position <= extent);
                if (settled_position >= extent - 2) CHECK(position <= settled_position);
                else CHECK(position >= settled_position);
                printf("sparse_scroll mode=%u settled=%d extent=%d after=%d direction=%s\n",
                    mode, settled_position, extent, position,
                    settled_position >= extent - 2 ? "reverse" : "forward");
            }
            CHECK(orb_model_snapshot(after)); CHECK(orb_home_snapshot(home_after));
            CHECK(!memcmp(before, after, sizeof(*before)));
            CHECK(!memcmp(home_before, home_after, sizeof(*home_before)));
            char target[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
            CHECK(orb_model_get_voice_focus(target, card));
            CHECK(!strcmp(target, "retained-owner-focus") && !strcmp(card, "retained-owner-card"));
            CHECK(orb_model_pending_answers() == 0);
            orb_home_request_t request; CHECK(!orb_home_take_request(&request));
            /* A benchmark may end during released momentum, not just at rest.
             * Its pointer animation must not survive fixture destruction. */
            int32_t before_stroke = orb_scroll_fixture_position(fixture);
            for (unsigned ms = 0; ms <= 256; ms += 16) {
                orb_scroll_fixture_tick(fixture, 3200 + ms); advance(16);
            }
            if (!orb_scroll_fixture_moving(fixture)) fprintf(stderr, "no active teardown: %s mode=%u\n", name, mode);
            if (surface == ORB_SCROLL_AGENTS) {
                CHECK(orb_scroll_fixture_moving(fixture));
                /* Cached rows can reach the lower boundary earlier. The next
                 * stroke then reverses, as the production fixture specifies. */
                CHECK(orb_scroll_fixture_position(fixture) != before_stroke);
            }
            orb_scroll_fixture_destroy(fixture);
            CHECK(orb_model_snapshot(after)); CHECK(orb_home_snapshot(home_after));
            CHECK(!memcmp(before, after, sizeof(*before)));
            CHECK(!memcmp(home_before, home_after, sizeof(*home_before)));
            orb_prefs_patch_t prefs; CHECK(!orb_model_take_prefs_request(&prefs));
            orb_idle_request_t idle; CHECK(!orb_model_take_idle_request(&idle));
            orb_voice_request_t voice; CHECK(!orb_model_take_voice_request(&voice));
            orb_message_t message; CHECK(!orb_model_take_message(&message));
            lv_screen_load(saved); lv_obj_delete(screen); advance(100);
        }
    }
    free(before); free(after); free(home_before); free(home_after);
    printf("scroll fixtures: %u checks passed (5 real surfaces × 4 type/transparency modes)\n", s_checks);
    return 0;
}
