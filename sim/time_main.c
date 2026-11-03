/* Deterministic Time previews and behavioural gates. All clocks and touches
 * are local fakes; this program opens no link and never accesses a board. */
#include "orb_time.h"
#include "orb_time_view.h"
#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_rim.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_kit_motion.h"
#include "sim_display.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
    uint32_t now, next_ui, wall_start_ms;
    time_t wall_start;
    lv_display_t *display;
    lv_indev_t *pointer;
    lv_indev_data_t input;
    orb_time_view_t *view;
    unsigned checks, frames, damage, flush_pixels, max_damage, max_frame;
    size_t view_base_bytes, view_base_blocks;
    bool measure, diagnostic;
    unsigned failures;
    const char *directory;
} time_harness_t;

static time_harness_t s_harness;
static unsigned s_qa_list_deletions;
static uint8_t s_partial[SIM_D * SIM_D * 3], s_full[sizeof(s_partial)];

static void check(bool value, const char *why)
{
    ++s_harness.checks;
    if (value) return;
    fprintf(stderr, "Time: %s (%u ms)\n", why, s_harness.now);
    /* Capture mode makes a complete diagnostic atlas even when a geometry
     * gate fails. CTest never passes this flag and retains fail-fast gates. */
    if (s_harness.diagnostic && !strstr(why, "create ") &&
        !strstr(why, "visible named action") && !strstr(why, "requested digit")) {
        ++s_harness.failures;
        return;
    }
    exit(EXIT_FAILURE);
}

static uint32_t tick(void) { return s_harness.now; }
static int64_t now_us(void) { return (int64_t)s_harness.now * 1000; }
static time_t wall_now(void)
{
    return s_harness.wall_start ? s_harness.wall_start +
        (time_t)((s_harness.now - s_harness.wall_start_ms) / 1000) : 0;
}

static void pointer_read(lv_indev_t *pointer, lv_indev_data_t *data)
{
    (void)pointer;
    *data = s_harness.input;
}

static void qa_list_deleted(lv_event_t *event)
{
    (void)event;
    ++s_qa_list_deletions;
}

static unsigned qa_transparent_labels(lv_obj_t *obj)
{
    unsigned count = lv_obj_check_type(obj, &lv_label_class) &&
        lv_obj_get_style_text_opa(obj, 0) == LV_OPA_TRANSP;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        count += qa_transparent_labels(lv_obj_get_child(obj, i));
    return count;
}

static void display_event(lv_event_t *event)
{
    if (!s_harness.measure) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_INVALIDATE_AREA: {
        const lv_area_t *area = lv_event_get_param(event);
        unsigned pixels = lv_area_get_size(area);
        if (pixels > s_harness.max_damage) s_harness.max_damage = pixels;
        ++s_harness.damage;
        break;
    }
    case LV_EVENT_RENDER_START: s_harness.flush_pixels = 0; break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *area = lv_event_get_param(event);
        s_harness.flush_pixels += lv_area_get_size(area);
        break;
    }
    case LV_EVENT_REFR_READY:
        if (s_harness.flush_pixels) {
            ++s_harness.frames;
            if (s_harness.flush_pixels > s_harness.max_frame)
                s_harness.max_frame = s_harness.flush_pixels;
        }
        s_harness.flush_pixels = 0;
        break;
    default: break;
    }
}

static void poll(bool force_ui)
{
    orb_time_poll(now_us(), wall_now());
    /* Firmware flushes on main's service loop, outside the LVGL lock. The
     * standalone simulator uses the allocation-free in-memory storage stub. */
    orb_time_persist_service();
    if (force_ui || s_harness.now >= s_harness.next_ui) {
        orb_time_view_poll(s_harness.view);
        s_harness.next_ui = s_harness.now + 250;
    }
    lv_timer_handler();
    lv_refr_now(s_harness.display);
}

static void advance(unsigned milliseconds)
{
    uint32_t end = s_harness.now + milliseconds;
    do {
        unsigned step = end - s_harness.now;
        if (step > 8) step = 8;
        s_harness.now += step;
        poll(false);
    } while (s_harness.now < end);
}

static void jump(unsigned milliseconds)
{
    s_harness.now += milliseconds;
    poll(true);
}

static void press(int x, int y)
{
    s_harness.input.point = (lv_point_t){x, y};
    s_harness.input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_harness.pointer);
    poll(false);
}

static void release(void)
{
    s_harness.input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_harness.pointer);
    poll(true);
}

static void tap(int x, int y)
{
    press(x, y);
    advance(32);
    release();
    advance(300);
}

static bool semantic_label(lv_obj_t *object)
{
    /* Glyph actions and curved titles retain a nonpainting label for semantic
     * discovery. Its visible parent, rather than its own hidden bit, is the
     * availability gate; hidden pages still cannot supply an action. */
    return lv_obj_check_type(object, &lv_label_class) &&
        (lv_obj_is_hidden(object) || !lv_obj_get_style_opa(object, 0)) &&
        lv_obj_get_style_text_opa(object, 0) &&
        lv_obj_get_parent(object) && lv_obj_is_visible(lv_obj_get_parent(object));
}

static bool fitted_text_matches(const char *shown, const char *semantic)
{
    if (!strcmp(shown, semantic)) return true;
    /* Kit lanes now fit before ellipsising. Find the same real option after
     * that visual fitting; production actions still use their model IDs. */
    const char *dots = strstr(shown, "...");
    if (!dots) dots = strstr(shown, "\xe2\x80\xa6");
    return dots && dots > shown && !strncmp(shown, semantic, (size_t)(dots - shown));
}

