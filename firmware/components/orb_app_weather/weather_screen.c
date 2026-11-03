#include "orb_app_weather.h"
#include "orb_app_weather_registry.h"
#include "orb_weather_icons.h"
#include "orb_page.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_kit_effects.h"
#include "orb_ui.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Geometry, device pixels. The viewport starts below the rim title so content
 * never scrolls underneath it (hig.md §2.1 scroll edge, §3 rim margins). */
#define VIEW_TOP 64
#define HERO_TOP 2          /* Hero line box at y66: the narrow crown holds a glyph and a number. */
#define LANE 360
#define COLUMN 62           /* Six hours span 372 px around the equator's 398 px chord. */
#define ROW_LANE 260        /* Dynamic Type rows stay inside the chord above the end padding. */
#define END_LANE 276        /* Chord at y400 inside the 16 px text margin (hig.md §3): the
                             * lowest resting line, whether the page fits or scrolls. */
#define PAD_BOTTOM 66       /* Fitting or scrolled to its end, the last line rests above y400. */
#define TEXT_MARGIN (ORB_RIM_MARGIN_TEXT + 4)
#define MINUS "\xE2\x88\x92" /* U+2212: a true minus, as on the Face. */

typedef struct { int32_t scroll; uint32_t wait_started; bool waiting; } weather_context_t;

typedef struct {
    lv_obj_t *time, *glyph, *temperature;
} weather_hour_t;

typedef struct {
    orb_weather_screen_model_t model;
    orb_weather_view_t view;
    lv_obj_t *heading, *viewport, *state;
    lv_obj_t *glyph, *temperature, *condition, *range, *place, *updated;
    weather_hour_t hours[ORB_MODEL_MAX_WEATHER_HOURS];
    unsigned hour_count, builds;
    bool rows, projected, waiting;
    uint32_t wait_started;
} weather_state_t;

orb_weather_view_t orb_app_weather_classify(const orb_weather_screen_model_t *model)
{
    /* Retained values outrank the link: an offline or failed refresh keeps the
     * last reading on screen, explicitly marked as not current. */
    if (model->available) return model->stale || !model->online ? ORB_WEATHER_VIEW_STALE : ORB_WEATHER_VIEW_CURRENT;
    if (!model->online) return ORB_WEATHER_VIEW_OFFLINE;
    /* Until the hub's hello settles, absent capability/location means unknown. */
    if (!model->synced) return ORB_WEATHER_VIEW_LOADING;
    if (!model->supported) return ORB_WEATHER_VIEW_UNAVAILABLE;
    if (!model->configured) return ORB_WEATHER_VIEW_NOT_CONFIGURED;
    return model->stale ? ORB_WEATHER_VIEW_UNAVAILABLE : ORB_WEATHER_VIEW_LOADING;
}

static bool has_data(orb_weather_view_t view)
{
    return view == ORB_WEATHER_VIEW_CURRENT || view == ORB_WEATHER_VIEW_STALE;
}

static void degrees(char *out, size_t capacity, float value)
{
    long rounded = isfinite(value) ? lroundf(value) : 0;
    snprintf(out, capacity, "%s%ld°", rounded < 0 ? MINUS : "", labs(rounded));
}

static void range_text(char *out, size_t capacity, const orb_weather_screen_model_t *model)
{
    char high[16], low[16];
    degrees(high, sizeof(high), model->high);
    degrees(low, sizeof(low), model->low);
    snprintf(out, capacity, "H:%s L:%s", high, low);
}

/* HIG §8: staleness is stated in words. Colour (the dimmed hero) only repeats it. */
static void updated_text(char *out, size_t capacity, const orb_weather_screen_model_t *model, bool stale)
{
    if (stale) snprintf(out, capacity, model->updated[0] ? "Not updated since %s" : "Not up to date", model->updated);
    else if (model->updated[0]) snprintf(out, capacity, "Updated %s", model->updated);
    else out[0] = '\0';
}

