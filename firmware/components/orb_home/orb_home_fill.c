#include "orb_ui.h"
#include "orb_home_view.h"
#include "orb_home_detail.h"
#include "orb_kit.h"
#include <string.h>
#include <strings.h>

/* One font source for all accessory identities (HIG §§4/5). State changes
 * only select a filled variant; rows, details and Control Center agree. */
static bool garage_name(const char *name)
{
    for (const char *c = name; *c; c++)
        if (!strncasecmp(c, "garage", 6) && (c == name || c[-1] == ' ') &&
            (!c[6] || c[6] == ' ')) return true;
    return false;
}

const char *orb_home_accessory_icon(const orb_home_accessory_t *a)
{
    bool on = (a->state.present & ORB_HOME_ON) && a->state.on;
    if (a->kind == ORB_HOME_LIGHT && (a->state.present & ORB_HOME_BRIGHTNESS) && a->state.brightness <= 0)
        on = false;
    switch (a->kind) {
    case ORB_HOME_LIGHT: return on ? ORB_ICON_LIGHTBULB_FILL : ORB_ICON_LIGHTBULB;
    case ORB_HOME_SWITCH: return ORB_ICON_TOGGLE;
    case ORB_HOME_OUTLET: return ORB_ICON_PLUG;
    case ORB_HOME_LOCK: return (a->state.present & ORB_HOME_LOCKED) && !a->state.locked ?
        ORB_ICON_LOCK_OPEN : ORB_ICON_LOCK_FILL;
    case ORB_HOME_FAN: return ORB_ICON_FAN;
    case ORB_HOME_THERMOSTAT:
    case ORB_HOME_TEMPERATURE: return ORB_ICON_THERMOMETER;
    case ORB_HOME_HUMIDITY: return ORB_ICON_DROP;
    case ORB_HOME_COVER: return garage_name(a->name) ? ORB_ICON_GARAGE :
        (a->state.present & ORB_HOME_POSITION) && a->state.position > 0 ? ORB_ICON_BLINDS_FILL : ORB_ICON_BLINDS;
    case ORB_HOME_MOTION: return ORB_ICON_WALK;
    case ORB_HOME_CONTACT: return garage_name(a->name) ? ORB_ICON_GARAGE : ORB_ICON_DOOR;
    case ORB_HOME_DOORBELL: return ORB_ICON_BELL;
    case ORB_HOME_BUTTON: return ORB_ICON_HAND_TAP;
    case ORB_HOME_SENSOR:
        if (a->state.present & ORB_HOME_TEMP) return ORB_ICON_THERMOMETER;
        if (a->state.present & ORB_HOME_HUMIDITY_VALUE) return ORB_ICON_DROP;
        if (a->state.present & ORB_HOME_MOTION_VALUE) return ORB_ICON_WALK;
        if (a->state.present & ORB_HOME_CONTACT_VALUE) return garage_name(a->name) ? ORB_ICON_GARAGE : ORB_ICON_DOOR;
        return ORB_ICON_SENSOR;
    case ORB_HOME_OTHER: return ORB_ICON_SENSOR;
    default: return ORB_ICON_QUESTION;
    }
}

const char *orb_home_room_icon(const char *name)
{
    static const struct { const char *word, *glyph; } icons[] = {
        {"bath", ORB_ICON_DROP}, {"wash", ORB_ICON_DROP},
        {"kitchen", ORB_ICON_FLAME}, {"garage", ORB_ICON_GARAGE},
        {"office", ORB_ICON_DESKTOP}, {"study", ORB_ICON_DESKTOP},
        {"bed", ORB_ICON_MOON}, {"outdoor", ORB_ICON_SUN}, {"entry", ORB_ICON_DOOR},
        {"door", ORB_ICON_DOOR}, {"foyer", ORB_ICON_DOOR}, {"hall", ORB_ICON_DOOR},
        {"garden", ORB_ICON_SUN}, {"patio", ORB_ICON_SUN}, {"yard", ORB_ICON_SUN},
        {"porch", ORB_ICON_SUN},
    };
    for (unsigned i = 0; name && i < sizeof(icons) / sizeof(icons[0]); i++)
        for (const char *c = name; *c; c++)
            if (!strncasecmp(c, icons[i].word, strlen(icons[i].word))) return icons[i].glyph;
    return ORB_ICON_HOUSE;
}