static lv_obj_t *find_text(lv_obj_t *object, const char *text)
{
    /* A transparent kit reservation is geometry, not a named action/value. */
    if (lv_obj_check_type(object, &lv_label_class) && !lv_obj_get_style_text_opa(object, 0)) return NULL;
    if (!lv_obj_is_visible(object) && !semantic_label(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && fitted_text_matches(lv_label_get_text(object), text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

/* A fixture may bring an offscreen retained row into view. Unlike the visible
 * action finder, this deliberately ignores ancestor viewport clipping. */
static lv_obj_t *find_retained_text(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) && !lv_obj_get_style_text_opa(object, 0)) return NULL;
    if (lv_obj_is_hidden(object) && !semantic_label(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && fitted_text_matches(lv_label_get_text(object), text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_retained_text(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_prefix(lv_obj_t *object, const char *prefix)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        !strncmp(lv_label_get_text(object), prefix, strlen(prefix))) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_prefix(lv_obj_get_child(object, (int32_t)i), prefix);
        if (found) return found;
    }
    return NULL;
}

static bool has_text(const char *text)
{
    return find_text(orb_time_view_root(s_harness.view), text) != NULL;
}

static lv_obj_t *live_text(lv_obj_t *object, const char *text)
{
    orb_kit_value_info_t info;
    if (lv_obj_check_type(object, &lv_label_class) &&
        !strcmp(lv_label_get_text(object), text) && orb_kit_value_info(object, &info)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = live_text(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static unsigned text_count(lv_obj_t *object, const char *text)
{
    unsigned count = lv_obj_check_type(object, &lv_label_class) &&
        !strcmp(lv_label_get_text(object), text);
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        count += text_count(lv_obj_get_child(object, (int32_t)i), text);
    return count;
}

/* The lifted Stopwatch hero's line box reaches y136 at AX3; its measured ink
 * remains below the viewport. Keep this lookup clear of the y104 pin. */
static unsigned counter_groups(lv_obj_t *object, lv_obj_t **groups, unsigned count)
{
    if (!lv_obj_is_visible(object)) return count;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *value = lv_label_get_text(object);
        lv_area_t area; lv_obj_get_coords(object, &area);
        if (*value >= '0' && *value <= '9' &&
            lv_obj_get_style_text_font(object, 0)->line_height >= 20 &&
            area.y1 >= 128 && area.y2 <= 290) {
            check(count < 3, "main counter has at most three numeric groups");
            groups[count++] = object;
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        count = counter_groups(lv_obj_get_child(object, (int32_t)i), groups, count);
    for (unsigned i = 1; i < count; ++i) {
        lv_obj_t *value = groups[i];
        unsigned j = i;
        while (j && lv_obj_get_x(lv_obj_get_parent(groups[j - 1])) >
            lv_obj_get_x(lv_obj_get_parent(value))) {
            groups[j] = groups[j - 1]; --j;
        }
        groups[j] = value;
    }
    return count;
}

static void counter_is(const char *expected)
{
    lv_obj_t *groups[3];
    unsigned count = counter_groups(orb_time_view_root(s_harness.view), groups, 0);
    char value[40] = "";
    for (unsigned i = 0; i < count; ++i) {
        if (i) strlcat(value, ":", sizeof(value));
        /* Alive projects arrival ink; format contracts and semantic traces
         * must continue to report the actual service reading. */
        const char *text = lv_label_get_text(groups[i]);
        (void)orb_kit_value_projection_semantic_text(groups[i], &text);
        strlcat(value, text, sizeof(value));
    }
    if (strcmp(value, expected)) fprintf(stderr, "Time counter expected %s, got %s\n", expected, value);
    check(!strcmp(value, expected), "main counter uses the specified duration/clock format");
    check(strchr(value, '.') == NULL, "main counter never shows hundredths");
}

static void tap_counter_group(unsigned index)
{
    lv_obj_t *groups[3];
    unsigned count = counter_groups(orb_time_view_root(s_harness.view), groups, 0);
    check(index < count, "requested digit group is visibly available");
    lv_area_t area;
    lv_obj_get_coords(lv_obj_get_parent(groups[index]), &area);
    tap((area.x1 + area.x2 + 1) / 2, (area.y1 + area.y2 + 1) / 2);
}

static lv_obj_t *find_named_action(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) && !lv_obj_get_style_text_opa(object, 0)) return NULL;
    if (!lv_obj_is_visible(object) && !semantic_label(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && fitted_text_matches(lv_label_get_text(object), text)) {
        lv_obj_t *action = lv_obj_is_clickable(object) ? object : lv_obj_get_parent(object);
        if (lv_obj_is_clickable(action) && lv_obj_get_height(action) < SIM_D) return action;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *action = find_named_action(lv_obj_get_child(object, (int32_t)i), text);
        if (action) return action;
    }
    return NULL;
}

static void describe_action(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) {
        for (lv_obj_t *node = object; node; node = lv_obj_get_parent(node)) {
            lv_area_t area; lv_obj_get_coords(node, &area);
            fprintf(stderr, "Time action ancestor (%d,%d)-(%d,%d) hidden=%d visible=%d clickable=%d scroll=%d\n",
                area.x1, area.y1, area.x2, area.y2, lv_obj_is_hidden(node), lv_obj_is_visible(node),
                lv_obj_is_clickable(node), lv_obj_get_scroll_y(node));
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        describe_action(lv_obj_get_child(object, (int32_t)i), text);
}

static lv_obj_t *find_action(const char *text)
{
    lv_obj_t *action = find_named_action(orb_time_view_root(s_harness.view), text);
    if (!action) {
        fprintf(stderr, "Time action missing: %s\n", text);
        describe_action(orb_time_view_root(s_harness.view), text);
    }
    check(action != NULL, "visible named action accepts real pointer input");
    return action;
}

static void tap_action(const char *text)
{
    lv_obj_t *action = find_action(text);
    lv_area_t area;
    lv_obj_get_coords(action, &area);
    tap((area.x1 + area.x2 + 1) / 2, (area.y1 + area.y2 + 1) / 2);
}

static void tap_retained_action(const char *text)
{
    lv_obj_t *label = find_retained_text(orb_time_view_root(s_harness.view), text);
    check(label != NULL, "retained option is present in its short list");
    lv_obj_t *target = label;
    while (lv_obj_get_parent(target) && !lv_obj_is_clickable(target)) target = lv_obj_get_parent(target);
    lv_obj_scroll_to_view_recursive(target, LV_ANIM_OFF);
    advance(300);
    tap_action(text);
}

static void return_stopwatch_summary(void)
{
    /* The curved title names the place; it is no longer a text-only button.
     * Use the visible lap list's rim owner to return to its summary. */
    orb_time_rotary_t owner;
    check(orb_time_view_rotary(s_harness.view, &owner),
        "overflowing stopwatch content exposes its local rim owner");
    orb_time_view_rotate(s_harness.view, owner.minimum);
    orb_time_view_rotary_finish(s_harness.view);
    advance(600);
}

static void frame_equal(const char *surface)
{
    lv_refr_now(s_harness.display);
    sim_display_rgb(s_partial);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_harness.display);
    sim_display_rgb(s_full);
    if (memcmp(s_partial, s_full, sizeof(s_full))) {
        unsigned count = 0;
        int left = SIM_D, top = SIM_D, right = -1, bottom = -1;
        for (int y = 0; y < SIM_D; ++y) for (int x = 0; x < SIM_D; ++x) {
            size_t offset = (size_t)(y * SIM_D + x) * 3;
            if (!memcmp(s_partial + offset, s_full + offset, 3)) continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
            if (count < 4) fprintf(stderr, "Time mismatch (%d,%d): retained=%u,%u,%u full=%u,%u,%u\n",
                x, y, s_partial[offset], s_partial[offset + 1], s_partial[offset + 2],
                s_full[offset], s_full[offset + 1], s_full[offset + 2]);
            ++count;
        }
        fprintf(stderr, "Time retained/full mismatch: %s pixels=%u bounds=(%d,%d)..(%d,%d)\n",
            surface, count, left, top, right, bottom);
        char path[1024];
        snprintf(path, sizeof(path), "%s/FAILED-full-%s.png", s_harness.directory, surface);
        (void)sim_display_save_png(path);
        check(false, "retained pixels match a full frozen redraw");
    }
    check(true, "retained pixels match a full frozen redraw");
}

static void verify_labels(lv_obj_t *object, bool scrolling)
{
    if (!lv_obj_is_visible(object) || !lv_obj_get_style_opa(object, 0)) return;
    bool is_label = lv_obj_check_type(object, &lv_label_class);
    /* LVGL labels retain the base scrollable bit despite having no scroll
     * content. Only actual ancestor list viewports exempt curved rows. */
    scrolling |= !is_label && lv_obj_is_scrollable(object);
    if (!scrolling && is_label && *lv_label_get_text(object)) {
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        lv_area_t rendered = area;
        lv_obj_get_transformed_area(object, &rendered, LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE);
        lv_point_t size;
        lv_text_get_size(&size, lv_label_get_text(object), lv_obj_get_style_text_font(object, 0),
            lv_obj_get_style_text_letter_space(object, 0), lv_obj_get_style_text_line_space(object, 0),
            lv_obj_get_width(object), LV_TEXT_FLAG_NONE);
        if (size.y > lv_obj_get_height(object)) fprintf(stderr, "Time clipped label: %s\n", lv_label_get_text(object));
        check(size.y <= lv_obj_get_height(object), "label contains every wrapped text line");
        int chord = orb_chord_width(rendered.y1, rendered.y2, ORB_RIM_MARGIN_TEXT);
        int rendered_width = lv_area_get_width(&rendered);
        if (chord < rendered_width) fprintf(stderr,
            "Time label exceeds chord: %s y=%d..%d width=%d chord=%d\n",
            lv_label_get_text(object), rendered.y1, rendered.y2, rendered_width, chord);
        check(chord >= rendered_width, "visible label fits its rendered circle chord");
        lv_obj_t *parent = lv_obj_get_parent(object);
        if (parent && lv_obj_is_clickable(parent) && lv_obj_get_height(parent) < SIM_D) {
            lv_area_t button;
            lv_obj_get_coords(parent, &button);
            check(area.x1 >= button.x1 && area.x2 <= button.x2 && area.y1 >= button.y1 && area.y2 <= button.y2,
                "action label fits inside its actual control");
            check(lv_obj_get_width(parent) >= 46 && lv_obj_get_height(parent) >= 46,
                "action retains minimum touch target");
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        verify_labels(lv_obj_get_child(object, (int32_t)i), scrolling);
}

static lv_area_t ascii_ink_bounds(lv_obj_t *label)
{
    /* Match lv_draw_label's glyph baseline math. The short ASCII lap strings
     * are single-line; comparing leading boxes would falsely call their
     * shared two-pixel line-leading margin an ink collision. */
    lv_area_t area; lv_obj_get_coords(label, &area);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const char *text = lv_label_get_text(label);
    lv_area_t ink = {area.x1, LV_COORD_MAX, area.x2, LV_COORD_MIN};
    for (size_t i = 0; text[i]; ++i) {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, (unsigned char)text[i], (unsigned char)text[i + 1]) || !glyph.box_h) continue;
        int32_t top = area.y1 + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        ink.y1 = LV_MIN(ink.y1, top); ink.y2 = LV_MAX(ink.y2, top + glyph.box_h - 1);
    }
    return ink;
}

static void verify_counter_and_lap_type(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *value = lv_label_get_text(object);
        if (value[0] == '.' && value[1] >= '0' && value[1] <= '9' && !value[2]) {
            lv_point_t extent;
            lv_text_get_size(&extent, value, lv_obj_get_style_text_font(object, 0),
                lv_obj_get_style_text_letter_space(object, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            check(extent.x <= lv_obj_get_width(object) && extent.y == lv_obj_get_height(object),
                "stopwatch decimal suffix stays on one line at every content size and weight");
        }
        if (*value >= '0' && *value <= '9' &&
            (strchr(value, ':') || strspn(value, "0123456789") == strlen(value))) {
            lv_font_glyph_dsc_t one, eight;
            const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
            bool tabular = lv_font_get_glyph_dsc(font, &one, '1', 0) &&
                lv_font_get_glyph_dsc(font, &eight, '8', 0) && one.adv_w == eight.adv_w;
            if (!tabular) fprintf(stderr, "Time proportional numeric label: %s\n", value);
            check(tabular,
                "time values use actual tabular figure advances");
        }
        if (!strncmp(value, "Lap ", 4)) {
            lv_obj_t *row = lv_obj_get_parent(object);
            for (unsigned i = 0; i < lv_obj_get_child_count(row); ++i) {
                lv_obj_t *other = lv_obj_get_child(row, (int32_t)i);
                if (!lv_obj_check_type(other, &lv_label_class)) continue;
                const char *digits = lv_label_get_text(other);
                if (*digits < '0' || *digits > '9') continue;
                const char *hundredths = strchr(digits, '.');
                check(hundredths && strlen(hundredths) == 3 &&
                    hundredths[1] >= '0' && hundredths[1] <= '9' &&
                    hundredths[2] >= '0' && hundredths[2] <= '9',
                    "lap splits retain exactly two hundredths digits");
                lv_area_t name = ascii_ink_bounds(object), time = ascii_ink_bounds(other);
                bool stacked = name.y2 < time.y1 && name.x1 == time.x1;
                if (!stacked) fprintf(stderr, "Time lap title/subtitle ink overlap: %s (%d,%d)-(%d,%d) split=%s (%d,%d)-(%d,%d)\n",
                    value, name.x1, name.y1, name.x2, name.y2, digits, time.x1, time.y1, time.x2, time.y2);
                check(stacked,
                    "kit lap title and tabular subtitle stack without overlap");
                lv_point_t unwrapped;
                lv_text_get_size(&unwrapped, digits, lv_obj_get_style_text_font(other, 0),
                    lv_obj_get_style_text_letter_space(other, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                check(unwrapped.x <= lv_obj_get_width(other),
                    "entire lap split fits one line, including hour groups and hundredths");
                for (unsigned marker_index = 0; marker_index < lv_obj_get_child_count(row); ++marker_index) {
                    lv_obj_t *marker = lv_obj_get_child(row, (int32_t)marker_index);
                    if (!lv_obj_check_type(marker, &lv_label_class)) continue;
                    const char *symbol = lv_label_get_text(marker);
                    if (strcmp(symbol, ORB_ICON_ARROW_UP) && strcmp(symbol, ORB_ICON_ARROW_DOWN)) continue;
                    lv_area_t arrow; lv_obj_get_coords(marker, &arrow);
                    check(time.x1 - arrow.x2 - 1 >= 8,
                        "lap extrema arrow has at least eight logical pixels before the entire split");
                }
            }
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        verify_counter_and_lap_type(lv_obj_get_child(object, (int32_t)i));
}

static lv_obj_t *scroll_list(lv_obj_t *object)
{
    if (lv_obj_is_visible(object) && !lv_obj_check_type(object, &lv_label_class) &&
        lv_obj_is_scrollable(object) &&
        (lv_obj_get_scroll_bottom(object) > 0 || lv_obj_get_scroll_y(object) > 0)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = scroll_list(lv_obj_get_child(object, (int32_t)i));
        if (found) return found;
    }
    return NULL;
}

static unsigned visible_glass(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return 0;
    unsigned count = orb_glass_get_amount(object) > 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        count += visible_glass(lv_obj_get_child(object, (int32_t)i));
    return count;
}

static bool visible_ink(int x, int y)
{
    for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
        unsigned offset = (unsigned)((y + dy) * SIM_D + x + dx) * 3;
        if (s_partial[offset] > 20 || s_partial[offset + 1] > 20 || s_partial[offset + 2] > 20) return true;
    }
    return false;
}

static void verify_digit_pixel_chord(void)
{
    /* Raster scaling intentionally keeps logical label bounds unscaled. HIG
     * §3 is a rendered-pixel rule, so test the actual white figure ink too. */
    unsigned outside = 0;
    const int radius = ORB_RIM_RADIUS - (ORB_RIM_WIDTH + 1) / 2 - ORB_RIM_MARGIN_TEXT;
    orb_time_snapshot_t state;
    orb_time_snapshot(now_us(), &state);
    bool selection = (orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER && state.timer_state == ORB_TIME_IDLE) ||
        (orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_ALARM && state.clock_valid &&
         !state.alarm_enabled && !state.alarm_ringing);
    for (int y = 160; y <= 278; ++y) for (int x = 0; x < SIM_D; ++x) {
        int dx = x - SIM_D / 2, dy = y - SIM_D / 2;
        if (dx * dx + dy * dy <= radius * radius) continue;
        /* The kit scroll arc is deliberately outside rows at radius224;
         * its white thumb is permitted ink, unlike digits near the ring. */
        if (dx * dx + dy * dy >= 206 * 206) continue;
        unsigned offset = (unsigned)(y * SIM_D + x) * 3;
        bool white = s_partial[offset] > 200 && s_partial[offset + 1] > 200 && s_partial[offset + 2] > 200;
        /* Selected figures only exist while editing, when the resting track
         * is gray. Running values are white: their teal progress ring must
         * not be mistaken for digit ink at its antialiased inner edge. */
        bool selected = selection && s_partial[offset] < 115 && s_partial[offset + 1] > 150 && s_partial[offset + 2] > 150;
        outside += white || selected;
    }
    check(outside == 0, "figure ink leaves the text margin inside the ring track");
}

static unsigned counter_ink_height(void)
{
    /* Timer's exported hero starts at y140. Include its complete ink while
     * excluding the independent status/Laps content below y240. In
     * particular the max stopwatch pauses, so "Paused" is visible there. */
    int top = 239, bottom = 139;
    for (int y = 140; y <= 238; ++y) for (int x = 60; x <= 406; ++x) {
        unsigned offset = (unsigned)(y * SIM_D + x) * 3;
        bool white = s_partial[offset] > 150 && s_partial[offset + 1] > 150 && s_partial[offset + 2] > 150;
        bool selected = s_partial[offset] < 115 && s_partial[offset + 1] > 150 && s_partial[offset + 2] > 150;
        if (!white && !selected) continue;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
    }
    check(bottom >= top, "main counter paints measurable figure ink");
    return (unsigned)(bottom - top + 1);
}

static void verify_scrolled_controls(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return;
    if (orb_glass_get_amount(object) > 0) {
        lv_area_t area; lv_obj_get_coords(object, &area);
        check(area.y1 >= 140 && area.y2 < 424,
            "scrolling action material disappears completely before viewport clipping");
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        verify_scrolled_controls(lv_obj_get_child(object, (int32_t)i));
}

static void verify_laps_header(void)
{
    lv_obj_t *root = orb_time_view_root(s_harness.view);
    check(text_count(root, "Stopwatch") == 1, "Stopwatch has one semantic rim title and no stray footer");
    lv_obj_t *title = find_text(root, "Stopwatch");
    check(title != NULL, "scrolled laps retain the mode title");
    lv_obj_t *rim = lv_obj_get_parent(title);
    orb_kit_rim_text_info_t info;
    check(orb_kit_rim_text_info(rim, &info), "Stopwatch title is built by the shared rim factory");
    check(info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + .1f,
        "Stopwatch rim title stays inside the shared maximum arc");
    lv_area_t title_area; lv_obj_get_coords(rim, &title_area);
    check(title_area.y2 < 140, "curved mode title stays above laps");
    verify_scrolled_controls(root);
    lv_obj_t *list = scroll_list(root);
    if (list) {
        lv_area_t viewport; lv_obj_get_coords(list, &viewport);
        check(viewport.y2 < 404, "lap viewport ends sixteen pixels before the page dots");
    }
    if (list && lv_obj_get_scroll_y(list) >= 180) {
        orb_time_snapshot_t state;
        orb_time_snapshot(now_us(), &state);
        /* Polish review 23/24: the header keeps the counter's precision. */
        char elapsed[40];
        orb_time_format_stopwatch(elapsed, sizeof(elapsed), state.stopwatch_elapsed_us);
        lv_obj_t *label = find_text(root, elapsed);
        check(label != NULL, "scrolled laps pin live elapsed time in the compact header");
        lv_area_t area = ascii_ink_bounds(label);
        int title_bottom = -1;
        for (int y = 0; y < 140; ++y) for (int x = 72; x < 394; ++x) {
            size_t at = (size_t)(y * SIM_D + x) * 3;
            unsigned r = s_partial[at], g = s_partial[at + 1], b = s_partial[at + 2];
            if (g > 80 && b > 80 && g * 10 > r * 14 && b * 10 > r * 15) title_bottom = y;
        }
        check(area.y1 > title_bottom && area.y2 <= 140,
            "secondary elapsed time fits between pinned title and lap viewport");
        check(!has_text("Reset") && !has_text("Resume") && !has_text("Pause") && !has_text("Lap"),
            "scrolled-away controls and captions leave no clipped halves");
    }
}

static unsigned neutral_ink(int x1, int y1, int x2, int y2)
{
    unsigned ink = 0;
    for (int y = y1; y <= y2; ++y) for (int x = x1; x <= x2; ++x) {
        unsigned offset = (unsigned)(y * SIM_D + x) * 3;
        ink += s_partial[offset] > 100 && s_partial[offset + 1] > 100 && s_partial[offset + 2] > 100;
    }
    return ink;
}

static unsigned title_ink(int x1, int y1, int x2, int y2)
{
    unsigned ink = 0;
    for (int y = y1; y <= y2; ++y) for (int x = x1; x <= x2; ++x) {
        unsigned offset = (unsigned)(y * SIM_D + x) * 3;
        bool neutral = s_partial[offset] > 100 && s_partial[offset + 1] > 100 && s_partial[offset + 2] > 100;
        bool teal = s_partial[offset + 1] > 100 && s_partial[offset + 2] > 100;
        ink += neutral || teal;
    }
    return ink;
}

static void verify_reminder_rows(void)
{
    if (!has_text("Reminders")) return;
    lv_obj_t *list = orb_time_view_scroll(s_harness.view);
    if (!list) return;
    /* A detail title is a scrolling prose label, not a list row. */
    if (lv_obj_get_child_count(list) && lv_obj_check_type(lv_obj_get_child(list, 0), &lv_label_class)) return;
    lv_area_t viewport; lv_obj_get_coords(list, &viewport);
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t *row = lv_obj_get_child(list, (int32_t)i);
        if (lv_obj_is_floating(row)) continue;
        lv_area_t area; lv_obj_get_coords(row, &area);
        if (area.y1 < viewport.y1 || area.y2 > viewport.y2) continue;
        /* Time opts into two-line titles at every size; AX3 also grows
         * for the shared title/subtitle ink and spacing budget. */
        lv_obj_t *title = lv_obj_get_child(row, 1);
        lv_point_t one_line = {0};
        if (title && lv_obj_check_type(title, &lv_label_class))
            lv_text_get_size(&one_line, "A", lv_obj_get_style_text_font(title, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        check(lv_obj_get_height(row) == ORB_KIT_ROW_H ||
            (lv_obj_get_height(row) > ORB_KIT_ROW_H &&
             (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ||
              (title && lv_obj_get_height(title) > one_line.y))),
            "reminder rows grow for wrapped titles or accessibility ink spacing");
        check(neutral_ink(area.x1, area.y1, area.x2, area.y2) > 100,
            "each complete reminder row paints actual title and due-time ink");
    }
}

static void verify_lap_ink(void)
{
    unsigned ink = 0;
    for (int y = 180; y <= 350; ++y) for (int x = 70; x <= 396; ++x) {
        unsigned offset = (unsigned)(y * SIM_D + x) * 3;
        ink += s_partial[offset] > 100 && s_partial[offset + 1] > 100 && s_partial[offset + 2] > 100;
    }
    check(ink > 100, "curved lap rows actually paint legible content after outer viewport scroll");
}

static void verify_page_dots(void)
{
    /* Read the actual pixels, including retained redraws: duplicated dots or
     * lap text in this band must not pass simply because the tree has one
     * navigation object. The timer ring also stays outside this clear band. */
    unsigned dots = 0;
    bool previous = false;
    for (int x = 201; x <= 265; ++x) {
        unsigned offset = (unsigned)(418 * SIM_D + x) * 3;
        bool painted = s_partial[offset] > 30 || s_partial[offset + 1] > 30 || s_partial[offset + 2] > 30;
        if (painted && !previous) ++dots;
        previous = painted;
    }
    orb_time_snapshot_t state;
    orb_time_snapshot(now_us(), &state);
    if (orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER && state.timer_state == ORB_TIME_FINISHED) {
        /* Polish review 26: modal alerts carry no page dots. */
        check(dots == 0, "the timer alert paints no page dots");
        return;
    }
    check(dots == 4, "exactly four kit dots paint on the bottom rim inside the ring");
    unsigned stray = 0;
    /* Stopwatch's approved second marker/tick can enter the band below the
     * dots; only Timer needs a 16 px gap to its continuous progress ring. */
    int bottom = orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER ? 435 : 414;
    for (int y = 404; y <= bottom; ++y) for (int x = 213; x < 253; ++x) {
        if (y >= 414 && y < 423) continue;
        unsigned offset = (unsigned)(y * SIM_D + x) * 3;
        stray += s_partial[offset] > 20 || s_partial[offset + 1] > 20 || s_partial[offset + 2] > 20;
    }
    check(stray == 0, "page dots have clear space above the ring and below every lap row");
}

static void save(const char *name)
{
    bool editor = !strncmp(name, "presets-", 8) || !strncmp(name, "options-", 8) ||
        !strncmp(name, "custom-days-", 12);
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s.png", s_harness.directory, name);
    check(length > 0 && (size_t)length < sizeof(path), "screenshot path fits");
    frame_equal(name);
    check(sim_display_save_png(path), "save frozen, full-redraw-equivalent Time screenshot");
    if (editor) {
        lv_obj_t *close = find_action("Close");
        lv_area_t box; lv_obj_get_coords(close, &box);
        check(orb_glass_get_amount(close) > .99f &&
            neutral_ink(box.x1, box.y1, box.x2, box.y2) > 100,
            "every editor retains a materialized Close control and visible X pixels");
        check(lv_obj_get_width(close) == ORB_KIT_DISC_MINI && lv_obj_get_height(close) == ORB_KIT_DISC_MINI,
            "editor Close uses the shared mini disc role");
    }
    if (!editor && strncmp(name, "motion-", 7) && orb_time_view_page(s_harness.view) != ORB_TIME_PAGE_REMINDER) {
        const char *titles[] = {"Timer", "Stopwatch", "Alarm"};
        lv_obj_t *title = find_text(orb_time_view_root(s_harness.view), titles[orb_time_view_page(s_harness.view)]);
        check(title != NULL, "current mode has a visible title");
        lv_obj_t *rim = lv_obj_get_parent(title);
        orb_kit_rim_text_info_t info;
        check(orb_kit_rim_text_info(rim, &info), "current mode title comes from the kit");
        check(info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + .1f,
            "current mode title fits the maximum curved arc");
        lv_area_t area; lv_obj_get_coords(rim, &area);
        check(title_ink(area.x1, area.y1, area.x2, area.y2) > 100,
            "mode title actually paints visible pixels above the counter/list");
    }
    verify_labels(orb_time_view_root(s_harness.view), false);
    verify_counter_and_lap_type(orb_time_view_root(s_harness.view));
    if (orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_REMINDER) verify_reminder_rows();
    check(!has_text(ORB_ICON_CHEVRON_LEFT) && !has_text(ORB_ICON_CHEVRON_RIGHT),
        "Time contains no navigation chevrons");
    check(!has_text("Remaining") && !has_text("Elapsed") && !has_text("Ready") &&
        !has_text("Turn rim · minutes") && !has_text("Set clock") && !has_text("Hour") &&
        !has_text("Min") && !has_text("Sec"), "Time has no redundant hints or form tabs");
    check(visible_glass(lv_screen_active()) <= 3, "Time stays within the three-glass budget");
    if (strncmp(name, "motion-", 7)) {
        lv_mem_monitor_t memory;
        lv_mem_monitor(&memory);
        printf("Time allocation %-24s LVGL_delta=%" PRId64 " bytes total=%zu blocks_delta=%" PRId64 " state=%zu bytes\n", name,
            (int64_t)memory.total_size - (int64_t)s_harness.view_base_bytes,
            memory.total_size, (int64_t)memory.used_cnt - (int64_t)s_harness.view_base_blocks,
            orb_time_view_state_size());
    }
    orb_time_snapshot_t state;
    orb_time_snapshot(now_us(), &state);
    if (!editor && strncmp(name, "motion-", 7) && orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER) {
        const char *action_name = state.timer_state == ORB_TIME_RUNNING ? "Pause" :
            state.timer_state == ORB_TIME_FINISHED ? "Stop" :
            state.timer_state == ORB_TIME_PAUSED ? "Resume" : "Start";
        lv_obj_t *action = find_action(action_name);
        lv_area_t box; lv_obj_get_coords(action, &box);
        unsigned ink = title_ink(box.x1, box.y1, box.x2, box.y2);
        if (ink <= 100) fprintf(stderr, "Time primary absent: %s %s bounds=(%d,%d)-(%d,%d) amount=%g opa=%d\n",
            name, action_name, box.x1, box.y1, box.x2, box.y2, (double)orb_glass_get_amount(action),
            lv_obj_get_style_opa(action, 0));
        check(ink > 100, "every timer state paints its primary action, including a new hour format after completion");
    }
    if (!editor && strncmp(name, "motion-", 7) && strncmp(name, "stopwatch-laps", 14) &&
        orb_time_view_page(s_harness.view) != ORB_TIME_PAGE_REMINDER) {
        verify_digit_pixel_chord();
        if (orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER)
            check(visible_ink(233, 20) && visible_ink(446, 233) &&
                visible_ink(233, 446) && visible_ink(20, 233),
                "digit raster leaves the specified rim track continuous at all four cardinal points");
    }
    if (orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_REMINDER && state.reminder_due) {
        check(has_text("In 1 hour") && has_text("Done"),
            "due reminder offers both Snooze and Done");
    }
    if (!strncmp(name, "stopwatch-laps", 14)) {
        verify_lap_ink();
        verify_laps_header();
    }
    if (!editor && strncmp(name, "motion-", 7) &&
        (orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER ||
         orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_STOPWATCH)) verify_page_dots();
    if (!strncmp(name, "motion-laps-scroll-", 19)) verify_laps_header();
}

static void sequence(const char *name, unsigned frame)
{
    char file[128];
    snprintf(file, sizeof(file), "motion-%s-%02u", name, frame);
    save(file);
}

static void tap_sequence(const char *action, const char *name, unsigned *frame)
{
    lv_obj_t *object = find_action(action);
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    press((bounds.x1 + bounds.x2 + 1) / 2, (bounds.y1 + bounds.y2 + 1) / 2);
    sequence(name, (*frame)++);
    advance(16); sequence(name, (*frame)++);
    release(); sequence(name, (*frame)++);
    advance(80); sequence(name, (*frame)++);
    advance(300); sequence(name, (*frame)++);
}

static void measure_start(void)
{
    s_harness.frames = s_harness.damage = s_harness.flush_pixels = 0;
    s_harness.max_damage = s_harness.max_frame = 0;
    (void)sim_display_take_changed();
    s_harness.measure = true;
}

static void measure_stop(void) { s_harness.measure = false; }

static void quiet(const char *surface)
{
    advance(1500);
    measure_start();
    advance(2000);
    measure_stop();
    printf("Time idle %-20s frames=%u damage=%u\n", surface, s_harness.frames, s_harness.damage);
    check(s_harness.frames == 0, "idle Time renders zero frames");
    check(s_harness.damage == 0, "idle Time invalidates nothing");
    frame_equal(surface);
}

static orb_time_snapshot_t snapshot(void)
{
    orb_time_snapshot_t state;
    orb_time_snapshot(now_us(), &state);
    return state;
}

static void reset(orb_time_page_t page, bool clock_valid)
{
    if (s_harness.view) orb_time_view_destroy(s_harness.view);
    /* Retire queued raster/task leases before measuring the next tree. Without
     * this frozen refresh, the baseline includes the deleted view's cache. */
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(s_harness.display);
    orb_time_init();
    /* The canonical Design 1.0 fixtures use the owner's 12-hour clock. Test
     * 24-hour formatting separately instead of inheriting a host locale. */
    orb_time_set_clock_format(false);
    struct tm local = {.tm_year = 126, .tm_mon = 9, .tm_mday = 27, .tm_hour = 10, .tm_min = 9};
    s_harness.wall_start = clock_valid ? mktime(&local) : 0;
    s_harness.wall_start_ms = s_harness.now;
    orb_time_poll(now_us(), wall_now());
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    s_harness.view_base_bytes = memory.total_size;
    s_harness.view_base_blocks = memory.used_cnt;
    s_harness.view = orb_time_view_create(lv_screen_active(), NULL);
    check(s_harness.view != NULL, "create Time view");
    orb_time_view_select(s_harness.view, page);
    poll(true);
    advance(500);
}

static void running_rate(const char *surface)
{
    advance(500);
    measure_start();
    advance(2000);
    measure_stop();
    printf("Time running %-17s frames=%u damage=%u max_damage=%u max_frame=%u\n",
        surface, s_harness.frames, s_harness.damage, s_harness.max_damage, s_harness.max_frame);
    check(s_harness.frames > 0 && s_harness.frames <= 8, "running display updates at most four times per second");
    check(s_harness.max_damage < SIM_D * SIM_D / 2, "running display damages retained regions, not whole screen");
    frame_equal(surface);
}

/* Real gestures exercise the same LVGL pointer recognizer as firmware. */
static void swipe(int x1, int y1, int x2, int y2)
{
    press(x1, y1);
    for (int i = 1; i <= 8; ++i) {
        press(x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8);
        advance(16);
    }
    release();
    advance(600);
}

static void page_dot(orb_time_page_t page)
{
    const int xs[] = {155, 207, 259, 311};
    tap(xs[page], 424);
    check(orb_time_view_page(s_harness.view) == page,
        "bottom-arc page dot is a visible paging alternative");
}

static unsigned numeric_font(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return 0;
    unsigned result = 0;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *value = lv_label_get_text(object);
        lv_area_t area; lv_obj_get_coords(object, &area);
        if (*value >= '0' && *value <= '9' && area.y1 >= 128 && area.y2 <= 290)
            result = lv_obj_get_style_text_font(object, 0)->line_height;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        unsigned child = numeric_font(lv_obj_get_child(object, (int32_t)i));
        if (child > result) result = child;
    }
    return result;
}

static bool teal_at(int x, int y)
{
    for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
        unsigned offset = (unsigned)((y + dy) * SIM_D + x + dx) * 3;
        if (s_partial[offset] < 115 && s_partial[offset + 1] > 150 && s_partial[offset + 2] > 150)
            return true;
    }
    return false;
}

static void fixtures(void)
{
    reset(ORB_TIME_PAGE_TIMER, true);
    save("timer-idle"); quiet("timer-idle");
    counter_is("5:00");
    unsigned timer_font = numeric_font(orb_time_view_root(s_harness.view));
    check(timer_font > 0, "timer has large numeric content");
    unsigned timer_ink_height = counter_ink_height();
    save("timer-minute");
    int64_t duration_before = snapshot().timer_duration_us;
    /* Polish review 18: digits rest white; the first tap targets the group
     * (teal plus the kit focus frame) and a second tap adds one. */
    tap_counter_group(0);
    check(snapshot().timer_duration_us == duration_before, "first minute tap targets without changing duration");
    tap_counter_group(0);
    check(snapshot().timer_duration_us == duration_before + INT64_C(60000000),
        "tapping active minutes advances one minute");
    tap_counter_group(1); save("timer-second");
    duration_before = snapshot().timer_duration_us;
    tap_counter_group(1);
    check(snapshot().timer_duration_us == duration_before + INT64_C(1000000),
        "tapping active seconds advances one second");
    check(orb_time_timer_set(3599), "set last sub-hour timer value"); poll(true);
    counter_is("59:59"); save("timer-underhour-boundary");
    tap_counter_group(0);
    duration_before = snapshot().timer_duration_us;
    tap_counter_group(0);
    check(snapshot().timer_duration_us == duration_before + INT64_C(60000000),
        "visible minute tap carries into a previously hidden hour group");
    counter_is("1:00:59"); save("timer-hour");
    duration_before = snapshot().timer_duration_us;
    tap_counter_group(0);
    check(snapshot().timer_duration_us == duration_before, "newly revealed hour tap selects without changing duration");
    tap_counter_group(0);
    check(snapshot().timer_duration_us == duration_before + INT64_C(3600000000),
        "tapping selected hours advances one hour");
    check(orb_time_timer_set(300), "restore five-minute duration"); poll(true);
    tap_action("Start");
    check(snapshot().timer_state == ORB_TIME_RUNNING, "Start pointer starts timer");
    jump(90000);
    counter_is("3:30");
    save("timer-running"); running_rate("timer-running");
    check(numeric_font(orb_time_view_root(s_harness.view)) == timer_font,
        "timer digits retain their size while running");
    tap_action("Pause");
    check(snapshot().timer_state == ORB_TIME_PAUSED, "Pause pointer pauses timer");
    save("timer-paused"); quiet("timer-paused");
    check(numeric_font(orb_time_view_root(s_harness.view)) == timer_font,
        "timer digits retain their size while paused");
    tap_action("Cancel");
    check(snapshot().timer_state == ORB_TIME_IDLE, "Cancel pointer restores timer");
    check(orb_time_timer_set(1), "set one-second finish fixture"); poll(true);
    tap_action("Start"); jump(1100);
    check(snapshot().timer_state == ORB_TIME_FINISHED, "timer finish is latched");
    save("timer-finished"); quiet("timer-finished");
    check(numeric_font(orb_time_view_root(s_harness.view)) == timer_font,
        "timer digits retain their size on finish");
    /* Polish review 26: the alert is modal, like a ringing alarm. */
    swipe(320, 170, 140, 170);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER, "a centre swipe never pages away from the timer alert");
    tap_action("Stop");
    check(orb_time_timer_set(1), "set background finish fixture"); poll(true);
    tap_action("Start");
    page_dot(ORB_TIME_PAGE_STOPWATCH);
    jump(3000);
    check(snapshot().timer_state == ORB_TIME_FINISHED, "navigation does not acknowledge timer finish");
    check(has_text("Timer finished"), "timer finish stays visible on another Time page");
    save("timer-finish-on-stopwatch");
    /* The finish receipt is content in 2.0; the kit page position supplies
     * navigation without reintroducing a text-only action. */
    page_dot(ORB_TIME_PAGE_TIMER);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER &&
        snapshot().timer_state == ORB_TIME_FINISHED,
        "returning to Timer preserves its finish until explicit dismissal");
    /* Polish review 26: the finished timer uses the shared alert template. */
    check(has_text("Repeat"), "timer alert offers Repeat beside its tinted Stop");
    tap_action("Stop");
    check(snapshot().timer_state == ORB_TIME_IDLE, "Stop explicitly acknowledges timer finish");
    reset(ORB_TIME_PAGE_TIMER, true);
    check(orb_time_timer_set(ORB_TIME_TIMER_MAX_SECONDS), "set 24-hour boundary"); poll(true);
    save("timer-longduration"); quiet("timer-longduration");
    counter_is("24:00:00");
    /* The native clock family and published Inter styles have different
     * line-box leading. Compare their actual ink below, not line heights. */
    check(numeric_font(orb_time_view_root(s_harness.view)) >= 20,
        "24-hour duration uses a readable native tabular style fitting its reserved template");
    /* Design 2.0 keeps M:SS at the native hero size from Timer.dc.html.
     * Reserving an unused hour group shrank every short timer. A visible long
     * H:MM:SS line may fit down, while its base font and chord gates stay fixed. */
    check(counter_ink_height() <= timer_ink_height && counter_ink_height() >= 28,
        "long timer figures fit inside the native hero size and stay readable");
    verify_digit_pixel_chord();

    reset(ORB_TIME_PAGE_TIMER, true);
    check(orb_time_timer_set(3601), "set timer hour-crossing fixture"); poll(true);
    orb_time_timer_start(now_us()); poll(true); counter_is("1:00:01");
    save("timer-hour-running");
    unsigned hour_ink_height = counter_ink_height();
    /* Polish review 3: the hour group leaves once it carries no information
     * and the reading recentres at the native hero size. */
    jump(2000); counter_is("59:59"); save("timer-hour-crossed");
    check(counter_ink_height() >= hour_ink_height,
        "countdown crossing one hour drops its hour group and returns to the native hero");

    reset(ORB_TIME_PAGE_STOPWATCH, true);
    save("stopwatch-idle"); quiet("stopwatch-idle");
    counter_is("00:00");
    unsigned stopwatch_font = numeric_font(orb_time_view_root(s_harness.view));
    tap_action("Start"); jump(82400);
    counter_is("01:22");
    save("stopwatch-running"); running_rate("stopwatch-running");
    tap_action("Lap");
    check(snapshot().lap_count == 1, "real Lap pointer records a split");
    save("stopwatch-lap");
    for (unsigned i = 0; i < 7; ++i) {
        jump(8300 + i * 2150);
        check(orb_time_stopwatch_lap(now_us()), "append deterministic lap fixture");
    }
    poll(true);
    tap_action("Pause");
    save("stopwatch-paused"); quiet("stopwatch-paused");
    check(numeric_font(orb_time_view_root(s_harness.view)) == stopwatch_font,
        "stopwatch digits retain their size across states");
    swipe(233, 300, 233, 120);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_STOPWATCH,
        "vertical lap scrolling stays in Stopwatch");
    save("stopwatch-laps"); quiet("stopwatch-laps");
    lv_obj_t *lap_list = scroll_list(orb_time_view_root(s_harness.view));
    lv_obj_t *fast_lap = find_retained_text(orb_time_view_root(s_harness.view), "Lap 2");
    check(lap_list && fast_lap, "retained fastest lap remains reachable");
    lv_area_t fast_bounds; lv_obj_get_coords(fast_lap, &fast_bounds);
    int target_scroll = lv_obj_get_scroll_y(lap_list) + (fast_bounds.y1 + fast_bounds.y2) / 2 - 233;
    lv_obj_scroll_to_y(lap_list, target_scroll, LV_ANIM_OFF); advance(300);
    save("stopwatch-laps-extremes");
    unsigned teal = 0, red = 0;
    for (int y = 130; y < 424; ++y) for (int x = 46; x < 420; ++x) {
        unsigned offset = (unsigned)(y * SIM_D + x) * 3;
        unsigned r = s_partial[offset], g = s_partial[offset + 1], b = s_partial[offset + 2];
        teal += g > 70 && b > 70 && g * 10 > r * 16 && b * 10 > r * 16;
        red += r > 100 && r * 10 > g * 16 && r * 10 > b * 14;
    }
    check(teal > 20 && red > 20, "actual lap pixels mark fastest teal and slowest red within two accents");
    quiet("laps-extremes");

    reset(ORB_TIME_PAGE_STOPWATCH, true);
    tap_action("Start"); jump(3599000);
    counter_is("59:59"); save("stopwatch-underhour-boundary");
    unsigned stopwatch_ink_height = counter_ink_height();
    jump(1000); counter_is("1:00:00"); save("stopwatch-hour-running");
    check(counter_ink_height() <= stopwatch_ink_height && counter_ink_height() >= 28,
        "one-hour stopwatch fits its visible groups inside the native hero size");
    verify_digit_pixel_chord();
    check(orb_time_stopwatch_lap(now_us()), "record one-hour lap split");
    jump((unsigned)((ORB_TIME_STOPWATCH_MAX_US - snapshot().stopwatch_elapsed_us) / 1000 - 1000));
    check(orb_time_stopwatch_lap(now_us()), "record two-digit-hour lap split");
    jump(500);
    check(orb_time_stopwatch_lap(now_us()), "record short lap beside long splits");
    poll(true); tap_action("Pause");
    tap_action("Laps"); advance(600);
    save("stopwatch-laps-long"); verify_laps_header();
    return_stopwatch_summary();
    tap_action("Resume"); jump(1500);
    counter_is("99:59:59"); save("stopwatch-max");
    check(counter_ink_height() <= stopwatch_ink_height && counter_ink_height() >= 28,
        "maximum stopwatch figures remain readable after long-line fitting");
    verify_digit_pixel_chord();

    reset(ORB_TIME_PAGE_ALARM, true);
    save("alarm-idle"); quiet("alarm-idle");
    counter_is("7:00");
    tap_counter_group(0); save("alarm-hour");
    unsigned hour_before = snapshot().alarm_hour;
    tap_counter_group(0);
    check(snapshot().alarm_hour == (hour_before + 1) % 24, "active alarm hour tap advances one hour");
    tap_counter_group(1); save("alarm-minute"); quiet("alarm-minute");
    unsigned minute_before = snapshot().alarm_minute;
    tap_counter_group(1);
    check(snapshot().alarm_minute == (minute_before + 1) % 60, "active alarm minute tap advances one minute");
    tap_action("Set");
    check(snapshot().alarm_enabled, "Set pointer arms alarm");
    save("alarm-enabled"); quiet("alarm-enabled");
    tap_counter_group(0); save("alarm-editing"); quiet("alarm-editing");
    check(orb_time_alarm_set(7, 0), "set the canonical alarm ringing fixture");
    poll(true);
    orb_time_snapshot_t state = snapshot();
    page_dot(ORB_TIME_PAGE_TIMER);
    s_harness.wall_start = state.alarm_next;
    s_harness.wall_start_ms = s_harness.now;
    check(orb_time_timer_set(1) && orb_time_timer_start(now_us()), "coincident timer finish fixture");
    jump(1000);
    check(snapshot().timer_state == ORB_TIME_FINISHED, "timer also finishes while alarm begins");
    check(snapshot().alarm_ringing, "one-shot alarm reaches persistent finish");
    advance(300);
    check(has_text("Stop") && has_text("Snooze"), "ringing alarm replaces another Time page with explicit actions");
    save("alarm-ringing-on-timer");
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_ALARM && snapshot().alarm_ringing,
        "alarm opens full screen without dismissing it");
    save("alarm-ringing"); quiet("alarm-ringing");
    /* Design2 reserves the bottom arc for Stop/Snooze. The shell harness
     * verifies actual safe double-tap cancellation; its old text hint is no
     * longer part of this view's composition. */
    check(!has_text("Weekdays"), "one-shot alarm does not invent a repeat schedule");
    orb_time_rotary_t owner;
    check(!orb_time_view_rotary(s_harness.view, &owner), "ringing alarm cannot edit alarm time by rim");
    tap_action("Snooze");
    check(!snapshot().alarm_ringing && snapshot().alarm_enabled, "Snooze reschedules the alarm");
    check(snapshot().alarm_hour == state.alarm_hour && snapshot().alarm_minute == state.alarm_minute,
        "Snooze preserves the configured civil alarm time");
    jump(9 * 60 * 1000);
    check(snapshot().alarm_ringing && has_text("Stop"), "Snooze rings again after nine minutes");
    check(orb_time_alarm_set(12, 59), "wide alarm civil-time fixture"); poll(true); advance(300);
    check(has_text("PM"), "ringing alarm uses 12-hour meridiem");
    save("alarm-ringing-wide");
    tap_action("Stop");
    check(!snapshot().alarm_ringing, "explicit Stop acknowledges alarm");
    reset(ORB_TIME_PAGE_ALARM, false);
    check(has_text("Waiting for time"), "unknown wall time explains disabled Alarm");
    save("alarm-waiting-for-time");
    advance(31000);
    check(!orb_kit_motion_timer_running(), "Waiting for time exhausts its 30 s budget");
    quiet("alarm-waiting-for-time");
    tap(233, 350);
    tap_counter_group(0); tap_counter_group(1);
    check(!snapshot().alarm_enabled && !snapshot().clock_valid, "offline Alarm cannot set or invent wall time");
    page_dot(ORB_TIME_PAGE_TIMER);
    tap_action("Start");
    check(snapshot().timer_state == ORB_TIME_RUNNING, "Timer works after offline boot");
    page_dot(ORB_TIME_PAGE_STOPWATCH);
    tap_action("Start");
    check(snapshot().stopwatch_state == ORB_TIME_RUNNING, "Stopwatch works after offline boot");
    reset(ORB_TIME_PAGE_ALARM, false);
    struct tm local = {.tm_year = 126, .tm_mon = 9, .tm_mday = 27, .tm_hour = 10, .tm_min = 9};
    s_harness.wall_start = mktime(&local); s_harness.wall_start_ms = s_harness.now;
    poll(true);
    check(snapshot().clock_valid && !has_text("Waiting for time"), "hub/SNTP wall sample enables Alarm");
    save("alarm-time-arrived");

    /* Design 1.0's Alarm frame shows an armed switch with the hour selected for
     * editing. The first tap picks the minute field so selecting the hour does
     * not also advance its value. */
    reset(ORB_TIME_PAGE_ALARM, true);
    struct tm design_now = {.tm_year = 126, .tm_mon = 9, .tm_mday = 26,
        .tm_hour = 22, .tm_min = 9};
    s_harness.wall_start = mktime(&design_now);
    s_harness.wall_start_ms = s_harness.now;
    check(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_WEEKDAYS, 0),
        "canonical Alarm frame has a real weekday repeat schedule");
    poll(true);
    tap_counter_group(1);
    tap_action("Set");
    check(snapshot().alarm_enabled, "alarm can be armed before touching the hour");
    tap_counter_group(0);
    save("alarm-design");
    check(has_text(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ?
        "Weekdays · On" : "Weekdays · in 8 h 51 min"),
        "Alarm status shows the real repeat and its type-profile estimate/state");
    quiet("alarm-design");
}

static void reminder_screen(void)
{
    reset(ORB_TIME_PAGE_TIMER, true);
    time_t due = wall_now() + 8 * 3600 + 21 * 60;
    uint32_t id = 0;
    check(orb_time_reminder_set("Take the second Orb to the office", due, &id),
        "a fake hub reminder enters the local store");
    check(id > 0, "the reminder store returns an id");
    jump(8 * 3600 * 1000 + 21 * 60 * 1000 + 1000);
    check(snapshot().reminder_due, "the wall clock latches the reminder due");
    poll(true);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_REMINDER,
        "a due reminder presents its own Time page");
    save("reminder-due");
    quiet("reminder-due");
    /* Polish review 17: a due reminder says so in words beside a filled bell. */
    check(has_text(ORB_ICON_BELL_FILL " Due · 6:30 PM"), "due reminder shows Due and its stored civil time");
    check(has_text("Take the second Orb to the office"), "due reminder shows its title");
    tap_action("In 1 hour");
    check(!snapshot().reminder_due && orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_REMINDER,
        "In 1 hour snoozes for 3600 seconds and returns to Reminders");
    jump(3601000);
    check(snapshot().reminder_due, "a snoozed reminder becomes due again");
    poll(true);
    save("reminder-due-again");
    quiet("reminder-due-again");
    tap_action("Done");
    check(!snapshot().reminder_due && orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_REMINDER,
        "Done clears the due reminder and returns to Reminders");
    save("reminder-cleared");
}

static void polish_controls(void)
{
    reset(ORB_TIME_PAGE_TIMER, true);
    tap_action("Quick timers");
    check(has_text("Quick timers"), "idle timer opens its quick presets from the secondary line");
    const char *presets[] = {"1 min", "3 min", "5 min", "10 min", "25 min"};
    for (unsigned i = 0; i < sizeof(presets) / sizeof(*presets); ++i)
        check(find_retained_text(orb_time_view_root(s_harness.view), presets[i]) != NULL,
            "every quick timer preset remains reachable in the short list");
    save("presets-list"); quiet("presets-list");
    tap_retained_action("25 min");
    check(snapshot().timer_state == ORB_TIME_IDLE &&
        snapshot().timer_duration_us == INT64_C(1500000000),
        "quick preset sets duration without starting the timer");
    tap_action("Start");
    check(snapshot().timer_last_seconds == 1500, "starting a preset remembers the actual used duration");
    tap_action("Cancel");
    tap_action("Quick timers");
    save("presets-last-used");
    tap_retained_action("1 min");
    check(snapshot().timer_duration_us == INT64_C(60000000),
        "small quick preset changes the picker without affecting last used");
    check(snapshot().timer_last_seconds == 1500, "editing an unused duration preserves the last-used timer");
    tap_action("Quick timers");
    tap_retained_action("Last used · 25:00");
    check(snapshot().timer_duration_us == INT64_C(1500000000),
        "Last used restores the most recently started timer");

    reset(ORB_TIME_PAGE_ALARM, true);
    lv_obj_t *toggle = find_action("Set");
    lv_area_t toggle_area; lv_obj_get_coords(toggle, &toggle_area);
    int toggle_x = (toggle_area.x1 + toggle_area.x2) / 2, toggle_y = (toggle_area.y1 + toggle_area.y2) / 2;
    press(toggle_x, toggle_y); advance(16); release(); advance(32);
    check(snapshot().alarm_enabled, "first alarm switch touch arms the alarm while its spring is in flight");
    press(toggle_x, toggle_y); advance(16); release(); advance(600);
    check(!snapshot().alarm_enabled, "second touch reverses the alarm toggle without waiting for its spring");
    save("alarm-switch-reversed"); quiet("alarm-switch-reversed");
    tap_action("Options");
    check(has_text("Alarm options"), "alarm secondary line opens repeat and snooze options");
    lv_obj_t *repeat_value = live_text(orb_time_view_root(s_harness.view), "One time");
    orb_kit_value_info_t repeat_before, repeat_after;
    check(repeat_value && orb_kit_value_info(repeat_value, &repeat_before) && repeat_before.tabular,
        "repeat choices reserve a live value lane before the first rotary detent");
    orb_time_rotary_t owner;
    check(orb_time_view_rotary(s_harness.view, &owner), "repeat picker owns rim and twist");
    orb_time_view_rotate(s_harness.view, ORB_TIME_REPEAT_CUSTOM);
    orb_time_view_rotary_finish(s_harness.view); poll(true);
    check(snapshot().alarm_repeat == ORB_TIME_REPEAT_CUSTOM && snapshot().alarm_days != 0,
        "Custom is reachable by rim directly from one-time without an empty invalid schedule");
    check(live_text(orb_time_view_root(s_harness.view), "Custom") == repeat_value &&
        orb_kit_value_info(repeat_value, &repeat_after) &&
        repeat_before.width == repeat_after.width &&
        repeat_before.template_measures == repeat_after.template_measures &&
        repeat_before.raster_prepares == repeat_after.raster_prepares,
        "custom repeat uses the retained reservation without rebuilding value ink");
    orb_time_view_rotate(s_harness.view, ORB_TIME_REPEAT_WEEKDAYS);
    orb_time_view_rotary_finish(s_harness.view); poll(true);
    check(snapshot().alarm_repeat == ORB_TIME_REPEAT_WEEKDAYS,
        "repeat rim applies a real weekday schedule");
    check(live_text(orb_time_view_root(s_harness.view), "Weekdays") == repeat_value &&
        orb_kit_value_info(repeat_value, &repeat_after) &&
        repeat_before.width == repeat_after.width && repeat_before.template_measures == repeat_after.template_measures,
        "weekday repeat retains the same value lane across different real strings");
    save("options-repeat-weekdays");
    tap_retained_action("Snooze");
    check(orb_time_view_rotary(s_harness.view, &owner), "snooze picker owns rim and twist");
    orb_time_view_rotate(s_harness.view, 7);
    orb_time_view_rotary_finish(s_harness.view); poll(true);
    check(snapshot().alarm_snooze_seconds == 420, "snooze rim sets seven minutes instead of a hard-coded duration");
    save("options-snooze-seven");
    tap_retained_action("Time");
    check(snapshot().hour24, "visible clock-format row changes the owner's preference");
    save("options-time-24h");
    tap_retained_action("Time");
    check(!snapshot().hour24, "clock-format preference can be restored without leaving options");
    tap_retained_action("Repeat");
    check(orb_time_view_rotary(s_harness.view, &owner), "repeat focus is restored by its visible row");
    orb_time_view_rotate(s_harness.view, ORB_TIME_REPEAT_CUSTOM);
    orb_time_view_rotary_finish(s_harness.view); poll(true);
    tap_retained_action("Custom days");
    save("custom-days-list"); quiet("custom-days-list");
    uint8_t days = snapshot().alarm_days;
    tap_retained_action("Sunday");
    check(snapshot().alarm_days == (uint8_t)(days ^ 1u),
        "custom weekday row changes the persisted Sunday bit without a hidden default");
    save("custom-days-selected");
    tap_action("Close");
    check(has_text("Alarm options") && !has_text("Done"),
        "alarm options retain one Close dismissal without a duplicate Done");
    tap_action("Close");
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_ALARM,
        "closing alarm settings returns to its civil-time picker");

    orb_time_set_clock_format(true);
    check(orb_time_alarm_set(0, 5), "24-hour midnight fixture"); poll(true);
    check(!has_text("AM") && !has_text("PM"), "24-hour picker has no meridiem suffix");
    counter_is("00:05"); save("alarm-24h-midnight");
    check(orb_time_alarm_set(23, 59), "24-hour late-day fixture"); poll(true);
    counter_is("23:59"); save("alarm-24h-late");
    orb_time_set_clock_format(false); poll(true);
    counter_is("11:59");
    check(has_text("PM"), "12-hour setting restores the localized meridiem");
    save("alarm-12h-late");

    reset(ORB_TIME_PAGE_ALARM, true);
    check(orb_time_alarm_set(10, 10) && orb_time_alarm_enable(true, wall_now()),
        "arm alarm while editing options");
    poll(true); tap_action("Options");
    check(has_text("Alarm options"), "armed alarm settings remain editable before its due boundary");
    s_harness.wall_start = snapshot().alarm_next;
    s_harness.wall_start_ms = s_harness.now;
    jump(1000);
    check(snapshot().alarm_ringing && has_text("Stop") && !has_text("Alarm options"),
        "ringing alarm interrupts an options sheet on the Alarm page");
    save("alarm-preempts-options");
    tap_action("Stop");

    reset(ORB_TIME_PAGE_REMINDER, true);
    check(has_text("No reminders"), "Reminders has an honest empty list");
    save("reminders-empty"); quiet("reminders-empty");
    uint32_t first = 0, second = 0;
    const char *long_title = "Medication review — bring the notes from yesterday's appointment";
    check(orb_time_reminder_set(long_title, wall_now() + 3600, &first) &&
        orb_time_reminder_set("Water the basil", wall_now() + 7200, &second),
        "future reminders populate the same offline list as the voice path");
    poll(true); advance(300);
    check(snapshot().reminder_count == 2 && !snapshot().reminder_due,
        "future reminders remain outstanding without interrupting");
    save("reminders-list"); quiet("reminders-list");
    tap(233, 170);
    check(has_text("In 1 hour") && has_text("Done"), "a reminder row opens explicit snooze and done actions");
    save("reminders-detail-long"); quiet("reminders-detail-long");
    if (orb_time_view_rotary(s_harness.view, &owner)) {
        check(owner.maximum > 0, "long reminder title exposes useful scroll range");
        orb_time_view_rotate(s_harness.view, owner.maximum);
        orb_time_view_rotary_finish(s_harness.view); advance(300);
        check(has_text("In 1 hour") && has_text("Done"),
            "scrolling a long reminder title keeps both actions visible");
        save("reminders-detail-long-scrolled");
    }
    tap_action("Done");
    check(snapshot().reminder_count == 1, "Done removes only the selected reminder");
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_REMINDER,
        "completing a list reminder returns to its list");
    save("reminders-list-after-done");
    orb_time_set_clock_format(true); poll(true);
    tap(233, 170);
    save("reminders-detail-24h");
    tap_action("In 1 hour");
    orb_reminder_t remaining;
    check(orb_time_reminder_get(0, &remaining) && remaining.id == second &&
        remaining.due_time >= wall_now() + 3599 && remaining.due_time <= wall_now() + 3600,
        "list snooze reschedules the selected reminder from the current time");
    save("reminders-list-after-snooze");
    for (unsigned i = 0; i < 6; ++i) {
        char title[64];
        snprintf(title, sizeof(title), "Review %u — notes, errands and a longer real-world title", i + 1);
        check(orb_time_reminder_set(title, wall_now() + (time_t)(i + 2) * 86400, NULL),
            "outstanding reminders fill the bounded list without allocating another snapshot array");
    }
    poll(true); advance(300);
    check(orb_time_view_rotary(s_harness.view, &owner), "long reminder list offers the same rim and twist scroll owner");
    orb_time_view_rotate(s_harness.view, owner.maximum);
    orb_time_view_rotary_finish(s_harness.view); advance(300);
    save("reminders-list-bottom"); quiet("reminders-list-bottom");
}

static void qa_interruptions(void)
{
    bool diagnostic = s_harness.diagnostic;
    unsigned failures = s_harness.failures;
    s_harness.diagnostic = true;
    /* A due alert must retire both an in-flight finger page and its peer.
     * Otherwise the old settle commits after the alert and covers it. */
    reset(ORB_TIME_PAGE_TIMER, true);
    check(orb_time_reminder_set("Interrupt paging", wall_now() + 1, NULL),
        "create reminder at the next wall boundary");
    press(300, 230); press(160, 230);
    jump(1100);
    check(lv_obj_get_child_count(orb_time_view_root(s_harness.view)) == 1,
        "alert immediately retires the old page preview while finger is held");
    release(); advance(900);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_REMINDER && has_text("Interrupt paging"),
        "due reminder retires a held page gesture and stays visible after release");
    save("qa-reminder-interrupts-page"); quiet("qa-reminder-interrupts-page");

    reset(ORB_TIME_PAGE_ALARM, true);
    check(orb_time_alarm_set(10, 10) && orb_time_alarm_enable(true, wall_now()),
        "arm alarm during a page gesture");
    press(300, 230); press(160, 230); release();
    s_harness.wall_start = snapshot().alarm_next;
    s_harness.wall_start_ms = s_harness.now;
    poll(true); advance(900);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_ALARM && has_text("Stop"),
        "ringing alarm retires a settling peer even from the Alarm page");
    save("qa-alarm-interrupts-page"); quiet("qa-alarm-interrupts-page");

    reset(ORB_TIME_PAGE_TIMER, true);
    int64_t duration_before = snapshot().timer_duration_us;
    /* 187px is still content: reaching the edge later cannot promote it. */
    swipe(420, 233, 405, 305);
    check(snapshot().timer_duration_us == duration_before,
        "centre contact in the 179..192px band cannot become a crown gesture");

    reset(ORB_TIME_PAGE_ALARM, true);
    tap_action("Options");
    lv_obj_t *list = orb_time_view_scroll(s_harness.view);
    lv_obj_scroll_to_y(list, 1000, LV_ANIM_OFF); advance(300);
    orb_time_rotary_t owner;
    check(orb_time_view_rotary(s_harness.view, &owner) && owner.detent == 72,
        "options rim scrolls when its selected value has left the viewport");
    orb_time_repeat_t repeat = snapshot().alarm_repeat;
    orb_time_view_rotate(s_harness.view, 0); orb_time_view_rotary_finish(s_harness.view);
    check(snapshot().alarm_repeat == repeat && lv_obj_get_scroll_y(list) == 0,
        "scrolling options never edits a hidden repeat value");

    reset(ORB_TIME_PAGE_REMINDER, true);
    uint32_t selected = 0;
    const char *title = "Read this long reminder carefully and keep the reading position";
    check(orb_time_reminder_set(title, wall_now() + 3600, &selected), "create stable reminder row");
    poll(true); advance(300);
    press(233, 170); advance(16);
    check(orb_time_reminder_set("Earlier insertion", wall_now() + 1800, NULL),
        "insert an earlier reminder during the row contact");
    release(); advance(300);
    orb_time_view_context_t context;
    orb_time_view_save(s_harness.view, &context);
    check(context.reminder_detail && context.reminder_id == selected,
        "a reminder contact opens its stable ID after store reordering");
    check(orb_time_view_back(s_harness.view), "return to reordered reminders");
    tap_retained_action(title);
    check(has_text(ORB_ICON_BELL " Today · 11:09 AM"), "reminder detail retains its relative day as well as time");
    list = orb_time_view_scroll(s_harness.view);
    lv_obj_scroll_to_y(list, 1000, LV_ANIM_OFF); advance(300);
    int offset = lv_obj_get_scroll_y(list);
    /* At default size three title lines fit without scrolling. */
    check(offset > 0 || orb_fonts_content_size() != ORB_CONTENT_SIZE_AX3, "long reminder has a reading position");
    orb_time_view_set_contrast(s_harness.view, true); advance(300);
    check(lv_obj_get_scroll_y(orb_time_view_scroll(s_harness.view)) == offset,
        "accessibility rebuild preserves reminder reading position");
    /* AX3 may break the caption at its separator, never inside the clock. */
    lv_obj_t *when = find_prefix(orb_time_view_root(s_harness.view), ORB_ICON_BELL " Today");
    check(when && lv_color_eq(lv_obj_get_style_text_color(when, 0), ORB_LABEL) &&
        lv_obj_get_style_text_opa(when, 0) == LV_OPA_COVER,
        "Increase Contrast strengthens reminder date/time ink");
    save("qa-reminder-contrast");
    check(orb_time_view_back(s_harness.view), "detail Back returns to the reminder list");
    list = orb_time_view_scroll(s_harness.view);
    s_qa_list_deletions = 0;
    lv_obj_add_event_cb(list, qa_list_deleted, LV_EVENT_DELETE, NULL);
    check(orb_time_timer_set(7) && orb_time_timer_start(now_us()), "start unrelated timer");
    poll(true);
    check(s_qa_list_deletions == 0 && orb_time_view_scroll(s_harness.view) == list,
        "unrelated utility changes preserve the reminder list and active contact");

    reset(ORB_TIME_PAGE_ALARM, true);
    check(orb_time_alarm_set(10, 10) && orb_time_alarm_enable(true, wall_now()), "arm options interruption");
    tap_counter_group(0); tap_action("Options");
    s_harness.wall_start = snapshot().alarm_next; s_harness.wall_start_ms = s_harness.now;
    poll(true); tap_action("Stop");
    orb_time_view_save(s_harness.view, &context);
    check(context.field == 0, "an alarm interrupt preserves its selected digit field instead of an editor field");

    reset(ORB_TIME_PAGE_ALARM, true); tap_action("Options");
    unsigned anchors = qa_transparent_labels(orb_time_view_root(s_harness.view));
    check(anchors >= 3, "options keep transparent reservation anchors behind live values");
    orb_time_view_set_contrast(s_harness.view, true); advance(300);
    check(qa_transparent_labels(orb_time_view_root(s_harness.view)) == anchors,
        "Increase Contrast never paints reserved live-value anchors");
    check(orb_time_view_rotary(s_harness.view, &owner), "contrast options retain their visible rotary owner");
    orb_time_view_rotate(s_harness.view, ORB_TIME_REPEAT_WEEKDAYS);
    orb_time_view_rotary_finish(s_harness.view); advance(300);
    check(qa_transparent_labels(orb_time_view_root(s_harness.view)) == anchors,
        "value updates preserve hidden source anchors under Increase Contrast");
    save("options-qa-contrast"); frame_equal("options-qa-contrast");

    for (unsigned snooze = 0; snooze < 2; ++snooze) {
        reset(ORB_TIME_PAGE_TIMER, true);
        check(orb_time_timer_set(1) && orb_time_timer_start(now_us()) &&
            orb_time_reminder_set("Coincident deadline", wall_now() + 1, NULL), "arm timer and reminder overlap");
        jump(1100); advance(300);
        tap_action(snooze ? "In 1 hour" : "Done");
        check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER && has_text("Stop"),
            "acknowledging the final due reminder reveals a pending finished timer");
    }
    s_harness.diagnostic = diagnostic;
    check(s_harness.failures == failures, "Time QA regressions all pass");
}

