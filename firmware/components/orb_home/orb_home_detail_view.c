/* One accessory-detail template for every Home kind (HIG §§1/3/4/5/7).
 * Light, fan, cover, thermostat, lock, switch, outlet and sensors share the
 * rim title on the kit's 200px baseline, one 80px identity disc, one value
 * style and one caption order: context (or "Last known"), status, battery.
 * Only the control differs: a bottom level arc, a hold, a tap or nothing. */
#include "orb_home_detail_private.h"
#include "orb_home_view.h"
#include "orb_home_motion.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_icons.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "src/misc/lv_text_private.h"

#define SECONDARY ORB_KIT_SECONDARY
#define TITLE_DEGREES 90 /* Siblings' names stay on the upper rim (HIG §3). */
#define VALUE_WIDTH 330

static float clamp(float value, float low, float high)
{
    return fminf(high, fmaxf(low, value));
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    lv_obj_set_event_bubble(o, true);
    return o;
}

/* A centred template lane. Chord fitting happens when the stack places it. */
static lv_obj_t *lane(lv_obj_t *parent, orb_text_style_t style, orb_text_variant_t variant, unsigned lines)
{
    lv_obj_t *o = lv_label_create(parent);
    orb_text_apply(o, style, variant, 0);
    lv_obj_set_style_text_color(o, SECONDARY, 0);
    lv_obj_set_style_text_opa(o, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_max_lines(o, lines);
    lv_obj_set_width(o, VALUE_WIDTH);
    lv_label_set_text(o, "");
    lv_obj_set_clickable(o, false);
    lv_obj_set_event_bubble(o, true);
    return o;
}

static void set_text(lv_obj_t *o, const char *text)
{
    if (o && strcmp(lv_label_get_text(o), text)) lv_label_set_text(o, text);
}

static void set_ink(lv_obj_t *o, lv_color_t colour)
{
    if (!o) return;
    lv_opa_t opacity = lv_color_eq(colour, SECONDARY) ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), colour)) lv_obj_set_style_text_color(o, colour, 0);
    if (lv_obj_get_style_text_opa(o, 0) != opacity) lv_obj_set_style_text_opa(o, opacity, 0);
}

static int32_t text_width(const char *text, const lv_font_t *font, orb_text_style_t style)
{
    lv_text_attributes_t attributes = {.letter_space = orb_text_style_info(style)->letter_space};
    return lv_text_get_width(text, strlen(text), font, &attributes);
}

/* One value style: Large Title Bold, stepping down only when a word would
 * leave the chord (HIG §4: no clipping, never a lighter weight). */
static orb_text_style_t value_style(const char *reserve)
{
    static const orb_text_style_t steps[] = {ORB_TEXT_LARGE_TITLE, ORB_TEXT_TITLE1, ORB_TEXT_TITLE2, ORB_TEXT_TITLE3};
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
        if (text_width(reserve, orb_font_variant(steps[i], ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR), steps[i]) <= VALUE_WIDTH)
            return steps[i];
    return ORB_TEXT_TITLE3;
}

static const char *mode_name(orb_home_mode_t mode)
{
    static const char *names[] = {"Off", "Heat", "Cool", "Auto"};
    return (unsigned)mode < 4 ? names[mode] : "Off";
}

static bool stale(const orb_home_detail_t *v)
{
    return orb_home_status_stale(orb_home_detail_status(v));
}

/* The live light colour while a colour/white turn previews (HIG §5). */
static lv_color_t live_fill(const orb_home_detail_t *v)
{
    const orb_home_accessory_t *a = &v->accessory;
    bool ready = orb_home_detail_ready(&v->snapshot);
    if (a->kind == ORB_HOME_LIGHT && !v->colour_page &&
        (v->dragging ? v->presented <= 0 :
         (a->state.present & ORB_HOME_BRIGHTNESS) && a->state.brightness <= 0))
        return ORB_GRAY4;
    if (a->kind == ORB_HOME_LIGHT && v->colour_page && v->dragging && ready && a->reachable) {
        orb_home_state_t state = a->state;
        if (a->writable & ORB_HOME_HUE) state.hue = clamp(v->presented, 0, 1) * 360;
        else state.ct = 140 + clamp(v->presented, 0, 1) * 360;
        return orb_home_light_colour(&state);
    }
    return orb_home_disc_colour(a, ready);
}

static bool dial_active(const orb_home_detail_t *v)
{
    const orb_home_state_t *s = &v->accessory.state;
    if (v->accessory.kind == ORB_HOME_LIGHT && !v->colour_page) {
        if (v->dragging) return v->presented > 0;
        if ((s->present & ORB_HOME_BRIGHTNESS) && s->brightness <= 0) return false;
    }
    if (v->dragging) return true;
    if (v->accessory.kind == ORB_HOME_COVER) return true;
    if (v->accessory.kind == ORB_HOME_THERMOSTAT)
        return (s->present & ORB_HOME_MODE) && s->mode != ORB_HOME_MODE_OFF;
    return !(s->present & ORB_HOME_ON) || s->on;
}

