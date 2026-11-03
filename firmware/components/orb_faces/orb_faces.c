#include "orb_faces_private.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"
#include "orb_memory.h"
#include "orb_sound.h"
#include "orb_faces_measure.h"
#include "orb_faces_data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE_PI 3.14159265358979323846f
#define FACE_WEATHER_WIDTH 296
#define FACE_COLOUR_SCALE 88

enum { ACTION_OPEN, ACTION_EDIT, ACTION_SELECT, ACTION_COLOUR, ACTION_SLOT, ACTION_RETRY };
enum { SAVE_STAY, SAVE_SELECT, SAVE_BACK_COLOUR, SAVE_BACK_GALLERY };

static const uint32_t s_colours[] = {
    0xFFFFFF, 0xFF9F0A, 0xFF453A, 0xFF375F, 0x64D2FF,
    0x0A84FF, 0x5E5CE6, 0x30D158, 0xFFD60A,
};
static const char *const s_colour_names[] = {
    "White", "Orange", "Red", "Pink", "Teal", "Blue", "Indigo", "Green", "Yellow",
};
static const char *const s_face_names[] = {"Orb", "Meridian", "Activity"};
static void build(orb_faces_t *faces);
static void refresh(orb_faces_t *faces);
static lv_obj_t *preview(orb_faces_t *v, orb_face_kind_t kind, int cx, int cy, bool selected, int scale);
static void preview_dim(orb_faces_t *child, bool selected, bool prepare);
static void gallery_update(orb_faces_t *v);
static void colour_update(orb_faces_t *v);
static bool save_poll(orb_faces_t *v);
static void save_status(orb_faces_t *v, int cy);

/* Lock and Always On draw the selected face itself; only the data that could
 * reveal the household (how many accessories are on) is withheld there. */
static bool locked_view(const orb_faces_t *v)
{
    return v->always_on && !v->artwork_scale;
}

/* HIG §7 Always On: the same layout at lower ink. Text keeps >= 130/255 so
 * secondary copy stays legible on the dimmed panel. */
static lv_opa_t lock_opa(const orb_faces_t *v, unsigned opacity)
{
    return locked_view(v) ? (lv_opa_t)LV_MAX(130u, (opacity * 184u + 127u) / 255u) : (lv_opa_t)opacity;
}

void orb_faces_seconds_policy(orb_faces_t *v)
{
    if (!v->seconds) return;
    if (v->active && !v->covered && !v->always_on && v->mode == ORB_FACES_FACE &&
        v->prefs.face == ORB_FACE_MERIDIAN && orb_faces_clock_valid(v->snapshot.now) &&
        !orb_ui_reduce_motion() && lv_obj_is_visible(v->root)) lv_timer_resume(v->seconds);
    else lv_timer_pause(v->seconds);
}

static uint8_t *slot_map(orb_faces_t *v)
{
    return v->prefs.face == ORB_FACE_MERIDIAN ? v->prefs.meridian_slot :
        v->prefs.face == ORB_FACE_ACTIVITY ? v->prefs.activity_slot : v->prefs.slot;
}
static const uint8_t *const_slot_map(const orb_faces_t *v)
{
    return v->prefs.face == ORB_FACE_MERIDIAN ? v->prefs.meridian_slot :
        v->prefs.face == ORB_FACE_ACTIVITY ? v->prefs.activity_slot : v->prefs.slot;
}
static unsigned slot_count(const orb_faces_t *v) { return v->prefs.face == ORB_FACE_MERIDIAN ? 4 : 3; }

void orb_faces_current_preferences(orb_faces_prefs_t *out)
{
    orb_settings_faces_current(out);
}

static lv_color_t face_accent(const orb_faces_t *v)
{
    return lv_color_hex(s_colours[v->prefs.colour]);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_clickable(object, false);
    lv_obj_set_event_bubble(object, true);
    return object;
}

static void remember(orb_faces_t *v, lv_obj_t *object, const char *role)
{
    if (v->ref_count < FACE_REFS) v->refs[v->ref_count++] = (face_ref_t){object, role};
}

static void text_changed(lv_obj_t *object, const char *value)
{
    if (object) orb_kit_value_set(object, value, NULL);
}

static lv_obj_t *text(orb_faces_t *v, lv_obj_t *parent, const char *value, int cx, int cy,
                      int width, orb_text_style_t style, lv_color_t colour, const char *role)
{
    lv_obj_t *object = orb_faces_text_create(parent, value, cx, cy, width, style, colour);
    if (role) remember(v, object, role);
    return object;
}

bool orb_faces_complication(const orb_faces_t *v, orb_face_provider_t provider, orb_face_complication_t *out)
{
    return v && orb_faces_complication_data(&v->snapshot, locked_view(v), provider, out);
}

static void notify(orb_faces_t *v)
{
    v->dirty = true;
    if (v->config.changed) v->config.changed(&v->prefs, v->config.context);
}

static void action(lv_event_t *event)
{
    face_action_t *a = lv_event_get_user_data(event);
    orb_faces_t *v = a->faces;
    if (orb_kit_press_is_cancelled(lv_event_get_current_target_obj(event)) || v->dragged || v->held) return;
    int kind = a->action, value = a->value;
    if (kind == ACTION_OPEN) {
        if (v->config.open) v->config.open((orb_faces_target_t)value, v->config.context);
    } else if (kind == ACTION_EDIT) orb_faces_set_mode(v, value ? ORB_FACES_SLOTS : ORB_FACES_COLOUR);
    else if (kind == ACTION_SELECT) orb_faces_confirm_selection(v);
    else if (kind == ACTION_COLOUR) {
        orb_faces_rotate(v, value);
    } else if (kind == ACTION_SLOT) {
        orb_faces_select_slot(v, (unsigned)value);
    } else if (kind == ACTION_RETRY) {
        /* Glossary §2.2: Try again repeats exactly the action that failed. */
        if (v->mode == ORB_FACES_GALLERY && v->failed_select) orb_faces_confirm_selection(v);
        else orb_faces_back(v);
    }
}

static void activate(orb_faces_t *v, lv_obj_t *object, int kind, int value)
{
    if (v->always_on || v->action_count >= FACE_ACTIONS) { lv_obj_set_clickable(object, false); return; }
    face_action_t *a = &v->actions[v->action_count++];
    *a = (face_action_t){v, kind, value};
    orb_kit_press_apply(object);
    lv_obj_set_event_bubble(object, true);
    lv_obj_add_event_cb(object, action, LV_EVENT_CLICKED, a);
}

static void clock_place(orb_faces_t *v, lv_obj_t *parent, const char *clock, int cy,
                        bool medium, int width, bool record)
{
    v->clock = text(v, parent, clock[0] ? clock : "Set time", 233, cy, width,
                    ORB_TEXT_TITLE2, face_accent(v), record ? "clock-text" : NULL);
    orb_faces_clock_fit(v->clock, clock, cy, medium, width);
    orb_kit_value_set_rolling(v->clock, clock[0] ? clock : "Set time", NULL);
}