static void interactions(void)
{
    reset(ORB_TIME_PAGE_TIMER, true);
    check(orb_time_timer_set(3599), "set rotary hour-reveal fixture"); poll(true);
    counter_is("59:59");
    press(233, 21); advance(32);
    int64_t duration_before = snapshot().timer_duration_us;
    check(snapshot().timer_duration_us == duration_before, "rim touch preserves the last sub-hour value");
    press(288, 28); advance(32);
    release(); advance(300);
    check(snapshot().timer_duration_us >= INT64_C(3600000000),
        "clockwise minute rim carries through 59:59 and reveals hours");
    save("timer-rim-hours");
    check(orb_time_timer_set(3600), "set reversible hour-drag fixture"); poll(true);
    tap_counter_group(0);
    press(233, 21); press(178, 28); advance(32);
    check(snapshot().timer_duration_us < INT64_C(3600000000),
        "descending hour turn hides the hour group below one hour");
    press(288, 28); advance(32);
    check(snapshot().timer_duration_us >= INT64_C(3600000000),
        "reversing the same gesture preserves its original hour unit");
    release(); advance(300); save("timer-rim-reverse");
    check(orb_time_timer_set(3600), "restore descending hour fixture"); poll(true);
    press(233, 21); press(178, 28); advance(32); release(); advance(300);
    check(snapshot().timer_duration_us < INT64_C(3600000000),
        "released descending hour turn leaves a sub-hour duration");
    duration_before = snapshot().timer_duration_us;
    tap_counter_group(0);
    check(snapshot().timer_duration_us == duration_before + INT64_C(60000000),
        "after hour group disappears, the visible minute group owns selection");
    check(orb_time_timer_set(3600), "set full-range rotary fixture"); poll(true);
    tap_counter_group(0);
    sequence("rim", 0);
    duration_before = snapshot().timer_duration_us;
    press(233, 21); advance(32);
    check(snapshot().timer_duration_us == duration_before, "rim touch does not jump the selected value");
    release();
    const float angles[] = {145, 190, 235, 280, 325, 370, 415, 460, 505};
    for (unsigned i = 0; i < sizeof(angles) / sizeof(*angles); ++i) {
        float angle = angles[i] * (float)M_PI / 180;
        press((int)lroundf(233 + 212 * cosf(angle)), (int)lroundf(233 + 212 * sinf(angle)));
        advance(16);
        sequence("rim", i + 1);
    }
    release(); advance(500); sequence("rim", 10);
    check(snapshot().timer_duration_us == INT64_C(86400000000), "real bezel drag reaches 24-hour endpoint");
    check(orb_time_timer_set(90), "set action-sequence duration"); poll(true);
    unsigned action_frame = 0;
    sequence("start", action_frame++);
    tap_sequence("Start", "start", &action_frame);
    jump(3000); sequence("start", action_frame++);
    action_frame = 0;
    sequence("pause", action_frame++);
    tap_sequence("Pause", "pause", &action_frame);
    tap_action("Cancel");
    check(orb_time_timer_set(4), "set ring orientation fixture"); poll(true);
    orb_time_timer_start(now_us()); jump(3000);
    check(!orb_kit_motion_timer_running(), "a delayed first running sample steps elapsed seconds without Follow");
    save("timer-ring-quarter");
    check(teal_at(233, 21) && teal_at(383, 83), "remaining ring starts at 12 and proceeds clockwise");
    check(!teal_at(83, 83) && !teal_at(233, 445), "expired ring shows gray track past remaining quarter");
    action_frame = 0;
    sequence("finish", action_frame++);
    for (unsigned i = 0; i < 5; ++i) {
        jump(250); sequence("finish", action_frame++);
    }
    check(snapshot().timer_state == ORB_TIME_FINISHED, "finish motion ends in persistent receipt");
    advance(300); sequence("finish", action_frame++);
    tap_action("Stop");
    unsigned mode_frame = 0;
    page_dot(ORB_TIME_PAGE_TIMER);
    press(300, 170); press(250, 170); advance(16);
    sequence("page-cancel", 0);
    release(); advance(600); sequence("page-cancel", 1);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER,
        "short horizontal drag cancels back to the same mode");
    press(320, 170); press(220, 170); advance(16);
    release(); advance(32); sequence("page-regrab", 0);
    press(240, 170); press(360, 170); advance(16);
    sequence("page-regrab", 1);
    release(); advance(700); sequence("page-regrab", 2);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_TIMER,
        "regrabbing a settling page can reverse and cancel navigation");
    quiet("page-cancel-regrab");
    for (unsigned i = 0; i < 2; ++i) {
        sequence("mode", mode_frame++);
        press(320, 170);
        for (unsigned j = 1; j <= 6; ++j) {
            press(320 - (int)j * 30, 170);
            advance(16); sequence("mode", mode_frame++);
        }
        release(); advance(600); sequence("mode", mode_frame++);
        check(orb_time_view_page(s_harness.view) == (orb_time_page_t)(i + 1),
            "centre horizontal swipe changes peer Time page");
    }
    swipe(140, 170, 320, 170);
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_STOPWATCH,
        "opposite horizontal swipe returns to previous page");
    page_dot(ORB_TIME_PAGE_TIMER);
    reset(ORB_TIME_PAGE_STOPWATCH, true);
    tap_action("Start");
    jump(23100);
    action_frame = 0;
    sequence("lap", action_frame++);
    tap_sequence("Lap", "lap", &action_frame);
    check(snapshot().lap_count == 1, "lap motion records one split");
    for (unsigned i = 0; i < 7; ++i) {
        jump(23100 + i * 600);
        tap_action("Lap");
    }
    tap_action("Pause");
    lv_obj_t *list = scroll_list(orb_time_view_root(s_harness.view));
    check(list != NULL, "Stopwatch contains an integrated scrolling viewport");
    int before_scroll = lv_obj_get_scroll_y(list);
    sequence("laps-scroll", 0);
    press(233, 300);
    for (unsigned i = 0; i < 10; ++i) {
        press(233, 300 - (int)i * 19);
        advance(16);
        sequence("laps-scroll", i + 1);
    }
    release(); advance(1200);
    check(lv_obj_get_scroll_y(list) > before_scroll + 40,
        "real finger drag reveals older stopwatch laps below summary");
    sequence("laps-scroll", 11);
    save("stopwatch-laps-scroll"); quiet("laps-after-scroll");
    check(orb_time_view_page(s_harness.view) == ORB_TIME_PAGE_STOPWATCH,
        "laps remain in the Stopwatch mode");
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); advance(100);
    tap_action("Laps"); advance(600);
    check(lv_obj_get_scroll_y(list) > before_scroll + 40,
        "visible Laps action reveals same integrated list");
    save("stopwatch-laps-button"); quiet("laps-after-button");
    lv_obj_scroll_to_y(list, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list), LV_ANIM_OFF);
    advance(100);
    return_stopwatch_summary();
    check(lv_obj_get_scroll_y(list) <= 1, "local stopwatch rim returns to summary");
    tap_action("Resume");
    tap_action("Laps"); advance(600);
    jump(2000); save("stopwatch-laps-live");
    verify_laps_header();
    return_stopwatch_summary();
    tap_action("Pause");
    /* The rim scrolls the same list, without changing the selected mode. */
    before_scroll = lv_obj_get_scroll_y(list);
    const float lap_angles[] = {345, 365, 385, 405, 425, 445};
    for (unsigned i = 0; i < sizeof(lap_angles) / sizeof(*lap_angles); ++i) {
        float angle = lap_angles[i] * (float)M_PI / 180;
        press((int)lroundf(233 + 212 * cosf(angle)), (int)lroundf(233 + 212 * sinf(angle)));
        advance(16);
    }
    release(); advance(600);
    check(lv_obj_get_scroll_y(list) > before_scroll + 40, "rim drag reveals stopwatch laps");
    save("stopwatch-laps-rim"); quiet("laps-after-rim");

    /* The visible UI may disappear while main's service keeps elapsed time.
     * Reopening must observe the original deadline, without an app-owned timer. */
    reset(ORB_TIME_PAGE_TIMER, true);
    check(orb_time_timer_set(8), "set background timer");
    tap_action("Start");
    orb_time_view_destroy(s_harness.view); s_harness.view = NULL;
    jump(9000);
    check(snapshot().timer_state == ORB_TIME_FINISHED, "timer finishes with no Time view");
    s_harness.view = orb_time_view_create(lv_screen_active(), NULL);
    check(s_harness.view != NULL, "reopen Time after background finish");
    poll(true);
    check(has_text("Timer finished"), "reopened app presents persistent timer finish");
    save("timer-background-finished");
    quiet("background-finished");
}