static void context_text(const orb_home_detail_t *v, char *out, size_t capacity);

/* A colour/white turn changes Kelvin on every detent: keep that one lane a
 * live tabular value so drags never re-typeset or re-prepare it. */
static void caption_set(orb_home_detail_t *v, const char *text)
{
    if (!v->caption) return;
    if (v->colour_page) orb_kit_value_set(v->caption, text, "88888 K · 100%");
    else set_text(v->caption, text);
}

void orb_home_detail_present(orb_home_detail_t *v, float value)
{
    float old = v->presented;
    if (old == value) return;
    v->presented = value;
    /* HIG §8: quiet detent feedback needs no decorative painted ticks. */
    orb_rim_detent(value, v->accessory.kind == ORB_HOME_THERMOSTAT ? 40 : 100,
        &v->tick_index, v->dragging);
    if (v->canvas && orb_home_detail_has_dial(v))
        orb_kit_level_set_value(v->canvas, (int32_t)lroundf(clamp(value, 0, 1) * 100), false);
    orb_home_detail_show_number(v);
    if (v->colour_page && v->dragging) {
        char text[64];
        context_text(v, text, sizeof(text));
        caption_set(v, text);
    }
}

static const char *dial_reserve(const orb_home_detail_t *v)
{
    if (v->accessory.kind == ORB_HOME_THERMOSTAT) return "88.5°";
    return v->colour_page ? "Warm White" : "100%";
}

static void format_number(float presented, char *text, size_t capacity, void *context)
{
    const orb_home_detail_t *v = context;
    const orb_home_state_t *s = &v->accessory.state;
    float value = clamp(presented / 100, 0, 1);
    bool live = v->dragging || fabsf(presented - orb_kit_rim_slider_get_value(v->canvas)) > .0001f;
    bool known = live || (s->present & orb_home_detail_field(v));
    bool active = dial_active(v);
    if (!known) snprintf(text, capacity, "—");
    else if (!active && !v->colour_page) snprintf(text, capacity, "Off");
    else if (v->accessory.kind == ORB_HOME_THERMOSTAT) {
        /* The rim owns the target. Keep the same quantity at rest and while
         * editing, including valid half-degree Celsius steps. */
        float celsius = live ? orb_temperature_celsius(orb_temperature_target(value, v->temperature), v->temperature) : s->target;
        orb_home_format_temperature(text, capacity, celsius, v->temperature, true);
    } else if (v->colour_page && (v->accessory.writable & ORB_HOME_HUE)) {
        /* HIG §§1/5: name the selected colour rather than an angle. The
         * gauge supplies the exact shade; words work without colour vision. */
        static const char *const colours[] = {"Red", "Orange", "Yellow", "Lime", "Green", "Mint",
            "Cyan", "Azure", "Blue", "Violet", "Purple", "Pink"};
        const char *name = colours[(unsigned)lroundf(value * 12) % 12];
        if ((s->present & ORB_HOME_SAT) && s->sat <= 10)
            name = !(s->present & ORB_HOME_CT) ? "White" : s->ct >= 300 ? "Warm White" : s->ct <= 200 ? "Cool White" : "White";
        snprintf(text, capacity, "%s", name);
    } else if (v->colour_page) {
        float mired = !live ? s->ct : 140 + value * 360;
        snprintf(text, capacity, "%s", mired >= 300 ? "Warm White" : mired <= 200 ? "Cool White" : "White");
    } else snprintf(text, capacity, "%.0f%%", (double)((live ? value : v->target) * 100));
}

bool orb_home_detail_animating(const orb_home_detail_t *v)
{
    return v && v->canvas && orb_home_detail_has_dial(v) &&
        fabsf(orb_kit_ring_get_presented_value(v->canvas) - orb_kit_rim_slider_get_value(v->canvas)) > .0001f;
}

void orb_home_detail_start_settle(orb_home_detail_t *v, float value)
{
    v->presented = v->target = clamp(value, 0, 1);
    /* A new contact takes over from the true target, without a phantom tick
     * on its first movement inside the same detent. Animation stays silent. */
    v->tick_index = (int)lroundf(v->target * (v->accessory.kind == ORB_HOME_THERMOSTAT ? 40 : 100));
    if (!v->canvas || !orb_home_detail_has_dial(v)) return;
    bool known = v->accessory.state.present & orb_home_detail_field(v);
    int32_t target = dial_active(v) ? (int32_t)lroundf(v->target * 100) : 0;
    /* The kit owns both Follow and Sweep; Home no longer integrates a second
     * trajectory or drives a value timer (HIG §6). Unknown never sweeps. */
    orb_kit_level_set_value(v->canvas, target, known);
}

