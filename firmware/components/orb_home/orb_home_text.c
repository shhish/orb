/* One Home vocabulary (HIG §§1/4/8). Five words for "offline" across the
 * detail, rows, Rooms and Control Center read as five products; every
 * surface asks this table instead of wording its own state. */
#include "orb_home_text.h"
#include "orb_icons.h"
#include "orb_kit.h"

#include "src/misc/lv_text_private.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

orb_home_status_t orb_home_status_home(const orb_home_snapshot_t *s)
{
    if (!s->online) return ORB_HOME_STATUS_OFFLINE;
    if (!s->synced) return ORB_HOME_STATUS_LOADING;
    if (!s->available) return ORB_HOME_STATUS_UNAVAILABLE;
    return ORB_HOME_STATUS_READY;
}

orb_home_status_t orb_home_status_accessory(const orb_home_snapshot_t *s,
    const orb_home_accessory_t *a, bool failure_visible)
{
    orb_home_status_t home = orb_home_status_home(s);
    if (home != ORB_HOME_STATUS_READY) return home;
    if (!a->reachable) return ORB_HOME_STATUS_NO_RESPONSE;
    /* A failure names this accessory, or the whole Home when the service
     * could not attribute it; a busy refusal keeps its own reason. */
    if (failure_visible && s->error[0] && (!s->error_id[0] || !strcmp(s->error_id, a->id)))
        return !strcmp(s->error, "Home is busy") ? ORB_HOME_STATUS_BUSY : ORB_HOME_STATUS_FAILED;
    if (a->pending) return ORB_HOME_STATUS_UPDATING;
    return ORB_HOME_STATUS_READY;
}

const char *orb_home_status_text(orb_home_status_t status)
{
    static const char *const table[] = {
        [ORB_HOME_STATUS_READY] = "",
        [ORB_HOME_STATUS_OFFLINE] = "Offline",
        [ORB_HOME_STATUS_LOADING] = "Loading…",
        [ORB_HOME_STATUS_UNAVAILABLE] = "Unavailable",
        [ORB_HOME_STATUS_NO_RESPONSE] = "No response",
        [ORB_HOME_STATUS_UPDATING] = "Updating…",
        [ORB_HOME_STATUS_FAILED] = "Couldn’t update",
        [ORB_HOME_STATUS_BUSY] = "Home is busy",
    };
    return (unsigned)status < sizeof(table) / sizeof(table[0]) ? table[status] : "";
}

bool orb_home_status_stale(orb_home_status_t status)
{
    return status == ORB_HOME_STATUS_OFFLINE || status == ORB_HOME_STATUS_LOADING ||
        status == ORB_HOME_STATUS_UNAVAILABLE || status == ORB_HOME_STATUS_NO_RESPONSE;
}

void orb_home_error_text(char *out, size_t capacity, const char *verb,
    const char *target, const char *service_error)
{
    if (service_error && !strcmp(service_error, "Home is busy"))
        snprintf(out, capacity, "Home is busy");
    else if (target && target[0]) snprintf(out, capacity, "Couldn’t %s %s", verb, target);
    else snprintf(out, capacity, "Couldn’t %s", verb);
}

void orb_home_format_temperature(char *out, size_t capacity, float celsius,
    orb_temperature_t unit, bool half_steps)
{
    float value = unit == ORB_TEMPERATURE_F ? celsius * 1.8f + 32 : celsius;
    value = fminf(999, fmaxf(-999, value)); /* never silently clamp a real target */
    bool half = false;
    if (half_steps && unit != ORB_TEMPERATURE_F) {
        value = roundf(value * 2) / 2;
        half = value != roundf(value);
    }
    if (!half) value = roundf(value);
    /* U+2212 minus, matching the tabular templates, never a hyphen. */
    snprintf(out, capacity, half ? "%s%.1f°" : "%s%.0f°", value < 0 ? "\u2212" : "", (double)fabsf(value));
}

void orb_home_format_age(char *out, size_t capacity, uint64_t updated)
{
    time_t now = time(NULL);
    out[0] = 0;
    if (!updated || now <= 0 || updated > (uint64_t)now) return;
    uint64_t minutes = ((uint64_t)now - updated) / 60;
    if (!minutes) snprintf(out, capacity, "Updated just now");
    else if (minutes < 60) snprintf(out, capacity, "Updated %lu min ago", (unsigned long)minutes);
    else if (minutes < 1440) snprintf(out, capacity, "Updated %lu hr ago", (unsigned long)(minutes / 60));
    else snprintf(out, capacity, "Updated %lu d ago", (unsigned long)(minutes / 1440));
}

bool orb_home_battery_low(const orb_home_state_t *s)
{
    /* A reported level is the authority: a stale low flag on an 80% battery
     * is not an alert (HIG §8: interrupt only for real problems). */
    if (s->present & ORB_HOME_BATTERY) return s->battery <= 20;
    return (s->present & ORB_HOME_LOW_BATTERY) && s->low_battery;
}

void orb_home_format_battery(char *out, size_t capacity, const orb_home_state_t *s)
{
    bool low = orb_home_battery_low(s);
    const char *glyph = low ? ORB_ICON_BATTERY_WARNING :
        s->battery >= 70 ? ORB_ICON_BATTERY_HIGH : ORB_ICON_BATTERY_MEDIUM;
    if (s->present & ORB_HOME_BATTERY)
        snprintf(out, capacity, "%s %.0f%%%s", glyph, (double)s->battery, low ? " · Low battery" : "");
    else snprintf(out, capacity, "%s", low ? ORB_ICON_BATTERY_WARNING " Low battery" : "");
}

const char *orb_home_kind_name(orb_home_kind_t kind)
{
    switch (kind) {
    case ORB_HOME_LIGHT: return "Light";
    case ORB_HOME_SWITCH: return "Switch";
    case ORB_HOME_OUTLET: return "Outlet";
    case ORB_HOME_THERMOSTAT: return "Thermostat";
    case ORB_HOME_LOCK: return "Lock";
    case ORB_HOME_FAN: return "Fan";
    case ORB_HOME_TEMPERATURE: return "Temperature";
    case ORB_HOME_HUMIDITY: return "Humidity";
    case ORB_HOME_MOTION: return "Motion";
    case ORB_HOME_CONTACT: return "Contact";
    case ORB_HOME_SENSOR: return "Sensor";
    case ORB_HOME_DOORBELL: return "Doorbell";
    case ORB_HOME_BUTTON: return "Button";
    case ORB_HOME_COVER: return "Blinds";
    default: return "Accessory";
    }
}

void orb_home_fit_name(lv_obj_t *label, const char *name)
{
    /* Names wrap to the lane's lines first (HIG §4: never cut mid-word when
     * the whole name fits). Only an overlong name falls back to the kit's
     * middle elision, which keeps "… East" and "… West" distinguishable. */
    lv_point_t size;
    lv_obj_update_layout(label);
    lv_text_get_size(&size, name, lv_obj_get_style_text_font(label, 0),
        lv_obj_get_style_text_letter_space(label, 0), lv_obj_get_style_text_line_space(label, 0),
        lv_obj_get_width(label), LV_TEXT_FLAG_NONE);
    if (size.y <= lv_obj_get_height(label)) lv_label_set_text(label, name);
    else orb_kit_label_middle_ellipsis(label, name);
}