static int32_t text_width(lv_obj_t *label, const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, 0),
        lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static void ink(lv_obj_t *label, bool secondary)
{
    lv_color_t colour = secondary ? ORB_KIT_SECONDARY : ORB_LABEL;
    lv_opa_t opacity = secondary ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), colour)) lv_obj_set_style_text_color(label, colour, 0);
    if (lv_obj_get_style_text_opa(label, 0) != opacity) orb_kit_live_ink_opa(label, opacity);
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, orb_text_style_t style,
    orb_text_variant_t variant, bool secondary)
{
    lv_obj_t *label = lv_label_create(parent);
    orb_text_apply(label, style, variant, 0);
    lv_label_set_text(label, value);
    ink(label, secondary);
    lv_obj_set_event_bubble(label, true);
    return label;
}

static lv_obj_t *glyph(lv_obj_t *parent, const char *value, orb_text_style_t style, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    orb_icon_apply(label, style, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_color(label, colour, 0);
    lv_obj_set_event_bubble(label, true);
    return label;
}

/* Centred wrapping paragraph, fitted to the chord of every line box it spans
 * at rest (hig.md §3). Wrapping, never ellipsis: AX3 grows the page instead.
 * Lines below the fold keep the END_LANE they occupy once scrolled into view. */
static int32_t paragraph(lv_obj_t *label, int32_t y, int32_t lane)
{
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    int32_t height = font->line_height;
    int32_t width = lane;
    for (unsigned pass = 0; pass < 3; pass++) {
        int32_t fitted = LV_MIN(lane, LV_MAX(END_LANE, orb_chord_width(VIEW_TOP + y, VIEW_TOP + y + height - 1, TEXT_MARGIN)));
        if (fitted == width && pass) break;
        width = fitted;
        lv_obj_set_width(label, width);
        lv_obj_update_layout(label);
        height = lv_obj_get_height(label);
    }
    lv_obj_set_pos(label, 233 - width / 2, y);
    return y + height;
}

/* Footnotes end the page. When it scrolls, they are read at its end, where the
 * last line rests on y400: fit them bottom-up to the chords they occupy there,
 * not to their resting place below the fold (hig.md §3). */
static void footnotes_fit(weather_state_t *state, int32_t bottom)
{
    if (VIEW_TOP + bottom + PAD_BOTTOM <= 466) return;
    lv_obj_t *lines[] = {state->updated, state->place};
    int32_t end = 466 - PAD_BOTTOM;
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *label = lines[i];
        if (!label) continue;
        int32_t height = lv_obj_get_height(label), width = lv_obj_get_width(label);
        for (unsigned pass = 0; pass < 3; pass++) {
            int32_t fitted = LV_MIN(LANE, LV_MAX(END_LANE, orb_chord_width(end - height, end - 1, TEXT_MARGIN)));
            if (fitted == width) break;
            width = fitted;
            lv_obj_set_width(label, width);
            lv_obj_update_layout(label);
            height = lv_obj_get_height(label);
        }
        lv_obj_set_x(label, 233 - width / 2);
        end -= height;
    }
    if (state->place && state->updated)
        lv_obj_set_y(state->updated, lv_obj_get_y(state->place) + lv_obj_get_height(state->place));
}

static void hero_layout(weather_state_t *state)
{
    lv_obj_update_layout(state->temperature);
    int32_t glyph_width = lv_label_get_text(state->glyph)[0] ? lv_obj_get_width(state->glyph) : 0;
    int32_t gap = glyph_width ? 12 : 0;
    int32_t number = lv_obj_get_width(state->temperature);
    int32_t height = lv_obj_get_height(state->temperature);
    int32_t x = 233 - (glyph_width + gap + number) / 2;
    /* Tabular figures keep this pair still while a reading changes; it only
     * re-centres when the number gains or loses a digit or its sign. */
    if (lv_obj_get_x(state->glyph) != x || lv_obj_get_y(state->glyph) != HERO_TOP + (height - lv_obj_get_height(state->glyph)) / 2)
        lv_obj_set_pos(state->glyph, x, HERO_TOP + (height - lv_obj_get_height(state->glyph)) / 2);
    lv_obj_set_hidden(state->glyph, !glyph_width);
    if (lv_obj_get_x(state->temperature) != x + glyph_width + gap || lv_obj_get_y(state->temperature) != HERO_TOP)
        lv_obj_set_pos(state->temperature, x + glyph_width + gap, HERO_TOP);
}