void orb_home_detail_arrival(orb_home_detail_t *v)
{
    if (!v || !v->canvas || !orb_home_detail_has_dial(v) || v->colour_page ||
        !(v->accessory.state.present & orb_home_detail_field(v))) return;
    int32_t target = dial_active(v) ? (int32_t)lroundf(v->target * 100) : 0;
    orb_kit_level_set_arrival(v->canvas, target);
}

void orb_home_detail_show_number(orb_home_detail_t *v)
{
    if (!v->canvas || !orb_home_detail_has_dial(v)) return;
    char text[48];
    float presented = orb_kit_ring_get_presented_value(v->canvas);
    format_number(presented, text, sizeof(text), v);
    bool live = v->dragging || orb_home_detail_animating(v);
    bool known = live || (v->accessory.state.present & orb_home_detail_field(v));
    bool active = dial_active(v);
    bool dim = !known || stale(v) || !active;
    /* Numeric changes may replace the kit's default percentage caption. */
    orb_kit_level_set_text(v->canvas, text, dial_reserve(v));
    lv_color_t fill = dim || !active ? ORB_KIT_GREY : live_fill(v);
    /* A live zero has no indicator segment. Retain its prepared accent until
     * the model settles, so crossing zero damages only the moving arc/value,
     * never the entire dial rectangle for an invisible colour change. */
    if (live && !active && known && !stale(v))
        fill = lv_obj_get_style_arc_color(v->canvas, LV_PART_INDICATOR);
    orb_kit_level_set_state(v->canvas, fill, dim ? SECONDARY : ORB_LABEL, known);
    /* HIG §5/8: a pending write keeps its accent on the indicator, dimmed;
     * the quiet neutral track never becomes the active part of the dial. */
    if (!lv_color_eq(lv_obj_get_style_arc_color(v->canvas, LV_PART_MAIN), ORB_KIT_GREY))
        lv_obj_set_style_arc_color(v->canvas, ORB_KIT_GREY, LV_PART_MAIN);
    lv_opa_t fill_opacity = known ? (v->accessory.pending ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER) : LV_OPA_TRANSP;
    if (lv_obj_get_style_arc_opa(v->canvas, LV_PART_INDICATOR) != fill_opacity)
        lv_obj_set_style_arc_opa(v->canvas, fill_opacity, LV_PART_INDICATOR);
}

/* A reading, its unit and whether it is reported at all. */
static bool sensor_reading(const orb_home_detail_t *v, char *out, size_t capacity)
{
    const orb_home_accessory_t *a = &v->accessory;
    const orb_home_state_t *s = &a->state;
    if (a->kind == ORB_HOME_HUMIDITY && (s->present & ORB_HOME_HUMIDITY_VALUE)) snprintf(out, capacity, "%.0f%%", (double)s->humidity);
    else if (a->kind == ORB_HOME_MOTION && (s->present & ORB_HOME_MOTION_VALUE)) snprintf(out, capacity, "%s", s->motion ? "Motion" : "Clear");
    else if (a->kind == ORB_HOME_CONTACT && (s->present & ORB_HOME_CONTACT_VALUE)) snprintf(out, capacity, "%s", s->contact ? "Open" : "Closed");
    else if (s->present & ORB_HOME_TEMP) orb_home_format_temperature(out, capacity, s->temp, v->temperature, false);
    else if (s->present & ORB_HOME_HUMIDITY_VALUE) snprintf(out, capacity, "%.0f%%", (double)s->humidity);
    else if (s->present & ORB_HOME_MOTION_VALUE) snprintf(out, capacity, "%s", s->motion ? "Motion" : "Clear");
    else if (s->present & ORB_HOME_CONTACT_VALUE) snprintf(out, capacity, "%s", s->contact ? "Open" : "Closed");
    else { snprintf(out, capacity, "—"); return false; }
    return true;
}