static void alarm_memory(void)
{
    /* emu_memory.c supplies host LVGL accounting. Shared Inter assets and the
     * glass mask/material owners are outside this tree measurement. */
    orb_time_init();
    orb_time_set_clock_format(false);
    s_harness.wall_start = (time_t)1704092340; /* 06:59 UTC. */
    s_harness.wall_start_ms = s_harness.now;
    orb_time_poll(now_us(), wall_now());
    check(orb_time_alarm_set(7, 0) && orb_time_alarm_enable(true, wall_now()), "arm measured alarm");
    jump(60000);
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    s_harness.view = orb_time_view_create(lv_screen_active(), NULL);
    check(s_harness.view != NULL, "create measured alarm modal");
    poll(true); advance(500);
    lv_mem_monitor(&after);
    printf("AlarmRinging LVGL tree: %zu bytes; state: %zu bytes; no digit raster\n",
        after.total_size - before.total_size, orb_time_view_state_size());
    save("AlarmRinging");
}

static void render_suite(void)
{
    /* Each case starts from a known model, so a failed layout cannot prevent
     * the remaining places/states from producing review evidence. */
    reset(ORB_TIME_PAGE_TIMER, true);
    save("timer-idle");
    tap_action("Quick timers"); save("presets-list");
    orb_time_view_back(s_harness.view); poll(true); advance(300);
    orb_time_timer_start(now_us()); jump(1000); save("timer-running");
    orb_time_timer_pause(now_us()); poll(true); advance(300); save("timer-paused");
    orb_time_timer_reset(); orb_time_timer_set(1); orb_time_timer_start(now_us());
    jump(1100); advance(300); save("timer-finished");
    orb_time_timer_reset(); orb_time_timer_set(ORB_TIME_TIMER_MAX_SECONDS);
    poll(true); advance(300); save("timer-longduration");

    reset(ORB_TIME_PAGE_STOPWATCH, true);
    save("stopwatch-idle");
    orb_time_stopwatch_start(now_us()); jump(82400); save("stopwatch-running");
    for (unsigned i = 0; i < 16; ++i) {
        jump(8300 + i * 2150); orb_time_stopwatch_lap(now_us());
    }
    orb_time_stopwatch_pause(now_us()); poll(true); advance(300); save("stopwatch-paused");
    tap_action("Laps"); advance(600); save("stopwatch-laps");
    orb_time_rotary_t owner;
    if (orb_time_view_rotary(s_harness.view, &owner)) {
        orb_time_view_rotate(s_harness.view, owner.maximum);
        orb_time_view_rotary_finish(s_harness.view); advance(300); save("stopwatch-laps-long");
    }

    reset(ORB_TIME_PAGE_ALARM, true);
    save("alarm-idle");
    orb_time_alarm_enable(true, wall_now()); poll(true); advance(300); save("alarm-design");
    s_harness.wall_start = snapshot().alarm_next; s_harness.wall_start_ms = s_harness.now;
    jump(1000); advance(300); save("alarm-ringing");
    check(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_CUSTOM, ORB_TIME_DAYS_ALL & ~(1u << 6)),
        "atlas long custom schedule is valid");
    poll(true); advance(300); save("alarm-ringing-custom");
    check(has_text("Custom days"), "ringing custom schedule stays glanceable above Snooze");
    check(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_ONCE, 0), "atlas restores the one-time schedule");
    orb_time_alarm_set(12, 59); poll(true); advance(300); save("alarm-ringing-wide");
    reset(ORB_TIME_PAGE_ALARM, false); save("alarm-waiting-for-time");
    reset(ORB_TIME_PAGE_ALARM, true);
    tap_action("Options"); save("options-repeat-weekdays");
    check(orb_time_view_rotary(s_harness.view, &owner), "atlas repeat picker admits its local rim field");
    orb_time_view_rotate(s_harness.view, ORB_TIME_REPEAT_WEEKDAYS);
    orb_time_view_rotary_finish(s_harness.view); poll(true); advance(300);
    check(snapshot().alarm_repeat == ORB_TIME_REPEAT_WEEKDAYS, "atlas weekday fixture applies its named state");
    save("options-repeat-weekdays");
    tap_retained_action("Snooze");
    check(orb_time_view_rotary(s_harness.view, &owner), "atlas snooze picker admits its local rim field");
    orb_time_view_rotate(s_harness.view, 7);
    orb_time_view_rotary_finish(s_harness.view); poll(true); advance(300);
    check(snapshot().alarm_snooze_seconds == 420, "atlas seven-minute fixture applies its named state");
    save("options-snooze-seven");
    tap_retained_action("Custom days"); save("custom-days-list");
    tap_retained_action("Sunday"); save("custom-days-selected");

    reset(ORB_TIME_PAGE_REMINDER, true); save("reminders-empty");
    const char *long_title = "Medication review — bring the notes from yesterday's appointment";
    orb_time_reminder_set(long_title, wall_now() + 3600, NULL);
    orb_time_reminder_set("Water the basil", wall_now() + 7200, NULL);
    poll(true); advance(300); save("reminders-list");
    orb_reminder_t first;
    check(orb_time_reminder_get(0, &first), "read bounded long reminder title for the capture");
    tap_retained_action(first.title); save("reminders-detail-long");
    if (orb_time_view_rotary(s_harness.view, &owner)) {
        orb_time_view_rotate(s_harness.view, owner.maximum);
        orb_time_view_rotary_finish(s_harness.view); advance(300); save("reminders-detail-long-scrolled");
    }
    reset(ORB_TIME_PAGE_REMINDER, true);
    orb_time_reminder_set("Take the second Orb to the office", wall_now() + 1, NULL);
    jump(1100); advance(300); save("reminder-due");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: orb_time_sim OUTDIR [ax3 bold rt rm capture]\n"); return 2; }
    s_harness.directory = argv[1];
    if (mkdir(argv[1], 0755) && errno != EEXIST) { perror(argv[1]); return 2; }
    check(setenv("TZ", "UTC", 1) == 0, "set isolated deterministic timezone"); tzset();
    lv_init(); lv_tick_set_cb(tick);
    s_harness.display = sim_display_create();
    check(s_harness.display != NULL, "create round fake display");
    orb_ui_init(NULL);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    check(orb_model_init(), "initialize isolated preference model");
    bool qa_only = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "default")) continue;
        if (!strcmp(argv[i], "ax3")) check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "set AX3");
        else if (!strcmp(argv[i], "bold")) check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "set Bold Text");
        else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
        else if (!strcmp(argv[i], "capture")) s_harness.diagnostic = true;
        else if (!strcmp(argv[i], "qa")) qa_only = true;
        else { fprintf(stderr, "unknown flag: %s\n", argv[i]); return 2; }
    }
    s_harness.pointer = lv_indev_create();
    lv_indev_set_type(s_harness.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_harness.pointer, s_harness.display);
    lv_indev_set_read_cb(s_harness.pointer, pointer_read);
    lv_display_add_event_cb(s_harness.display, display_event, LV_EVENT_ALL, NULL);
    printf("Time view state: %zu bytes (LVGL children/materials excluded)\n", orb_time_view_state_size());
    if (qa_only) qa_interruptions();
    else if (s_harness.diagnostic) { alarm_memory(); render_suite(); }
    else {
        alarm_memory();
        fixtures();
        reminder_screen();
        polish_controls();
        qa_interruptions();
        interactions();
    }
    if (s_harness.diagnostic) printf("Time diagnostic captures: %u checks evaluated, %u failed; screenshots=%s\n",
        s_harness.checks, s_harness.failures, s_harness.directory);
    else printf("Time view: %u checks passed; screenshots=%s\n", s_harness.checks, s_harness.directory);
    orb_time_view_destroy(s_harness.view);
    lv_indev_delete(s_harness.pointer);
    lv_deinit();
    return 0;
}