static void form_update(orb_faces_t *v, unsigned slot, orb_face_provider_t provider, bool animated)
{
    face_form_t *form = &v->forms[slot][provider];
    animated = animated && !v->covered && lv_obj_is_visible(form->root);
    orb_face_complication_t data;
    orb_faces_complication(v, provider, &data);
    bool ring = form->ring != NULL;
    /* Always On keeps the face's own complications (polish #9): the battery
     * glyph and disc stay; only the household count is withheld when locked. */
    bool redact = provider == ORB_FACE_HOME && locked_view(v);
    bool battery_glyph = provider == ORB_FACE_BATTERY && v->prefs.face == ORB_FACE_ORB && data.percent >= 0;
    if (ring && (data.percent < 0 || battery_glyph)) orb_kit_ring_set_value_label(form->ring, NULL, NULL);
    char glyph[24] = "";
    if (provider == ORB_FACE_DATE) {
        char day[8], clock[16];
        orb_faces_day_parts(&v->snapshot, glyph, sizeof(glyph), day, sizeof(day), clock, sizeof(clock));
        orb_faces_weekday_short(glyph);
    } else snprintf(glyph, sizeof(glyph), "%s", data.stale ? ORB_FACES_STALE_GLYPH : data.glyph ? data.glyph : "");
    bool wide_weather = provider == ORB_FACE_WEATHER && orb_faces_weather_state(&v->snapshot) != ORB_FACES_WEATHER_NONE &&
        fabsf(v->snapshot.weather.temperature) >= 100;
    if (wide_weather && !data.stale) glyph[0] = '\0';
    if (provider == ORB_FACE_HOME && v->snapshot.home_on >= 10 && data.percent >= 0)
        snprintf(data.text, sizeof(data.text), "%u", v->snapshot.home_on);
    /* A ring carries its value; without one, the glyph names the empty slot. */
    bool show_glyph = glyph[0] && !redact && (!ring || data.percent < 0 || data.stale);
    orb_faces_static_text(form->glyph, form->glyph_text, sizeof(form->glyph_text), glyph);
    if (ring) orb_faces_static_text(form->value, form->value_text, sizeof(form->value_text),
        battery_glyph || redact ? data.glyph : data.text);
    else orb_faces_rolling_static(v, form->value, form->value_text, sizeof(form->value_text),
        redact ? data.glyph : data.text);
    orb_text_style_t style = ring && data.percent >= 0 ? (battery_glyph ? ORB_TEXT_FOOTNOTE1 :
        orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? ORB_TEXT_FOOTNOTE2 : ORB_TEXT_CAPTION2) :
        provider == ORB_FACE_DATE ? ORB_TEXT_CAPTION2 : redact ? ORB_TEXT_FOOTNOTE1 : ORB_TEXT_FOOTNOTE2;
    if (lv_obj_get_style_text_font(form->value, 0) != orb_font_variant(style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR))
        orb_text_apply(form->value, style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
    /* A value is never clipped (polish #27, "13 min" at AX3): a number and
     * unit that cannot share one line stack, the unit in the glyph's place. */
    int tracking = wide_weather && orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? -1 : 0;
    if (lv_obj_get_style_text_letter_space(form->value, 0) != tracking)
        lv_obj_set_style_text_letter_space(form->value, tracking, 0);
    bool stacked = false;
    char *unit = strstr(form->value_text, "\xC2\xA0");
    if (show_glyph && unit && orb_faces_text_width(form->value, form->value_text) > lv_obj_get_width(form->value)) {
        char number[16], suffix[16];
        snprintf(number, sizeof(number), "%.*s", (int)(unit - form->value_text), form->value_text);
        snprintf(suffix, sizeof(suffix), "%s", unit + 2);
        orb_faces_rolling_static(v, form->value, form->value_text, sizeof(form->value_text), number);
        orb_faces_static_text(form->glyph, form->glyph_text, sizeof(form->glyph_text), suffix);
        stacked = true;
    }
    if (form->glyph) lv_obj_set_hidden(form->glyph, !show_glyph);
    lv_color_t ink = battery_glyph || redact ? data.accent : data.stale ? ORB_KIT_SECONDARY : ORB_LABEL;
    lv_color_t mark = data.stale ? ORB_KIT_SECONDARY : data.accent;
    lv_opa_t opacity = data.stale ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    /* Forms own their opacity, Always On included, so a value turning stale
     * while locked is dimmed without a rebuild. */
    orb_faces_ink_set(form->value, ink, lock_opa(v, opacity));
    if (form->glyph) orb_faces_ink_set(form->glyph, mark, lock_opa(v, opacity));
    /* At AX3 the three-letter weekday (polish #11) sits lower, on a wider
     * chord of its 80px disc, so its caps stay inside the disc. */
    bool ax3 = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    int cy = !show_glyph ? 40 : stacked ? 30 : provider == ORB_FACE_DATE ? (ax3 ? 58 : 57) :
        provider == ORB_FACE_WEATHER ? (wide_weather ? 45 : 50) : 53;
    orb_faces_at(form->value, 40, cy);
    if (form->glyph) orb_faces_at(form->glyph, 40, stacked ? 56 : provider == ORB_FACE_DATE ? (ax3 ? 31 : 21) : wide_weather ? 18 : ring && data.stale ? 25 : 22);
    if (ring) {
        /* A retained ring is still a value: dim its ink as well as its text.
         * The clock mark above the number makes freshness independent of hue. */
        orb_faces_ring_ink(form->ring, locked_view(v), data.stale);
        if (!lv_color_eq(lv_obj_get_style_arc_color(form->ring, LV_PART_INDICATOR), data.accent)) {
            lv_obj_set_style_arc_color(form->ring, data.accent, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(form->ring, data.accent, LV_PART_MAIN);
        }
        if (form->target != LV_MAX(0, data.percent)) {
            form->target = LV_MAX(0, data.percent);
            orb_kit_ring_set_value(form->ring, form->target, animated && form->known && data.percent >= 0);
        }
        form->known = data.percent >= 0;
        orb_kit_ring_set_value_label(form->ring, data.percent >= 0 && !battery_glyph ? form->value : NULL, NULL);
        if (data.percent >= 0 && !battery_glyph) orb_kit_ring_set_text(form->ring, form->value_text, NULL);
    }
}

static void form_show(orb_faces_t *v, unsigned slot, orb_face_provider_t provider)
{
    for (unsigned i = 0; i < ORB_FACE_PROVIDER_COUNT; i++) {
        lv_obj_set_hidden(v->forms[slot][i].root, i != (unsigned)provider);
        if (i != (unsigned)provider && v->forms[slot][i].ring)
            orb_kit_ring_set_value(v->forms[slot][i].ring, v->forms[slot][i].target, false);
    }
    face_form_t *form = &v->forms[slot][provider];
    v->value[slot] = form->value; v->glyph[slot] = form->glyph; v->ring[slot] = form->ring;
}

static void complication(orb_faces_t *v, unsigned slot, orb_face_provider_t provider, int cx, int cy, bool editable)
{
    lv_obj_t *disc = orb_kit_disc_create(v->content, ORB_KIT_DISC_STANDARD, ORB_KIT_TRACK, ORB_GRAY6, "");
    orb_faces_at(disc, cx, cy); v->disc[slot] = disc;
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    remember(v, disc, "complication");
    /* Every provider form is built while idle. Crown selection only changes
     * visibility/focus and the kit caption's documented content cache. */
    for (unsigned i = 0; i < ORB_FACE_PROVIDER_COUNT; i++) {
        face_form_t *form = &v->forms[slot][i];
        orb_face_complication_t data; orb_faces_complication(v, (orb_face_provider_t)i, &data);
        form->root = plain(disc); lv_obj_set_size(form->root, 80, 80);
        bool ring = i == ORB_FACE_CLAUDE || i == ORB_FACE_CODEX || i == ORB_FACE_BATTERY;
        if (ring) {
            form->ring = orb_kit_ring_create(form->root, 34, 6, data.accent);
            orb_faces_at(form->ring, 40, 40); form->target = -1;
        }
        form->glyph = text(v, form->root, "", 40, i == ORB_FACE_DATE ? 21 : 22, 66,
                           i == ORB_FACE_DATE ? ORB_TEXT_FOOTNOTE2 : ORB_TEXT_FOOTNOTE1, data.accent, "complication-text");
        form->value = text(v, form->root, "", 40, 40, ring ? 60 : i == ORB_FACE_WEATHER ? 74 : 66,
                           ring || i == ORB_FACE_DATE ? ORB_TEXT_CAPTION2 : ORB_TEXT_FOOTNOTE2,
                           ORB_LABEL, "complication-text");
        /* DOTS edits its backing buffer while fitting. Fixed provider buffers
         * are exact bounded values, so preserve them with one-line CLIP. */
        lv_label_set_long_mode(form->glyph, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_long_mode(form->value, LV_LABEL_LONG_MODE_CLIP);
        orb_kit_value_set(form->glyph, "", NULL);
        const char *reserve = i == ORB_FACE_CLAUDE || i == ORB_FACE_CODEX || i == ORB_FACE_BATTERY ? "100" :
            i == ORB_FACE_WEATHER ? "−188°" : i == ORB_FACE_HOME ? "64" :
            i == ORB_FACE_NEXT ? "88\xC2\xA0min" : i == ORB_FACE_DATE ? "88" : NULL;
        orb_kit_value_set(form->value, "", reserve);
        form_update(v, slot, (orb_face_provider_t)i, false);
    }
    form_show(v, slot, provider);
    if (editable) {
        orb_kit_disc_set_focus(disc, true);
        lv_obj_set_style_opa(disc, slot == v->slot ? LV_OPA_COVER : 112, 0);
        activate(v, disc, ACTION_SLOT, (int)slot);
    } else {
        orb_face_complication_t data; orb_faces_complication(v, provider, &data);
        activate(v, disc, ACTION_OPEN, data.target);
    }
}

static void weather_row_update(orb_faces_t *v)
{
    if (!v->weather_label) return;
    const orb_weather_snapshot_t *weather = &v->snapshot.weather;
    orb_faces_weather_t state = orb_faces_weather_state(&v->snapshot);
    char temperature[24] = "";
    const char *glyph = orb_faces_weather_glyph(&v->snapshot);
    /* HIG §§4/8: name absent weather, including on the quiet face. Stale and
     * offline cues share this lane; a bare dash cannot explain provider state. */
    bool status = state == ORB_FACES_WEATHER_NONE;
    const char *condition = !v->snapshot.online ? "Mac offline" : state == ORB_FACES_WEATHER_STALE ? "Last known" :
        state == ORB_FACES_WEATHER_NONE ? (weather->configured ?
            weather->label[0] ? weather->label : "Weather unavailable" : "Weather not set") : weather->label[0] ? weather->label :
        orb_weather_condition_label(weather->condition, weather->is_day);
    if (state != ORB_FACES_WEATHER_NONE) snprintf(temperature, sizeof(temperature), "%.0f°", (double)weather->temperature);
    if (!condition) condition = "";
    lv_color_t ink = state == ORB_FACES_WEATHER_CURRENT ? ORB_LABEL : ORB_KIT_SECONDARY;
    lv_opa_t opacity = lock_opa(v, state == ORB_FACES_WEATHER_CURRENT ? LV_OPA_COVER : ORB_KIT_SECONDARY_OPA);
    orb_faces_ink_set(v->weather_icon, state == ORB_FACES_WEATHER_CURRENT ? ORB_KIT_HOME : ORB_KIT_SECONDARY, opacity);
    orb_faces_ink_set(v->weather_temp, ink, opacity);
    text_changed(v->weather_icon, glyph);
    orb_faces_rolling_text(v, v->weather_temp, temperature);
    orb_text_style_t caption_style = status ? ORB_TEXT_FOOTNOTE2 : ORB_TEXT_CAPTION2;
    /* Retained values use tabular fonts; matching that face avoids restyling unchanged Always On status. */
    if (lv_obj_get_style_text_font(v->weather_label, 0) != orb_font_variant(caption_style, ORB_TEXT_TABULAR))
        orb_text_apply(v->weather_label, caption_style, ORB_TEXT_TABULAR, 0);
    int icon_width = orb_faces_text_width(v->weather_icon, glyph);
    int temp_width = orb_faces_text_width(v->weather_temp, temperature);
    int room = FACE_WEATHER_WIDTH - icon_width - temp_width - 16;
    int label_width = condition[0] ? orb_faces_text_width(v->weather_label, condition) : 0;
    /* A provider failure must be readable in full at AX3. Its explicit words
     * can name the complication when the redundant cloud would crowd them. */
    if (status && !temp_width && label_width > room) {
        icon_width = 0;
        room = FACE_WEATHER_WIDTH;
    }
    lv_obj_set_hidden(v->weather_icon, !icon_width);
    /* Polish #13: a condition is read whole or not at all. At AX3 a long one
     * yields its lane to the symbol and temperature instead of "Partly cl…";
     * the offline/stale words are short and must always fit. */
    if (label_width > room && state == ORB_FACES_WEATHER_CURRENT && v->snapshot.online &&
        orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3) label_width = 0;
    char caption[96];
    if (label_width) orb_faces_caption_fit(v->weather_label, condition, LV_MIN(label_width, room), caption, sizeof(caption));
    else caption[0] = '\0';
    label_width = label_width ? LV_MIN(label_width, room) : 0;
    text_changed(v->weather_label, caption);
    orb_faces_ink_set(v->weather_label, ORB_KIT_SECONDARY, lock_opa(v, ORB_KIT_SECONDARY_OPA));
    int gaps = (icon_width && (temp_width || label_width) ? 8 : 0) + (label_width && temp_width ? 8 : 0);
    int total = icon_width + temp_width + gaps + label_width;
    int x = (FACE_WEATHER_WIDTH - total) / 2;
    lv_obj_set_hidden(v->weather_label, !label_width);
    lv_obj_set_width(v->weather_icon, LV_MAX(1, icon_width)); orb_faces_at(v->weather_icon, x + icon_width / 2, 24);
    x += icon_width ? icon_width + 8 : 0;
    lv_obj_set_hidden(v->weather_temp, !temp_width);
    lv_obj_set_width(v->weather_temp, LV_MAX(1, temp_width)); orb_faces_at(v->weather_temp, x + temp_width / 2, 24);
    if (temp_width) x += temp_width + 8;
    lv_obj_set_width(v->weather_label, LV_MAX(1, label_width));
    orb_faces_at(v->weather_label, x + label_width / 2, 24);
}

static void weather_row_create(orb_faces_t *v)
{
    v->weather = plain(v->content);
    lv_obj_set_size(v->weather, FACE_WEATHER_WIDTH, 48); orb_faces_at(v->weather, 233, 89);
    v->weather_icon = text(v, v->weather, "", 0, 24, 1, ORB_TEXT_CAPTION2, ORB_KIT_HOME, "weather-text");
    v->weather_temp = text(v, v->weather, "", 0, 24, 1, ORB_TEXT_CAPTION2, ORB_LABEL, "weather-text");
    orb_kit_value_set(v->weather_temp, "", "−188°");
    v->weather_label = text(v, v->weather, "", 0, 24, 1, ORB_TEXT_CAPTION2, ORB_KIT_SECONDARY, "weather-text");
    orb_text_apply(v->weather_label, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_opa(v->weather_label, ORB_KIT_SECONDARY_OPA, 0);
    weather_row_update(v);
}

static void build_orb(orb_faces_t *v, bool slots)
{
    char weekday[24], day[8], clock[16];
    orb_faces_day_parts(&v->snapshot, weekday, sizeof(weekday), day, sizeof(day), clock, sizeof(clock));
    if (!slots) {
        v->date = orb_kit_rim_date_create(v->content, weekday, day);
        remember(v, v->date, "rim-date");
        snprintf(v->date_weekday, sizeof(v->date_weekday), "%s", weekday);
        snprintf(v->date_day, sizeof(v->date_day), "%s", day);
    }
    weather_row_create(v);
    clock_place(v, v->content, clock, 183, false, 434, true);
    for (unsigned i = 0; i < 3; i++) complication(v, i, (orb_face_provider_t)v->prefs.slot[i], 128 + 105 * (int)i, 316, slots);
    if (!slots) {
        lv_obj_t *apps = orb_kit_disc_create(v->content, ORB_KIT_DISC_KIND_MINI,
                                            ORB_KIT_NEUTRAL_GLASS, ORB_LABEL, ORB_ICON_APPS);
        orb_faces_at(apps, 233, 408); remember(v, apps, "apps");
        if (v->always_on) {
            lv_obj_delete(apps);
            v->ref_count--;
            lv_obj_t *lock = text(v, v->content, ORB_ICON_LOCK, 233, 408, 56,
                                  ORB_TEXT_CAPTION2, ORB_KIT_SECONDARY, "lock-text");
            lv_obj_set_style_text_opa(lock, ORB_KIT_SECONDARY_OPA, 0);
        } else {
            activate(v, apps, ACTION_OPEN, ORB_FACES_OPEN_APPS);
            if (!v->config.preview) orb_glass_materialise(apps, true);
        }
    } else {
        lv_obj_set_style_opa(v->clock, 82, 0);
        lv_obj_set_style_opa(v->weather, 82, 0);
        v->date = orb_kit_rim_title_create(v->content, "Complications", ORB_LABEL);
        remember(v, v->date, "rim-title");
        orb_face_complication_t data;
        orb_faces_complication(v, (orb_face_provider_t)v->prefs.slot[v->slot], &data);
        v->caption = orb_kit_rim_caption_create(v->content, data.name);
        orb_kit_rim_caption_set_style(v->caption, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED,
                                    data.accent, LV_OPA_COVER);
        remember(v, v->caption, "rim-caption");
        lv_obj_t *position = orb_kit_scroll_arc_create(v->content, v->prefs.slot[v->slot], ORB_FACE_PROVIDER_COUNT);
        v->slot_position = position;
        remember(v, position, "scroll-arc");
    }
}

static void hands_update(orb_faces_t *v)
{
    if (!v->hands[0]) return;
    struct tm local = {0};
    localtime_r(&v->snapshot.now, &local);
    float seconds = v->always_on && !v->artwork_scale ? 0.f : (float)local.tm_sec;
    if (v->active && !v->always_on && !orb_ui_reduce_motion())
        seconds += LV_MIN(999u, lv_tick_get() - v->sample_tick) / 1000.f;
    float minute = local.tm_min + seconds / 60.f;
    float hour = local.tm_hour % 12 + minute / 60.f;
    float angles[] = {hour * 30.f, minute * 6.f, seconds * 6.f};
    int lengths[] = {115, 175, 189};
    for (unsigned i = 0; i < 3; i++) {
        if (!v->hands[i]) continue;
        lv_point_precise_t points[2];
        orb_faces_polar(&points[0], angles[i], i == 2 ? -29 : 0);
        orb_faces_polar(&points[1], angles[i], lengths[i]);
        if (v->artwork_scale) for (unsigned j = 0; j < 2; j++) {
            points[j].x = points[j].x * v->artwork_scale / 256;
            points[j].y = points[j].y * v->artwork_scale / 256;
        }
        if (!memcmp(points, v->hand_points[i], sizeof(points))) continue;
        memcpy(v->hand_points[i], points, sizeof(points));
        lv_line_set_points(v->hands[i], v->hand_points[i], 2);
    }
}

static void build_meridian(orb_faces_t *v)
{
    /* Clock-specific ticks and hands are content primitives, not another
     * control/ring implementation. All subdial discs/gauges use the kit. */
    for (unsigned i = 0; i < 60; i++) {
        bool hour = i % 5 == 0;
        orb_faces_polar(&v->ticks[i][0], i * 6.f, hour ? 204 : 212);
        orb_faces_polar(&v->ticks[i][1], i * 6.f, 220);
        lv_obj_t *tick = orb_faces_clock_line(v->content, v->ticks[i], hour ? 5 : 2, hour ? ORB_LABEL : ORB_KIT_SECONDARY);
        lv_obj_set_style_line_opa(tick, hour ? (v->always_on ? 184 : LV_OPA_COVER) : (v->always_on ? 64 : 89), 0);
    }
    int widths[] = {11, 7, 3};
    bool valid = orb_faces_clock_valid(v->snapshot.now);
    for (unsigned i = 0; i < (v->always_on ? 2u : 3u) && valid; i++) {
        v->hands[i] = orb_faces_clock_line(v->content, v->hand_points[i], widths[i],
                                  i == 2 ? face_accent(v) : ORB_LABEL);
    }
    hands_update(v);
    if (!valid) v->summary = text(v, v->content, "Set time", 233, 233, 122,
                                 ORB_TEXT_FOOTNOTE2, ORB_LABEL, "clock-text");
    /* HIG §7 + the owner's hard non-overlap rule: hands pass behind opaque
     * subdial content. The reference's exposed crossings cannot ship. */
    complication(v, 0, (orb_face_provider_t)v->prefs.meridian_slot[0], 233, 128, false);
    complication(v, 1, (orb_face_provider_t)v->prefs.meridian_slot[1], 128, 233, false);
    complication(v, 2, (orb_face_provider_t)v->prefs.meridian_slot[2], 338, 233, false);
    complication(v, 3, (orb_face_provider_t)v->prefs.meridian_slot[3], 233, 338, false);
    /* The shared offline cue (polish #15). Orb/Activity carry it in their
     * text lanes; the analog dial's only free lane is above the top subdial. */
    v->offline = text(v, v->content, "Mac offline", 233, 62, 200, ORB_TEXT_FOOTNOTE2, ORB_KIT_SECONDARY, "offline-text");
    lv_obj_set_style_text_opa(v->offline, ORB_KIT_SECONDARY_OPA, 0);
    /* Like the subdials, an opaque chip: the hands pass behind the words. */
    lv_obj_set_width(v->offline, orb_faces_text_width(v->offline, "Mac offline") + 24);
    orb_faces_at(v->offline, 233, 62);
    lv_obj_set_style_bg_color(v->offline, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(v->offline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(v->offline, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_hidden(v->offline, v->snapshot.online);
    if (v->always_on) {
        lv_obj_t *lock = text(v, v->content, ORB_ICON_LOCK, 233, 411, 56, ORB_TEXT_FOOTNOTE1,
                              ORB_KIT_SECONDARY, "lock-text");
        lv_obj_set_style_text_opa(lock, ORB_KIT_SECONDARY_OPA, 0);
    }
}

/* Lanes whose widths follow their text are re-centred on every data change.
 * In scaled artwork (gallery/editor previews) the native centre maps through
 * the preview scale, as preview() placed it, about a re-centred pivot. */
/* FaceActivity's "1 working · 72°" line, then a legend that names each ring
 * by its provider's glyph and value in the ring's colour (HIG §5: meaning is
 * never colour alone). Labels own fixed buffers; this runs on data changes. */
static void activity_text_update(orb_faces_t *v)
{
    if (!v->summary || !v->weather) return;
    char summary[48], weather[48], temperature[24];
    orb_faces_activity_summary(&v->snapshot, summary, sizeof(summary));
    orb_faces_weather_short(&v->snapshot, temperature, sizeof(temperature));
    snprintf(weather, sizeof(weather), "· %s %s", orb_faces_weather_glyph(&v->snapshot), temperature);
    orb_faces_rolling_static(v, v->summary, v->summary_text, sizeof(v->summary_text), summary);
    orb_faces_rolling_static(v, v->weather, v->weather_text, sizeof(v->weather_text), weather);
    orb_faces_weather_t state = orb_faces_weather_state(&v->snapshot);
    orb_faces_ink_set(v->weather, state == ORB_FACES_WEATHER_CURRENT ? ORB_LABEL : ORB_KIT_SECONDARY,
            lock_opa(v, state == ORB_FACES_WEATHER_CURRENT ? LV_OPA_COVER : ORB_KIT_SECONDARY_OPA));
    int summary_width = orb_faces_text_width(v->summary, summary), weather_width = orb_faces_text_width(v->weather, weather);
    /* The line box at y290 spans 300px inside the innermost ring. */
    bool both = summary_width + 8 + weather_width <= 300;
    lv_obj_set_hidden(v->weather, !both);
    int total = both ? summary_width + 8 + weather_width : summary_width;
    lv_obj_set_width(v->summary, LV_MAX(1, summary_width));
    lv_obj_set_width(v->weather, LV_MAX(1, weather_width));
    orb_faces_lane_at(v->artwork_scale, v->summary, 233 - total / 2 + summary_width / 2, 290);
    orb_faces_lane_at(v->artwork_scale, v->weather, 233 + total / 2 - weather_width / 2, 290);
    int widths[3] = {0}, sum = 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        sum = 0;
        for (unsigned i = 0; i < 3; i++) {
            orb_face_complication_t data;
            orb_faces_complication(v, (orb_face_provider_t)v->prefs.activity_slot[i], &data);
            char chip[48];
            /* A redacted Home ring keeps its glyph only. */
            bool value = pass == 0 && !(v->prefs.activity_slot[i] == ORB_FACE_HOME && locked_view(v));
            const char *mark = data.stale && strcmp(data.glyph, ORB_FACES_STALE_GLYPH) ? ORB_FACES_STALE_GLYPH : "";
            if (value) snprintf(chip, sizeof(chip), "%s%s\xC2\xA0%s", data.glyph ? data.glyph : "", mark, data.text);
            else snprintf(chip, sizeof(chip), "%s%s", data.glyph ? data.glyph : "", mark);
            orb_faces_static_text(v->legend[i], v->legend_text[i], sizeof(v->legend_text[i]), chip);
            orb_faces_ink_set(v->legend[i], data.stale ? ORB_KIT_SECONDARY : data.accent,
                    lock_opa(v, data.stale ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER));
            widths[i] = orb_faces_text_width(v->legend[i], chip);
            sum += widths[i];
        }
        /* Chord at y333 inside the innermost ring is 264px. At AX3 the glyphs
         * alone remain the ring names rather than truncating the values. */
        if (sum + 2 * 16 <= 264) break;
    }
    int x = 233 - (sum + 32) / 2;
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_set_width(v->legend[i], LV_MAX(1, widths[i]));
        orb_faces_lane_at(v->artwork_scale, v->legend[i], x + widths[i] / 2, 333);
        x += widths[i] + 16;
    }
}

static void build_activity(orb_faces_t *v)
{
    for (unsigned i = 0; i < 3; i++) {
        orb_face_complication_t data;
        orb_faces_complication(v, (orb_face_provider_t)v->prefs.activity_slot[i], &data);
        v->activity[i] = orb_kit_ring_create(v->content, 213 - 20 * (int)i, 14, data.accent);
        int percent = data.percent;
        lv_obj_set_style_arc_color(v->activity[i], data.accent, LV_PART_MAIN);
        lv_obj_set_style_arc_color(v->activity[i], data.accent, LV_PART_INDICATOR);
        orb_kit_ring_set_value(v->activity[i], percent < 0 ? 0 : percent, false);
        v->activity_target[i] = LV_MAX(0, percent);
        v->activity_known[i] = percent >= 0;
        orb_faces_ring_ink(v->activity[i], locked_view(v), data.stale);
        remember(v, v->activity[i], "rim-ring");
    }
    char weekday[24], day[8], clock[16];
    orb_faces_day_parts(&v->snapshot, weekday, sizeof(weekday), day, sizeof(day), clock, sizeof(clock));
    orb_faces_weekday_short(weekday);
    /* Date is content inside the three gauges, as in FaceActivity. A rim title
     * on the kit's inset176 baseline would cross the innermost173 gauge. */
    v->date = text(v, v->content, weekday, 207, 132, 82, ORB_TEXT_FOOTNOTE1, ORB_KIT_DATE, "date-text");
    v->day_label = text(v, v->content, day, 278, 132, 48, ORB_TEXT_FOOTNOTE1, ORB_LABEL, "date-text");
    clock_place(v, v->content, clock, 211, true, 310, true);
    v->summary = text(v, v->content, "", 233, 290, 1, ORB_TEXT_FOOTNOTE1, ORB_LABEL, "working-text");
    v->weather = text(v, v->content, "", 233, 290, 1, ORB_TEXT_FOOTNOTE1, ORB_KIT_SECONDARY, "weather-text");
    for (unsigned i = 0; i < 3; i++) {
        v->legend[i] = text(v, v->content, "", 233, 333, 1, ORB_TEXT_FOOTNOTE2, ORB_LABEL, "legend-text");
        lv_label_set_long_mode(v->legend[i], LV_LABEL_LONG_MODE_CLIP);
    }
    lv_label_set_long_mode(v->summary, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_long_mode(v->weather, LV_LABEL_LONG_MODE_CLIP);
    v->summary_text[0] = v->weather_text[0] = '\0';
    for (unsigned i = 0; i < 3; i++) v->legend_text[i][0] = '\0';
    activity_text_update(v);
    if (v->always_on) text(v, v->content, ORB_ICON_LOCK, 233, 370, 40, ORB_TEXT_FOOTNOTE2, ORB_KIT_SECONDARY, "lock-text");
}

static void editor_chrome(orb_faces_t *v)
{
    v->date = orb_kit_rim_title_create(v->content, "Complications", ORB_LABEL);
    remember(v, v->date, "rim-title");
    orb_face_complication_t data;
    orb_faces_complication(v, (orb_face_provider_t)slot_map(v)[v->slot], &data);
    v->caption = orb_kit_rim_caption_create(v->content, data.name);
    orb_kit_rim_caption_set_style(v->caption, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, data.accent, LV_OPA_COVER);
    remember(v, v->caption, "rim-caption");
    lv_obj_t *position = orb_kit_scroll_arc_create(v->content, slot_map(v)[v->slot], ORB_FACE_PROVIDER_COUNT);
    v->slot_position = position;
    remember(v, position, "scroll-arc");
}

static void build_slots(orb_faces_t *v)
{
    if (v->prefs.face == ORB_FACE_ORB) { build_orb(v, true); return; }
    if (v->prefs.face == ORB_FACE_MERIDIAN) {
        /* The full tick rim and standard bottom caption cannot share the
         * native face. Real180px artwork preserves that geometry, while four
         * canonical edit handles provide the required safe spacing below. */
        preview(v, ORB_FACE_MERIDIAN, 233, 130, false, 99);
        for (unsigned i = 0; i < 4; i++)
            complication(v, i, (orb_face_provider_t)v->prefs.meridian_slot[i],
                         i % 2 ? 293 : 173, i < 2 ? 265 : 365, true);
    } else {
        preview(v, ORB_FACE_ACTIVITY, 233, 160, false, 99);
        for (unsigned i = 0; i < 3; i++)
            complication(v, i, (orb_face_provider_t)v->prefs.activity_slot[i], 128 + 105 * (int)i, 316, true);
    }
    editor_chrome(v);
}

/* Shared face builders produce the gallery artwork too. Presentation scales
 * individual retained controls, rather than allocating three full-frame caches.
 * Rim slices and the clock's ink clip are bounded below256KiB per source. */
static lv_obj_t *preview(orb_faces_t *v, orb_face_kind_t kind, int cx, int cy, bool selected, int scale)
{
    orb_faces_config_t config = {.preview = true};
    orb_faces_t *child = orb_faces_create(v->content, &config);
    if (!child) return NULL;
    child->prefs = v->prefs;
    child->prefs.face = (uint8_t)kind;
    child->snapshot = v->snapshot;
    build(child);
    int extent = 466 * scale / 256;
    lv_obj_t *art = child->root;
    lv_obj_t *canvas = child->content;
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
    /* A face preview is artwork: its children cannot open applications. */
    for (unsigned i = 0; i < child->action_count; i++) {
        child->actions[i].action = -1;
    }
    if (child->clock) {
        lv_area_t ink = orb_faces_label_ink(child->clock);
        lv_area_t box; lv_obj_get_coords(child->clock, &box);
        lv_obj_t *clip = plain(canvas);
        lv_obj_set_size(clip, lv_obj_get_width(child->clock), ink.y2 - ink.y1 + 1);
        lv_obj_set_pos(clip, lv_obj_get_x(child->clock), lv_obj_get_y(child->clock) + ink.y1 - box.y1);
        lv_obj_set_parent(child->clock, clip);
        lv_obj_set_pos(child->clock, 0, box.y1 - ink.y1);
    }
    lv_obj_update_layout(art);
    unsigned count = lv_obj_get_child_count(canvas);
    lv_obj_t *objects[128];
    count = LV_MIN(count, 128u);
    for (unsigned i = 0; i < count; i++) objects[i] = lv_obj_get_child(canvas, i);
    for (unsigned i = 0; i < count; i++) {
        lv_obj_t *object = objects[i];
        if (scale != 143 && lv_obj_get_width(object) == ORB_KIT_DISC_MINI && lv_obj_get_height(object) == ORB_KIT_DISC_MINI) {
            lv_obj_delete(object); continue;
        }
        lv_obj_set_clickable(object, false);
        int x = lv_obj_get_x(object), y = lv_obj_get_y(object);
        lv_obj_set_align(object, LV_ALIGN_TOP_LEFT);
        int w = lv_obj_get_width(object), h = lv_obj_get_height(object);
        if (object == child->date && kind == ORB_FACE_ORB) {
            orb_kit_rim_text_set_presentation(object, scale, selected ? LV_OPA_COVER : 110);
            continue;
        }
        bool gauge = false;
        for (unsigned j = 0; j < 3; j++) {
            if (object != child->activity[j]) continue;
            lv_color_t accent = lv_obj_get_style_arc_color(object, LV_PART_INDICATOR);
            int percent = child->activity_target[j];
            lv_obj_delete(object);
            object = orb_kit_ring_create(canvas, (213 - 20 * (int)j) * scale / 256, LV_MAX(2, 14 * scale / 256), accent);
            child->activity[j] = object;
            orb_kit_ring_set_value(object, percent, false); orb_faces_at(object, extent / 2, extent / 2);
            if (!selected) {
                lv_obj_set_style_arc_opa(object, 110, LV_PART_MAIN);
                lv_obj_set_style_arc_opa(object, 110, LV_PART_INDICATOR);
            }
            gauge = true; break;
        }
        if (gauge) continue;
        if (lv_obj_check_type(object, &lv_line_class)) {
            uint32_t points_count = lv_line_get_point_count(object);
            const lv_point_precise_t *points = lv_line_get_points(object);
            /* All clock line points live in child state, never temporary
             * draw storage. Their native coordinates are no longer needed. */
            for (uint32_t j = 0; j < points_count; j++) {
                ((lv_point_precise_t *)points)[j].x = points[j].x * scale / 256;
                ((lv_point_precise_t *)points)[j].y = points[j].y * scale / 256;
            }
            lv_line_set_points(object, points, points_count);
            lv_obj_set_style_line_width(object, LV_MAX(1, lv_obj_get_style_line_width(object, 0) * scale / 256), 0);
            if (!selected) lv_obj_set_style_line_opa(object, 110, 0);
            continue;
        }
        lv_obj_set_style_transform_pivot_x(object, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(object, h / 2, 0);
        lv_obj_set_style_transform_scale(object, scale, 0);
        lv_obj_set_pos(object, (x + w / 2) * scale / 256 - w / 2,
                              (y + h / 2) * scale / 256 - h / 2);
        if (!selected) lv_obj_set_style_opa(object, 110, 0);
    }
    lv_obj_set_size(canvas, extent, extent);
    lv_obj_set_size(art, extent, extent); orb_faces_at(art, cx, cy);
    /* Retire child recognizers; the gallery alone owns the preview contact. */
    child->artwork_scale = scale;
    child->always_on = true;
    lv_obj_set_clickable(canvas, false);
    preview_dim(child, selected, true);
    if (scale == 143) {
        v->gallery[kind] = child;
        activate(v, art, ACTION_SELECT, kind);
        lv_obj_set_clickable(art, selected);
    } else {
        v->colour_preview = child;
        lv_obj_set_clickable(art, false);
    }
    return art;
}

static void preview_dim(orb_faces_t *child, bool selected, bool prepare)
{
    unsigned line = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(child->content); i++) {
        lv_obj_t *object = lv_obj_get_child(child->content, i);
        if (object == child->date && child->prefs.face == ORB_FACE_ORB) {
            orb_kit_rim_text_set_opa(object, selected ? LV_OPA_COVER : 110);
        } else if (lv_obj_check_type(object, &lv_arc_class)) {
            bool stale = false;
            for (unsigned j = 0; j < 3; j++) if (object == child->activity[j]) {
                orb_face_complication_t data;
                orb_faces_complication(child, (orb_face_provider_t)child->prefs.activity_slot[j], &data);
                stale = data.stale;
            }
            lv_opa_t main = selected ? (stale ? 28 : 46) : 20;
            lv_opa_t indicator = selected ? (stale ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER) : (stale ? 66 : 110);
            if (prepare || lv_obj_get_style_arc_opa(object, LV_PART_MAIN) != main)
                lv_obj_set_style_arc_opa(object, main, LV_PART_MAIN);
            if (prepare || lv_obj_get_style_arc_opa(object, LV_PART_INDICATOR) != indicator)
                lv_obj_set_style_arc_opa(object, indicator, LV_PART_INDICATOR);
        } else if (lv_obj_check_type(object, &lv_line_class)) {
            unsigned base = line < 60 && line % 5 ? 89 : 255;
            lv_opa_t opacity = selected ? base : base * 110 / 255;
            if (prepare || lv_obj_get_style_line_opa(object, 0) != opacity) lv_obj_set_style_line_opa(object, opacity, 0);
            line++;
        } else {
            lv_opa_t opacity = selected ? LV_OPA_COVER : 110;
            if (prepare || lv_obj_get_style_opa(object, 0) != opacity) lv_obj_set_style_opa(object, opacity, 0);
        }
    }
}

static void gallery_update(orb_faces_t *v)
{
    unsigned selected = v->prefs.face;
    for (unsigned i = 0; i < ORB_FACE_COUNT; i++) {
        orb_faces_t *child = v->gallery[i];
        if (!child) continue;
        bool active = i == selected;
        /* Polish #38: the gallery is a finite row. A neighbour peeks only
         * where one exists; the ends never show the opposite face. */
        int cx = active ? 233 : i == selected + 1 ? 511 : i + 1 == selected ? -115 : 0;
        lv_obj_set_hidden(child->root, !cx);
        if (cx) orb_faces_at(child->root, cx, 197);
        lv_obj_set_clickable(child->root, active);
        preview_dim(child, active, false);
    }
    orb_kit_page_dots_set_active(v->gallery_dots, selected, ORB_LABEL);
    if (strcmp(orb_kit_rim_text_get(v->date), s_face_names[selected]))
        orb_kit_rim_text_set(v->date, s_face_names[selected], "");
}

static void colour_update(orb_faces_t *v)
{
    for (unsigned i = 0; i < 9; i++) {
        bool selected = i == v->prefs.colour;
        orb_kit_disc_set_focus(v->swatches[i], selected);
        orb_kit_disc_set_glyph_hidden(v->swatches[i], !selected);
    }
    lv_label_set_text_static(v->summary, s_colour_names[v->prefs.colour]);
    /* The colour name remains white like the frame; the swatch/check and
     * preview clock carry the selected colour, preserving small-text contrast. */
    if (!lv_color_eq(lv_obj_get_style_text_color(v->summary, 0), ORB_LABEL))
        lv_obj_set_style_text_color(v->summary, ORB_LABEL, 0);
    orb_faces_t *child = v->colour_preview;
    if (child) {
        child->prefs.colour = v->prefs.colour;
        if (child->clock) lv_obj_set_style_text_color(child->clock, face_accent(v), 0);
        if (child->hands[2]) lv_obj_set_style_line_color(child->hands[2], face_accent(v), 0);
    }
}

static void build_gallery(orb_faces_t *v)
{
    v->date = orb_kit_rim_title_create(v->content, s_face_names[v->prefs.face], ORB_LABEL);
    remember(v, v->date, "rim-title");
    preview(v, (orb_face_kind_t)((v->prefs.face + 2) % 3), -115, 197, false, 143);
    preview(v, (orb_face_kind_t)((v->prefs.face + 1) % 3), 511, 197, false, 143);
    preview(v, (orb_face_kind_t)v->prefs.face, 233, 197, true, 143);
    for (unsigned i = 0; i < ORB_FACE_COUNT; i++)
        if (v->gallery[i] && i != v->prefs.face && i != v->prefs.face + 1u && i + 1u != v->prefs.face)
            lv_obj_set_hidden(v->gallery[i]->root, true);
    lv_obj_t *selection = orb_kit_ring_create(v->content, 133, 3, ORB_LABEL);
    orb_faces_at(selection, 233, 197); orb_kit_ring_set_value(selection, 100, false);
    lv_obj_t *dots = orb_kit_page_dots_create(v->content, 3);
    /* Polish #38 / HIG §7: page dots follow the rim. Kit dots sit on radius
     * 186, where Edit (y374-426) is; move the group onto the rim caption
     * baseline (radius 212), 14px below Edit and inside the 16px text margin. */
    lv_obj_set_y(dots, lv_obj_get_y(dots) + 26);
    v->gallery_dots = dots;
    orb_kit_page_dots_set_active(dots, v->prefs.face, ORB_LABEL);
    remember(v, dots, "page-dots");
    /* Polish #18: a failed save says so, keeps the face name and offers the
     * retry in the action's own place. */
    lv_obj_t *edit = orb_kit_button_secondary_create(v->content, v->save_error ? "Try again" : "Edit");
    orb_faces_at(edit, 233, 400); activate(v, edit, v->save_error ? ACTION_RETRY : ACTION_EDIT, 0); remember(v, edit, "secondary");
    if (!v->config.preview) orb_glass_materialise(edit, true);
    if (v->save_error) save_status(v, 350);
}

static void save_status(orb_faces_t *v, int cy)
{
    /* Glossary §2.2 "Couldn’t <verb>", in the failure red beside the words. */
    lv_obj_t *status = text(v, v->content, "Couldn’t save", 233, cy, 220, ORB_TEXT_FOOTNOTE2, ORB_RED, "status-text");
    orb_text_apply(status, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 0);
}

static void build_colour(orb_faces_t *v)
{
    v->date = orb_kit_rim_title_create(v->content, "Color", ORB_LABEL); /* Glossary §1.2 US English. */
    remember(v, v->date, "rim-title");
    /* The frame's40px swatches conflict with IMPLEMENT's standalone80 role.
     * Nine80px swatches fit a270-degree arc at radius174 with21px gaps. Selection keeps
     * the centre clear and uses the same focus ring without scaling. */
    for (unsigned i = 0; i < sizeof(s_colours) / sizeof(s_colours[0]); i++) {
        float angle = (i * 33.75f - 45.f) * FACE_PI / 180.f;
        lv_obj_t *swatch = orb_kit_disc_create(v->content, ORB_KIT_DISC_STANDARD,
                                              ORB_KIT_ACCENT_FILL, orb_color_from_rgb(s_colours[i]),
                                              ORB_ICON_CHECK);
        int cx = 233 + lroundf(cosf(angle) * 174), cy = 233 + lroundf(sinf(angle) * 174);
        orb_faces_at(swatch, cx, cy); v->swatches[i] = swatch;
        orb_kit_disc_set_focus(swatch, i == v->prefs.colour);
        orb_kit_disc_set_glyph_hidden(swatch, i != v->prefs.colour);
        activate(v, swatch, ACTION_COLOUR, (int)i); remember(v, swatch, "swatch");
    }
    /* Polish #17: the artwork's outer ring/ticks end >= 8px from the selected
     * swatch's focus outline (radius 47 at (356,110)): centre (233,156) is
     * 131px away and scale 88 makes the outermost content radius 76px. */
    /* A failure adds a status line: the artwork rises and shrinks (still 12px
     * clear of the swatch) so the wider Try again capsule at y305 stays 8px
     * from the lower swatches. */
    bool failed = v->save_error;
    preview(v, (orb_face_kind_t)v->prefs.face, 233, failed ? 146 : 156, false,
            failed ? FACE_COLOUR_SCALE - 8 : FACE_COLOUR_SCALE);
    v->summary = text(v, v->content, s_colour_names[v->prefs.colour], 233, failed ? 236 : 267, 140,
                      ORB_TEXT_CAPTION2, ORB_LABEL, "colour-text");
    lv_obj_t *slots = orb_kit_button_secondary_create(v->content, failed ? "Try again" : "Slots");
    orb_faces_at(slots, 233, failed ? 305 : 315);
    if (failed) save_status(v, 263);
    /* Colour's central control enters the next editor. The only bottom action
     * on this radial selector is supplied by physical Back. */
    face_action_t *a = &v->actions[v->action_count++];
    *a = v->save_error ? (face_action_t){v, ACTION_RETRY, 0} : (face_action_t){v, ACTION_EDIT, 1};
    lv_obj_set_event_bubble(slots, true);
    lv_obj_add_event_cb(slots, action, LV_EVENT_CLICKED, a);
    remember(v, slots, "secondary");
    if (!v->config.preview) orb_glass_materialise(slots, true);
}

static void root_event(lv_event_t *event)
{
    orb_faces_t *v = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE && lv_event_get_target_obj(event) == v->root) {
        lv_obj_remove_event_cb_with_user_data(v->root, root_event, v);
        if (v->seconds) lv_timer_delete(v->seconds);
        orb_settings_faces_release(v->save_ticket);
        orb_memory_free(v); return;
    }
    if (v->building || v->always_on) return;
    if (code == LV_EVENT_PRESSED) {
        v->held = false; v->dragged = false;
        lv_indev_t *input = lv_indev_active();
        lv_point_t point = {0}; if (input) lv_indev_get_point(input, &point);
        v->drag_x = point.x; v->drag_y = point.y;
    } else if (code == LV_EVENT_LONG_PRESSED && v->mode == ORB_FACES_FACE && !v->dragged) orb_faces_hold(v);
    else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *input = lv_indev_active(); lv_point_t point = {0};
        if (input) lv_indev_get_point(input, &point);
        int dx = point.x - v->drag_x, dy = point.y - v->drag_y;
        if (dx * dx + dy * dy >= 100) v->dragged = true;
    } else if (code == LV_EVENT_RELEASED && v->mode == ORB_FACES_GALLERY && v->dragged) {
        lv_indev_t *input = lv_indev_active(); lv_point_t point = {0};
        if (input) lv_indev_get_point(input, &point);
        orb_faces_horizontal_drag(v, point.x - v->drag_x);
    }
}

static void build(orb_faces_t *v)
{
    v->building = true;
    if (v->content) lv_obj_delete(v->content);
    v->content = plain(v->root);
    lv_obj_set_size(v->content, 466, 466);
    lv_obj_set_style_bg_color(v->content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v->content, LV_OPA_COVER, 0);
    lv_obj_set_clickable(v->content, true);
    v->clock = v->date = v->day_label = v->weather = v->weather_icon = v->weather_temp = v->weather_label = v->summary = v->caption = NULL;
    memset(v->disc, 0, sizeof(v->disc)); memset(v->value, 0, sizeof(v->value));
    memset(v->glyph, 0, sizeof(v->glyph)); memset(v->ring, 0, sizeof(v->ring));
    memset(v->activity, 0, sizeof(v->activity)); memset(v->hands, 0, sizeof(v->hands));
    memset(v->legend, 0, sizeof(v->legend)); v->offline = NULL;
    memset(v->forms, 0, sizeof(v->forms));
    memset(v->gallery, 0, sizeof(v->gallery));
    memset(v->swatches, 0, sizeof(v->swatches));
    v->gallery_dots = NULL; v->colour_preview = NULL; v->slot_position = NULL;
    v->ref_count = v->action_count = 0;
    v->size = orb_fonts_content_size(); v->bold = orb_fonts_bold_text();
    v->opaque = orb_ui_reduce_transparency();
    if (v->mode == ORB_FACES_GALLERY && !v->always_on) build_gallery(v);
    else if (v->mode == ORB_FACES_COLOUR && !v->always_on) build_colour(v);
    else if (v->mode == ORB_FACES_SLOTS && !v->always_on) build_slots(v);
    else if (v->prefs.face == ORB_FACE_MERIDIAN) build_meridian(v);
    else if (v->prefs.face == ORB_FACE_ACTIVITY) build_activity(v);
    else build_orb(v, false);
    if (v->always_on) {
        for (unsigned i = 0; i < v->ref_count; i++) {
            lv_obj_t *object = v->refs[i].object;
            /* These roles set their own (state-dependent) Always On opacity. */
            if (!strcmp(v->refs[i].role, "complication-text") || !strcmp(v->refs[i].role, "weather-text") || !strcmp(v->refs[i].role, "legend-text")) continue;
            if (strstr(v->refs[i].role, "-text"))
                lv_obj_set_style_text_opa(object, lock_opa(v, lv_obj_get_style_text_opa(object, 0)), 0);
            else if (!strcmp(v->refs[i].role, "rim-date")) {
                orb_kit_rim_text_set_opa(object, 184);
            }
        }
        for (unsigned i = 0; i < 3; i++) {
            if (v->hands[i]) lv_obj_set_style_line_opa(v->hands[i], 184, 0);
        }
    }
    /* Gallery and Color show their own failure; a face only notes it. */
    if (v->save_error && !v->always_on && v->mode == ORB_FACES_FACE) {
        v->caption = orb_kit_rim_caption_create(v->content, "Not saved");
        remember(v, v->caption, "rim-caption");
    }
    lv_obj_update_layout(v->root);
    v->building = false;
    orb_faces_seconds_policy(v);
}

static void refresh(orb_faces_t *v)
{
    if (v->mode != ORB_FACES_FACE && !v->always_on) {
        /* Artwork is a retained representation of the same data, not a stale
         * screenshot. Refresh in place so an incoming weather state cannot
         * destroy a swatch or selected face while the finger is still down. */
        for (unsigned i = 0; i < ORB_FACE_COUNT; i++) if (v->gallery[i]) {
            v->gallery[i]->snapshot = v->snapshot;
            refresh(v->gallery[i]);
        }
        if (v->colour_preview) {
            v->colour_preview->snapshot = v->snapshot;
            refresh(v->colour_preview);
        }
        if (v->mode != ORB_FACES_SLOTS) return;
        if (v->prefs.face == ORB_FACE_ORB) {
            char weekday[24], day[8], clock[16];
            orb_faces_day_parts(&v->snapshot, weekday, sizeof(weekday), day, sizeof(day), clock, sizeof(clock));
            orb_faces_rolling_text(v, v->clock, clock[0] ? clock : "Set time");
            weather_row_update(v);
        }
        for (unsigned slot = 0; slot < slot_count(v); slot++)
            for (unsigned provider = 0; provider < ORB_FACE_PROVIDER_COUNT; provider++)
                form_update(v, slot, (orb_face_provider_t)provider, false);
        return;
    }
    char weekday[24], day[8], clock[16];
    orb_faces_day_parts(&v->snapshot, weekday, sizeof(weekday), day, sizeof(day), clock, sizeof(clock));
    if (v->clock) orb_faces_rolling_text(v, v->clock, clock[0] ? clock : "Set time");
    if (v->prefs.face == ORB_FACE_ORB) {
        if (v->date) {
            if (strcmp(v->date_weekday, weekday) || strcmp(v->date_day, day)) {
                char suffix[12]; snprintf(suffix, sizeof(suffix), " %s", day);
                orb_kit_rim_text_set(v->date, weekday, suffix);
                snprintf(v->date_weekday, sizeof(v->date_weekday), "%s", weekday);
                snprintf(v->date_day, sizeof(v->date_day), "%s", day);
            }
        }
        weather_row_update(v);
    } else if (v->prefs.face == ORB_FACE_MERIDIAN) {
        hands_update(v);
        if (v->offline) lv_obj_set_hidden(v->offline, v->snapshot.online);
    }
    else {
        orb_faces_weekday_short(weekday); text_changed(v->date, weekday); text_changed(v->day_label, day);
        activity_text_update(v);
        for (unsigned i = 0; i < 3; i++) {
            orb_face_complication_t data;
            orb_faces_complication(v, (orb_face_provider_t)v->prefs.activity_slot[i], &data);
            int percent = data.percent;
            if (percent < 0) orb_kit_ring_set_value_label(v->activity[i], NULL, NULL);
            orb_faces_ring_ink(v->activity[i], locked_view(v), data.stale);
            if (v->activity_target[i] != LV_MAX(0, percent)) {
                v->activity_target[i] = LV_MAX(0, percent);
                orb_kit_ring_set_value(v->activity[i], LV_MAX(0, percent), percent >= 0 && v->activity_known[i] &&
                    !(v->prefs.activity_slot[i] == ORB_FACE_NEXT && v->timer_tick) && v->active && !v->covered && !v->always_on && lv_obj_is_visible(v->root));
            }
            v->activity_known[i] = percent >= 0;
            orb_kit_ring_set_value_label(v->activity[i], percent >= 0 ? v->legend[i] : NULL, NULL);
            if (percent >= 0) orb_kit_ring_set_text(v->activity[i], v->legend_text[i], NULL);
        }
    }
    for (unsigned slot = 0; slot < 4; slot++) {
        if (!v->disc[slot]) continue;
        for (unsigned provider = 0; provider < ORB_FACE_PROVIDER_COUNT; provider++)
            form_update(v, slot, (orb_face_provider_t)provider, v->active && !v->always_on && lv_obj_is_visible(v->root));
    }

}

static void seconds_tick(lv_timer_t *timer)
{
    orb_faces_t *v = lv_timer_get_user_data(timer);
    orb_faces_seconds_policy(v);
    if (v->active && !v->covered && !v->always_on && v->mode == ORB_FACES_FACE &&
        v->prefs.face == ORB_FACE_MERIDIAN && !orb_ui_reduce_motion() && lv_obj_is_visible(v->root)) hands_update(v);
}


orb_faces_t *orb_faces_create(lv_obj_t *parent, const orb_faces_config_t *config)
{
    if (!parent) return NULL;
    orb_faces_t *v = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*v));
    if (!v) return NULL;
    if (config) v->config = *config;
    orb_settings_faces_current(&v->prefs);
    v->snapshot.usage_percent[0] = v->snapshot.usage_percent[1] = -1;
    v->snapshot.battery_percent = -1;
    v->always_on = v->config.always_on;
    v->active = !v->config.preview;
    v->sample_tick = lv_tick_get();
    v->seconds = lv_timer_create(seconds_tick, 50, v);
    if (v->seconds) lv_timer_pause(v->seconds);
    v->root = plain(parent);
    lv_obj_set_size(v->root, 466, 466);
    lv_obj_set_clickable(v->root, true);
    lv_obj_add_event_cb(v->root, root_event, LV_EVENT_ALL, v);
    build(v);
    return v;
}

void orb_faces_destroy(orb_faces_t *v)
{
    if (v) lv_obj_delete(v->root);
}

lv_obj_t *orb_faces_root(const orb_faces_t *v) { return v ? v->root : NULL; }
size_t orb_faces_state_size(void) { return sizeof(orb_faces_t); }

void orb_faces_update(orb_faces_t *v, const orb_faces_snapshot_t *snapshot)
{
    if (!v || !snapshot) return;
    bool save_changed = save_poll(v);
    orb_faces_seconds_policy(v);
    orb_faces_snapshot_t incoming = *snapshot;
    if (!snapshot->weather_override) {
        orb_weather_snapshot_t weather = {0};
        if (orb_model_weather_snapshot(&weather)) incoming.weather = weather;
        else incoming.weather = (orb_weather_snapshot_t){0};
    }
    bool clock_valid_changed = orb_faces_clock_valid(v->snapshot.now) != orb_faces_clock_valid(incoming.now) ||
        (v->snapshot.clock[0] || orb_faces_clock_valid(v->snapshot.now)) !=
        (incoming.clock[0] || orb_faces_clock_valid(incoming.now));
    if (!memcmp(&v->snapshot, &incoming, sizeof(incoming)) && v->size == orb_fonts_content_size() &&
        v->bold == orb_fonts_bold_text() && v->opaque == orb_ui_reduce_transparency() && !save_changed) return;
    if (incoming.now != v->snapshot.now) v->sample_tick = lv_tick_get();
    v->timer_tick = incoming.next_total_seconds == v->snapshot.next_total_seconds && incoming.next_seconds < v->snapshot.next_seconds;
    v->snapshot = incoming;
    bool fonts_changed = v->size != orb_fonts_content_size() || v->bold != orb_fonts_bold_text() ||
        v->opaque != orb_ui_reduce_transparency();
    if (fonts_changed || clock_valid_changed || save_changed) build(v);
    else refresh(v);
}

void orb_faces_set_always_on(orb_faces_t *v, bool always_on)
{
    if (!v || v->always_on == always_on) return;
    v->always_on = always_on;
    if (always_on) {
        orb_faces_save(v);
        if (v->browsing) v->prefs.face = v->face_before_gallery;
        v->browsing = false; v->mode = ORB_FACES_FACE;
    }
    build(v); refresh(v);
}

void orb_faces_set_mode(orb_faces_t *v, orb_faces_mode_t mode)
{
    if (!v || mode > ORB_FACES_SLOTS || v->always_on || v->mode == mode) return;
    if (mode == ORB_FACES_GALLERY && v->mode == ORB_FACES_FACE) {
        v->face_before_gallery = v->prefs.face; v->browsing = true;
    } else if (mode == ORB_FACES_FACE && v->browsing) {
        v->prefs.face = v->face_before_gallery; v->browsing = false;
    }
    if (!v->config.preview) orb_sound_play(mode == ORB_FACES_FACE ? ORB_SOUND_CLOSE : ORB_SOUND_OPEN);
    v->mode = mode; build(v);
}
orb_faces_mode_t orb_faces_mode(const orb_faces_t *v) { return v ? v->mode : ORB_FACES_FACE; }

void orb_faces_select(orb_faces_t *v, orb_face_kind_t face)
{
    if (!v || face >= ORB_FACE_COUNT || v->prefs.face == face) return;
    v->prefs.face = (uint8_t)face; v->slot = 0; notify(v); build(v);
}

bool orb_faces_rotary(const orb_faces_t *v, int *value, int *maximum)
{
    if (!v || v->always_on || v->mode == ORB_FACES_FACE) return false;
    if (value) *value = v->mode == ORB_FACES_GALLERY ? v->prefs.face :
        v->mode == ORB_FACES_COLOUR ? v->prefs.colour : const_slot_map(v)[v->slot];
    if (maximum) *maximum = v->mode == ORB_FACES_GALLERY ? ORB_FACE_COUNT - 1 :
        v->mode == ORB_FACES_COLOUR ? (int)(sizeof(s_colours) / sizeof(s_colours[0])) - 1 : ORB_FACE_PROVIDER_COUNT - 1;
    return true;
}

void orb_faces_rotate(orb_faces_t *v, int value)
{
    int current, maximum;
    if (!orb_faces_rotary(v, &current, &maximum)) return;
    value = LV_CLAMP(0, value, maximum);
    if (current == value) return;
    if (v->mode == ORB_FACES_GALLERY) { v->prefs.face = (uint8_t)value; v->slot = 0; }
    else if (v->mode == ORB_FACES_COLOUR) v->prefs.colour = (uint8_t)value;
    else slot_map(v)[v->slot] = (uint8_t)value;
    notify(v);
    if (v->mode == ORB_FACES_GALLERY) gallery_update(v);
    else if (v->mode == ORB_FACES_COLOUR) colour_update(v);
    else {
        form_show(v, v->slot, (orb_face_provider_t)slot_map(v)[v->slot]);
        orb_face_complication_t data;
        orb_faces_complication(v, (orb_face_provider_t)slot_map(v)[v->slot], &data);
        orb_kit_rim_text_set(v->caption, data.name, "");
        orb_kit_rim_caption_set_style(v->caption, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, data.accent, LV_OPA_COVER);
        orb_kit_scroll_arc_set_position(v->slot_position, slot_map(v)[v->slot], ORB_FACE_PROVIDER_COUNT);
    }
}

void orb_faces_turn(orb_faces_t *v, int steps)
{
    int value;
    if (!orb_faces_rotary(v, &value, NULL)) return;
    /* Every rotary selection clamps at its ends, the gallery included. */
    orb_faces_rotate(v, value + steps);
}

bool orb_faces_back(orb_faces_t *v)
{
    if (!v || v->mode == ORB_FACES_FACE) return false;
    if (v->save_ticket) return true;
    if (v->mode == ORB_FACES_SLOTS) v->mode = ORB_FACES_COLOUR;
    else if (v->mode == ORB_FACES_COLOUR) {
        if (!orb_faces_save(v)) {
            if (v->save_ticket) v->save_action = SAVE_BACK_COLOUR;
            build(v); return true;
        }
        v->mode = ORB_FACES_GALLERY;
    }
    else {
        if (v->browsing) v->prefs.face = v->face_before_gallery;
        v->browsing = false;
        if (!orb_faces_save(v)) {
            if (v->save_ticket) v->save_action = SAVE_BACK_GALLERY;
            build(v); return true;
        }
        v->mode = ORB_FACES_FACE;
    }
    if (!v->config.preview) orb_sound_play(ORB_SOUND_CLOSE);
    build(v); return true;
}

void orb_faces_hold(orb_faces_t *v)
{
    if (!v || v->always_on || v->mode != ORB_FACES_FACE) return;
    v->held = true;
    orb_faces_set_mode(v, ORB_FACES_GALLERY);
}

bool orb_faces_horizontal_drag(orb_faces_t *v, int dx)
{
    if (!v || v->mode != ORB_FACES_GALLERY || abs(dx) < 40) return false;
    v->dragged = true;
    orb_faces_turn(v, dx < 0 ? 1 : -1); return true;
}

void orb_faces_select_slot(orb_faces_t *v, unsigned slot)
{
    if (!v || slot >= slot_count(v)) return;
    if (v->slot == slot) return;
    v->slot = slot;
    if (v->mode == ORB_FACES_SLOTS) {
        for (unsigned i = 0; i < slot_count(v); i++)
            lv_obj_set_style_opa(v->disc[i], i == slot ? LV_OPA_COVER : 112, 0);
        orb_face_complication_t data;
        orb_faces_complication(v, (orb_face_provider_t)slot_map(v)[slot], &data);
        orb_kit_rim_text_set(v->caption, data.name, "");
        orb_kit_rim_caption_set_style(v->caption, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, data.accent, LV_OPA_COVER);
        orb_kit_scroll_arc_set_position(v->slot_position, slot_map(v)[slot], ORB_FACE_PROVIDER_COUNT);
    }
}

void orb_faces_preferences(const orb_faces_t *v, orb_faces_prefs_t *out)
{
    if (v && out) *out = v->prefs;
}

void orb_faces_set_preferences(orb_faces_t *v, const orb_faces_prefs_t *prefs)
{
    if (!v || !orb_settings_faces_valid(prefs) || !memcmp(&v->prefs, prefs, sizeof(*prefs))) return;
    v->prefs = *prefs;
    if (v->slot >= slot_count(v)) v->slot = 0;
    build(v);
}

bool orb_faces_save_context(const orb_faces_t *v, orb_faces_context_t *out)
{
    if (!v || !out) return false;
    *out = (orb_faces_context_t){.prefs = v->prefs, .mode = (uint8_t)v->mode,
        .slot = (uint8_t)v->slot, .face_before_gallery = v->face_before_gallery,
        .browsing = v->browsing, .dirty = v->dirty, .save_error = v->save_error,
        .failed_select = v->failed_select};
    return true;
}

bool orb_faces_restore_context(orb_faces_t *v, const orb_faces_context_t *context)
{
    if (!v || !context || v->save_ticket || !orb_settings_faces_valid(&context->prefs) ||
        context->mode > ORB_FACES_SLOTS || context->face_before_gallery >= ORB_FACE_COUNT ||
        context->slot >= (context->prefs.face == ORB_FACE_MERIDIAN ? 4 : 3) ||
        (v->always_on && context->mode != ORB_FACES_FACE)) return false;
    v->prefs = context->prefs; v->mode = (orb_faces_mode_t)context->mode;
    v->slot = context->slot; v->face_before_gallery = context->face_before_gallery;
    v->browsing = context->browsing; v->dirty = context->dirty;
    v->save_error = context->save_error; v->failed_select = context->failed_select;
    /* A restore is presentation only: no command, notification or cue replays. */
    build(v);
    return true;
}

void orb_faces_confirm_selection(orb_faces_t *v)
{
    if (!v || v->mode != ORB_FACES_GALLERY || v->save_ticket) return;
    bool browsing = v->browsing;
    v->browsing = false; v->dirty = true;
    if (!orb_faces_save(v)) {
        if (v->save_ticket) {
            v->save_action = SAVE_SELECT; v->save_browsing = browsing;
        } else { v->browsing = browsing; v->failed_select = true; }
        build(v); return;
    }
    v->failed_select = false;
    orb_faces_set_mode(v, ORB_FACES_FACE);
}

static bool save_poll(orb_faces_t *v)
{
    if (!v->save_ticket) return false;
    orb_settings_result_t result = orb_settings_faces_result(v->save_ticket);
    if (result == ORB_SETTINGS_PENDING) return false;
    orb_settings_faces_release(v->save_ticket); v->save_ticket = 0;
    v->save_error = result != ORB_SETTINGS_SAVED;
    v->failed_select = v->save_error && v->save_action == SAVE_SELECT;
    if (v->save_error && v->save_action == SAVE_SELECT && v->save_browsing) {
        if (!v->always_on && v->mode != ORB_FACES_FACE) v->browsing = true;
        else v->prefs.face = v->face_before_gallery;
    }
    orb_faces_prefs_t candidate = v->prefs;
    if (v->browsing && v->save_action != SAVE_SELECT) candidate.face = v->face_before_gallery;
    if (!v->save_error && !memcmp(&candidate, &v->save_candidate, sizeof(candidate))) {
        v->dirty = false;
        /* Only the matching successful save may finish its original UI action.
         * A covered view or later edit must not be navigated by stale I/O. */
        if (!v->always_on && v->mode == v->save_mode && v->save_action != SAVE_STAY) {
            if (v->save_action == SAVE_SELECT) v->browsing = false;
            v->mode = v->save_action == SAVE_BACK_COLOUR ? ORB_FACES_GALLERY : ORB_FACES_FACE;
            if (!v->config.preview) orb_sound_play(ORB_SOUND_CLOSE);
        }
    }
    v->save_action = SAVE_STAY;
    return true;
}

bool orb_faces_save(orb_faces_t *v)
{
    if (!v) return false;
    if (v->save_ticket) return false;
    if (!v->dirty) return !v->save_error;
    orb_faces_prefs_t candidate = v->prefs, current;
    if (v->browsing) candidate.face = v->face_before_gallery;
    orb_settings_faces_current(&current);
    if (!v->config.persist && !memcmp(&candidate, &current, sizeof(candidate))) {
        v->dirty = false; v->save_error = false; return true;
    }
    bool saved = true;
    if (v->config.persist) {
        saved = v->config.persist(&candidate, v->config.context);
        if (saved && !v->config.preview) orb_settings_faces_accept(&candidate);
    } else if (!v->config.preview) {
        uint64_t ticket = v->config.request ? v->config.request(&candidate, v->config.context) :
            orb_settings_faces_request(&candidate);
        if (ticket && orb_settings_faces_result(ticket) == ORB_SETTINGS_PENDING) {
            v->save_ticket = ticket; v->save_candidate = candidate;
            v->save_mode = v->mode; v->save_action = SAVE_STAY;
            v->save_error = false; return false;
        }
        saved = ticket && orb_settings_faces_result(ticket) == ORB_SETTINGS_SAVED;
        orb_settings_faces_release(ticket);
    }
    v->save_error = !saved;
    if (saved) v->dirty = false;
    return saved;
}

bool orb_faces_save_error(const orb_faces_t *v) { return v && v->save_error; }

size_t orb_faces_layout(const orb_faces_t *v, orb_faces_bounds_t *out, size_t capacity)
{
    if (!v) return 0;
    lv_obj_update_layout(v->root);
    size_t count = 0;
    for (size_t i = 0; i < v->ref_count; i++) {
        if (!lv_obj_is_visible(v->refs[i].object)) continue;
        if (count < capacity) {
            out[count].role = v->refs[i].role;
            out[count].object = v->refs[i].object;
            if (strstr(out[count].role, "-text")) out[count].bounds = orb_faces_label_ink(v->refs[i].object);
            else lv_obj_get_coords(v->refs[i].object, &out[count].bounds);
        }
        count++;
    }
    return count;
}

static unsigned primary_count(lv_obj_t *object)
{
    unsigned count = orb_kit_is_primary(object);
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) count += primary_count(lv_obj_get_child(object, i));
    return count;
}
unsigned orb_faces_primary_count(const orb_faces_t *v) { return v ? primary_count(v->root) : 0; }