lv_color_t orb_home_accessory_colour(const orb_home_accessory_t *a, bool online)
{
    const orb_home_state_t *s = &a->state;
    if (!online || !a->reachable) return ORB_GRAY6;
    if (a->kind == ORB_HOME_THERMOSTAT) {
        if (!(s->present & ORB_HOME_MODE) || s->mode == ORB_HOME_MODE_OFF) return ORB_GRAY6;
        /* Each mode has its own meaning: heat orange, cool blue, auto green. */
        return s->mode == ORB_HOME_MODE_COOL ? ORB_BLUE : s->mode == ORB_HOME_MODE_AUTO ? ORB_GREEN : ORB_ORANGE;
    }
    if (a->kind == ORB_HOME_LOCK)
        return (s->present & ORB_HOME_LOCKED) && !s->locked ? ORB_KIT_HOME : ORB_GRAY6;
    if (a->kind == ORB_HOME_COVER)
        return (s->present & ORB_HOME_POSITION) && s->position > 0 ? ORB_KIT_HOME : ORB_GRAY6;
    if ((s->present & ORB_HOME_MOTION_VALUE) && (a->kind == ORB_HOME_MOTION || a->kind == ORB_HOME_SENSOR))
        return s->motion ? ORB_KIT_HOME : ORB_GRAY6;
    if ((s->present & ORB_HOME_CONTACT_VALUE) && (a->kind == ORB_HOME_CONTACT || a->kind == ORB_HOME_SENSOR))
        return s->contact ? ORB_KIT_HOME : ORB_GRAY6;
    if (!(s->present & ORB_HOME_ON) || !s->on) return ORB_GRAY6;
    if (a->kind == ORB_HOME_LIGHT)
        return (s->present & ORB_HOME_BRIGHTNESS) && s->brightness <= 0 ? ORB_GRAY6 : orb_home_light_colour(s);
    /* Fans, switches and outlets share Home's on tint; teal is Time's. */
    return ORB_YELLOW;
}

lv_color_t orb_home_disc_colour(const orb_home_accessory_t *a, bool online)
{
    lv_color_t colour = orb_home_accessory_colour(a, online);
    /* Inactive discs are a visible neutral, matching kit rows (HIG §5). */
    return lv_color_eq(colour, ORB_GRAY6) ? ORB_GRAY4 : colour;
}

const char *orb_home_accessory_name(const orb_home_snapshot_t *model,
                                  const orb_home_accessory_t *a, bool trim_room)
{
    if (!a->name[0]) return "Unnamed device";
    if (!trim_room) return a->name;
    for (unsigned i = 0; i < model->room_count; ++i) {
        if (strcmp(model->rooms[i].id, a->room)) continue;
        size_t n = strlen(model->rooms[i].name);
        if (!n || strncasecmp(a->name, model->rooms[i].name, n) || a->name[n] != ' ') break;
        const char *name = a->name + n;
        while (*name == ' ' || *name == '-' || *name == ':') ++name;
        if (!strncmp(name, "–", 3) || !strncmp(name, "—", 3)) name += 3;
        while (*name == ' ') ++name;
        if (*name) return name;
    }
    return a->name;
}
lv_obj_t *orb_home_identity_disc(lv_obj_t *parent, int kind,
    const orb_home_accessory_t *a, lv_color_t fill)
{
    return orb_kit_disc_create(parent, (orb_kit_disc_kind_t)kind, ORB_KIT_ACCENT_FILL, fill,
        orb_home_accessory_icon(a));
}