/* Non-dial values: switch/outlet power, lock state or a sensor reading. */
static void value_refresh(orb_home_detail_t *v)
{
    if (!v->value) return;
    const orb_home_accessory_t *a = &v->accessory;
    const orb_home_state_t *s = &a->state;
    char text[48];
    bool known = true, active = true;
    const char *reserve = text;
    if (a->kind == ORB_HOME_SWITCH || a->kind == ORB_HOME_OUTLET) {
        known = s->present & ORB_HOME_ON;
        active = known && s->on;
        snprintf(text, sizeof(text), "%s", !known ? "—" : s->on ? "On" : "Off");
        reserve = "Off";
    } else if (a->kind == ORB_HOME_LOCK) {
        known = s->present & ORB_HOME_LOCKED;
        snprintf(text, sizeof(text), "%s", !known ? "—" : s->locked ? "Locked" : "Unlocked");
        reserve = "Unlocked";
    } else {
        known = sensor_reading(v, text, sizeof(text));
        if (s->present & (ORB_HOME_TEMP | ORB_HOME_HUMIDITY_VALUE)) reserve = ORB_HOME_TEMPERATURE_TEMPLATE;
    }
    orb_text_style_t style = value_style(reserve);
    if (lv_obj_get_style_text_font(v->value, 0) != orb_font_variant(style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR))
        orb_text_apply(v->value, style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
    set_text(v->value, text);
    set_ink(v->value, known && active && !stale(v) ? ORB_LABEL : SECONDARY);
}

const char *orb_home_detail_lock_instruction(const orb_home_detail_t *v)
{
    if (!(v->accessory.state.present & ORB_HOME_LOCKED)) return "";
    if (v->holding) return "Keep holding";
    if (!(v->accessory.writable & ORB_HOME_LOCKED)) return "Read-only";
    return v->accessory.state.locked ? "Hold to unlock" : "Tap to lock";
}

static const char *room_name(const orb_home_detail_t *v)
{
    for (unsigned i = 0; i < v->snapshot.room_count; i++)
        if (!strcmp(v->accessory.room, v->snapshot.rooms[i].id)) return v->snapshot.rooms[i].name;
    return "";
}

/* Slot one: "Last known" while stale, else the kind's context, else the
 * full name when the rim title had to end in an ellipsis, else the room. */
static bool title_cut(const orb_home_detail_t *v)
{
    orb_kit_rim_text_info_t title = {0};
    return v->name && orb_kit_rim_text_info(v->name, &title) && title.ellipsised;
}

static void thermostat_context(const orb_home_detail_t *v, char *out, size_t capacity)
{
    const orb_home_state_t *s = &v->accessory.state;
    char now[24] = "";
    if (s->present & ORB_HOME_TEMP) orb_home_format_temperature(now, sizeof(now), s->temp, v->temperature, false);
    /* Off is already the value; the caption keeps only the reading. */
    bool off = (s->present & ORB_HOME_MODE) && s->mode == ORB_HOME_MODE_OFF;
    if ((s->present & ORB_HOME_MODE) && now[0] && !off) snprintf(out, capacity, "%s · Now %s", mode_name(s->mode), now);
    else if (off && now[0]) snprintf(out, capacity, "Now %s", now);
    else if (off) out[0] = 0;
    else if (s->present & ORB_HOME_MODE) snprintf(out, capacity, "%s", mode_name(s->mode));
    else if (now[0]) snprintf(out, capacity, "Now %s", now);
    else out[0] = 0;
}

static void context_text(const orb_home_detail_t *v, char *out, size_t capacity)
{
    const orb_home_accessory_t *a = &v->accessory;
    const orb_home_state_t *s = &a->state;
    if (stale(v)) {
        char age[48];
        orb_home_format_age(age, sizeof(age), a->updated);
        /* A reading's age qualifies it; a control's cached state says Last known. */
        bool reading = !orb_home_detail_has_dial(v) && a->kind != ORB_HOME_LOCK &&
            a->kind != ORB_HOME_SWITCH && a->kind != ORB_HOME_OUTLET;
        snprintf(out, capacity, "%s", age[0] && reading ? age : ORB_HOME_TEXT_LAST_KNOWN);
    } else if (a->kind == ORB_HOME_LOCK && !title_cut(v)) {
        snprintf(out, capacity, "%s", a->pending ? "" : orb_home_detail_lock_instruction(v));
    } else if (a->kind == ORB_HOME_THERMOSTAT) {
        if (title_cut(v)) snprintf(out, capacity, "%s", a->name);
        else thermostat_context(v, out, capacity);
    } else if (v->colour_page) {
        float mired = v->dragging ? 140 + clamp(v->presented, 0, 1) * 360 : s->ct;
        if (!(a->writable & ORB_HOME_HUE) && ((s->present & ORB_HOME_CT) || v->dragging))
            snprintf(out, capacity, "%.0f K · %.0f%%", roundf(1000000 / mired / 10) * 10, (double)s->brightness);
        else snprintf(out, capacity, "Brightness %.0f%%", (double)s->brightness);
    } else if (a->kind == ORB_HOME_COVER && !(a->writable & ORB_HOME_POSITION)) {
        snprintf(out, capacity, "Use the Home app");
    } else if (title_cut(v)) {
        snprintf(out, capacity, "%s", a->name);
    } else snprintf(out, capacity, "%s", room_name(v));
}

static void status_text(const orb_home_detail_t *v, char *out, size_t capacity, bool *error)
{
    orb_home_status_t status = orb_home_detail_status(v);
    *error = status == ORB_HOME_STATUS_FAILED || status == ORB_HOME_STATUS_BUSY;
    /* A cut lock title spends the context lane on the full name, so its
     * instruction moves down one lane while nothing else needs it. */
    if (status == ORB_HOME_STATUS_READY && v->accessory.kind == ORB_HOME_LOCK && title_cut(v)) {
        snprintf(out, capacity, "%s", orb_home_detail_lock_instruction(v));
        return;
    }
    if (status == ORB_HOME_STATUS_READY && v->accessory.kind == ORB_HOME_THERMOSTAT && title_cut(v)) {
        thermostat_context(v, out, capacity);
        return;
    }
    /* One error glyph and colour across Home (HIG §8: never colour alone). */
    snprintf(out, capacity, "%s%s", *error ? ORB_ICON_WARNING_FILL " " : "", orb_home_status_text(status));
}

/* value -> context -> status -> battery, each fitted to its own chord. */
static void stack(orb_home_detail_t *v)
{
    int y = DETAIL_DISC_TOP + ORB_KIT_DISC + 6;
    if (v->canvas && orb_home_detail_has_dial(v)) {
        orb_text_style_t style = value_style(dial_reserve(v));
        orb_kit_level_set_text_style(v->canvas, style, ORB_TEXT_EMPHASIZED);
        int height = orb_font_variant(style, ORB_TEXT_EMPHASIZED)->line_height;
        orb_kit_level_set_value_center(v->canvas, 233, y + height / 2);
        y += height + 2;
    }
    lv_obj_t *lanes[] = {v->value, v->caption, v->status, v->battery};
    for (unsigned i = 0; i < sizeof(lanes) / sizeof(lanes[0]); i++) {
        lv_obj_t *o = lanes[i];
        if (!o) continue;
        bool empty = !lv_label_get_text(o)[0];
        if (lv_obj_is_hidden(o) != empty) lv_obj_set_hidden(o, empty);
        if (empty) continue;
        int line = lv_obj_get_style_text_font(o, 0)->line_height;
        int width = LV_MIN(VALUE_WIDTH, orb_chord_width(y, y + line, ORB_RIM_MARGIN_TEXT + 6));
        if (lv_obj_get_width(o) != width) lv_obj_set_width(o, width);
        lv_obj_update_layout(o);
        if (lv_obj_get_y(o) != y || lv_obj_get_x(o) != 233 - width / 2) lv_obj_set_pos(o, 233 - width / 2, y);
        y += lv_obj_get_height(o) + (o == v->value ? 2 : 4);
    }
}

static void disc_create(orb_home_detail_t *v)
{
    const orb_home_accessory_t *a = &v->accessory;
    v->disc_fill = live_fill(v);
    v->disc_glyph = orb_home_accessory_icon(a);
    v->disc = orb_home_identity_disc(v->content, ORB_KIT_DISC_STANDARD, a, v->disc_fill);
    lv_obj_set_pos(v->disc, 233 - ORB_KIT_DISC / 2, DETAIL_DISC_TOP);
    lv_obj_set_event_bubble(v->disc, true);
    v->lock_disc = a->kind == ORB_HOME_LOCK ? v->disc : NULL;
    int action = a->kind == ORB_HOME_THERMOSTAT ? ACTION_MODE : ACTION_POWER;
    bool control = a->kind == ORB_HOME_LIGHT || a->kind == ORB_HOME_FAN || a->kind == ORB_HOME_SWITCH ||
        a->kind == ORB_HOME_OUTLET || a->kind == ORB_HOME_THERMOSTAT;
    if (control) {
        lv_obj_set_user_data(v->disc, (void *)(intptr_t)action);
        lv_obj_add_event_cb(v->disc, orb_home_detail_clicked, LV_EVENT_CLICKED, v);
    } else if (!v->lock_disc) lv_obj_set_clickable(v->disc, false);
}

static void controls_refresh(orb_home_detail_t *v, lv_obj_t *parent)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *control = lv_obj_get_child(parent, i);
        bool action_target = false;
        for (uint32_t e = 0; e < lv_obj_get_event_count(control); ++e)
            if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(control, e)) == orb_home_detail_clicked) action_target = true;
        if (action_target) {
            int action = (int)(intptr_t)lv_obj_get_user_data(control);
            bool scene = action >= ACTION_SCENE && action < ACTION_SCENE + ORB_HOME_MAX_SCENES;
            bool mode = action >= ACTION_MODE_SET && action < ACTION_MODE_SET + 4;
            bool unsupported = action == ACTION_POWER &&
                (!(v->accessory.writable & ORB_HOME_ON) || !(v->accessory.state.present & ORB_HOME_ON));
            bool disabled = action != ACTION_MODE && (!orb_home_detail_writable(v) || unsupported ||
                (scene && v->snapshot.scene_pending) || (mode && !(v->accessory.writable & ORB_HOME_MODE)));
            if (lv_obj_has_state(control, LV_STATE_DISABLED) != disabled)
                lv_obj_set_state(control, LV_STATE_DISABLED, disabled);
        }
        if (lv_obj_get_child_count(control)) controls_refresh(v, control);
    }
}

