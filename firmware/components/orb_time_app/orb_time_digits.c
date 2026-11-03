#include "orb_time_view_internal.h"

#include <stdio.h>
#include <string.h>

/* What the hero shows, independent of LVGL. Group indices are editor fields:
 * 0 hours, 1 minutes, 2 seconds. */
typedef struct {
    unsigned value[3], first, last, lead;
    char suffix[8], meridiem[8];
} readout_t;

typedef struct {
    const lv_font_t *font, *small;
    orb_text_style_t style, small_style;
    int lead, pair, colon, suffix, meridiem, height, width;
    bool clock;
} fit_t;

static unsigned digits_of(unsigned value)
{
    unsigned count = 1;
    while (value >= 10) { value /= 10; ++count; }
    return count;
}

void tv_stopwatch_text(char *out, size_t capacity, int64_t us)
{
    orb_time_format_stopwatch(out, capacity, us);
}

/* HIG §4: the leading group carries only the digits it has ("0:00",
 * "1:30:00", "7:00 AM"), so every reading centres as one string. Stopwatch
 * minutes keep two digits (Apple's 02:03.14); its hour group, like the
 * Timer's, appears only once the value reaches an hour. */
static void readout(const orb_time_view_t *v, readout_t *out)
{
    memset(out, 0, sizeof(*out));
    out->last = 2;
    if (v->page == ORB_TIME_PAGE_ALARM) {
        unsigned hour = v->model.alarm_hour;
        if (!v->model.hour24) {
            char civil[32];
            orb_time_format_clock(civil, sizeof(civil), v->model.alarm_hour, v->model.alarm_minute);
            const char *period = strchr(civil, ' ');
            snprintf(out->meridiem, sizeof(out->meridiem), "%s", period ? period + 1 : "");
            hour = hour % 12 ? hour % 12 : 12;
        }
        out->value[0] = hour; out->value[1] = v->model.alarm_minute;
        out->last = 1; out->lead = v->model.hour24 ? 2 : digits_of(hour);
        return;
    }
    uint64_t seconds;
    if (v->page == ORB_TIME_PAGE_STOPWATCH) {
        int64_t us = v->model.stopwatch_elapsed_us > 0 ? v->model.stopwatch_elapsed_us : 0;
        seconds = (uint64_t)us / 1000000;
        snprintf(out->suffix, sizeof(out->suffix), ".%02u", (unsigned)(us / 10000 % 100));
    } else {
        int64_t shown = v->model.timer_state == ORB_TIME_IDLE ? v->model.timer_duration_us : v->model.timer_remaining_us;
        seconds = shown > 0 ? (uint64_t)(shown + 999999) / 1000000 : 0;
    }
    out->value[0] = (unsigned)(seconds / 3600);
    out->value[1] = (unsigned)(seconds / 60 % 60);
    out->value[2] = (unsigned)(seconds % 60);
    /* An hour edit in progress keeps its group while the rim passes zero. */
    bool editing_hours = v->page == ORB_TIME_PAGE_TIMER && v->field == 0 && (v->drag.active || v->system_rotary);
    out->first = seconds >= 3600 || editing_hours ? 0 : 1;
    out->lead = out->first == 0 ? digits_of(out->value[0]) :
        v->page == ORB_TIME_PAGE_STOPWATCH ? 2 : digits_of(out->value[1]);
}