static void hero_ink(weather_state_t *state, bool stale)
{
    lv_color_t colour = stale ? ORB_KIT_SECONDARY : ORB_KIT_HOME;
    lv_opa_t opacity = stale ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    if (!lv_color_eq(lv_obj_get_style_text_color(state->glyph, 0), colour)) lv_obj_set_style_text_color(state->glyph, colour, 0);
    if (lv_obj_get_style_text_opa(state->glyph, 0) != opacity) lv_obj_set_style_text_opa(state->glyph, opacity, 0);
    ink(state->temperature, stale);
    /* The notice itself stays primary ink; only the retained values recede. */
    if (state->updated) ink(state->updated, !stale);
}

static bool column_fits(const weather_state_t *state)
{
    for (unsigned i = 0; i < state->hour_count; i++) {
        const weather_hour_t *hour = &state->hours[i];
        if (text_width(hour->time, lv_label_get_text(hour->time)) > COLUMN - 6 ||
            text_width(hour->temperature, lv_label_get_text(hour->temperature)) > COLUMN - 6 ||
            text_width(hour->glyph, lv_label_get_text(hour->glyph)) > COLUMN - 6) return false;
    }
    return true;
}

/* Default sizes read across like watchOS's hourly strip. Larger type stacks the
 * same three values into rows, the Dynamic Type reflow Apple uses (hig.md §4). */
