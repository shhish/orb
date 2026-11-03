/* Design 2.0 Time layout contract over production views and fake clocks.
 * Insets have a separate gate in every type profile so a ring/title regression
 * cannot be hidden by an unrelated fixture's failure. */
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_time.h"
#include "orb_time_view.h"
#include "sim_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LABELS 160

static uint32_t s_now = 1000;
static time_t s_wall = 1793182140; /* 2026-10-28, stable UTC fixture. */
static lv_display_t *s_display;
static orb_time_view_t *s_view;
static unsigned s_checks, s_failures;
static const char *s_scene;
static bool s_inset;

typedef struct {
    lv_obj_t *obj;
    lv_area_t ink;
} label_t;

typedef struct {
    unsigned titles, primary, glass, rings, labels;
    label_t label[MAX_LABELS];
} layout_t;

static uint32_t tick(void) { return s_now; }

static void expect(bool valid, const char *why)
{
    ++s_checks;
    if (valid) return;
    ++s_failures;
    fprintf(stderr, "Time Design2 [%s]: %s\n", s_scene, why);
}

static void settle(void)
{
    for (unsigned i = 0; i < 40; ++i) {
        s_now += 16;
        orb_time_poll((int64_t)s_now * 1000, s_wall);
        orb_time_view_poll(s_view);
        lv_timer_handler();
    }
    lv_obj_update_layout(orb_time_view_root(s_view));
    lv_refr_now(s_display);
}

static void view(orb_time_page_t page, bool clock)
{
    if (s_view) orb_time_view_destroy(s_view);
    orb_time_init();
    orb_time_set_clock_format(false);
    orb_time_poll((int64_t)s_now * 1000, clock ? s_wall : 0);
    s_view = orb_time_view_create(lv_screen_active(), NULL);
    expect(s_view != NULL, "real Time view is created");
    orb_time_view_select(s_view, page);
    /* Keep the wall-clock unavailable for the offline fixture. */
    if (clock) settle();
    else {
        orb_time_view_poll(s_view);
        lv_obj_update_layout(orb_time_view_root(s_view));
        lv_refr_now(s_display);
    }
}

static bool intersect(lv_area_t *a, const lv_area_t *b)
{
    if (a->x1 < b->x1) a->x1 = b->x1;
    if (a->y1 < b->y1) a->y1 = b->y1;
    if (a->x2 > b->x2) a->x2 = b->x2;
    if (a->y2 > b->y2) a->y2 = b->y2;
    return a->x1 <= a->x2 && a->y1 <= a->y2;
}

static uint32_t codepoint(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t cp = *p++;
    unsigned extra = cp < 0x80 ? 0 : cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
    if (extra) cp &= (1u << (6 - extra)) - 1;
    while (extra-- && (*p & 0xc0) == 0x80) cp = cp << 6 | (*p++ & 0x3f);
    *text = (const char *)p;
    return cp;
}