static const char *scene_name(const orb_home_detail_t *v, const char *id)
{
    for (unsigned i = 0; i < v->snapshot.scene_count; i++)
        if (!strcmp(v->snapshot.scenes[i].id, id)) return v->snapshot.scenes[i].name;
    return "";
}

static void scene_status(orb_home_detail_t *v, char *out, size_t capacity, bool *error)
{
    *error = false;
    out[0] = 0;
    orb_home_status_t home = orb_home_status_home(&v->snapshot);
    if (home != ORB_HOME_STATUS_READY) { snprintf(out, capacity, "%s", orb_home_status_text(home)); return; }
    if (!v->requested_scene[0]) return;
    const char *name = scene_name(v, v->requested_scene);
    if (v->snapshot.error[0] && !strcmp(v->snapshot.error_id, v->requested_scene)) {
        char message[96];
        orb_home_error_text(message, sizeof(message), "run", name, v->snapshot.error);
        snprintf(out, capacity, ORB_ICON_WARNING_FILL " %s", message);
        *error = true;
        return;
    }
    if (strcmp(v->requested_scene, v->snapshot.last_scene)) return;
    /* The containing Home owner owns the single authoritative Done cue. */
    if (!v->snapshot.scene_pending) v->scene_acknowledged = true;
    snprintf(out, capacity, "%s", v->snapshot.scene_pending ? "Running…" : "Requested");
}