static int32_t hourly_layout(weather_state_t *state, int32_t y)
{
    if (!state->hour_count) return y;
    state->rows = !column_fits(state);
    int32_t time_h = lv_font_get_line_height(lv_obj_get_style_text_font(state->hours[0].time, 0));
    int32_t glyph_h = lv_font_get_line_height(lv_obj_get_style_text_font(state->hours[0].glyph, 0));
    int32_t temp_h = lv_font_get_line_height(lv_obj_get_style_text_font(state->hours[0].temperature, 0));
    if (!state->rows) {
        int32_t left = 233 - (int32_t)state->hour_count * COLUMN / 2;
        for (unsigned i = 0; i < state->hour_count; i++) {
            weather_hour_t *hour = &state->hours[i];
            int32_t x = left + (int32_t)i * COLUMN;
            lv_obj_t *parts[] = {hour->time, hour->glyph, hour->temperature};
            int32_t tops[] = {y, y + time_h + 2, y + time_h + glyph_h + 4};
            for (unsigned j = 0; j < 3; j++) {
                lv_obj_set_width(parts[j], COLUMN);
                lv_obj_set_style_text_align(parts[j], LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_pos(parts[j], x, tops[j]);
            }
        }
        return y + time_h + glyph_h + temp_h + 4;
    }
    int32_t row = LV_MAX(time_h, LV_MAX(glyph_h, temp_h)) + 4;
    int32_t left = 233 - ROW_LANE / 2;
    for (unsigned i = 0; i < state->hour_count; i++) {
        weather_hour_t *hour = &state->hours[i];
        int32_t top = y + (int32_t)i * row;
        lv_obj_set_width(hour->time, ROW_LANE / 2 - 30);
        lv_obj_set_style_text_align(hour->time, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(hour->time, left, top + (row - time_h) / 2);
        lv_obj_set_width(hour->glyph, 60);
        lv_obj_set_style_text_align(hour->glyph, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hour->glyph, 233 - 30, top + (row - glyph_h) / 2);
        lv_obj_set_width(hour->temperature, ROW_LANE / 2 - 30);
        lv_obj_set_style_text_align(hour->temperature, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(hour->temperature, 233 + 30, top + (row - temp_h) / 2);
    }
    return y + (int32_t)state->hour_count * row;
}

static void hour_text(weather_state_t *state, unsigned index)
{
    const orb_weather_screen_hour_t *source = &state->model.hourly[index];
    char value[24];
    degrees(value, sizeof(value), source->temperature);
    set_text(state->hours[index].time, source->time);
    set_text(state->hours[index].glyph, orb_weather_icon(source->condition, source->is_day));
    orb_kit_value_set_rolling(state->hours[index].temperature, value, NULL);
}

static void state_build(weather_state_t *state, lv_obj_t *root)
{
    const orb_weather_screen_model_t *model = &state->model;
    const char *icon = ORB_ICON_CLOUD_ARROW_DOWN, *title = "Loading weather", *detail = model->place[0] ? model->place : "Waiting for your hub.";
    if (state->view == ORB_WEATHER_VIEW_NOT_CONFIGURED) {
        icon = ORB_ICON_PUSH_PIN; title = "No location"; detail = "Set a location in orb-hub on your Mac.";
    } else if (state->view == ORB_WEATHER_VIEW_OFFLINE) {
        icon = ORB_ICON_WIFI_SLASH; title = "Offline"; detail = "Weather returns when Orb reconnects.";
    } else if (state->view == ORB_WEATHER_VIEW_UNAVAILABLE) {
        icon = ORB_ICON_CLOUD; title = "Weather unavailable";
        detail = model->supported ? "Your hub will try again." : "This hub doesn't provide weather.";
    }
    /* HIG §7 / delight §6: the named wait breathes, without a spinner or an
     * invented command. Refresh remains owned by the hub. */
    state->state = orb_kit_state_create(root, &(orb_kit_state_config_t){
        .glyph = icon, .title = title, .detail = detail,
        .secondary = ORB_KIT_SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA,
        .top = 88, .bottom = 378,
    });
    orb_kit_state_set_waiting_since(state->state, state->waiting, state->wait_started);
}

static void data_build(weather_state_t *state)
{
    const orb_weather_screen_model_t *model = &state->model;
    bool stale = state->view == ORB_WEATHER_VIEW_STALE;
    lv_obj_t *view = state->viewport;
    char value[48];
    state->glyph = glyph(view, orb_weather_icon(model->condition, model->is_day), ORB_TEXT_TITLE2, ORB_KIT_HOME);
    degrees(value, sizeof(value), model->temperature);
    state->temperature = text(view, value, ORB_TEXT_LARGE_TITLE, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, false);
    orb_kit_value_set_rolling(state->temperature, value, NULL);
    lv_obj_update_layout(view);
    hero_layout(state);
    int32_t y = HERO_TOP + lv_obj_get_height(state->temperature) + 2;
    state->condition = text(view, orb_weather_condition_label(model->condition, model->is_day),
        ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, false);
    y = paragraph(state->condition, y, LANE) + 2;
    range_text(value, sizeof(value), model);
    state->range = text(view, value, ORB_TEXT_FOOTNOTE1, ORB_TEXT_TABULAR, true);
    y = paragraph(state->range, y, LANE) + 14;
    state->hour_count = LV_MIN(model->hourly_count, ORB_MODEL_MAX_WEATHER_HOURS);
    for (unsigned i = 0; i < state->hour_count; i++) {
        weather_hour_t *hour = &state->hours[i];
        hour->time = text(view, "", ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, true);
        hour->glyph = glyph(view, "", ORB_TEXT_TITLE3, ORB_LABEL);
        hour->temperature = text(view, "", ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, false);
        hour_text(state, i);
    }
    lv_obj_update_layout(view);
    y = hourly_layout(state, y) + (state->hour_count ? 10 : 0);
    if (model->place[0]) {
        state->place = text(view, model->place, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, true);
        y = paragraph(state->place, y, 300);
    }
    updated_text(value, sizeof(value), model, stale);
    if (value[0]) {
        state->updated = text(view, value, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, !stale);
        y = paragraph(state->updated, y, 300);
    }
    footnotes_fit(state, y);
    hero_ink(state, stale);
}

static void clear_view(weather_state_t *state)
{
    /* The rim title is framework chrome and may already live above the shell's
     * moving body; only this screen's own content is replaced. The viewport is
     * retained too: the shell keeps it as the scroll owner between renders. */
    lv_obj_clean(state->viewport);
    if (state->state) lv_obj_delete(state->state);
    state->state = NULL;
    state->glyph = state->temperature = state->condition = state->range = NULL;
    state->place = state->updated = NULL;
    memset(state->hours, 0, sizeof(state->hours));
    state->hour_count = 0;
    state->rows = false;
}

static void build(orb_screen_instance_t *instance)
{
    weather_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    clear_view(state);
    state->view = orb_app_weather_classify(&state->model);
    bool waiting = state->view == ORB_WEATHER_VIEW_LOADING;
    if (waiting && !state->waiting) state->wait_started = lv_tick_get();
    state->waiting = waiting;
    if (has_data(state->view)) data_build(state);
    else state_build(state, root);
    orb_kit_scroll_arc_refresh(state->viewport);
    /* Created once: a rebuild must not replace chrome the shell has staged.
     * The framework observes chrome deletion, so ask it, not a cached pointer. */
    state->heading = orb_screen_page_title(instance);
    if (!state->heading) state->heading = orb_kit_rim_title_create(root, "Weather", ORB_KIT_HOME);
    orb_screen_page_bind(instance, state->viewport, state->heading);
    if (lv_obj_get_parent(state->heading) == root) lv_obj_move_foreground(state->heading);
    state->builds++;
}

/* Live revisions change text and ink in the retained labels. Only a different
 * composition (state, hour count, place or a reflowing line) rebuilds. */
static bool refresh(weather_state_t *state, const orb_weather_screen_model_t *next)
{
    orb_weather_view_t view = orb_app_weather_classify(next);
    bool stale = view == ORB_WEATHER_VIEW_STALE;
    char notice[48];
    updated_text(notice, sizeof(notice), next, stale);
    if (!has_data(view) || !has_data(state->view) || next->hourly_count != state->model.hourly_count ||
        strcmp(next->place, state->model.place) || !notice[0] != !state->updated) return false;
    lv_obj_t *wrapped[] = {state->condition, state->range, state->updated};
    int32_t heights[3];
    for (unsigned i = 0; i < 3; i++) heights[i] = wrapped[i] ? lv_obj_get_height(wrapped[i]) : 0;
    state->model = *next;
    state->view = view;
    char value[48];
    set_text(state->glyph, orb_weather_icon(next->condition, next->is_day));
    degrees(value, sizeof(value), next->temperature);
    orb_kit_value_set_rolling(state->temperature, value, NULL);
    hero_layout(state);
    set_text(state->condition, orb_weather_condition_label(next->condition, next->is_day));
    range_text(value, sizeof(value), next);
    set_text(state->range, value);
    set_text(state->updated, notice);
    for (unsigned i = 0; i < state->hour_count; i++) hour_text(state, i);
    hero_ink(state, stale);
    lv_obj_update_layout(state->viewport);
    for (unsigned i = 0; i < 3; i++)
        if (wrapped[i] && lv_obj_get_height(wrapped[i]) != heights[i]) return false;
    /* A column that no longer fits needs the stacked composition. */
    return state->rows || column_fits(state);
}

static void project(orb_screen_instance_t *instance, weather_state_t *state)
{
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context, ORB_WEATHER_SCREEN_ID, &projection) &&
        projection.model && projection.model->data && projection.model->size == sizeof(state->model)) {
        memcpy(&state->model, projection.model->data, sizeof(state->model));
        state->projected = true;
    }
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    lv_obj_set_clickable(orb_screen_root(instance), false);
    lv_obj_set_event_bubble(orb_screen_root(instance), true);
    weather_state_t *state = orb_screen_state(instance);
    /* Seed the exact immutable projection before composing, so the router's
     * first update() is idempotent instead of a second build. */
    project(instance, state);
    state->viewport = orb_kit_list_page(orb_screen_root(instance), &(orb_kit_list_page_config_t){
        .top = VIEW_TOP, .height = 466 - VIEW_TOP, .pad_bottom = PAD_BOTTOM, .reading = true});
    if (!state->viewport) return false;
    build(instance);
    orb_kit_scroll_arc_attach(state->viewport);
    return state->heading && (has_data(state->view) || state->state);
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    weather_state_t *state = orb_screen_state(instance);
    orb_weather_screen_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    if (state->projected && !memcmp(&model, &state->model, sizeof(model))) return;
    bool first = !state->projected;
    state->projected = true;
    if (!first && refresh(state, &model)) return;
    int32_t scroll = lv_obj_get_scroll_y(state->viewport);
    state->model = model;
    build(instance);
    if (scroll || lv_obj_get_scroll_y(state->viewport))
        orb_page_restore(state->viewport, &(orb_page_context_t){.scroll = scroll});
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(weather_context_t)) return false;
    const weather_state_t *state = orb_screen_state(instance);
    weather_context_t value = {0};
    value.scroll = orb_page_save(state->viewport).scroll;
    value.wait_started = state->wait_started; value.waiting = state->waiting;
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    weather_state_t *state = orb_screen_state(instance);
    weather_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.waiting && state->waiting) state->wait_started = value.wait_started;
    if (state->state) orb_kit_state_set_waiting_since(state->state, state->waiting, state->wait_started);
    if (value.scroll || lv_obj_get_scroll_y(state->viewport))
        orb_page_restore(state->viewport, &(orb_page_context_t){.scroll = value.scroll});
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    /* No local detail: BOOT and the left edge return through the shell's
     * canonical launch-token policy to the Face that opened Weather. */
    return ORB_BACK_LEAVE_SCREEN;
}