static bool label_ink(lv_obj_t *label, lv_area_t *ink)
{
    const char *value = lv_label_get_text(label);
    if (!*value || !lv_obj_get_style_text_opa(label, 0) || !lv_obj_get_style_opa(label, 0)) return false;
    lv_area_t box;
    lv_obj_get_coords(label, &box);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_point_t size;
    lv_text_get_size(&size, value, font, lv_obj_get_style_text_letter_space(label, 0),
        lv_obj_get_style_text_line_space(label, 0), lv_obj_get_width(label), LV_TEXT_FLAG_NONE);
    if (lv_label_get_long_mode(label) != LV_LABEL_LONG_DOT && size.y > lv_obj_get_height(label)) {
        fprintf(stderr, "  clipped label: %s (%d > %d)\n", value, size.y, lv_obj_get_height(label));
        expect(false, "label contains all wrapped lines");
    }
    /* Glyph ink, not advances or line boxes: the font reserves descender
     * space and a colon's advance can share a boundary pixel with the next
     * tabular field even though their painted ink remains well separated. */
    bool any = false;
    unsigned index = 0;
    const char *cursor = value;
    while (*cursor) {
        uint32_t cp = codepoint(&cursor);
        const char *peek = cursor;
        uint32_t next = *peek ? codepoint(&peek) : 0;
        lv_font_glyph_dsc_t glyph;
        lv_point_t pos;
        lv_label_get_letter_pos(label, index++, &pos);
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_w || !glyph.box_h) continue;
        lv_area_t rect = {
            box.x1 + pos.x + glyph.ofs_x,
            box.y1 + pos.y + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y,
            box.x1 + pos.x + glyph.ofs_x + glyph.box_w - 1,
            box.y1 + pos.y + font->line_height - font->base_line - glyph.ofs_y - 1,
        };
        if (!intersect(&rect, &box)) continue;
        if (!any) { *ink = rect; any = true; }
        else {
            ink->x1 = LV_MIN(ink->x1, rect.x1); ink->y1 = LV_MIN(ink->y1, rect.y1);
            ink->x2 = LV_MAX(ink->x2, rect.x2); ink->y2 = LV_MAX(ink->y2, rect.y2);
        }
    }
    if (!any) return false;
    lv_obj_get_transformed_area(label, ink, LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE);
    /* Round-list caches retain logical layout; map its child to the panel's
     * actual chord presentation before checking neighbouring text. */
    for (lv_obj_t *p = lv_obj_get_parent(label); p; p = lv_obj_get_parent(p)) {
        lv_area_t mapped;
        if (!orb_round_list_child_bounds(p, label, &mapped)) continue;
        int logical_w = LV_MAX(1, lv_area_get_width(&box));
        int logical_h = LV_MAX(1, lv_area_get_height(&box));
        int x1 = mapped.x1 + (ink->x1 - box.x1) * lv_area_get_width(&mapped) / logical_w;
        int y1 = mapped.y1 + (ink->y1 - box.y1) * lv_area_get_height(&mapped) / logical_h;
        int x2 = mapped.x1 + (ink->x2 - box.x1 + 1) * lv_area_get_width(&mapped) / logical_w - 1;
        int y2 = mapped.y1 + (ink->y2 - box.y1 + 1) * lv_area_get_height(&mapped) / logical_h - 1;
        *ink = (lv_area_t){x1, y1, x2, y2};
        break;
    }
    for (lv_obj_t *p = lv_obj_get_parent(label); p; p = lv_obj_get_parent(p)) {
        lv_area_t clip;
        lv_obj_get_coords(p, &clip);
        if (!intersect(ink, &clip)) return false;
    }
    return true;
}