void orb_home_detail_refresh_age(orb_home_detail_t *v)
{
    if (!v->caption || orb_home_detail_has_dial(v) || v->page || !stale(v)) return;
    char text[96];
    context_text(v, text, sizeof(text));
    set_text(v->caption, text);
}

void orb_home_detail_refresh(orb_home_detail_t *v, bool animate)
{
    if (!v->available || !v->content) return;
    bool dial = orb_home_detail_has_dial(v);
    if (v->holding && !orb_home_detail_writable(v)) {
        /* Losing authority ends a deliberate hold; it never completes. */
        v->holding = false;
        if (v->canvas) lv_obj_set_hidden(v->canvas, true);
    }
    if (!v->dragging && dial) {
        if (animate) orb_home_detail_start_settle(v, orb_home_detail_model_value(v));
        else if (!orb_home_detail_animating(v)) {
            v->presented = v->target = orb_home_detail_model_value(v);
            orb_kit_level_set_value(v->canvas, dial_active(v) ? (int32_t)lroundf(v->target * 100) : 0, false);
        }
    }
    char text[128];
    bool error = false;
    if (v->page && !dial) {
        /* Mode and scene lists: one status lane under the rim title. */
        bool acknowledged = v->scene_acknowledged;
        if (v->page == 2) scene_status(v, text, sizeof(text), &error);
        else {
            orb_home_status_t status = orb_home_detail_status(v);
            error = status == ORB_HOME_STATUS_FAILED || status == ORB_HOME_STATUS_BUSY;
            snprintf(text, sizeof(text), "%s%s", error ? ORB_ICON_WARNING_FILL " " : "", orb_home_status_text(status));
        }
        set_text(v->status, text);
        set_ink(v->status, error ? ORB_RED : SECONDARY);
        orb_home_ui_wait(v->status, text, &v->wait);
        orb_home_ui_check(v->content, &v->outcome, v->page == 2 && !strcmp(text, "Requested"),
            !acknowledged, 122, 65);
        if (v->status) {
            lv_obj_set_hidden(v->status, !text[0]);
            lv_label_set_long_mode(v->status, LV_LABEL_LONG_MODE_WRAP);
            lv_label_set_max_lines(v->status, 2);
            lv_obj_set_height(v->status, LV_SIZE_CONTENT);
            lv_obj_update_layout(v->status);
            if (v->list) {
                int top = text[0] ? LV_MAX(110, lv_obj_get_y(v->status) + lv_obj_get_height(v->status) + 12) : 110;
                lv_obj_set_y(v->list, top);
                lv_obj_set_height(v->list, 412 - top);
            }
        }
        controls_refresh(v, v->content);
        return;
    }
    orb_home_detail_show_number(v);
    value_refresh(v);
    context_text(v, text, sizeof(text));
    caption_set(v, text);
    status_text(v, text, sizeof(text), &error);
    set_text(v->status, text);
    set_ink(v->status, error ? ORB_RED : SECONDARY);
    orb_home_ui_wait(v->status, text, &v->wait);
    if (v->battery) {
        orb_home_format_battery(text, sizeof(text), &v->accessory.state);
        set_text(v->battery, text);
        set_ink(v->battery, orb_home_battery_low(&v->accessory.state) ? ORB_RED : SECONDARY);
    }
    /* Keep the control and its physical-input listeners in place. The kit's
     * fill is bounded to this disc; only real model changes receive Light up. */
    if (v->disc) {
        lv_color_t fill = live_fill(v);
        const char *glyph = orb_home_accessory_icon(&v->accessory);
        if (strcmp(v->disc_glyph, glyph)) {
            orb_kit_disc_set_glyph(v->disc, glyph);
            v->disc_glyph = glyph;
        }
        if (!lv_color_eq(v->disc_fill, fill)) {
            lv_color_t before = v->disc_fill;
            v->disc_fill = fill;
            orb_kit_disc_set_ink(v->disc, orb_kit_accent_ink(fill));
            if (animate && !v->dragging) orb_kit_disc_light_up(v->disc, before, fill);
            else {
                orb_kit_disc_light_up_cancel(v->disc);
                lv_obj_set_style_bg_color(v->disc, fill, 0);
            }
        }
    }
    controls_refresh(v, v->content);
    stack(v);
}

