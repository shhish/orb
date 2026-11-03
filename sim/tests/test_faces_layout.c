/* Faces 2.0: render the actual component, check retained text geometry and
 * interaction contracts. PNGs are the separate human visual-review evidence. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "orb_faces.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_memory.h"
#include "orb_shell.h"
#include "sim_display.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/misc/lv_text_private.h"

static uint32_t s_now;
static unsigned s_checks, s_failures, s_opens;
static const char *s_scene, *s_state, *s_mode, *s_output;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static FILE *s_metrics;
static orb_faces_target_t s_target;
extern size_t sim_memory_allocation_calls(void);

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "Faces %s/%s/%s: %s:%d: %s\n", s_scene, s_state, s_mode, \
        __FILE__, __LINE__, #value); exit(EXIT_FAILURE); } } while (0)
#define FIT(value) do { ++s_checks; if (!(value)) { ++s_failures; \
    fprintf(stderr, "Faces layout %s/%s/%s: %s:%d: %s\n", s_scene, s_state, s_mode, \
        __FILE__, __LINE__, #value); } } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_input;
}
static void opened(orb_faces_target_t target, void *context)
{
    (void)context;
    s_target = target;
    s_opens++;
}
static bool persist_failed(const orb_faces_prefs_t *prefs, void *context)
{
    (void)prefs; (void)context;
    return false;
}
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 16) {
        s_now += 16;
        if (s_pointer) lv_indev_read(s_pointer);
        if (s_shell) orb_shell_poll(s_shell);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}
static void pointer(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    advance(16);
}
static void tap_object(lv_obj_t *object)
{
    CHECK(object);
    lv_area_t a;
    lv_obj_get_coords(object, &a);
    int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
    pointer(x, y, true);
    advance(64);
    pointer(x, y, false);
    advance(240);
}
static size_t retained(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor.total_size;
}
static lv_obj_t *screen(void)
{
    lv_obj_t *old = lv_screen_active();
    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 466, 466);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    lv_screen_load(root);
    lv_obj_delete(old);
    return root;
}
static unsigned primary_count(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return 0;
    unsigned count = orb_kit_is_primary(root);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        count += primary_count(lv_obj_get_child(root, i));
    return count;
}
static bool visible_text(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return false;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        if (visible_text(lv_obj_get_child(root, i), text)) return true;
    return false;
}
typedef struct { lv_obj_t *object; lv_area_t bounds; } shape_t;
static size_t rings(lv_obj_t *root, shape_t *out, size_t count)
{
    if (!lv_obj_is_visible(root)) return count;
    bool native = true;
    for (lv_obj_t *p = root; p; p = lv_obj_get_parent(p))
        if (lv_obj_get_style_transform_scale_x(p, 0) != 256 ||
            lv_obj_get_style_transform_scale_y(p, 0) != 256) native = false;
    if (lv_obj_check_type(root, &lv_arc_class) && count < 16 && native) {
        out[count].object = root;
        lv_obj_get_coords(root, &out[count++].bounds);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        count = rings(lv_obj_get_child(root, i), out, count);
    return count;
}
static size_t lines(lv_obj_t *root, shape_t *out, size_t count)
{
    if (!lv_obj_is_visible(root)) return count;
    if (lv_obj_check_type(root, &lv_line_class) && count < 256) {
        out[count].object = root;
        lv_obj_get_coords(root, &out[count++].bounds);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) count = lines(lv_obj_get_child(root, i), out, count);
    return count;
}
static bool line_contains(const shape_t *line, double x, double y)
{
    for (lv_obj_t *p = lv_obj_get_parent(line->object); p; p = lv_obj_get_parent(p)) {
        if (lv_obj_is_overflow_visible(p)) continue;
        lv_area_t a; lv_obj_get_coords(p, &a);
        if (x < a.x1 || x > a.x2 || y < a.y1 || y > a.y2) return false;
    }
    const lv_point_precise_t *points = lv_line_get_points(line->object);
    if (lv_line_get_point_count(line->object) != 2) return false;
    double ax = line->bounds.x1 + points[0].x, ay = line->bounds.y1 + points[0].y;
    double bx = line->bounds.x1 + points[1].x, by = line->bounds.y1 + points[1].y;
    double dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
    double t = length ? fmax(0, fmin(1, ((x - ax) * dx + (y - ay) * dy) / length)) : 0;
    return hypot(x - (ax + t * dx), y - (ay + t * dy)) <=
        lv_obj_get_style_line_width(line->object, 0) / 2. + .5;
}
static bool descendant(lv_obj_t *object, lv_obj_t *ancestor)
{
    for (; object; object = lv_obj_get_parent(object)) if (object == ancestor) return true;
    return false;
}
static bool arc_contains(const shape_t *arc, double x, double y)
{
    const lv_area_t *a = &arc->bounds;
    double cx = (a->x1 + a->x2 + 1) / 2., cy = (a->y1 + a->y2 + 1) / 2.;
    double outer = lv_area_get_width(a) / 2.;
    int stroke = lv_obj_get_style_arc_width(arc->object, LV_PART_MAIN);
    double distance = hypot(x - cx, y - cy);
    if (distance > outer + .5 || distance < outer - stroke - .5) return false;
    double start = lv_arc_get_bg_angle_start(arc->object);
    double end = lv_arc_get_bg_angle_end(arc->object);
    if (fabs(end - start) >= 359.9) return true;
    double angle = atan2(y - cy, x - cx) * 180. / M_PI - lv_arc_get_rotation(arc->object);
    angle = fmod(angle + 720. - start, 360.);
    double sweep = fmod(end - start + 720., 360.);
    return angle <= sweep;
}
static void check_shapes(orb_faces_t *faces, const orb_faces_bounds_t *bounds, size_t count)
{
    /* Snapshot actual glyph alpha, including font bearings/kerning, instead
     * of rejecting a clock because an empty line-box corner reaches a rim.
     * This is test-only work; production drawing does not take snapshots. */
    static uint8_t occupied[466 * 466];
    memset(occupied, 0, sizeof(occupied));
    shape_t arcs[16];
    size_t arc_count = rings(orb_faces_root(faces), arcs, 0);
    shape_t strokes[256];
    size_t stroke_count = lines(orb_faces_root(faces), strokes, 0);
    for (unsigned i = 0; i < count; ++i) {
        bool rim = !strncmp(bounds[i].role, "rim-", 4) && strcmp(bounds[i].role, "rim-ring");
        if (!strstr(bounds[i].role, "text") && !rim) continue;
        lv_obj_t *object = bounds[i].object;
        if (!lv_obj_is_visible(object)) continue;
        lv_draw_buf_t *mask = lv_snapshot_take(object, LV_COLOR_FORMAT_ARGB8888);
        CHECK(mask);
        lv_area_t origin; lv_obj_get_coords(object, &origin);
        int ext = lv_obj_get_ext_draw_size(object);
        origin.x1 -= ext; origin.y1 -= ext;
        unsigned lit = 0, clipped = 0, text_overlap = 0, disc_overlap = 0, ring_overlap = 0, line_overlap = 0;
        for (unsigned py = 0; py < mask->header.h; ++py) {
            const lv_color32_t *row = (const lv_color32_t *)(mask->data + py * mask->header.stride);
            for (unsigned px = 0; px < mask->header.w; ++px) {
                if (row[px].alpha < 16) continue;
                lit++;
                int x = origin.x1 + px, y = origin.y1 + py;
                double radius = rim ? 232 : 217;
                if (x < 0 || x >= 466 || y < 0 || y >= 466 || hypot(x - 233, y - 233) > radius) {
                    clipped++; continue;
                }
                unsigned index = y * 466 + x;
                if (occupied[index]) text_overlap++;
                occupied[index] = i + 1;
                bool opaque_disc = false;
                for (unsigned j = 0; j < count; ++j) {
                    bool disc = !strcmp(bounds[j].role, "complication") || !strcmp(bounds[j].role, "swatch");
                    if (!disc) continue;
                    const lv_area_t *a = &bounds[j].bounds;
                    double cx = (a->x1 + a->x2 + 1) / 2., cy = (a->y1 + a->y2 + 1) / 2.;
                    double r = lv_area_get_width(a) / 2.;
                    bool own = descendant(object, bounds[j].object);
                    double d = hypot(x - cx, y - cy);
                    if (own && d < r && lv_obj_get_style_bg_opa(bounds[j].object, 0) == LV_OPA_COVER)
                        opaque_disc = true;
                    if ((own && d > r - 1) || (!own && d < r + .5)) disc_overlap++;
                }
                for (unsigned j = 0; j < arc_count; ++j)
                    if (arc_contains(&arcs[j], x, y)) ring_overlap++;
                /* An opaque text chip (Meridian's offline cue) hides hands too. */
                if (lv_obj_get_style_bg_opa(object, 0) == LV_OPA_COVER) opaque_disc = true;
                if (!opaque_disc) for (unsigned j = 0; j < stroke_count; ++j)
                    if (line_contains(&strokes[j], x, y)) line_overlap++;
            }
        }
        if (clipped || text_overlap || disc_overlap || ring_overlap || line_overlap)
            fprintf(stderr, "glyph mask %s \"%s\" lit=%u clipped=%u text=%u disc=%u ring=%u line=%u\n",
                bounds[i].role, lv_obj_check_type(object, &lv_label_class) ? lv_label_get_text(object) : "",
                lit, clipped, text_overlap, disc_overlap, ring_overlap, line_overlap);
        bool expected_ink = !lv_obj_check_type(object, &lv_label_class) || lv_label_get_text(object)[0];
        FIT(!expected_ink || lit > 0); FIT(!clipped); FIT(!text_overlap); FIT(!disc_overlap); FIT(!ring_overlap);
        FIT(!line_overlap);
        lv_draw_buf_destroy(mask);
    }
}
static void check_kit(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return;
    orb_kit_rim_text_info_t info;
    if (orb_kit_rim_text_info(root, &info)) {
        FIT(info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + .01f);
        /* Verify the actual retained arc label baseline, not a caller token. */
        for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
            lv_obj_t *slice = lv_obj_get_child(root, i);
            for (unsigned j = 0; j < lv_obj_get_child_count(slice); ++j) {
                lv_obj_t *arc = lv_obj_get_child(slice, j);
                if (!lv_obj_check_type(arc, &lv_arclabel_class)) continue;
                int radius = lv_arclabel_get_radius(arc);
                FIT(radius == ORB_KIT_TITLE_RADIUS || radius == ORB_KIT_TITLE_INSET_RADIUS ||
                    radius == ORB_KIT_CAPTION_RADIUS);
            }
        }
    }
    if (lv_obj_check_type(root, &lv_label_class)) {
        FIT(strchr(lv_label_get_text(root), '?') == NULL);
        /* The regular weather caption remains essential in Lock. Decorative
         * miniature artwork is exempt; native caption ink must keep its HIG
         * contrast against the actual black face, including AOD dimming. */
        bool native = true;
        for (lv_obj_t *p = root; p; p = lv_obj_get_parent(p))
            if (lv_obj_get_style_transform_scale_x(p, 0) != 256) native = false;
        if (native && lv_label_get_text(root)[0] &&
            lv_obj_get_style_text_font(root, 0) == orb_font_variant(ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR)) {
            lv_color_t c = lv_obj_get_style_text_color(root, 0);
            double alpha = lv_obj_get_style_text_opa(root, 0) / 255.;
            double channels[] = {c.red * alpha / 255., c.green * alpha / 255., c.blue * alpha / 255.};
            for (unsigned i = 0; i < 3; ++i)
                channels[i] = channels[i] <= .04045 ? channels[i] / 12.92 : pow((channels[i] + .055) / 1.055, 2.4);
            double contrast = (.2126 * channels[0] + .7152 * channels[1] + .0722 * channels[2] + .05) / .05;
            FIT(contrast >= (orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT && !orb_fonts_bold_text() ? 4.5 : 3.));
        }
    }
    if (lv_obj_get_style_outline_width(root, 0) == ORB_KIT_FOCUS_WIDTH) {
        FIT(lv_obj_get_style_outline_pad(root, 0) == ORB_KIT_FOCUS_OFFSET);
        FIT(lv_obj_get_style_transform_scale_x(root, 0) == 256);
        FIT(lv_obj_get_style_transform_scale_y(root, 0) == 256);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        check_kit(lv_obj_get_child(root, i));
}
static void meridian_masks(orb_faces_t *faces)
{
    orb_faces_prefs_t prefs; orb_faces_preferences(faces, &prefs);
    if (prefs.face != ORB_FACE_MERIDIAN || orb_faces_mode(faces) != ORB_FACES_FACE) return;
    /* A hand may geometrically cross a subdial, but it must be painted before
     * an opaque subdial. Combined with the circular text containment test this
     * proves no hand remains visible under text, at any analog hand angle. */
    lv_obj_t *root = orb_faces_root(faces);
    lv_obj_t *content = lv_obj_get_child(root, 0); CHECK(content);
    int last_line = -1, first_disc = 1000;
    unsigned line_count = 0, disc_count = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(content); ++i) {
        lv_obj_t *child = lv_obj_get_child(content, i);
        if (lv_obj_check_type(child, &lv_line_class)) { last_line = i; line_count++; }
        if (lv_obj_get_width(child) == 80 && lv_obj_get_height(child) == 80 &&
            lv_obj_get_style_radius(child, 0) >= 40) {
            disc_count++; first_disc = LV_MIN(first_disc, (int)i);
            FIT(lv_obj_get_style_bg_opa(child, 0) == LV_OPA_COVER);
        }
    }
    bool locked = !strncmp(s_scene, "Lock", 4);
    bool loading = !strcmp(s_state, "loading");
    FIT(line_count == (loading ? 60u : locked ? 62u : 63u) && disc_count == 4);
    FIT(last_line < first_disc);
}
static orb_faces_snapshot_t snapshot(const char *state)
{
    orb_faces_snapshot_t data = {.now = 1793208480, .clock = "17:28", .weekday = "Wednesday", .day = "28",
        .usage_percent = {62, 38}, .battery_percent = 100, .home_on = 1, .home_total = 8,
        .working = 1, .online = true, .next_seconds = 480, .weather_override = true,
        .weather = {.configured = true, .available = true, .unit = ORB_TEMPERATURE_F,
            .temperature = 72, .condition = ORB_WEATHER_CLEAR, .is_day = true,
            .place = "Brooklyn", .label = "Sunny"}};
    if (!strcmp(state, "long")) {
        strcpy(data.clock, "23:59");
        strcpy(data.weekday, "Wednesday");
        data.working = 999; data.home_on = 128; data.home_total = 256;
        data.weather.temperature = -127;
        data.weather.condition = ORB_WEATHER_THUNDER;
        strcpy(data.weather.label, "Thunderstorms and freezing rain");
        strcpy(data.weather.place, "Rancho Santa Margarita");
    } else if (!strcmp(state, "empty")) {
        data.usage_percent[0] = data.usage_percent[1] = -1;
        data.battery_percent = -1; data.home_on = data.home_total = data.working = 0;
        data.next_seconds = 0;
        data.weather.configured = data.weather.available = false;
        data.weather.label[0] = '\0';
    } else if (!strcmp(state, "offline")) {
        data.online = false; data.weather.stale = true; data.weather.temperature = -127;
    } else if (!strcmp(state, "stale")) {
        data.weather.stale = true;
    } else if (!strcmp(state, "low-battery")) {
        data.battery_percent = 5;
    } else if (!strcmp(state, "loading")) {
        data.now = 0;
        data.clock[0] = data.weekday[0] = data.day[0] = '\0';
        data.usage_percent[0] = data.usage_percent[1] = -1;
        data.home_on = data.home_total = data.working = data.next_seconds = 0;
        data.weather.available = false;
        data.weather.label[0] = '\0';
    } else if (!strcmp(state, "error")) {
        data.online = false; data.weather.available = false;
        data.weather.condition = ORB_WEATHER_UNKNOWN;
        data.usage_percent[0] = data.usage_percent[1] = -1;
        strcpy(data.weather.label, "Weather unavailable");
    }
    return data;
}
static void check_layout(orb_faces_t *faces)
{
    lv_obj_t *root = orb_faces_root(faces);
    lv_obj_update_layout(root);
    FIT(primary_count(root) <= 1);
    FIT(orb_faces_primary_count(faces) == primary_count(root));
    check_kit(root);
    meridian_masks(faces);
    orb_faces_bounds_t bounds[80];
    size_t count = orb_faces_layout(faces, bounds, 80);
    CHECK(count <= 80);
    check_shapes(faces, bounds, count);
    for (unsigned i = 0; i < count; ++i) {
        const lv_area_t *a = &bounds[i].bounds;
        FIT(a->x2 >= a->x1 && a->y2 >= a->y1);
    }
}
static void capture(orb_face_kind_t face, orb_faces_mode_t mode, bool locked)
{
    lv_obj_t *root = screen();
    orb_glass_build_stats_t glass_before, glass_after;
    orb_glass_build_stats(&glass_before);
    size_t before = retained();
    lv_obj_raster_stats_t raster_before; lv_obj_raster_get_stats(&raster_before);
    orb_memory_stats_t owners_before[ORB_MEMORY_COUNT];
    for (unsigned i = 0; i < ORB_MEMORY_COUNT; ++i) orb_memory_get(i, &owners_before[i]);
    orb_faces_t *faces = orb_faces_create(root, &(orb_faces_config_t){
        .open = opened, .preview = true, .always_on = locked,
        .persist = mode == ORB_FACES_COLOUR && !strcmp(s_state, "error") ? persist_failed : NULL});
    CHECK(faces);
    orb_faces_snapshot_t data = snapshot(s_state);
    /* The Meridian reference shows 10:09; other face frames show 17:28. */
    if (face == ORB_FACE_MERIDIAN && strcmp(s_state, "loading")) data.now = 1793182140;
    orb_faces_update(faces, &data);
    orb_faces_select(faces, face);
    orb_faces_set_mode(faces, mode);
    if (mode == ORB_FACES_COLOUR && !strcmp(s_state, "error")) {
        orb_faces_turn(faces, 1);
        CHECK(orb_faces_back(faces));
        CHECK(orb_faces_mode(faces) == ORB_FACES_COLOUR && orb_faces_save_error(faces));
    }
    advance(900);
    check_layout(faces);
    if (face == ORB_FACE_MERIDIAN && mode == ORB_FACES_FACE && !strcmp(s_state, "long"))
        CHECK(visible_text(orb_faces_root(faces), "-127°"));
    if (face == ORB_FACE_ORB && mode == ORB_FACES_FACE && !strcmp(s_state, "long")) {
        orb_faces_bounds_t bounds[80];
        size_t count = orb_faces_layout(faces, bounds, 80);
        bool found = false;
        for (unsigned i = 0; i < count; ++i) {
            lv_obj_t *label = bounds[i].object;
            if (strcmp(bounds[i].role, "weather-text") || !lv_obj_check_type(label, &lv_label_class) ||
                !lv_color_eq(lv_obj_get_style_text_color(label, 0), ORB_KIT_SECONDARY)) continue;
            const char *value = lv_label_get_text(label);
            /* Polish #13: at AX3 a condition is whole or absent. */
            if (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3) {
                CHECK(!strstr(value, "…") && !strstr(value, "..."));
                continue;
            }
            found = true;
            lv_point_t measured;
            lv_text_get_size(&measured, value, lv_obj_get_style_text_font(label, 0),
                lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            /* Long weather descriptions preserve their identifying prefix
             * and fit their lane instead of showing a cropped middle. */
            CHECK(!strncmp(value, "Th", 2));
            CHECK(measured.x <= lv_obj_get_width(label));
        }
        CHECK(found || orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3);
    }
    size_t after = retained();
    lv_obj_raster_stats_t raster_after; lv_obj_raster_get_stats(&raster_after);
    orb_glass_build_stats(&glass_after);
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s-%s.png", s_output, s_scene, s_state) < sizeof(path));
    CHECK(sim_display_save_png(path));
    fprintf(s_metrics, "%s,%s,%s,%lld,%lld,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%zu", s_scene, s_state, s_mode,
        (long long)after - (long long)before,
        (long long)glass_after.bytes - (long long)glass_before.bytes, orb_faces_state_size(),
        raster_after.source_bytes, raster_after.source_budget, raster_after.page_bytes,
        raster_after.page_budget, raster_after.mip_bytes, raster_after.mip_budget,
        raster_after.rejections - raster_before.rejections, raster_after.negative_hits - raster_before.negative_hits,
        glass_after.bytes);
    FIT(raster_after.source_bytes <= raster_after.source_budget);
    FIT(raster_after.page_bytes <= raster_after.page_budget);
    FIT(raster_after.mip_bytes <= raster_after.mip_budget);
    FIT(raster_after.rejections == raster_before.rejections);
    for (unsigned i = 0; i < ORB_MEMORY_COUNT; ++i) {
        orb_memory_stats_t owner; orb_memory_get(i, &owner);
        fprintf(s_metrics, ",%zu,%zu,%lld,%zu,%u", owners_before[i].used, owner.used,
            (long long)owner.used - (long long)owners_before[i].used, owner.budget, owner.rejections);
        FIT(owner.rejections == owners_before[i].rejections);
    }
    fputc('\n', s_metrics);
    orb_faces_destroy(faces);
}
static lv_obj_t *clickable_disc(lv_obj_t *root, int diameter)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_is_clickable(root) && lv_obj_get_width(root) == diameter &&
        lv_obj_get_height(root) == diameter) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = clickable_disc(lv_obj_get_child(root, i), diameter);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *disc_at(lv_obj_t *root, int cx, int cy)
{
    if (!lv_obj_is_visible(root)) return NULL;
    lv_area_t a; lv_obj_get_coords(root, &a);
    if (lv_obj_is_clickable(root) && lv_obj_get_width(root) == 80 && lv_obj_get_height(root) == 80 &&
        abs(a.x1 + 40 - cx) <= 1 && abs(a.y1 + 40 - cy) <= 1) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = disc_at(lv_obj_get_child(root, i), cx, cy);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *button_named(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *semantic = orb_kit_button_get_text(root);
    if (semantic && !strcmp(semantic, caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = button_named(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}
static void pointer_interaction(void)
{
    s_scene = "PointerInteraction"; s_state = "default";
    lv_obj_t *root = screen();
    orb_faces_t *faces = orb_faces_create(root, &(orb_faces_config_t){.preview = true, .open = opened});
    CHECK(faces);
    orb_faces_snapshot_t data = snapshot("default");
    orb_faces_update(faces, &data);
    orb_faces_prefs_t initial;
    orb_faces_preferences(faces, &initial);
    initial.face = ORB_FACE_ORB; initial.colour = 0;
    orb_faces_set_preferences(faces, &initial);
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    lv_indev_set_mode(s_pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_long_press_time(s_pointer, 400);
    advance(300);
    unsigned opens = s_opens;
    tap_object(clickable_disc(orb_faces_root(faces), 56));
    CHECK(s_opens == opens + 1 && s_target == ORB_FACES_OPEN_APPS);
    /* A drag that comes back over its initial target must stay cancelled. */
    pointer(233, 408, true);
    pointer(253, 408, true);
    pointer(233, 408, true);
    pointer(233, 408, false);
    advance(300);
    CHECK(s_opens == opens + 1);
    /* A moved contact cannot turn into a hold after it stops moving. LVGL
     * still emits LONG_PRESSED on a non-scrollable parent, so Faces must
     * remember movement instead of relying on the widget's click guard. */
    pointer(233, 220, true);
    pointer(253, 220, true);
    pointer(233, 220, true);
    advance(480);
    CHECK(orb_faces_mode(faces) == ORB_FACES_FACE);
    pointer(233, 220, false);
    /* Holding the actual blank Face enters Gallery; releasing that same
     * contact must not select its newly-created artwork. */
    pointer(233, 220, true);
    advance(480);
    CHECK(orb_faces_mode(faces) == ORB_FACES_GALLERY);
    pointer(233, 220, false);
    advance(300);
    CHECK(orb_faces_mode(faces) == ORB_FACES_GALLERY);
    pointer(233, 197, true);
    for (int x = 213; x >= 113; x -= 20) pointer(x, 197, true);
    pointer(113, 197, false);
    advance(300);
    orb_faces_prefs_t selected;
    orb_faces_preferences(faces, &selected);
    CHECK(selected.face == ORB_FACE_MERIDIAN && orb_faces_mode(faces) == ORB_FACES_GALLERY);
    tap_object(button_named(orb_faces_root(faces), "Edit"));
    CHECK(orb_faces_mode(faces) == ORB_FACES_COLOUR);
    /* An actual press on Orange selects exactly that swatch. */
    pointer(403, 199, true);
    pointer(403, 199, false);
    advance(300);
    orb_faces_preferences(faces, &selected);
    CHECK(selected.colour == 1);
    tap_object(button_named(orb_faces_root(faces), "Slots"));
    CHECK(orb_faces_mode(faces) == ORB_FACES_SLOTS);
    tap_object(disc_at(orb_faces_root(faces), 293, 365));
    int value, maximum;
    CHECK(orb_faces_rotary(faces, &value, &maximum));
    CHECK(value == selected.meridian_slot[3] && maximum == ORB_FACE_PROVIDER_COUNT - 1);
    orb_faces_turn(faces, -1);
    orb_faces_preferences(faces, &selected);
    CHECK(selected.meridian_slot[3] == value - 1);
    CHECK(orb_faces_back(faces) && orb_faces_mode(faces) == ORB_FACES_COLOUR);
    CHECK(orb_faces_back(faces) && orb_faces_mode(faces) == ORB_FACES_GALLERY);
    /* Tapping selected artwork explicitly commits the previewed face. */
    pointer(233, 197, true);
    pointer(233, 197, false);
    advance(300);
    CHECK(orb_faces_mode(faces) == ORB_FACES_FACE);
    orb_faces_preferences(faces, &selected);
    CHECK(selected.face == ORB_FACE_MERIDIAN);
    opens = s_opens;
    orb_faces_set_always_on(faces, true);
    pointer(233, 220, true); advance(480); pointer(233, 220, false);
    CHECK(orb_faces_mode(faces) == ORB_FACES_FACE && s_opens == opens);
    CHECK(!orb_faces_rotary(faces, &value, &maximum));
    lv_indev_delete(s_pointer); s_pointer = NULL;
    orb_faces_destroy(faces);
}
static lv_obj_t *captioned(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root);
    if (rim && !strcasecmp(rim, caption)) return root;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = captioned(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}
static void shell_button(orb_button_t button, orb_button_gesture_t gesture)
{
    if (gesture == ORB_BUTTON_SHORT) {
        orb_shell_button(s_shell, &(orb_button_event_t){.button = button, .gesture = ORB_BUTTON_DOWN, .at_ms = s_now});
        advance(64);
        orb_shell_button(s_shell, &(orb_button_event_t){.button = button, .gesture = ORB_BUTTON_UP, .at_ms = s_now});
        advance(320);
    }
    orb_shell_button(s_shell, &(orb_button_event_t){.button = button, .gesture = gesture, .at_ms = s_now});
    advance(1000);
}
static void shell_drag(int x, int y, int dx, int dy)
{
    pointer(x, y, true);
    for (int step = 1; step <= 10; ++step) pointer(x + dx * step / 10, y + dy * step / 10, true);
    pointer(x + dx, y + dy, false);
    advance(1200);
}
static void shell_face(void)
{
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
}
static void shell_rim(void)
{
    pointer(449, 233, true);
    for (unsigned i = 1; i <= 12; ++i) {
        double angle = i * M_PI / 72.;
        pointer(233 + (int)lround(216 * cos(angle)), 233 + (int)lround(216 * sin(angle)), true);
    }
    pointer(s_input.point.x, s_input.point.y, false); advance(500);
}
static void shell_shot(const char *name)
{
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s.png", s_output, name) < sizeof(path));
    CHECK(sim_display_save_png(path));
}
static void shell_interaction(bool open_requests)
{
    s_scene = "ShellInteraction"; s_state = "default";
    lv_obj_t *root = screen();
    CHECK(orb_home_init());
    orb_model_load_demo(); orb_home_load_demo(); orb_model_set_connection(true, true);
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    lv_indev_set_mode(s_pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_long_press_time(s_pointer, 400);
    s_shell = orb_shell_create(root, NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70, .usb = true, .clock_time = "17:28",
        .clock_date = "Wednesday, October 28", .firmware_version = "0.5.2"};
    orb_shell_update(s_shell, &status); advance(1200); shell_face();
    /* The Face alone retains four centre-navigation exceptions. Those same
     * coordinates remain local content after entering its destinations. */
    static const struct { int dx, dy; orb_shell_surface_t target; } routes[] = {
        {-160, 0, ORB_SHELL_HOME}, {160, 0, ORB_SHELL_AGENTS},
        {0, -160, ORB_SHELL_STACK}, {0, 160, ORB_SHELL_NOTIFICATIONS},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        shell_face(); shell_drag(233, 220, routes[i].dx, routes[i].dy);
        CHECK(orb_shell_surface(s_shell) == routes[i].target);
        shell_drag(233, 220, 0, 100);
        CHECK(orb_shell_surface(s_shell) == routes[i].target);
        shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    }
    shell_face();
    CHECK(!orb_shell_twist(s_shell, 2)); shell_rim();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!captioned(lv_display_get_layer_top(s_display), "Volume"));
    for (unsigned i = 0; i < 16; ++i) {
        orb_shell_environment(s_shell, .3f, -.2f, .93f, false); advance(16);
    }
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    shell_button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    orb_shell_power_enable(s_shell, true);
    shell_button(ORB_BUTTON_PWR, ORB_BUTTON_SHORT);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    shell_button(ORB_BUTTON_PWR, ORB_BUTTON_SHORT);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    /* The wake contact has a complete navigation gesture, but its release
     * must remain consumed. Only a fresh gesture may leave the Face. */
    shell_drag(233, 220, -160, 0);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    shell_drag(233, 220, -160, 0);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    /* Out-of-scope shell restoration request is opt-in so the complete
     * passing suite still records all Faces-owned regressions separately. */
    if (open_requests) {
        pointer(233, 220, true); advance(480); pointer(233, 220, false); advance(500);
        tap_object(button_named(orb_shell_root(s_shell), "Edit"));
        CHECK(captioned(orb_shell_root(s_shell), "Color"));
        shell_shot("qa-editor-before-control");
        shell_button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
        shell_shot("qa-editor-control");
        shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
        shell_shot("qa-editor-after-control-back");
        FIT(captioned(orb_shell_root(s_shell), "Color"));
        if (!captioned(orb_shell_root(s_shell), "Color")) {
            shell_face();
            pointer(233, 220, true); advance(480); pointer(233, 220, false); advance(500);
            tap_object(button_named(orb_shell_root(s_shell), "Edit"));
            CHECK(captioned(orb_shell_root(s_shell), "Color"));
        }
        shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
        shell_shot("qa-editor-after-boot");
        FIT(button_named(orb_shell_root(s_shell), "Edit"));
    }
    orb_shell_destroy(s_shell); s_shell = NULL;
    lv_indev_delete(s_pointer); s_pointer = NULL;
}
static void interaction(void)
{
    s_scene = "Interaction"; s_state = "default";
    lv_obj_t *root = screen();
    orb_faces_t *faces = orb_faces_create(root, &(orb_faces_config_t){.preview = true, .open = opened});
    CHECK(faces);
    orb_faces_snapshot_t data = snapshot("default");
    orb_faces_update(faces, &data);
    advance(1000);
    int value, maximum;
    CHECK(!orb_faces_rotary(faces, &value, &maximum));
    lv_obj_t *apps = clickable_disc(orb_faces_root(faces), 56); CHECK(apps);
    unsigned opens = s_opens;
    lv_obj_send_event(apps, LV_EVENT_PRESSED, NULL);
    orb_kit_press_cancel(apps);
    lv_obj_send_event(apps, LV_EVENT_CLICKED, NULL);
    CHECK(s_opens == opens);
    lv_obj_send_event(apps, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(apps, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(apps, LV_EVENT_CLICKED, NULL);
    CHECK(s_opens == opens + 1 && s_target == ORB_FACES_OPEN_APPS);
    /* All providers share their displayed truth and destination across short,
     * glyph and ring forms. Tapping each live slot opens that provider's app. */
    for (unsigned provider = 0; provider < ORB_FACE_PROVIDER_COUNT; ++provider) {
        orb_faces_prefs_t prefs; orb_faces_preferences(faces, &prefs);
        prefs.slot[0] = provider;
        orb_faces_set_preferences(faces, &prefs);
        advance(200);
        check_layout(faces);
        orb_face_complication_t content;
        CHECK(orb_faces_complication(faces, provider, &content));
        CHECK(content.name && content.name[0] && content.text[0] && !strchr(content.text, '?'));
        lv_obj_t *disc = disc_at(orb_faces_root(faces), 128, 316); CHECK(disc);
        opens = s_opens;
        lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
        orb_kit_press_cancel(disc);
        lv_obj_send_event(disc, LV_EVENT_CLICKED, NULL);
        CHECK(s_opens == opens);
        lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(disc, LV_EVENT_RELEASED, NULL);
        lv_obj_send_event(disc, LV_EVENT_CLICKED, NULL);
        CHECK(s_opens == opens + 1 && s_target == content.target);
    }
    for (unsigned locked = 0; locked < 2; ++locked) {
        orb_faces_set_always_on(faces, locked);
        for (int pct = 20; pct >= 5; pct -= 15) {
            data.battery_percent = pct;
            orb_faces_update(faces, &data); advance(300); check_layout(faces);
            /* Polish #9: Always On keeps the face's own battery glyph. */
            CHECK(visible_text(orb_faces_root(faces), ORB_ICON_BATTERY_LOW));
        }
    }
    orb_faces_set_always_on(faces, false);
    data.battery_percent = 100; orb_faces_update(faces, &data); advance(300);
    orb_faces_hold(faces);
    CHECK(orb_faces_mode(faces) == ORB_FACES_GALLERY);
    CHECK(orb_faces_rotary(faces, &value, &maximum));
    CHECK(maximum == ORB_FACE_COUNT - 1);
    /* Polish #38: the gallery is a finite row; turning past an end stays. */
    orb_faces_turn(faces, -1);
    orb_faces_prefs_t wrapped; orb_faces_preferences(faces, &wrapped);
    CHECK(wrapped.face == ORB_FACE_ORB);
    orb_faces_turn(faces, 1);
    orb_faces_prefs_t selected; orb_faces_preferences(faces, &selected);
    CHECK(selected.face == ORB_FACE_MERIDIAN);
    CHECK(orb_faces_horizontal_drag(faces, -120));
    orb_faces_preferences(faces, &wrapped); CHECK(wrapped.face == ORB_FACE_ACTIVITY);
    CHECK(orb_faces_horizontal_drag(faces, -120));
    orb_faces_preferences(faces, &wrapped); CHECK(wrapped.face == ORB_FACE_ACTIVITY);
    orb_faces_set_mode(faces, ORB_FACES_COLOUR);
    CHECK(orb_faces_rotary(faces, &value, &maximum));
    orb_faces_turn(faces, 1);
    orb_faces_preferences(faces, &selected);
    CHECK(selected.colour != 0);
    orb_faces_set_mode(faces, ORB_FACES_SLOTS);
    orb_faces_select_slot(faces, 2);
    orb_faces_turn(faces, 1);
    orb_faces_preferences(faces, &selected);
    orb_faces_set_mode(faces, ORB_FACES_FACE);
    orb_faces_set_preferences(faces, &selected);
    orb_faces_prefs_t restored; orb_faces_preferences(faces, &restored);
    CHECK(!memcmp(&selected, &restored, sizeof(selected)));
    CHECK(orb_faces_save(faces));
    orb_faces_set_always_on(faces, true);
    CHECK(!orb_faces_rotary(faces, &value, &maximum));
    size_t allocations = sim_memory_allocation_calls();
    for (unsigned i = 0; i < 100; ++i) orb_faces_update(faces, &data);
    CHECK(sim_memory_allocation_calls() == allocations);
    orb_faces_destroy(faces);
}
static void inset_composition(void)
{
    s_scene = "InsetCalendar"; s_state = "default";
    lv_obj_t *root = screen();
    lv_obj_t *ring = orb_kit_ring_create(root, 213, 14, ORB_KIT_CLAUDE); CHECK(ring);
    orb_kit_ring_set_value(ring, 100, false);
    lv_obj_t *title = orb_kit_rim_date_create(root, "Wednesday", "28"); CHECK(title);
    CHECK(orb_kit_rim_title_set_inset(title, true));
    advance(200);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *arc = lv_obj_get_child(lv_obj_get_child(title, i), 0);
        CHECK(lv_arclabel_get_radius(arc) == 176);
    }
    lv_draw_buf_t *mask = lv_snapshot_take(title, LV_COLOR_FORMAT_ARGB8888); CHECK(mask);
    lv_area_t area; lv_obj_get_coords(title, &area);
    int ext = lv_obj_get_ext_draw_size(title);
    unsigned ink = 0;
    for (unsigned y = 0; y < mask->header.h; ++y) {
        const lv_color32_t *row = (const lv_color32_t *)(mask->data + y * mask->header.stride);
        for (unsigned x = 0; x < mask->header.w; ++x) {
            if (row[x].alpha < 16) continue;
            ink++;
            FIT(hypot((int)x + area.x1 - ext - 233, (int)y + area.y1 - ext - 233) < 206);
        }
    }
    CHECK(ink > 0);
    lv_draw_buf_destroy(mask);
}
static void seconds_policy(void)
{
    s_scene = "MeridianSeconds"; s_state = "default";
    lv_obj_t *root = screen();
    orb_faces_t *faces = orb_faces_create(root, &(orb_faces_config_t){.preview = true}); CHECK(faces);
    orb_faces_snapshot_t data = snapshot("default");
    orb_faces_update(faces, &data); orb_faces_select(faces, ORB_FACE_MERIDIAN);
    advance(500);
    lv_obj_t *content = lv_obj_get_child(orb_faces_root(faces), 0);
    lv_obj_t *second = lv_obj_get_child(content, 62); CHECK(lv_obj_check_type(second, &lv_line_class));
    lv_timer_t *refresh = lv_display_get_refr_timer(s_display); lv_timer_pause(refresh);
    orb_faces_set_active(faces, true);
    /* Warm LVGL's first invalidation bookkeeping; only the retained steady
     * hand update is the component's draw-path allocation contract. */
    s_now += 64; lv_timer_handler(); lv_refr_now(s_display);
    /* Invalidation normally resumes the refresh timer. Disable it while
     * measuring the hand computation, excluding documented LVGL draw-task
     * allocations instead of accidentally counting a complete repaint. */
    lv_display_enable_invalidation(s_display, false); lv_timer_pause(refresh);
    lv_point_precise_t before = lv_line_get_points(second)[1];
    size_t allocations = sim_memory_allocation_calls();
    s_now += 64; lv_timer_handler();
    lv_point_precise_t after = lv_line_get_points(second)[1];
    CHECK(before.x != after.x || before.y != after.y);
    if (sim_memory_allocation_calls() != allocations)
        fprintf(stderr, "seconds steady update allocator delta=%zu\n", sim_memory_allocation_calls() - allocations);
    CHECK(sim_memory_allocation_calls() == allocations);
    orb_faces_set_active(faces, false);
    before = lv_line_get_points(second)[1]; s_now += 128; lv_timer_handler();
    after = lv_line_get_points(second)[1]; CHECK(before.x == after.x && before.y == after.y);
    orb_faces_set_always_on(faces, true);
    content = lv_obj_get_child(orb_faces_root(faces), 0);
    shape_t hands[256];
    CHECK(lines(content, hands, 0) == 62); /* sixty ticks, no seconds hand */
    lv_obj_t *minute = lv_obj_get_child(content, 61);
    CHECK(lv_obj_check_type(minute, &lv_line_class));
    before = lv_line_get_points(minute)[1];
    orb_faces_set_active(faces, true); s_now += 128; lv_timer_handler();
    after = lv_line_get_points(minute)[1]; CHECK(before.x == after.x && before.y == after.y);
    data.now++; orb_faces_update(faces, &data);
    after = lv_line_get_points(minute)[1]; CHECK(before.x == after.x && before.y == after.y);
    orb_faces_destroy(faces); lv_display_enable_invalidation(s_display, true); lv_timer_resume(refresh);
}
static size_t node_ids(lv_obj_t *object, uintptr_t *ids, size_t count)
{
    CHECK(count < 2048);
    ids[count++] = (uintptr_t)object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        count = node_ids(lv_obj_get_child(object, i), ids, count);
    return count;
}
static int compare_ids(const void *a, const void *b)
{
    uintptr_t aa = *(const uintptr_t *)a, bb = *(const uintptr_t *)b;
    return aa < bb ? -1 : aa > bb;
}
static void retained_crown(void)
{
    s_scene = "RetainedCrown"; s_state = "long";
    static uintptr_t before[2048], after[2048];
    for (unsigned kind = 0; kind < ORB_FACE_COUNT; ++kind) {
        for (unsigned mode = ORB_FACES_GALLERY; mode <= ORB_FACES_SLOTS; ++mode) {
            lv_obj_t *root = screen();
            orb_faces_t *faces = orb_faces_create(root, &(orb_faces_config_t){.preview = true}); CHECK(faces);
            orb_faces_snapshot_t data = snapshot("long");
            orb_faces_update(faces, &data); orb_faces_select(faces, kind); orb_faces_set_mode(faces, mode);
            advance(900);
            int maximum; CHECK(orb_faces_rotary(faces, NULL, &maximum));
            /* Warm every value while idle so first style registration is not
             * mistaken for steady crown work. Mode admission owns creation. */
            for (int value = 0; value <= maximum; ++value) orb_faces_rotate(faces, value);
            size_t count = node_ids(orb_faces_root(faces), before, 0);
            qsort(before, count, sizeof(before[0]), compare_ids);
            orb_memory_stats_t state_before, state_after;
            orb_memory_get(ORB_MEMORY_WORKSPACE, &state_before);
            size_t allocations = sim_memory_allocation_calls();
            for (int value = 0; value <= maximum; ++value) {
                orb_faces_rotate(faces, value);
                size_t current = node_ids(orb_faces_root(faces), after, 0);
                qsort(after, current, sizeof(after[0]), compare_ids);
                CHECK(current == count && !memcmp(before, after, count * sizeof(before[0])));
            }
            orb_memory_get(ORB_MEMORY_WORKSPACE, &state_after);
            CHECK(state_after.used == state_before.used);
            if (mode == ORB_FACES_COLOUR) CHECK(sim_memory_allocation_calls() == allocations);
            printf("CROWN %s face=%u mode=%u nodes=%zu LVGL-allocator-calls=%zu (rim text caches documented)\n",
                s_mode, kind, mode, count, sim_memory_allocation_calls() - allocations);
            /* Check every provider's actual long form in its editor slot,
             * including Calendar, negative weather, next event and unknowns. */
            if (mode == ORB_FACES_SLOTS) for (int value = 0; value <= maximum; ++value) {
                orb_faces_rotate(faces, value); advance(300); check_layout(faces);
                if (value == ORB_FACE_WEATHER) CHECK(visible_text(orb_faces_root(faces), "-127°"));
            }
            if (mode == ORB_FACES_GALLERY) {
                CHECK(orb_faces_back(faces));
                orb_faces_prefs_t cancelled; orb_faces_preferences(faces, &cancelled);
                CHECK(orb_faces_mode(faces) == ORB_FACES_FACE && cancelled.face == kind);
            }
            orb_faces_destroy(faces);
        }
    }
}
int main(int argc, char **argv)
{
    if (argc != 3 && !(argc == 4 && !strcmp(argv[3], "--qa-shell-open"))) return EXIT_FAILURE;
    s_output = argv[1]; s_mode = argv[2];
    s_scene = "Initialization"; s_state = "default";
    CHECK(mkdir(s_output, 0755) == 0 || errno == EEXIST);
    CHECK(setenv("TZ", "UTC", 1) == 0); tzset();
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init());
    bool ax = strcmp(s_mode, "default") != 0, bold = !strcmp(s_mode, "ax3-bold-rt");
    CHECK(orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(bold);
    orb_glass_cache_prewarm();
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/metrics.csv", s_output) < sizeof(path));
    s_metrics = fopen(path, "w"); CHECK(s_metrics);
    fprintf(s_metrics, "scene,state,mode,retained_lvgl_bytes,shared_glass_delta,state_bytes,"
        "renderer_sources,renderer_source_budget,renderer_pages,renderer_page_budget,renderer_mips,renderer_mip_budget,"
        "renderer_rejection_delta,renderer_negative_hit_delta,shared_glass_current");
    for (unsigned i = 0; i < ORB_MEMORY_COUNT; ++i)
        fprintf(s_metrics, ",%s_before,%s_after,%s_delta,%s_budget,%s_rejections",
            orb_memory_name(i), orb_memory_name(i), orb_memory_name(i), orb_memory_name(i), orb_memory_name(i));
    fputc('\n', s_metrics);
    static const struct { const char *name; orb_face_kind_t face; orb_faces_mode_t mode; bool locked; } scenes[] = {
        {"FaceOrb", ORB_FACE_ORB, ORB_FACES_FACE, false},
        {"FaceMeridian", ORB_FACE_MERIDIAN, ORB_FACES_FACE, false},
        {"FaceActivity", ORB_FACE_ACTIVITY, ORB_FACES_FACE, false},
        {"FaceGallery", ORB_FACE_ORB, ORB_FACES_GALLERY, false},
        {"FaceGalleryMeridian", ORB_FACE_MERIDIAN, ORB_FACES_GALLERY, false},
        {"FaceGalleryActivity", ORB_FACE_ACTIVITY, ORB_FACES_GALLERY, false},
        {"FaceColor", ORB_FACE_ORB, ORB_FACES_COLOUR, false},
        {"FaceColorMeridian", ORB_FACE_MERIDIAN, ORB_FACES_COLOUR, false},
        {"FaceColorActivity", ORB_FACE_ACTIVITY, ORB_FACES_COLOUR, false},
        {"FaceSlots", ORB_FACE_ORB, ORB_FACES_SLOTS, false},
        {"FaceSlotsMeridian", ORB_FACE_MERIDIAN, ORB_FACES_SLOTS, false},
        {"FaceSlotsActivity", ORB_FACE_ACTIVITY, ORB_FACES_SLOTS, false},
        {"Lock", ORB_FACE_ORB, ORB_FACES_FACE, true},
        {"LockMeridian", ORB_FACE_MERIDIAN, ORB_FACES_FACE, true},
        {"LockActivity", ORB_FACE_ACTIVITY, ORB_FACES_FACE, true},
    };
    static const char *states[] = {"default", "long", "empty", "offline", "error", "loading", "stale", "low-battery"};
    for (unsigned i = 0; i < sizeof(scenes) / sizeof(scenes[0]); ++i) {
        s_scene = scenes[i].name;
        for (unsigned j = 0; j < sizeof(states) / sizeof(states[0]); ++j) {
            s_state = states[j];
            capture(scenes[i].face, scenes[i].mode, scenes[i].locked);
        }
    }
    interaction();
    pointer_interaction();
    inset_composition();
    seconds_policy();
    retained_crown();
    shell_interaction(argc == 4);
    fclose(s_metrics);
    printf("Faces %s: %u checks, %u layout failures, %zu PNGs\n", s_mode, s_checks, s_failures,
        sizeof(scenes) / sizeof(scenes[0]) * sizeof(states) / sizeof(states[0]));
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