static int32_t maximum_scroll(weather_state_t *state)
{
    lv_obj_update_layout(state->viewport);
    return LV_MAX(0, lv_obj_get_scroll_y(state->viewport) + lv_obj_get_scroll_bottom(state->viewport));
}

static void set_scroll(weather_state_t *state, int32_t value)
{
    value = orb_screen_clamp_scroll(value, maximum_scroll(state));
    if (value != lv_obj_get_scroll_y(state->viewport))
        lv_obj_scroll_to_y(state->viewport, value, LV_ANIM_OFF);
}

static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
}

static void rim_preview(orb_screen_instance_t *instance, float value)
{
    set_scroll(orb_screen_state(instance), (int32_t)lrintf(value));
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)commit; (void)origin; (void)physical;
    set_scroll(orb_screen_state(instance), (int32_t)lrintf(value));
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    weather_state_t *state = orb_screen_state(instance);
    int32_t maximum = maximum_scroll(state);
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.property = 1, .kind = ORB_RIM_SCROLL,
        .minimum = 0, .maximum = maximum,
        .value = orb_screen_clamp_scroll(lv_obj_get_scroll_y(state->viewport), maximum),
        .detent = 48, .units_per_turn = 900,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Weather"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(weather_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(weather_context_t, wait_started), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(weather_context_t, waiting), sizeof(bool), ORB_CONTEXT_BOOL},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_WEATHER_SCREEN_ID, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(weather_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(weather_context_t),
    .context_version = 2, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(weather_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(weather_context_t), .declared = true},
    /* Place and forecast are public: the Face already shows them when locked. */
    .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_weather_descriptor(void)
{
    return &s_descriptor;
}

const orb_app_manifest_t *orb_app_weather_manifest(void)
{
    static const orb_screen_descriptor_t *const s_screens[] = {&s_descriptor};
    static const orb_app_manifest_t s_manifest = {
        .id = ORB_WEATHER_SCREEN_ID, .name = "Weather",
        .screens = s_screens, .screen_count = 1, .entry = ORB_WEATHER_SCREEN_ID,
    };
    return &s_manifest;
}

orb_weather_view_t orb_app_weather_view(const orb_screen_instance_t *instance)
{
    const weather_state_t *state = orb_screen_state(instance);
    return state ? state->view : ORB_WEATHER_VIEW_LOADING;
}

lv_obj_t *orb_app_weather_scroll(const orb_screen_instance_t *instance)
{
    const weather_state_t *state = orb_screen_state(instance);
    return state ? state->viewport : NULL;
}

bool orb_app_weather_hourly_rows(const orb_screen_instance_t *instance)
{
    const weather_state_t *state = orb_screen_state(instance);
    return state && state->rows;
}

unsigned orb_app_weather_builds(const orb_screen_instance_t *instance)
{
    const weather_state_t *state = orb_screen_state(instance);
    return state ? state->builds : 0;
}