static lv_obj_t *detail_list(orb_home_detail_t *v, int top)
{
    lv_obj_t *list = plain(v->content);
    v->list = list;
    /* Home's list viewport (110..412), so the first row clears the round
     * list's edge fade exactly as Home's own Scenes page does. */
    lv_obj_set_pos(list, 0, top);
    lv_obj_set_size(list, 466, 412 - top);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(list, LV_MIN(78, (412 - top - ORB_KIT_ROW_H) / 2), 0);
    lv_obj_set_event_bubble(list, true);
    return list;
}

static lv_obj_t *detail_row(orb_home_detail_t *v, lv_obj_t *list, const orb_kit_row_config_t *config, int action)
{
    lv_obj_t *row = orb_kit_row_create(list, config);
    lv_obj_set_user_data(row, (void *)(intptr_t)action);
    lv_obj_set_event_bubble(row, true);
    lv_obj_add_event_cb(row, orb_home_detail_clicked, LV_EVENT_CLICKED, v);
    return row;
}

static void list_finish(orb_home_detail_t *v)
{
    orb_kit_list_apply(v->list);
    orb_kit_scroll_arc_attach(v->list);
    lv_obj_scroll_to_y(v->list, v->scroll_y[v->page], LV_ANIM_OFF);
}

/* Auto leads so every mode is discoverable before Off, the least likely
 * choice, at the scroll's end. The current mode carries a checkmark and its
 * own colour (heat orange, cool blue, auto green); the others stay neutral. */
static void modes_build(orb_home_detail_t *v)
{
    static const char *const glyphs[] = {ORB_ICON_POWER, ORB_ICON_FLAME_FILL, ORB_ICON_SNOWFLAKE, ORB_ICON_REPEAT};
    const lv_color_t colours[] = {ORB_GRAY4, ORB_ORANGE, ORB_BLUE, ORB_GREEN};
    static const orb_home_mode_t order[] = {ORB_HOME_MODE_AUTO, ORB_HOME_MODE_HEAT, ORB_HOME_MODE_COOL, ORB_HOME_MODE_OFF};
    v->name = orb_kit_rim_title_create(v->content, "Mode", ORB_KIT_HOME);
    lv_obj_t *list = detail_list(v, 110);
    for (unsigned n = 0; n < 4; ++n) {
        unsigned i = (unsigned)order[n] & 3u;
        bool selected = (v->accessory.state.present & ORB_HOME_MODE) && v->accessory.state.mode == (orb_home_mode_t)i;
        detail_row(v, list, &(orb_kit_row_config_t){.title = mode_name((orb_home_mode_t)i),
            .value = selected ? ORB_ICON_CHECK : "", .glyph = glyphs[i],
            .accent = selected ? colours[i] : ORB_GRAY4}, ACTION_MODE_SET + (int)i);
    }
    list_finish(v);
}