static void inspect(lv_obj_t *obj, layout_t *layout, bool top_title)
{
    if (!lv_obj_is_visible(obj) || !lv_obj_get_style_opa(obj, 0)) return;
    orb_kit_rim_text_info_t rim;
    if (orb_kit_rim_text_info(obj, &rim) && lv_obj_get_y(obj) < 233) {
        ++layout->titles;
        top_title = true;
        expect(rim.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + .01f,
            "curved place title fits its 120 degree budget");
    }
    if (lv_obj_check_type(obj, &lv_arclabel_class)) {
        if (top_title) expect(lv_color_eq(lv_obj_get_style_text_color(obj, 0), ORB_KIT_TIME),
            "curved Time title uses the shared accent token");
        if (s_inset && top_title) expect(lv_arclabel_get_radius(obj) == 176,
            "full-ring page title requires a kit inset baseline of 176px");
    }
    if (lv_obj_check_type(obj, &lv_arc_class)) {
        ++layout->rings;
        int width = lv_obj_get_style_arc_width(obj, LV_PART_MAIN);
        expect(width == 4 || (width >= 10 && width <= 14), "ring/scroll stroke comes from a system role");
    }
    if (orb_glass_get_amount(obj) > 0) {
        ++layout->glass;
        int h = lv_obj_get_height(obj), w = lv_obj_get_width(obj);
        expect((w == h && (h == ORB_KIT_DISC || h == ORB_KIT_DISC_MINI)) ||
            h == ORB_KIT_PRIMARY_H || h == ORB_KIT_SECONDARY_H,
            "glass action uses a canonical kit disc or capsule size");
        bool primary = orb_kit_is_primary(obj);
        if (primary) {
            ++layout->primary;
            lv_area_t bounds; lv_obj_get_coords(obj, &bounds);
            expect((bounds.y1 + bounds.y2) / 2 >= 300,
                "the single tinted primary sits on the bottom arc");
        }
        if (w == h && (h == ORB_KIT_DISC || h == ORB_KIT_DISC_MINI)) {
            int before = lv_obj_get_width(obj);
            orb_kit_disc_set_focus(obj, true);
            expect(lv_obj_get_width(obj) == before && lv_obj_get_height(obj) == before,
                "focus keeps the canonical disc diameter");
            expect(lv_obj_get_style_outline_width(obj, 0) == 3 &&
                lv_obj_get_style_outline_pad(obj, 0) == 4,
                "focus is the shared 3px ring at a 4px offset");
            orb_kit_disc_set_focus(obj, false);
        }
    }
    if (lv_obj_get_width(obj) == ORB_KIT_DISC_ROW && lv_obj_get_height(obj) == ORB_KIT_DISC_ROW) {
        bool focused = lv_obj_get_style_outline_width(obj, 0) == ORB_KIT_FOCUS_WIDTH;
        orb_kit_disc_set_focus(obj, true);
        expect(lv_obj_get_width(obj) == ORB_KIT_DISC_ROW &&
            lv_obj_get_height(obj) == ORB_KIT_DISC_ROW &&
            lv_obj_get_style_transform_scale_x(obj, 0) == 256 &&
            lv_obj_get_style_transform_scale_y(obj, 0) == 256,
            "focused row leading discs retain their40px unscaled role");
        expect(lv_obj_get_style_outline_width(obj, 0) == ORB_KIT_FOCUS_WIDTH &&
            lv_obj_get_style_outline_pad(obj, 0) == ORB_KIT_FOCUS_OFFSET,
            "row focus uses the complete shared outline outside the content disc");
        orb_kit_disc_set_focus(obj, focused);
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t ink;
        if (label_ink(obj, &ink)) {
            expect(layout->labels < MAX_LABELS, "label inventory is bounded");
            if (layout->labels < MAX_LABELS) layout->label[layout->labels++] = (label_t){obj, ink};
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        inspect(lv_obj_get_child(obj, (int32_t)i), layout, top_title);
}

static void cancelled_actions(lv_obj_t *obj)
{
    if (!lv_obj_is_visible(obj)) return;
    if (lv_obj_is_clickable(obj) && !lv_obj_is_scrollable(obj) &&
        lv_obj_get_width(obj) > 0 && lv_obj_get_width(obj) < 400 &&
        lv_obj_get_height(obj) > 0 && lv_obj_get_height(obj) < 400) {
        orb_time_snapshot_t before, after;
        orb_time_snapshot((int64_t)s_now * 1000, &before);
        orb_time_page_t page = orb_time_view_page(s_view);
        lv_obj_send_event(obj, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(obj, LV_EVENT_PRESS_LOST, NULL);
        bool retired = orb_kit_press_is_cancelled(obj);
        expect(retired, "each tappable uses the kit cancelled-contact response");
        /* A missing press handler can delete this subtree on CLICKED. Report
         * it without deliberately dereferencing a deleted object afterwards. */
        if (!retired) return;
        lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
        orb_time_snapshot((int64_t)s_now * 1000, &after);
        expect(before.generation == after.generation && orb_time_view_page(s_view) == page,
            "a cancelled scroll contact cannot activate a Time action");
        /* Leave the control ready for future contacts without an active press. */
        lv_obj_send_event(obj, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(obj, LV_EVENT_RELEASED, NULL);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        cancelled_actions(lv_obj_get_child(obj, (int32_t)i));
}

static void scene(const char *name)
{
    s_scene = name;
    settle();
    layout_t layout = {0};
    inspect(orb_time_view_root(s_view), &layout, false);
    expect(layout.titles == 1, "each Time place has one kit curved teal title");
    expect(layout.primary <= 1, "screen has at most one tinted primary action");
    expect(layout.glass <= 3, "glass action count stays within the design gate");
    cancelled_actions(orb_time_view_root(s_view));
    for (unsigned i = 0; i < layout.labels; ++i) {
        bool digit_source = false;
        for (lv_obj_t *p = lv_obj_get_parent(layout.label[i].obj); p; p = lv_obj_get_parent(p))
            digit_source |= lv_obj_get_raster(p) && lv_obj_get_width(p) == 466 && lv_obj_get_height(p) == 124;
        /* The number row's raster scale is intentionally opaque in LVGL's
         * public API; the full Time harness checks its actual panel pixels.
         * All other real-string ink checks use the presented corner geometry. */
        if (!digit_source) {
            const lv_area_t *ink = &layout.label[i].ink;
            int xs[] = {ink->x1 - 233, ink->x2 - 233};
            int ys[] = {ink->y1 - 233, ink->y2 - 233};
            bool fits = true;
            for (unsigned x = 0; x < 2; ++x) for (unsigned y = 0; y < 2; ++y)
                fits &= xs[x] * xs[x] + ys[y] * ys[y] <= (233 - 16) * (233 - 16);
            if (!fits) fprintf(stderr, "  chord overflow: %s (%d,%d)-(%d,%d)\n",
                lv_label_get_text(layout.label[i].obj), ink->x1, ink->y1, ink->x2, ink->y2);
            expect(fits, "real-string ink stays inside its circular text chord");
        }
        for (unsigned j = i + 1; j < layout.labels; ++j) {
            lv_area_t overlap = layout.label[i].ink;
            if (intersect(&overlap, &layout.label[j].ink)) {
                fprintf(stderr, "  overlap: %s / %s (%d,%d)-(%d,%d)\n",
                    lv_label_get_text(layout.label[i].obj), lv_label_get_text(layout.label[j].obj),
                    overlap.x1, overlap.y1, overlap.x2, overlap.y2);
                expect(false, "visible text ink boxes do not overlap");
            }
        }
    }
}

static lv_obj_t *named_action(lv_obj_t *obj, const char *name)
{
    if (lv_obj_is_hidden(obj)) return NULL;
    /* Kit live lanes retain a transparent template in their source row. It
     * must never compete with the actual Custom days navigation action. */
    if (lv_obj_check_type(obj, &lv_label_class) && !lv_obj_get_style_text_opa(obj, 0)) return NULL;
    const char *shown = lv_obj_check_type(obj, &lv_label_class) ? lv_label_get_text(obj) : "";
    const char *dots = strstr(shown, "...");
    if (!dots) dots = strstr(shown, "\xe2\x80\xa6");
    /* The new bounded row lanes expose fitted text. Discovery follows its
     * preserved prefix; the complete reminder is verified in the detail. */
    if (*shown && (!strcmp(shown, name) ||
        (dots && dots > shown && !strncmp(shown, name, (size_t)(dots - shown))))) {
        for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p))
            if (lv_obj_is_clickable(p) && !lv_obj_is_scrollable(p) && lv_obj_get_height(p) < 466) return p;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = named_action(lv_obj_get_child(obj, (int32_t)i), name);
        if (found) return found;
    }
    return NULL;
}

static void click_named(const char *name)
{
    lv_obj_t *obj = named_action(orb_time_view_root(s_view), name);
    expect(obj != NULL, "named production kit action is reachable");
    if (!obj) return;
    lv_obj_send_event(obj, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(obj, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    settle();
}

static lv_obj_t *seconds_field(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), "00")) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        lv_area_t area; lv_obj_get_coords(obj, &area);
        if (lv_obj_is_clickable(parent) && area.x1 + area.x2 > 466) return parent;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = seconds_field(lv_obj_get_child(obj, (int32_t)i));
        if (found) return found;
    }
    return NULL;
}

static void context_round_trip(const char *name)
{
    s_scene = name;
    orb_time_snapshot_t before, after;
    orb_time_view_context_t saved, restored;
    orb_time_snapshot((int64_t)s_now * 1000, &before);
    orb_time_view_save(s_view, &saved);
    orb_time_view_destroy(s_view);
    s_view = orb_time_view_create(lv_screen_active(), NULL);
    expect(s_view != NULL, "navigation lifecycle recreates the production Time view");
    orb_time_view_restore(s_view, &saved);
    settle();
    orb_time_view_save(s_view, &restored);
    expect(orb_time_view_page(s_view) == saved.page && restored.page == saved.page &&
        restored.field == saved.field && restored.sheet == saved.sheet,
        "destroy/create/restore preserves active page, field and sheet");
    expect(!memcmp(saved.fields, restored.fields, sizeof(saved.fields)) &&
        !memcmp(saved.scroll, restored.scroll, sizeof(saved.scroll)) &&
        !memcmp(saved.sheet_scroll, restored.sheet_scroll, sizeof(saved.sheet_scroll)),
        "navigation lifecycle preserves every page's focus and scroll memory");
    expect(saved.reminder_detail == restored.reminder_detail &&
        saved.reminder_id == restored.reminder_id && saved.detail_scroll == restored.detail_scroll,
        "navigation lifecycle preserves selected reminder and detail scroll");
    orb_time_snapshot((int64_t)s_now * 1000, &after);
    expect(after.generation == before.generation,
        "context save/destroy/create/restore never mutates service generation");
}

static void back_restoration(void)
{
    s_scene = "Back-restores-timer-focus";
    view(ORB_TIME_PAGE_TIMER, true);
    expect(orb_time_timer_set(300), "timer seconds-focus fixture is valid");
    orb_time_view_poll(s_view);
    lv_obj_t *field = seconds_field(orb_time_view_root(s_view));
    expect(field != NULL, "seconds field has a visible tappable alternative");
    if (field) {
        lv_obj_send_event(field, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(field, LV_EVENT_RELEASED, NULL);
        lv_obj_send_event(field, LV_EVENT_CLICKED, NULL);
    }
    orb_time_rotary_t before, after;
    expect(orb_time_view_rotary(s_view, &before), "selected seconds field owns the rim");
    click_named("Quick timers");
    scene("quick-timers");
    expect(orb_time_view_back(s_view), "BOOT Back closes the quick timer sheet");
    settle();
    expect(orb_time_view_page(s_view) == ORB_TIME_PAGE_TIMER &&
        orb_time_view_rotary(s_view, &after), "Back restores Timer and its visible rotary owner");
    expect(before.units_per_turn == after.units_per_turn && before.value == after.value &&
        before.maximum == after.maximum, "Back restores the exact selected field and value");
    context_round_trip("context-timer-seconds-focus");

    s_scene = "Back-restores-reminder-scroll";
    view(ORB_TIME_PAGE_REMINDER, true);
    static const char *titles[] = {"Morning medication", "Water the basil", "Call the dentist",
        "Review yesterday's appointment notes", "Take the recycling out", "Check the oven",
        "Bring the parcel downstairs", "Medication review: bring yesterday's appointment note and water"};
    for (unsigned i = 0; i < sizeof(titles) / sizeof(titles[0]); ++i)
        expect(orb_time_reminder_set(titles[i], s_wall + (i + 1) * 3600, NULL), "scroll restoration fixture adds real reminder");
    orb_time_view_poll(s_view); settle();
    lv_obj_t *list = orb_time_view_scroll(s_view);
    expect(list != NULL, "Reminder list exposes its visible scroll owner");
    if (!list) return;
    lv_obj_scroll_to_y(list, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list), LV_ANIM_OFF);
    settle();
    int scroll = lv_obj_get_scroll_y(list);
    expect(scroll > 0, "fixture reaches a nonzero real scroll offset");
    click_named(titles[7]);
    scene("reminder-detail");
    list = orb_time_view_scroll(s_view);
    if (list) {
        lv_obj_scroll_to_y(list, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list), LV_ANIM_OFF);
        settle();
        /* Polish review 4: a body that holds every line at rest need not
         * scroll; one that overflows must reach a reading position. */
        expect(lv_obj_get_scroll_y(list) > 0 || lv_obj_get_scroll_bottom(list) <= 0,
            "long reminder detail shows every line or reaches a nonzero scroll offset");
    }
    context_round_trip("context-reminder-detail-scroll");
    expect(orb_time_view_back(s_view), "BOOT Back closes a reminder detail");
    settle();
    list = orb_time_view_scroll(s_view);
    expect(orb_time_view_page(s_view) == ORB_TIME_PAGE_REMINDER && list &&
        lv_obj_get_scroll_y(list) == scroll, "Back restores the exact reminder page and scroll offset");
    context_round_trip("context-reminder-list-scroll");
}