static void measure(lv_point_t *out, const char *text, const lv_font_t *font)
{
    lv_text_get_size(out, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
}

/* HIG §4 and the review's AX3 finding: the hero keeps the native clock size;
 * the secondary suffix or meridiem steps down first, then the hero yields
 * only when the whole reading still misses the strictest visible chord. */
static void fit(const orb_time_view_t *v, int cy, const readout_t *r, fit_t *out)
{
    static const orb_text_style_t small_steps[] = {ORB_TEXT_TITLE3, ORB_TEXT_HEADLINE, ORB_TEXT_CAPTION1};
    static const char *const leads[] = {"8", "88", "888"};
    unsigned small = 0, groups = r->last - r->first + 1;
    *out = (fit_t){.style = ORB_TEXT_LARGE_TITLE, .clock = true, .font = orb_clock_font(ORB_CLOCK_MEDIUM)};
    int margin = ORB_SCREEN_R - ORB_RIM_RADIUS + (ORB_RIM_WIDTH + 1) / 2 + ORB_RIM_MARGIN_TEXT;
    int row_y = cy + (v->summary ? lv_obj_get_y(v->list) : 0);
    for (;;) {
        lv_point_t lead, pair, colon, suffix = {0}, meridiem = {0};
        out->small_style = small_steps[small];
        out->small = orb_font_variant(out->small_style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR);
        measure(&lead, leads[LV_MIN(r->lead, 3u) - 1], out->font);
        measure(&pair, "88", out->font);
        measure(&colon, ":", out->font);
        if (r->suffix[0]) measure(&suffix, ".88", out->small);
        if (r->meridiem[0]) measure(&meridiem, "AM", out->small);
        out->lead = lead.x; out->pair = pair.x; out->colon = colon.x;
        out->suffix = suffix.x; out->meridiem = meridiem.x;
        out->height = LV_MAX(pair.y, LV_MAX(suffix.y, meridiem.y));
        out->width = lead.x + (int)(groups - 1) * (pair.x + colon.x) + suffix.x + (meridiem.x ? meridiem.x + 8 : 0);
        if (out->width <= orb_chord_width(row_y - out->height / 2, row_y + (out->height + 1) / 2, margin)) return;
        if ((suffix.x || meridiem.x) && small + 1 < sizeof(small_steps) / sizeof(small_steps[0])) ++small;
        else if (out->clock) out->clock = false;
        else if (out->style < ORB_TEXT_TITLE3) ++out->style;
        else return;
        if (!out->clock) out->font = orb_font_variant(out->style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR);
    }
}

/* Secondary text shares the hero's baseline rather than its line box. */
static void baseline_align(lv_obj_t *label, const lv_font_t *hero, int hero_top)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    int baseline = hero_top + hero->line_height - hero->base_line;
    lv_obj_set_y(label, baseline - (font->line_height - font->base_line));
}

/* HIG §§5/8 and review finding 18: the group being edited is underlined as
 * well as teal, so the edit target never rests on colour alone. A frame
 * would cross the neighbouring colons; the bar stays inside its own group. */