static void scenes_build(orb_home_detail_t *v)
{
    v->name = orb_kit_rim_title_create(v->content, "Scenes", ORB_KIT_HOME);
    unsigned count = 0;
    for (unsigned i = 0; i < v->snapshot.scene_count; ++i) count += !v->snapshot.scenes[i].contains_unlock;
    if (!count) {
        orb_kit_state_create(v->content, &(orb_kit_state_config_t){.glyph = ORB_ICON_SPARKLE,
            .title = "No scenes", .detail = "Add scenes in the Home app.",
            .secondary = SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA});
        return;
    }
    lv_obj_t *list = detail_list(v, 110);
    for (unsigned i = 0; i < v->snapshot.scene_count; ++i) {
        if (v->snapshot.scenes[i].contains_unlock) continue;
        detail_row(v, list, &(orb_kit_row_config_t){.title = v->snapshot.scenes[i].name,
            .glyph = ORB_ICON_SPARKLE, .accent = ORB_KIT_HOME}, ACTION_SCENE + (int)i);
    }
    list_finish(v);
}

static void template_build(orb_home_detail_t *v)
{
    const orb_home_accessory_t *a = &v->accessory;
    bool dial = orb_home_detail_has_dial(v);
    v->name = orb_kit_rim_title_create(v->content, a->name[0] ? a->name : "Accessory", ORB_KIT_HOME);
    orb_kit_rim_title_set_max_degrees(v->name, TITLE_DEGREES);
    if (dial) {
        /* The owner chose one level vocabulary: Control Center's bottom arc. */
        v->canvas = orb_kit_level_create(v->content, ORB_KIT_HOME, 0, 100, 1, NULL, NULL);
        /* Physical provenance and shell edge arbitration still own writes. */
        lv_obj_set_clickable(v->canvas, false);
        lv_obj_set_event_bubble(v->canvas, true);
        orb_kit_ring_set_formatter(v->canvas, format_number, v);
        orb_kit_level_set_value(v->canvas, (int32_t)lroundf(v->presented * 100), false);
    } else {
        v->value = lane(v->content, ORB_TEXT_LARGE_TITLE, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 1);
        lv_label_set_long_mode(v->value, LV_LABEL_LONG_MODE_CLIP);
    }
    disc_create(v);
    if (a->kind == ORB_HOME_LOCK) {
        /* The deliberate-unlock progress ring is concentric with the disc. */
        v->canvas = orb_kit_ring_create(v->content, 57, 6, ORB_KIT_HOME);
        lv_obj_align(v->canvas, LV_ALIGN_CENTER, 0, DETAIL_DISC_CY - 233);
        lv_obj_set_hidden(v->canvas, true);
    }
    v->caption = lane(v->content, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 2);
    v->status = lane(v->content, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 1);
    if (a->state.present & (ORB_HOME_BATTERY | ORB_HOME_LOW_BATTERY))
        v->battery = lane(v->content, ORB_TEXT_FOOTNOTE1, ORB_TEXT_REGULAR, 1);
}

void orb_home_detail_build(orb_home_detail_t *v)
{
    if (v->list && v->page < 3) v->scroll_y[v->page] = lv_obj_get_scroll_y(v->list);
    if (v->content) lv_obj_delete(v->content);
    v->content = plain(v->root);
    lv_obj_set_size(v->content, 466, 466);
    lv_obj_set_event_bubble(v->content, true);
    v->canvas = v->name = v->value = v->caption = v->status = v->battery = NULL;
    v->disc = v->lock_disc = v->list = v->outcome = NULL;
    v->colour_page = v->accessory.kind == ORB_HOME_LIGHT && v->page == 1;
    v->presented = v->target = orb_home_detail_model_value(v);
    v->tick_index = (int)lroundf(v->presented *
        (v->accessory.kind == ORB_HOME_THERMOSTAT ? 40 : 100));
    v->dragging = v->holding = v->rotating = false;
    if (!v->available) {
        v->wait.kind = 0;
        orb_kit_rim_title_create(v->content, "Home", ORB_KIT_HOME);
        orb_kit_state_create(v->content, &(orb_kit_state_config_t){.glyph = ORB_ICON_HOUSE,
            .title = "Accessory not found", .detail = "It may have been removed in the Home app.",
            .secondary = SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA});
        return;
    }
    if (v->accessory.kind == ORB_HOME_THERMOSTAT && v->page == 1) modes_build(v);
    else if (v->accessory.kind == ORB_HOME_LIGHT && v->page == 2) scenes_build(v);
    else template_build(v);
    if (v->page && !orb_home_detail_has_dial(v)) {
        v->status = lane(v->content, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 1);
        lv_obj_set_width(v->status, 280);
        lv_obj_set_pos(v->status, 233 - 140, 60);
    }
    orb_home_detail_refresh(v, false);
}