static void fixtures(void)
{
    view(ORB_TIME_PAGE_TIMER, true);
    scene("timer-idle");
    expect(orb_time_timer_set(300) && orb_time_timer_start((int64_t)s_now * 1000), "timer fixture starts");
    orb_time_view_poll(s_view);
    scene("timer-running");
    orb_time_rotary_t rotary;
    expect(!orb_time_view_rotary(s_view, &rotary), "running timer has no unrelated rim owner");
    orb_time_timer_pause((int64_t)s_now * 1000); orb_time_view_poll(s_view);
    scene("timer-paused");

    view(ORB_TIME_PAGE_STOPWATCH, true);
    scene("stopwatch-idle");
    expect(orb_time_stopwatch_start((int64_t)s_now * 1000), "stopwatch fixture starts");
    orb_time_view_poll(s_view); scene("stopwatch-running");
    for (unsigned i = 0; i < 4; ++i) {
        s_now += 61500;
        orb_time_poll((int64_t)s_now * 1000, s_wall);
        expect(orb_time_stopwatch_lap((int64_t)s_now * 1000), "real lap fixture is retained");
    }
    orb_time_stopwatch_pause((int64_t)s_now * 1000); orb_time_view_poll(s_view);
    scene("stopwatch-laps");

    view(ORB_TIME_PAGE_ALARM, false);
    s_scene = "alarm-offline";
    layout_t offline = {0};
    inspect(orb_time_view_root(s_view), &offline, false);
    expect(offline.titles == 1 && offline.primary <= 1, "offline alarm keeps kit place/action roles");
    view(ORB_TIME_PAGE_ALARM, true);
    scene("alarm-idle");
    click_named("Options"); scene("alarm-options");
    click_named("Custom days"); scene("custom-days");
    click_named("Sunday"); scene("custom-days-focused");
    expect(orb_time_view_back(s_view) && orb_time_view_back(s_view),
        "nested day editor returns through options to Alarm");
    settle();
    expect(orb_time_alarm_set(23, 59), "wide civil time fixture is valid");
    orb_time_view_poll(s_view); scene("alarm-long-time");
    expect(orb_time_alarm_enable(true, s_wall), "ringing alarm fixture arms");
    orb_time_snapshot_t ringing;
    orb_time_snapshot((int64_t)s_now * 1000, &ringing);
    time_t old_wall = s_wall;
    s_wall = ringing.alarm_next;
    s_now += 1000;
    orb_time_poll((int64_t)s_now * 1000, s_wall);
    orb_time_view_poll(s_view);
    scene("alarm-ringing");
    expect(!orb_time_view_rotary(s_view, &rotary), "ringing alarm cannot take a value-edit rim owner");
    orb_time_dismiss(false, true);
    s_wall = old_wall;

    view(ORB_TIME_PAGE_REMINDER, true);
    scene("reminders-empty");
    uint32_t id;
    expect(orb_time_reminder_set("Medication review: bring yesterday's appointment notes", s_wall + 3600, &id),
        "real long reminder is accepted");
    expect(orb_time_reminder_set("Water the basil", s_wall + 7200, NULL), "second real reminder is accepted");
    orb_time_view_poll(s_view); scene("reminders-long-list");

    /* Selecting another place must preserve its own content; BOOT from a top
     * level page returns ownership to shell without silently submitting. */
    orb_time_view_select(s_view, ORB_TIME_PAGE_ALARM);
    orb_time_view_select(s_view, ORB_TIME_PAGE_REMINDER);
    expect(orb_time_view_page(s_view) == ORB_TIME_PAGE_REMINDER, "page selection restores the requested place");
    orb_time_snapshot_t before, after;
    orb_time_snapshot((int64_t)s_now * 1000, &before);
    expect(!orb_time_view_back(s_view), "top-level Back is returned to shell");
    orb_time_snapshot((int64_t)s_now * 1000, &after);
    expect(after.generation == before.generation, "Back never saves or acknowledges a service action");
}