static lv_obj_t *edit_mark(orb_time_view_t *v, const fit_t *f, int x, int width, int hero_top)
{
    lv_font_glyph_dsc_t glyph;
    int bottom = f->height;
    if (lv_font_get_glyph_dsc(f->font, &glyph, '8', 0) && glyph.box_h)
        bottom = f->font->line_height - f->font->base_line - glyph.ofs_y;
    lv_obj_t *mark = tv_plain(v->digits);
    lv_obj_set_size(mark, LV_MAX(8, width - 12), 4);
    lv_obj_set_pos(mark, x + 6, hero_top + bottom + 5);
    lv_obj_set_style_radius(mark, 2, 0);
    lv_obj_set_style_bg_color(mark, TV_ACCENT, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_hidden(mark, true);
    return mark;
}

static void build(orb_time_view_t *v, lv_obj_t *parent, int cy, const readout_t *r, unsigned key)
{
    if (v->digits) lv_obj_clean(v->digits);
    memset(v->groups, 0, sizeof(v->groups));
    memset(v->focus, 0, sizeof(v->focus));
    v->suffix = v->ampm = NULL;
    v->digit_format = key;
    if (!v->digits) v->digits = tv_plain(parent);
    lv_obj_set_size(v->digits, 466, 124);
    lv_obj_set_pos(v->digits, 0, cy - 62);
    fit_t f;
    fit(v, cy, r, &f);
    static const char *const leads[] = {"8", "88", "888"};
    int left = (466 - f.width) / 2, target_h = LV_MAX(72, f.height), hero_top = 62 - f.height / 2;
    for (unsigned i = r->first; i <= r->last; ++i) {
        int width = i == r->first ? f.lead : f.pair;
        v->focus[i] = edit_mark(v, &f, left, width, hero_top);
        /* A one-digit group keeps the 46 px minimum target (HIG §3). */
        int target_w = LV_MAX(46, width);
        lv_obj_t *target = tv_hit(v->digits, left + width / 2, 62, target_w, target_h, FIELD_HOUR + (int)i, v);
        v->groups[i] = tv_text(target, "0", target_w / 2, target_h / 2, width,
            f.style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR);
        if (f.clock) orb_clock_apply(v->groups[i], ORB_CLOCK_MEDIUM, 0);
        orb_kit_hero_set_value(v->groups[i], "0", i == r->first ? leads[LV_MIN(r->lead, 3u) - 1] : "88");
        lv_obj_set_style_text_align(v->groups[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_update_layout(v->groups[i]); lv_obj_center(v->groups[i]);
        left += width;
        if (i == r->last) break;
        lv_obj_t *separator = tv_text(v->digits, ":", left + f.colon / 2, 62, f.colon,
            f.style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR);
        if (f.clock) orb_clock_apply(separator, ORB_CLOCK_MEDIUM, 0);
        lv_obj_update_layout(separator);
        lv_obj_set_y(separator, 62 - lv_obj_get_height(separator) / 2);
        left += f.colon;
    }
    if (r->meridiem[0]) {
        v->ampm = tv_text(v->digits, r->meridiem, left + 8 + f.meridiem / 2, 62, f.meridiem,
            f.small_style, ORB_TEXT_EMPHASIZED);
        orb_kit_value_set(v->ampm, r->meridiem, "AM");
        tv_colour(v, v->ampm, TV_SECONDARY);
        baseline_align(v->ampm, f.font, hero_top);
    } else if (r->suffix[0]) {
        v->suffix = tv_text(v->digits, ".00", left + f.suffix / 2, 62, f.suffix,
            f.small_style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR);
        orb_kit_value_set(v->suffix, ".00", ".88");
        tv_colour(v, v->suffix, TV_SECONDARY);
        baseline_align(v->suffix, f.font, hero_top);
    }
}

static bool refresh(orb_time_view_t *v, const readout_t *r)
{
    bool editable = tv_editable(v), damage = false;
    /* Digits are white at rest; teal plus the underline mark an active edit. */
    bool editing = editable && (v->editing || v->drag.active || v->system_rotary);
    for (unsigned i = r->first; i <= r->last; ++i) {
        if (!v->groups[i]) continue;
        char value[16];
        snprintf(value, sizeof(value), "%0*u", (int)(i == r->first ? r->lead : 2u), r->value[i]);
        bool selected = editing && v->field == i;
        lv_color_t ink = selected ? TV_ACCENT : ORB_LABEL;
        damage |= strcmp(lv_label_get_text(v->groups[i]), value) != 0 ||
            !lv_color_eq(lv_obj_get_style_text_color(v->groups[i], 0), ink);
        tv_changed(v->groups[i], value);
        tv_colour(v, v->groups[i], ink);
        if (v->focus[i] && lv_obj_is_hidden(v->focus[i]) == selected) lv_obj_set_hidden(v->focus[i], !selected);
        lv_obj_t *target = lv_obj_get_parent(v->groups[i]);
        if (lv_obj_is_clickable(target) != editable) lv_obj_set_clickable(target, editable);
    }
    if (v->ampm) {
        damage |= strcmp(lv_label_get_text(v->ampm), r->meridiem) != 0;
        tv_changed(v->ampm, r->meridiem);
    }
    if (v->suffix) {
        damage |= strcmp(lv_label_get_text(v->suffix), r->suffix) != 0;
        tv_changed(v->suffix, r->suffix);
    }
    /* A single bounded line damage includes the fraction and meridiem. The
     * line remains live labels, without a retained source per tick. */
    if (damage) lv_obj_invalidate(v->digits);
    return damage;
}

/* Reuse the true readout's groups and hit boxes. Only ink is projected;
 * semantics continue to report the service value, including during arrival. */
void tv_digits_project(float presented, float target, void *context)
{
    orb_time_view_t *v = context;
    if (v->page != ORB_TIME_PAGE_TIMER || !v->digits) return;
    readout_t actual;
    readout(v, &actual);
    int64_t remaining = v->model.timer_state == ORB_TIME_IDLE ?
        v->model.timer_duration_us : v->model.timer_remaining_us;
    double fraction = target > 0 ? LV_CLAMP(0.f, presented / target, 1.f) : 1.;
    uint64_t seconds = remaining > 0 ? (uint64_t)((double)remaining * fraction + 999999.) / 1000000 : 0;
    unsigned values[] = {(unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60)};
    for (unsigned i = actual.first; i <= actual.last; i++) {
        if (!v->groups[i]) continue;
        char shown[16], truth[16];
        unsigned width = i == actual.first ? actual.lead : 2u;
        snprintf(shown, sizeof(shown), "%0*u", (int)width, values[i]);
        snprintf(truth, sizeof(truth), "%0*u", (int)width, actual.value[i]);
        orb_kit_value_set_projection(v->groups[i], shown, truth, NULL);
    }
}

/* HIG §§3/4: the layout is rebuilt only when the reading's shape changes
 * (a digit or group appears); live ticks update tabular labels in place. */
bool tv_digits_show(orb_time_view_t *v, lv_obj_t *parent, int cy)
{
    readout_t r;
    readout(v, &r);
    if (r.first && v->field == 0 && v->page == ORB_TIME_PAGE_TIMER && !v->drag.active && !v->system_rotary)
        v->field = 1;
    unsigned key = 1u + (unsigned)v->page + (r.first << 2) + (r.lead << 3) +
        ((unsigned)(r.meridiem[0] != 0) << 6) + ((unsigned)(r.suffix[0] != 0) << 7);
    if (!v->digits || v->digit_format != key) build(v, parent, cy, &r, key);
    bool damage = refresh(v, &r);
    if (v->page == ORB_TIME_PAGE_TIMER && v->ring)
        tv_digits_project(orb_kit_ring_get_presented_value(v->ring), v->ring_percent, v);
    return damage;
}