int main(int argc, char **argv)
{
    s_scene = "setup";
    setenv("TZ", "UTC0", 1); tzset();
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create();
    orb_ui_init(NULL);
    expect(orb_model_init(), "isolated model initializes");
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "ax3")) expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 fonts available");
        else if (!strcmp(argv[i], "bold")) expect(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold Text fonts available");
        else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "inset")) s_inset = true;
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    if (s_inset) {
        view(ORB_TIME_PAGE_TIMER, true);
        scene("timer-inset-title");
        view(ORB_TIME_PAGE_STOPWATCH, true);
        scene("stopwatch-inset-title");
        view(ORB_TIME_PAGE_ALARM, true);
        expect(orb_time_alarm_set(7, 0) && orb_time_alarm_enable(true, s_wall), "inset alarm fixture arms");
        orb_time_snapshot_t alarm;
        orb_time_snapshot((int64_t)s_now * 1000, &alarm);
        s_wall = alarm.alarm_next;
        s_now += 1000;
        orb_time_poll((int64_t)s_now * 1000, s_wall);
        orb_time_view_poll(s_view);
        scene("ringing-alarm-inset-title");
    } else { fixtures(); back_restoration(); }
    orb_time_view_destroy(s_view);
    printf("Time Design2: %u checks, %u failures%s\n", s_checks, s_failures,
        s_inset ? "; full-ring titles use the kit inset" : "");
    lv_deinit();
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
