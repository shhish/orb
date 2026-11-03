/* Frozen public model states and real production presenters. No board/storage. */
#include "parity.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_faces.h"
#include "orb_fonts.h"
#include "orb_game_view.h"
#include "orb_home.h"
#include "orb_home_view.h"
#include "orb_time.h"
#include "orb_time_view.h"
#include "orb_ui.h"

#define WALL ((time_t)1793196300)
static orb_faces_t *s_faces;
static orb_home_view_t *s_home;
static orb_time_view_t *s_time;
static orb_game_view_t *s_game_view;
static orb_game_state_t s_game;
static bool s_game_save_error, s_game_paused;
static unsigned s_game_actions[ORB_GAME_STEER_STOP + 1];
static time_t s_time_wall;
static int64_t s_time_offset;
static bool s_time_keep_pending;
void parity_storage_reset(void);
void parity_storage_fail(bool failed);

void parity_apps_poll(void)
{
    if (s_home) orb_home_view_poll(s_home);
    if (s_time) {
        orb_time_poll((int64_t)parity_now * 1000 + s_time_offset,
            s_time_wall ? s_time_wall + parity_now / 1000 : 0);
        if (!s_time_keep_pending) orb_time_persist_service();
        orb_time_view_poll(s_time);
    }
    if (s_game_view) orb_game_view_update(s_game_view, &s_game, 120, s_game_paused, s_game_save_error);
}

static void face_open(orb_faces_target_t target, void *context)
{
    (void)context;
    char detail[32]; snprintf(detail, sizeof(detail), "target=%d", target);
    parity_event("face-open", detail);
}
static bool face_persist(const orb_faces_prefs_t *preferences, void *context)
{
    (void)preferences;
    bool success = context == NULL;
    parity_event("face-save", success ? "saved" : "error");
    return success;
}
static void faces(const char *id)
{
    if (strstr(id, "lock") || strstr(id, "always-on")) {
        orb_faces_t *preferences = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.preview = true, .persist = face_persist});
        parity_check(preferences != NULL, "prepare public face preference");
        orb_faces_select(preferences, strstr(id, "meridian") ? ORB_FACE_MERIDIAN :
            strstr(id, "activity") ? ORB_FACE_ACTIVITY : ORB_FACE_ORB);
        parity_check(orb_faces_save(preferences), "retain selected public face");
        /* A preview never commits storage. Commit the saved choice exactly as
         * a successful device save does, so Lock reads the chosen face. */
        orb_faces_prefs_t chosen; orb_faces_preferences(preferences, &chosen);
        parity_check(orb_settings_faces_accept(&chosen), "commit selected public face");
        orb_faces_destroy(preferences);
        parity_shell_create(true);
        orb_shell_power_enable(parity_shell, true);
        if (strstr(id, "always-on")) orb_shell_set_quiet(parity_shell, true);
        else {
            const orb_button_event_t event = {.button = ORB_BUTTON_PWR, .gesture = ORB_BUTTON_SHORT, .at_ms = parity_now};
            orb_shell_button(parity_shell, &event);
        }
        parity_advance(800);
        orb_power_snapshot_t power = orb_shell_power_snapshot(parity_shell);
        parity_check(power.redact && power.state == ORB_POWER_ALWAYS_ON, "public privacy projection active");
        parity_event("privacy", strstr(id, "always-on") ? "always-on-public-face quiet=true" : "locked-public-face quiet=false");
        return;
    }
    orb_faces_config_t config = {.open = face_open, .persist = face_persist};
    if (strstr(id, "save-error")) config.context = &s_faces;
    s_faces = orb_faces_create(lv_screen_active(), &config);
    parity_check(s_faces != NULL, "create real Faces presenter");
    parity_set_root(orb_faces_root(s_faces));
    orb_face_kind_t kind = strstr(id, "meridian") ? ORB_FACE_MERIDIAN :
        strstr(id, "activity") ? ORB_FACE_ACTIVITY : ORB_FACE_ORB;
    orb_faces_select(s_faces, kind);
    orb_faces_snapshot_t data = {.now = WALL, .usage_percent = {62, 38},
        .battery_percent = 80, .home_on = 4, .home_total = 12, .working = 2,
        .online = true, .next_seconds = 762, .next_total_seconds = 1500};
    strcpy(data.clock, "10:05"); strcpy(data.weekday, "WED"); strcpy(data.day, "28");
    if (strstr(id, "offline")) data.online = false;
    if (strstr(id, "unknown")) {
        data.usage_percent[0] = data.usage_percent[1] = -1;
        data.battery_percent = -1; data.next_seconds = 0; data.working = 0;
        data.home_total = data.home_on = 0;
    }
    if (strstr(id, "weather")) {
        data.weather_override = true;
        data.weather = (orb_weather_snapshot_t){.revision = 7, .configured = true,
            .available = strstr(id, "unavailable") == NULL, .stale = strstr(id, "stale") != NULL,
            .unit = ORB_TEMPERATURE_F, .temperature = 72, .high = 78, .low = 62,
            .condition = ORB_WEATHER_PARTLY_CLOUDY, .is_day = true, .updated_at = 1793182140};
        strcpy(data.weather.place, "Fixture Town"); strcpy(data.weather.label, "Partly cloudy");
        if (strstr(id, "unavailable")) {
            /* protocol.md: unavailable snapshots carry no readings and the
             * provider's status label, never a leftover condition. */
            data.weather.temperature = data.weather.high = data.weather.low = NAN;
            data.weather.condition = ORB_WEATHER_UNKNOWN; data.weather.stale = true;
            strcpy(data.weather.label, "Weather unavailable");
        }
    }
    if (strstr(id, "provider-next")) {
        orb_faces_prefs_t prefs; orb_faces_preferences(s_faces, &prefs);
        prefs.slot[0] = ORB_FACE_NEXT; orb_faces_set_preferences(s_faces, &prefs);
    }
    orb_faces_update(s_faces, &data);
    if (strstr(id, "gallery")) orb_faces_set_mode(s_faces, ORB_FACES_GALLERY);
    if (strstr(id, "colour")) orb_faces_set_mode(s_faces, ORB_FACES_COLOUR);
    if (strstr(id, "slots")) {
        orb_faces_set_mode(s_faces, ORB_FACES_SLOTS);
        if (strstr(id, "slot-2")) orb_faces_select_slot(s_faces, 1);
    }
    if (strstr(id, "save-error")) {
        orb_faces_turn(s_faces, 1);
        parity_check(orb_faces_back(s_faces) && orb_faces_save_error(s_faces), "inject explicit face storage failure");
    }
    if (strstr(id, "restore")) {
        orb_faces_prefs_t preferences; orb_faces_preferences(s_faces, &preferences);
        orb_faces_set_mode(s_faces, ORB_FACES_COLOUR); orb_faces_rotate(s_faces, 2);
        orb_faces_set_preferences(s_faces, &preferences); orb_faces_set_mode(s_faces, ORB_FACES_GALLERY);
        parity_event("context", "faces-preferences-restored-without-save");
    }
    parity_advance(800);
}

static const orb_home_kind_t s_home_kinds[] = {
    ORB_HOME_LIGHT, ORB_HOME_SWITCH, ORB_HOME_OUTLET, ORB_HOME_THERMOSTAT,
    ORB_HOME_LOCK, ORB_HOME_FAN, ORB_HOME_TEMPERATURE, ORB_HOME_HUMIDITY,
    ORB_HOME_MOTION, ORB_HOME_CONTACT, ORB_HOME_SENSOR, ORB_HOME_DOORBELL,
    ORB_HOME_BUTTON, ORB_HOME_COVER, ORB_HOME_OTHER, ORB_HOME_UNKNOWN,
};
static const char *const s_home_ids[] = {
    "light", "switch", "outlet", "thermostat", "lock", "fan", "temperature", "humidity",
    "motion", "contact", "sensor", "doorbell", "button", "cover", "other", "unknown",
};
/* Owner-style names, so captures read like a real Home rather than echoing
 * fixture identifiers ("LIVING UNKNOWN"); identifiers stay the stable keys. */
static const char *const s_home_names[] = {
    "Floor Lamp", "Desk Switch", "Kettle Outlet", "Thermostat", "Front Door", "Ceiling Fan",
    "Temperature Sensor", "Humidity Sensor", "Hallway Motion", "Window Sensor", "Air Sensor",
    "Doorbell", "Smart Button", "Blinds", "Air Purifier", "Bridge",
};
static void home_fixture(orb_home_snapshot_t *data)
{
    memset(data, 0, sizeof(*data));
    data->online = data->synced = data->available = true;
    data->revision = 42; data->room_count = 2;
    data->rooms[0] = (orb_home_room_t){"living", "Living Room"};
    data->rooms[1] = (orb_home_room_t){"office", "Office"};
    data->scene_count = 3;
    data->scenes[0] = (orb_home_scene_t){.id = "evening", .name = "Evening", .favourite = true};
    data->scenes[1] = (orb_home_scene_t){.id = "focus", .name = "Focus", .favourite = true};
    data->scenes[2] = (orb_home_scene_t){.id = "unsafe", .name = "Unlock all", .contains_unlock = true};
    data->accessory_count = sizeof(s_home_kinds) / sizeof(s_home_kinds[0]);
    for (unsigned i = 0; i < data->accessory_count; ++i) {
        orb_home_accessory_t *accessory = &data->accessories[i];
        snprintf(accessory->id, sizeof(accessory->id), "%s", s_home_ids[i]);
        snprintf(accessory->name, sizeof(accessory->name), "%s", s_home_names[i]);
        strcpy(accessory->room, "living"); accessory->kind = s_home_kinds[i];
        accessory->reachable = accessory->favourite = true; accessory->updated = WALL;
        accessory->state = (orb_home_state_t){.on = true, .brightness = 62,
            .hue = 210, .sat = 65, .ct = 350, .temp = 22, .target = 23,
            .humidity = 45, .speed = 60, .position = 40, .locked = true,
            .contact = true, .motion = true, .battery = 80, .mode = ORB_HOME_MODE_AUTO};
        switch (accessory->kind) {
        case ORB_HOME_LIGHT: accessory->state.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS | ORB_HOME_HUE | ORB_HOME_SAT | ORB_HOME_CT; break;
        case ORB_HOME_SWITCH: case ORB_HOME_OUTLET: accessory->state.present = ORB_HOME_ON; break;
        case ORB_HOME_THERMOSTAT: accessory->state.present = ORB_HOME_TEMP | ORB_HOME_TARGET | ORB_HOME_MODE; break;
        case ORB_HOME_FAN: accessory->state.present = ORB_HOME_ON | ORB_HOME_SPEED; break;
        case ORB_HOME_LOCK: accessory->state.present = ORB_HOME_LOCKED; break;
        case ORB_HOME_TEMPERATURE: accessory->state.present = ORB_HOME_TEMP; break;
        case ORB_HOME_HUMIDITY: accessory->state.present = ORB_HOME_HUMIDITY_VALUE; break;
        case ORB_HOME_MOTION: accessory->state.present = ORB_HOME_MOTION_VALUE; break;
        case ORB_HOME_CONTACT: accessory->state.present = ORB_HOME_CONTACT_VALUE; break;
        case ORB_HOME_COVER: accessory->state.present = ORB_HOME_POSITION; break;
        default: accessory->state.present = ORB_HOME_BATTERY; break;
        }
        accessory->writable = accessory->state.present & ORB_HOME_WRITABLE;
    }
}
static void home(const char *id)
{
    orb_home_snapshot_t *data = calloc(1, sizeof(*data)); parity_check(data != NULL, "allocate Home fixture");
    home_fixture(data);
    unsigned selected = 0;
    for (unsigned i = 0; i < sizeof(s_home_ids) / sizeof(s_home_ids[0]); ++i) {
        char prefix[64]; snprintf(prefix, sizeof(prefix), "home-%s", s_home_ids[i]);
        size_t length = strlen(prefix);
        if (!strncmp(id, prefix, length) && (id[length] == 0 || id[length] == '-')) { selected = i; break; }
    }
    if (strstr(id, "climate-empty") || strstr(id, "sensors-empty")) {
        // The room must still exist: Home falls back to Rooms for a room that
        // is not in the topology, so only the section's own accessories go.
        // A thermostat's reading also counts as a room sensor, so both go.
        unsigned kept = 0;
        for (unsigned i = 0; i < data->accessory_count; ++i) {
            orb_home_kind_t kind = data->accessories[i].kind;
            bool sensor = kind == ORB_HOME_TEMPERATURE || kind == ORB_HOME_HUMIDITY ||
                          kind == ORB_HOME_MOTION || kind == ORB_HOME_CONTACT;
            if (kind == ORB_HOME_THERMOSTAT || sensor) continue;
            data->accessories[kept++] = data->accessories[i];
        }
        data->accessory_count = kept;
    } else if (strstr(id, "scenes-empty")) {
        data->scene_count = 0;
    } else if (strstr(id, "empty") || strstr(id, "service-unavailable")) {
        data->room_count = data->accessory_count = data->scene_count = 0;
    }
    if (strstr(id, "long")) {
        strcpy(data->rooms[0].name, "A very long living room name");
        strcpy(data->accessories[selected].name, "Very long living room lamp name");
        strcpy(data->scenes[0].name, "An unusually long evening scene");
    }
    if (strstr(id, "unavailable")) data->accessories[selected].reachable = false;
    if (strstr(id, "off" ) && !strstr(id, "offline")) data->accessories[selected].state.on = false;
    if (strstr(id, "reading-unknown")) {
        data->accessories[selected].state.present = 0; data->accessories[selected].writable = 0;
    }
    if (strstr(id, "white-only")) {
        data->accessories[0].state.present &= ~(ORB_HOME_HUE | ORB_HOME_SAT);
        data->accessories[0].writable &= ~(ORB_HOME_HUE | ORB_HOME_SAT);
    }
    if (strstr(id, "low-battery")) {
        data->accessories[selected].state.present |= ORB_HOME_LOW_BATTERY;
        data->accessories[selected].state.low_battery = true;
    }
    if (strstr(id, "aged")) data->accessories[selected].updated = UINT64_C(1793182140) - 7200;
    if (strstr(id, "motion-clear")) data->accessories[selected].state.motion = false;
    if (strstr(id, "contact-closed")) data->accessories[selected].state.contact = false;
    if (strstr(id, "thermostat-heat")) data->accessories[selected].state.mode = ORB_HOME_MODE_HEAT;
    if (strstr(id, "thermostat-cool")) data->accessories[selected].state.mode = ORB_HOME_MODE_COOL;
    if (strstr(id, "thermostat-off")) data->accessories[selected].state.mode = ORB_HOME_MODE_OFF;
    if (strstr(id, "target-unknown")) {
        data->accessories[selected].state.present &= ~ORB_HOME_TARGET;
        data->accessories[selected].writable &= ~ORB_HOME_TARGET;
    }
    if (strstr(id, "current-unknown")) data->accessories[selected].state.present &= ~ORB_HOME_TEMP;
    if (strstr(id, "light-missing")) {
        memmove(data->accessories, data->accessories + 1, (data->accessory_count - 1) * sizeof(data->accessories[0]));
        --data->accessory_count;
    }
    if (strstr(id, "unlocked")) data->accessories[4].state.locked = false;
    orb_home_reset(); parity_check(orb_home_init(), "initialize isolated Home service");
    bool loading = strstr(id, "loading") != NULL;
    if (!loading) parity_check(orb_home_snapshot_chunk_available(data->rooms, data->room_count,
        data->accessories, data->accessory_count, data->scenes, data->scene_count,
        !strstr(id, "service-unavailable"), true, false), "publish fixed Home snapshot");
    orb_home_set_connection(!strstr(id, "offline"), !loading && !strstr(id, "waiting"));
    if ((strstr(id, "pending") || strstr(id, "error")) && !strstr(id, "light-scenes")) {
        const char *target = strstr(id, "scene") ? "evening" : s_home_ids[selected];
        orb_home_state_t values = data->accessories[selected].state;
        values.present = data->accessories[selected].writable; values.on = false;
        values.brightness = 45; values.target = 24; values.locked = false; values.speed = 45; values.position = 55;
        bool accepted = strstr(id, "scene") ? orb_home_scene(target) : orb_home_set(target, &values, true);
        parity_check(accepted, "enqueue fixture Home command");
        if (strstr(id, "error")) {
            orb_home_request_t request; parity_check(orb_home_take_request(&request), "take fixture request");
            orb_home_complete(&request, NULL, "Accessory did not respond");
        }
    }
    s_home = orb_home_view_create(lv_screen_active(), NULL);
    parity_check(s_home != NULL, "create production Home view"); parity_set_root(orb_home_view_root(s_home));
    orb_home_view_state_t context = {.surface = ORB_HOME_ROOMS};
    strcpy(context.room, "living");
    if (strstr(id, "room-") || !strcmp(id, "home-room")) context.surface = ORB_HOME_ROOM;
    if (strstr(id, "climate") || strstr(id, "sensors")) context.surface = ORB_HOME_CLIMATE;
    context.sections = context.surface == ORB_HOME_ROOM && strstr(id, "sections") != NULL;
    context.sensors = strstr(id, "sensors") != NULL;
    context.scene_page = !strstr(id, "light-scenes") && (strstr(id, "scenes") != NULL || strstr(id, "scene-pending") || strstr(id, "scene-error"));
    for (unsigned i = 0; i < sizeof(s_home_ids) / sizeof(s_home_ids[0]); ++i) {
        char prefix[64]; snprintf(prefix, sizeof(prefix), "home-%s", s_home_ids[i]);
        if (!strncmp(id, prefix, strlen(prefix)) &&
            (id[strlen(prefix)] == 0 || id[strlen(prefix)] == '-')) {
            strcpy(context.selected, s_home_ids[i]); break;
        }
    }
    if (strstr(id, "light-colour") || strstr(id, "white-only")) context.detail_page = 1;
    if (strstr(id, "light-scenes")) context.detail_page = 2;
    if (strstr(id, "thermostat-modes")) context.detail_page = 1;
    orb_home_view_restore(s_home, &context);
    if (strstr(id, "restore")) {
        orb_home_view_save(s_home, &context); orb_home_view_select(s_home, ORB_HOME_CLIMATE);
        orb_home_view_restore(s_home, &context); parity_event("context", "home-public-context-restored");
    }
    parity_event("fixture", "home-public-snapshot-fixed-order");
    free(data);
    parity_advance(800);
    if (strstr(id, "light-scenes-pending") || strstr(id, "light-scenes-error") || strstr(id, "light-scenes-ack")) {
        parity_check(parity_tap_text("Evening"), "physically request detail-page scene"); parity_advance(160);
        orb_home_request_t request;
        parity_check(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SCENE && !strcmp(request.id, "evening"),
            "light detail scene callback retains scene target");
        if (!strstr(id, "pending")) {
            orb_home_complete(&request, NULL, strstr(id, "error") ? "Scene unavailable" : NULL);
            parity_advance(160);
        }
        orb_home_view_state_t restored; orb_home_view_save(s_home, &restored);
        parity_check(restored.detail_page == 2 && !strcmp(restored.selected, "light"), "scene feedback stays on light scene page");
    }
}

static void time_app(const char *id)
{
    parity_storage_reset();
    if (strstr(id, "load-error")) parity_storage_fail(true);
    s_time_keep_pending = strstr(id, "pending") != NULL;
    orb_time_init(); s_time_wall = strstr(id, "no-clock") && !strstr(id, "reminder") ? 0 : WALL;
    orb_time_poll((int64_t)parity_now * 1000, s_time_wall);
    orb_time_set_clock_format(strstr(id, "12h") == NULL);
    orb_time_page_t page = strstr(id, "stopwatch") ? ORB_TIME_PAGE_STOPWATCH :
        strstr(id, "alarm") ? ORB_TIME_PAGE_ALARM : strstr(id, "reminder") ?
        ORB_TIME_PAGE_REMINDER : ORB_TIME_PAGE_TIMER;
    int64_t now = (int64_t)parity_now * 1000;
    if (page == ORB_TIME_PAGE_TIMER) {
        parity_check(orb_time_timer_set(strstr(id, "hour") ? 5400 : 1500), "set fixed timer");
        if (strstr(id, "running") || strstr(id, "paused") || strstr(id, "due")) {
            parity_check(orb_time_timer_start(now), "start fixture timer");
            if (strstr(id, "paused")) orb_time_timer_pause(now + 122000000);
            if (strstr(id, "due")) { s_time_offset = INT64_C(1501000000); orb_time_poll(now + s_time_offset, WALL + 1501); }
        }
    } else if (page == ORB_TIME_PAGE_STOPWATCH && !strstr(id, "idle")) {
        parity_check(orb_time_stopwatch_start(now), "start fixture stopwatch");
        if (strstr(id, "laps")) for (unsigned i = 1; i <= 8; ++i)
            parity_check(orb_time_stopwatch_lap(now + i * 10000000), "record fixed laps");
        s_time_offset = strstr(id, "hour") ? INT64_C(3722340000) : INT64_C(122340000);
        if (strstr(id, "paused")) orb_time_stopwatch_pause(now + s_time_offset);
    } else if (page == ORB_TIME_PAGE_ALARM) {
        orb_time_alarm_set(strstr(id, "12h-pm") ? 22 : 10, 6);
        if (strstr(id, "weekdays")) orb_time_alarm_repeat_set(ORB_TIME_REPEAT_WEEKDAYS, ORB_TIME_DAYS_WEEKDAYS);
        if (strstr(id, "custom-days")) orb_time_alarm_repeat_set(ORB_TIME_REPEAT_CUSTOM, ORB_TIME_DAYS_WEEKDAYS);
        if (strstr(id, "enabled") || strstr(id, "ringing")) {
            parity_check(orb_time_alarm_enable(true, WALL), "arm fixture alarm");
            if (strstr(id, "ringing")) {
                orb_time_snapshot_t snapshot; orb_time_snapshot(now, &snapshot);
                s_time_wall = snapshot.alarm_next;
                orb_time_poll(now, s_time_wall);
            }
        }
    } else if (page == ORB_TIME_PAGE_REMINDER && !strstr(id, "empty")) {
        uint32_t reminder;
        const char *title = strstr(id, "long") ? "Take the morning medicine and bring the delivery upstairs" : "Take the medicine";
        parity_check(orb_time_reminder_set(title, WALL + 3600, &reminder), "create fixed reminder");
        if (strstr(id, "due")) {
            s_time_wall = WALL + 3601; orb_time_poll(now, s_time_wall);
        }
    }
    if (strstr(id, "reminder") && strstr(id, "no-clock")) {
        s_time_wall = 0; orb_time_poll(now, 0);
    }
    if (strstr(id, "save-error")) parity_storage_fail(true);
    s_time = orb_time_view_create(lv_screen_active(), NULL); parity_check(s_time != NULL, "create Time presenter");
    parity_set_root(orb_time_view_root(s_time));
    orb_time_view_context_t context = {.page = page};
    if (strstr(id, "quick")) context.sheet = 1;
    if (strstr(id, "options") || strstr(id, "weekdays")) context.sheet = 2;
    if (strstr(id, "custom-days")) context.sheet = 3;
    if (strstr(id, "custom-editor")) { context.field = 2; context.fields[page] = 2; }
    if (strstr(id, "snooze-editor")) {
        // A real visit scrolls Snooze into view; an offscreen row yields the rim to scroll.
        context.sheet = 2; context.field = 4; context.sheet_scroll[page] = 10000;
    }
    if (strstr(id, "hour-editor")) { context.field = 0; context.fields[page] = 0; }
    if (strstr(id, "minute-editor")) { context.field = 1; context.fields[page] = 1; }
    if (strstr(id, "detail")) { orb_reminder_t reminder;
        parity_check(orb_time_reminder_get(0, &reminder), "select actual reminder identity");
        context.reminder_detail = true; context.reminder_id = reminder.id;
    }
    orb_time_view_restore(s_time, &context);
    if (strstr(id, "restore")) {
        orb_time_view_save(s_time, &context); orb_time_view_select(s_time, ORB_TIME_PAGE_STOPWATCH);
        orb_time_view_restore(s_time, &context); parity_event("context", "time-public-context-restored");
    }
    parity_apps_poll();
    parity_advance(800);
    orb_time_snapshot_t result; orb_time_snapshot(orb_time_now_us(), &result);
    if (strstr(id, "ringing")) parity_check(result.alarm_ringing, "actual alarm ringing latch");
    if (strstr(id, "timer-due")) parity_check(result.timer_state == ORB_TIME_FINISHED, "actual timer due latch");
    if (strstr(id, "reminders-due")) parity_check(result.reminder_due, "actual reminder due latch");
    if (strstr(id, "error")) parity_check(result.save_error, "actual service storage error");
    if (strstr(id, "pending")) parity_check(result.saving, "actual service save pending");
    if (strstr(id, "snooze-editor")) {
        orb_time_rotary_t owner; parity_check(orb_time_view_rotary(s_time, &owner) && owner.minimum == 1 && owner.maximum == 60,
            "Snooze editor binds minutes rather than Repeat");
    }
    if (strstr(id, "snooze-failed")) {
        parity_check(parity_tap_text("In 1 hour"), "physically attempt reminder snooze while clock offline");
        parity_advance(160);
        orb_time_snapshot_t unavailable; orb_time_snapshot(orb_time_now_us(), &unavailable);
        parity_check(!unavailable.clock_valid && unavailable.reminder_count == 1,
            "failed snooze retains reminder without inventing deadline");
        parity_event("reminder-result", "clock-offline-snooze-failed");
    }
    if (strstr(id, "scrolled")) {
        lv_obj_t *list = orb_time_view_scroll(s_time); parity_check(list != NULL, "stopwatch owns real lap viewport");
        /* The newest-split capsule is the visible Laps action. */
        parity_check(parity_tap_text("Lap 8 · 00:10.00"), "physically open the pinned laps");
        parity_advance(600);
        parity_check(lv_obj_get_scroll_y(list) >= 140, "stopwatch enters pinned elapsed header state");
    }
}

static void game_action(int action, float x, float y, void *context)
{
    (void)context;
    char detail[96]; snprintf(detail, sizeof(detail), "action=%d x=%.3f y=%.3f", action, (double)x, (double)y);
    parity_event("game-action", detail);
    if (action >= 0 && action <= ORB_GAME_STEER_STOP) ++s_game_actions[action];
    if (action != ORB_GAME_CLOSE) orb_game_action(&s_game, (orb_game_action_t)action, x, y);
}
static void game(const char *id)
{
    if (!strcmp(id, "play-picker")) {
        /* Navigation settles like every other routed shell scenario; without
         * the advance the capture showed the outgoing Face, not the picker. */
        /* The production catalogue must be registered or the shell shows
         * its empty "No games installed" state instead of the picker. */
        orb_apps_init(ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_AUDIO | ORB_APP_NEEDS_STORAGE);
        parity_shell_create(true); parity_navigate(ORB_SHELL_PLAY); parity_advance(720);
        return;
    }
    orb_game_kind_t kind = strstr(id, "marble") ? ORB_GAME_MARBLE : strstr(id, "beads") ?
        ORB_GAME_BEADS : strstr(id, "orbit") ? ORB_GAME_ORBIT : strstr(id, "snake") ? ORB_GAME_SNAKE : ORB_GAME_POP;
    orb_game_init(&s_game, kind, UINT32_C(0x4f5242));
    s_game.started = strstr(id, "start") == NULL;
    s_game.user_paused = strstr(id, "pause") != NULL;
    s_game_paused = false; /* user pause belongs to the real game state; external overlays are absent. */
    s_game.game_over = strstr(id, "result") != NULL;
    s_game.controls = strstr(id, "controls") != NULL;
    if (kind == ORB_GAME_SNAKE && strstr(id, "won")) s_game.game.snake.won = true;
    s_game.score = strstr(id, "long") ? 999999 : 10; s_game.lives = 1;
    s_game.motion_unavailable = strstr(id, "motion-unavailable") != NULL;
    s_game.touch_fallback_hint = s_game.motion_unavailable;
    s_game.tilt_calibrating = strstr(id, "calibrating") != NULL;
    s_game_save_error = strstr(id, "save-error") != NULL;
    if (kind == ORB_GAME_BEADS && !strstr(id, "no-clock")) {
        orb_game_beads_set_time(&s_game, 10, 5);
        if (strstr(id, "clock")) {
            /* The clock forms only after sampled, gyro-confirmed stillness, as
             * on the stand. Feed the production quiet detector an upright,
             * motionless IMU; stepping without it never left the fluid. */
            orb_game_tilt_t tilt;
            orb_game_tilt_reset(&tilt);
            const float gyro[3] = {0, 0, 0};
            for (unsigned i = 0; i < 240; ++i) {
                float ax = 0, ay = 0;
                (void)orb_game_gravity_update_motion(&tilt, 0, 1, 0, gyro, ORB_GAME_STEP_US, &ax, &ay);
                orb_game_set_motion(&s_game, &tilt);
                orb_game_step(&s_game, ax, ay, true, orb_ui_reduce_motion());
            }
            parity_check(s_game.game.beads.settled, "quiet stand forms the bead clock");
        }
    }
    if (kind == ORB_GAME_MARBLE) {
        s_game.game.marble.stage_cleared = strstr(id, "clear") != NULL;
        if (s_game.game.marble.stage_cleared) {
            s_game.game.marble.keepsake_count = 1;
            s_game.game.marble.keepsakes[0] = (orb_marble_keepsake_t){.kind = 1, .stage = 1};
        }
        s_game.game.marble.garden_open = strstr(id, "garden") != NULL;
        if (strstr(id, "final")) s_game.game.marble.stage = ORB_MARBLE_STAGES - 1;
        if (strstr(id, "garden-full")) {
            s_game.game.marble.keepsake_count = ORB_MARBLE_KEEPSAKES_MAX;
            for (unsigned i = 0; i < ORB_MARBLE_KEEPSAKES_MAX; ++i)
                s_game.game.marble.keepsakes[i] = (orb_marble_keepsake_t){
                    .kind = i % 3, .stage = 1 + i % 3,
                    .x = (int16_t)(20 * (int)(i % 5) - 40), .y = (int16_t)(25 * (int)(i / 5) - 25)};
        }
    }
    if (strstr(id, "rewind-active")) {
        for (unsigned i = 0; i < 120; ++i) orb_game_step(&s_game, 0.08f, 0.03f, true, orb_ui_reduce_motion());
        orb_game_action(&s_game, ORB_GAME_REWIND_START, 0, 0);
        for (unsigned i = 0; i < 18; ++i) orb_game_step(&s_game, 0, 0, true, orb_ui_reduce_motion());
        parity_check(s_game.game.marble.rewind_active, "real model rewind state active");
    }
    s_game_view = orb_game_view_create(lv_screen_active(), kind, game_action, NULL);
    parity_check(s_game_view != NULL, "create actual game presenter");
    /* Game view fills its caller-owned screen; use that screen as trace root. */
    parity_set_root(lv_screen_active()); parity_apps_poll();
    parity_advance(strstr(id, "clear-bloom") ? 175 : strstr(id, "clear-plant") ? 600 : 1000);
}


/* Input scenarios complement projections: a disconnected callback must fail
 * even when its control still has exactly the baseline pixels. */
static void tapped(const char *caption)
{
    parity_check(parity_tap_text(caption), "resolve and physically tap named production control");
    parity_advance(240);
}
static orb_time_snapshot_t time_snapshot(void)
{
    orb_time_snapshot_t snapshot; orb_time_snapshot(orb_time_now_us(), &snapshot); return snapshot;
}
static void home_request(const char *id, orb_home_request_kind_t kind, uint32_t fields)
{
    orb_home_request_t request;
    parity_check(orb_home_take_request(&request), "physical Home input emitted a request");
    parity_check(!strcmp(request.id, id) && request.kind == kind &&
        (request.values.present & fields) == fields, "Home callback retains target, role and value binding");
    char event[128]; snprintf(event, sizeof(event), "id=%s kind=%d fields=%u", request.id, request.kind, request.values.present);
    parity_event("home-request-observed", event);
    parity_check(!orb_home_take_request(&request), "one physical Home action emitted exactly one request");
}
static void interaction(const char *id)
{
    if (!strcmp(id, "interaction.faces-gallery-select")) {
        faces("face-orb-gallery");
        parity_drag(330, 197, 130, 197, 160); parity_advance(500);
        orb_faces_prefs_t prefs; orb_faces_preferences(s_faces, &prefs);
        parity_check(prefs.face == ORB_FACE_MERIDIAN, "gallery drag selected adjacent face");
        parity_tap(233, 197); parity_advance(400);
        parity_check(orb_faces_mode(s_faces) == ORB_FACES_FACE, "physical gallery selection saved and returned to face");
        orb_faces_current_preferences(&prefs);
        parity_check(prefs.face == ORB_FACE_MERIDIAN, "gallery selected face persisted through adapter");
    } else if (!strcmp(id, "interaction.faces-colour-save")) {
        faces("face-orb-gallery"); tapped("Edit");
        parity_check(orb_faces_mode(s_faces) == ORB_FACES_COLOUR, "Edit callback opened colour");
        orb_faces_prefs_t before, after; orb_faces_preferences(s_faces, &before);
        /* Colour swatch i=2, from the public radial specification. */
        parity_tap(394, 300); parity_advance(240); orb_faces_preferences(s_faces, &after);
        parity_check(after.colour != before.colour, "physical swatch changed selected colour");
        parity_event("component.back", "faces public Back");
        parity_check(orb_faces_back(s_faces), "Colour Back handled by component");
        parity_check(orb_faces_mode(s_faces) == ORB_FACES_GALLERY, "Colour Back restored gallery");
        orb_faces_current_preferences(&before);
        parity_check(before.colour == after.colour, "Colour Back saved edited preference");
    } else if (!strcmp(id, "interaction.faces-slots-select")) {
        faces("face-orb-colour"); tapped("Slots");
        parity_check(orb_faces_mode(s_faces) == ORB_FACES_SLOTS, "Slots callback opened complications editor");
        parity_tap(338, 316); parity_advance(240);
        orb_faces_prefs_t prefs; orb_faces_preferences(s_faces, &prefs);
        int value, maximum;
        parity_check(orb_faces_rotary(s_faces, &value, &maximum) && value == prefs.slot[2],
            "physical third complication selected its own provider binding");
    } else if (strstr(id, "interaction.home-")) {
        if (strstr(id, "scene")) {
            home("home-scenes"); tapped("Evening");
            home_request("evening", ORB_HOME_REQUEST_SCENE, 0);
        } else if (strstr(id, "thermostat")) {
            /* The thermostat's identity disc opens its mode list. */
            home("home-thermostat"); parity_tap(233, 132); parity_advance(240); tapped("Heat");
            home_request("thermostat", ORB_HOME_REQUEST_SET, ORB_HOME_MODE);
        } else if (strstr(id, "dial")) {
            home("home-light"); parity_drag(80, 390, 40, 170, 200); parity_advance(240);
            home_request("light", ORB_HOME_REQUEST_SET, ORB_HOME_BRIGHTNESS);
        } else {
            const char *target = strstr(id, "switch") ? "switch" : strstr(id, "outlet") ? "outlet" : "light";
            char scene[64]; snprintf(scene, sizeof(scene), "home-%s", target); home(scene);
            /* Every accessory's power is its template identity disc. */
            parity_tap(233, 132);
            parity_advance(240); home_request(target, ORB_HOME_REQUEST_TOGGLE, 0);
        }
    } else if (!strcmp(id, "interaction.timer-start-pause")) {
        time_app("timer-idle"); tapped("Start");
        parity_check(time_snapshot().timer_state == ORB_TIME_RUNNING, "Start callback started timer");
        tapped("Pause"); parity_check(time_snapshot().timer_state == ORB_TIME_PAUSED, "Pause callback paused timer");
    } else if (!strcmp(id, "interaction.timer-quick-preset")) {
        time_app("timer-idle"); tapped("Quick timers"); tapped("3 min");
        orb_time_snapshot_t data = time_snapshot();
        parity_check(data.timer_state == ORB_TIME_IDLE && data.timer_duration_us == INT64_C(180000000),
            "preset callback edited duration without starting timer");
    } else if (!strcmp(id, "interaction.stopwatch-lap-pause")) {
        time_app("stopwatch-idle"); tapped("Start");
        parity_check(time_snapshot().stopwatch_state == ORB_TIME_RUNNING, "Start callback started stopwatch");
        tapped("Lap"); parity_check(time_snapshot().lap_count == 1, "Lap callback recorded exactly one split");
        tapped("Pause"); parity_check(time_snapshot().stopwatch_state == ORB_TIME_PAUSED, "Pause callback paused stopwatch");
    } else if (!strcmp(id, "interaction.alarm-toggle")) {
        time_app("alarm-idle"); tapped("Set");
        parity_check(time_snapshot().alarm_enabled, "physical switch armed alarm");
    } else if (!strcmp(id, "interaction.alarm-weekday")) {
        time_app("alarm-custom-days"); uint8_t before = time_snapshot().alarm_days;
        tapped("Sunday"); parity_check(time_snapshot().alarm_days == (uint8_t)(before ^ 1), "weekday callback toggled Sunday bit once");
        tapped("Close"); tapped("Close");
        orb_time_view_context_t context; orb_time_view_save(s_time, &context);
        parity_check(context.sheet == 0 && context.page == ORB_TIME_PAGE_ALARM, "explicit editor close restored Alarm");
    } else if (!strcmp(id, "interaction.reminder-done")) {
        time_app("reminders-detail"); tapped("Done");
        parity_check(time_snapshot().reminder_count == 0, "Done callback completed selected reminder");
    } else if (strstr(id, "interaction.game-")) {
        const char *name = id + strlen("interaction.game-"); char scene[64]; snprintf(scene, sizeof(scene), "%s-start", name);
        game(scene); tapped("Play");
        parity_check(s_game.started && !s_game.user_paused && s_game_actions[ORB_GAME_START] == 1,
            "Play callback started exactly one real game round");
        if (s_game.kind == ORB_GAME_ORBIT) {
            unsigned before = s_game.game.orbit.ring; parity_tap(100, 233); parity_advance(240);
            parity_check(s_game_actions[ORB_GAME_TAP] == 1 && s_game.game.orbit.ring != before, "arena tap changed Orbit ring");
            parity_tap(233, 233);
        } else if (s_game.kind == ORB_GAME_POP) {
            unsigned bubble = 0;
            while (bubble < ORB_GAME_BUBBLES && !s_game.game.pop.bubbles[bubble].active) ++bubble;
            parity_check(bubble < ORB_GAME_BUBBLES, "seeded Pop game has an active target");
            uint32_t score = s_game.score;
            parity_tap((int)lroundf(233 + s_game.game.pop.bubbles[bubble].x),
                (int)lroundf(233 + s_game.game.pop.bubbles[bubble].y)); parity_advance(240);
            parity_check(s_game.score > score && s_game_actions[ORB_GAME_TAP] == 1, "physical bubble tap scored once");
            parity_tap(233, 80);
        } else parity_tap(233, 233);
        parity_advance(400);
        parity_check(s_game.user_paused && s_game_actions[ORB_GAME_PAUSE] == 1, "physical arena or score tap paused game");
        tapped("Resume");
        parity_check(!s_game.user_paused && s_game_actions[ORB_GAME_PAUSE] == 2, "Resume callback returned same round to play");
    } else parity_check(false, "unknown app interaction scenario");
    parity_advance(500);
}

/* Weather through the real shell, model and compatibility host: the same
 * projection, staging and chrome path the Face complication opens. */
static lv_obj_t *weather_label(lv_obj_t *root, const char *prefix)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strncmp(lv_label_get_text(root), prefix, strlen(prefix))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = weather_label(lv_obj_get_child(root, i), prefix);
        if (found) return found;
    }
    return NULL;
}
static void weather_fixture(const char *id, orb_weather_snapshot_t *w)
{
    bool cold = strstr(id, "cold") != NULL;
    *w = (orb_weather_snapshot_t){.configured = true, .available = true, .unit = ORB_TEMPERATURE_F,
        .temperature = 72.4f, .high = 78, .low = 61, .condition = ORB_WEATHER_PARTLY_CLOUDY, .is_day = true,
        .updated_at = (uint32_t)WALL - 1440, .hourly_count = cold ? 2 : 6};
    static const orb_weather_condition_t conditions[] = {ORB_WEATHER_PARTLY_CLOUDY, ORB_WEATHER_CLOUDY,
        ORB_WEATHER_RAIN, ORB_WEATHER_THUNDER, ORB_WEATHER_CLOUDY, ORB_WEATHER_CLEAR};
    static const float temperatures[] = {73, 74, 70, 66, 64, 61};
    for (unsigned i = 0; i < w->hourly_count; i++) {
        /* Next whole UTC hours after the fixture's 14:05 wall clock. */
        w->hourly[i] = (orb_weather_hour_t){.hour = (uint32_t)(WALL - WALL % 3600) + 3600 * (i + 1),
            .temperature = cold ? -20.f - i : temperatures[i], .condition = cold ? ORB_WEATHER_SNOW : conditions[i],
            .is_day = i < 3};
    }
    if (cold) {
        w->unit = ORB_TEMPERATURE_C; w->temperature = -18.2f; w->high = -12; w->low = -23;
        w->condition = ORB_WEATHER_SNOW; w->is_day = false;
    }
    strcpy(w->place, strstr(id, "long-place") ? "Llanfairpwllgwyngyll, Isle of Anglesey, Wales" : "Columbia, MD");
    strcpy(w->label, orb_weather_condition_label(w->condition, w->is_day));
    if (strstr(id, "stale")) w->stale = true;
    if (strstr(id, "loading") || strstr(id, "unavailable") || strstr(id, "not-configured")) {
        orb_weather_snapshot_t pending = {.configured = !strstr(id, "not-configured"), .unit = ORB_TEMPERATURE_F,
            .stale = strstr(id, "unavailable") != NULL};
        if (pending.configured) strcpy(pending.place, w->place);
        strcpy(pending.label, !pending.configured ? "Set a location in orb-hub" :
            pending.stale ? "Weather unavailable" : "Loading weather");
        *w = pending;
    }
}
static void weather(const char *id)
{
    bool from_face = strstr(id, "from-face") != NULL;
    if (from_face) {
        /* A live editor's accepted save is the Face's current slot; preview
         * editors never publish preferences. Storage stays process-local. */
        orb_faces_t *preferences = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.persist = face_persist});
        parity_check(preferences != NULL, "prepare Weather face slot");
        orb_faces_prefs_t prefs; orb_faces_preferences(preferences, &prefs);
        prefs.face = ORB_FACE_ORB; prefs.slot[0] = ORB_FACE_WEATHER;
        orb_faces_select(preferences, ORB_FACE_MERIDIAN);
        orb_faces_set_preferences(preferences, &prefs);
        parity_check(orb_faces_save(preferences), "retain Weather face slot");
        orb_faces_destroy(preferences);
    }
    parity_shell_create(false);
    orb_time_set_clock_format(strstr(id, "24h") != NULL);
    orb_model_weather_capability(!strstr(id, "unsupported"));
    if (!strstr(id, "unsupported") && !strstr(id, "offline.empty")) {
        orb_weather_snapshot_t w; weather_fixture(id, &w);
        parity_check(orb_model_set_weather(&w), "accept normalized weather fixture");
    }
    if (strstr(id, "offline")) orb_model_set_connection(false, false);
    parity_advance(64);
    if (from_face) {
        parity_navigate(ORB_SHELL_FACE); parity_advance(800);
        parity_event("input.complication", "Weather slot 1");
        parity_tap(128, 316);
        /* The mid-iris sample shows the shared complication launch transition;
         * its late frame is the settled destination. */
        if (strstr(id, "iris")) { parity_advance(24); return; }
        parity_advance(1200);
        parity_check(orb_shell_surface(parity_shell) == ORB_SHELL_WEATHER, "Weather complication opened Weather");
        if (strstr(id, "back")) {
            parity_event("input.back", "BOOT-equivalent public Back");
            orb_shell_back(parity_shell); parity_advance(1200);
            parity_check(orb_shell_surface(parity_shell) == ORB_SHELL_FACE, "Back returned to the Face");
        }
        return;
    }
    parity_navigate(ORB_SHELL_WEATHER); parity_advance(720);
    if (strstr(id, "-end")) {
        lv_obj_t *last = weather_label(parity_root(), "Updated");
        if (!last) last = weather_label(parity_root(), "Not updated");
        parity_check(last != NULL, "Weather footnote present");
        lv_obj_t *view = lv_obj_get_parent(last);
        lv_obj_update_layout(view);
        int32_t maximum = lv_obj_get_scroll_y(view) + lv_obj_get_scroll_bottom(view);
        parity_check(maximum > 0, "long Weather content scrolls");
        lv_obj_scroll_to_y(view, maximum, LV_ANIM_OFF);
        parity_event("input.scroll-target", "Weather end");
        parity_advance(120);
    }
}

#define S(id, family, state, rim, fn) {id, family, state, rim, fn}
#define F(id, state) S(id, "Faces", state, "faces-public-rotary-or-none", faces)
#define H(id, state) S(id, "Home", state, "home-public-dial-or-list", home)
#define T(id, state) S(id, "Time", state, "time-public-rotary", time_app)
#define G(id, state) S(id, "Play", state, "none-game-input", game)
#define W(id, state) S(id, "Weather", state, "weather-scroll-or-none", weather)
#define HOME_WRITE_SET(name) \
    H("home-" name "-pending", "pending"), H("home-" name "-error", "error"), \
    H("home-" name "-offline", "offline"), H("home-" name "-unavailable", "unavailable"), \
    H("home-" name "-long", "long-text")
#define HOME_READ_SET(name) \
    H("home-" name "-offline", "offline"), H("home-" name "-unavailable", "unavailable")
#define GAME_SET(name) \
    G(name "-start", "start"), G(name "-play", "play"), G(name "-pause", "pause"), \
    G(name "-result", "result"), G(name "-pause-save-error", "save-error"), \
    G(name "-controls", "controls"), G(name "-long-score", "long-score")
const parity_scenario_t parity_app_scenarios[] = {
    F("face-orb", "settled"), F("face-meridian", "settled"), F("face-activity", "settled"),
    F("face-orb-offline", "offline"), F("face-meridian-offline", "offline"), F("face-activity-offline", "offline"),
    F("face-orb-unknown", "unknown"), F("face-meridian-unknown", "unknown"), F("face-activity-unknown", "unknown"),
    F("face-orb-gallery", "gallery"), F("face-meridian-gallery", "gallery"), F("face-activity-gallery", "gallery"),
    F("face-orb-colour", "colour"), F("face-meridian-colour", "colour"), F("face-activity-colour", "colour"),
    F("face-orb-slots", "slots"), F("face-meridian-slots", "slots"), F("face-activity-slots", "slots"),
    F("face-orb-slots-slot-2", "slot-editor"), F("face-orb-colour-save-error", "save-error"),
    F("face-orb-gallery-restore", "restore"), F("face-lock", "locked"), F("face-always-on", "always-on"),
    F("face-meridian-lock", "locked"), F("face-meridian-always-on", "always-on"),
    F("face-activity-lock", "locked"), F("face-activity-always-on", "always-on"),
    H("home-rooms", "rooms"), H("home-rooms-empty", "empty"), H("home-rooms-loading", "loading"),
    H("home-rooms-offline", "offline"), H("home-rooms-long", "long-text"),
    H("home-rooms-service-unavailable", "service-unavailable"), H("home-sections", "sections"),
    H("home-room", "room"), H("home-room-sections", "room-actions"), H("home-room-restore", "restore"),
    H("home-climate", "climate"), H("home-climate-offline", "stale"), H("home-climate-empty", "empty"),
    H("home-sensors", "sensors"), H("home-sensors-empty", "empty"),
    H("home-scenes", "scenes"), H("home-scenes-empty", "empty"), H("home-scenes-long", "long-text"),
    H("home-scene-pending", "pending"), H("home-scene-error", "error"),
    H("home-light", "brightness"), H("home-light-off", "off"), H("home-light-colour", "colour"),
    H("home-light-scenes", "scenes"), H("home-light-pending", "pending"), H("home-light-error", "error"),
    H("home-light-unavailable", "unavailable"), H("home-light-long", "long-text"), H("home-light-offline", "offline"),
    H("home-switch", "switch"), H("home-outlet", "outlet"), H("home-thermostat", "thermostat"),
    H("home-thermostat-modes", "mode-picker"), H("home-fan", "fan"), H("home-lock", "locked"),
    H("home-lock-unlocked", "unlocked"), H("home-temperature", "temperature"), H("home-humidity", "humidity"),
    H("home-motion", "motion"), H("home-contact", "contact"), H("home-sensor", "sensor"),
    H("home-doorbell", "doorbell"), H("home-button", "button"), H("home-cover", "cover"),
    H("home-other", "other"), H("home-unknown", "unknown"),
    HOME_WRITE_SET("switch"), HOME_WRITE_SET("outlet"), HOME_WRITE_SET("thermostat"),
    HOME_WRITE_SET("fan"), HOME_WRITE_SET("lock"), HOME_WRITE_SET("cover"),
    HOME_READ_SET("temperature"), HOME_READ_SET("humidity"), HOME_READ_SET("motion"),
    HOME_READ_SET("contact"), HOME_READ_SET("sensor"), HOME_READ_SET("doorbell"),
    HOME_READ_SET("button"), HOME_READ_SET("other"), HOME_READ_SET("unknown"),
    T("timer-load-error", "load-error"), T("timer-save-error", "save-error"), T("timer-pending", "saving"),
    T("alarm-save-error", "save-error"), T("reminders-save-error", "save-error"),
    T("timer-idle", "idle"), T("timer-running", "running"), T("timer-paused", "paused"), T("timer-due", "due"),
    T("timer-quick-editor", "quick-editor"), T("timer-custom-editor", "custom-editor"), T("timer-idle-restore", "restore"),
    T("stopwatch-idle", "idle"), T("stopwatch-running", "running"), T("stopwatch-paused", "paused"),
    T("stopwatch-laps", "laps"), T("stopwatch-laps-paused", "paused-laps"),
    T("alarm-idle", "idle"), T("alarm-enabled", "enabled"), T("alarm-no-clock", "no-clock"),
    T("alarm-options", "options"), T("alarm-weekdays", "weekdays"), T("alarm-custom-days", "custom-days"),
    T("alarm-snooze-editor", "snooze-editor"), T("alarm-ringing", "ringing"),
    T("reminders-empty", "empty"), T("reminders-list", "list"), T("reminders-detail", "detail"),
    T("reminders-detail-long", "long-text"), T("reminders-due", "due"),
    G("play-picker", "picker"),
    GAME_SET("marble"), G("beads-start", "start"), G("beads-play", "play"), G("beads-pause", "pause"), GAME_SET("orbit"), GAME_SET("snake"), GAME_SET("pop"),
    G("marble-clear", "clear"), G("marble-final-clear", "final-clear"),
    G("marble-garden-empty", "garden-empty"), G("marble-garden-full", "garden-full"),
    G("marble-garden-full-save-error", "garden-save-error"),
    G("marble-calibrating", "calibrating"), G("marble-motion-unavailable", "motion-unavailable"),
    G("beads-clock", "clock"), G("beads-no-clock", "no-clock"), G("beads-calibrating", "calibrating"),
    G("beads-motion-unavailable", "motion-unavailable"), G("snake-calibrating", "calibrating"),
    G("snake-motion-unavailable", "motion-unavailable"),
    F("face-orb-weather", "weather"), F("face-meridian-weather", "weather"), F("face-activity-weather", "weather"),
    F("face-orb-weather-stale", "stale"), F("face-meridian-weather-stale", "stale"), F("face-activity-weather-stale", "stale"),
    F("face-orb-weather-unavailable", "weather-unavailable"), F("face-orb-provider-next", "timer-complication"),
    H("home-light-white-only", "white-colour"), H("home-light-white-only-offline", "white-colour-offline"),
    H("home-light-scenes-pending", "detail-scene-pending"), H("home-light-scenes-error", "detail-scene-error"),
    H("home-light-scenes-ack", "detail-scene-ack"), H("home-light-missing", "removed-selected-accessory"),
    H("home-light-reading-unknown", "reading-unknown"), H("home-lock-reading-unknown", "lock-state-unknown"),
    H("home-thermostat-target-unknown", "target-unknown"), H("home-thermostat-current-unknown", "current-unknown"),
    H("home-sensor-reading-unknown", "reading-unknown"), H("home-temperature-reading-unknown", "reading-unknown"),
    H("home-humidity-reading-unknown", "reading-unknown"), H("home-fan-reading-unknown", "reading-unknown"),
    H("home-sensor-low-battery", "low-battery"), H("home-sensor-aged", "aged-reading"),
    H("home-switch-off", "off"), H("home-outlet-off", "off"), H("home-fan-off", "off"),
    H("home-motion-clear", "clear"), H("home-contact-closed", "closed"),
    H("home-thermostat-heat", "heat"), H("home-thermostat-cool", "cool"), H("home-thermostat-off", "off"),
    T("timer-hour-editor", "hour-editor"), T("timer-hour-running", "hour-running"),
    T("stopwatch-hour-running", "hour-running"), T("stopwatch-laps-scrolled", "pinned-laps"),
    T("alarm-hour-editor", "hour-editor"), T("alarm-minute-editor", "minute-editor"),
    T("alarm-12h-am", "12h-am"), T("alarm-12h-pm", "12h-pm"), T("alarm-12h-ringing", "12h-ringing"),
    T("alarm-pending", "saving"), T("reminders-pending", "saving"),
    G("marble-result-save-error", "result-save-error"), G("orbit-result-save-error", "result-save-error"),
    G("snake-result-save-error", "result-save-error"), G("pop-result-save-error", "result-save-error"),
    G("snake-result-won", "complete"), G("beads-controls", "touch-controls"), G("beads-pause-controls", "touch-pause"),
    G("marble-clear-bloom", "sample-clear-bloom-175ms"), G("marble-clear-plant", "sample-clear-plant-600ms"),
    H("home-room-offline", "offline"), H("home-scenes-offline", "offline"), H("home-sensors-offline", "offline"),
    H("home-light-colour-offline", "colour-offline"), H("home-light-scenes-offline", "detail-scenes-offline"),
    H("home-thermostat-modes-offline", "modes-offline"), H("home-light-waiting", "waiting-for-home"),
    T("reminders-detail-no-clock-snooze-failed", "clock-offline-error"),
    G("marble-rewind-active", "rewind-active"),
    S("interaction.faces-gallery-select", "Faces", "interaction", "faces-selection", interaction),
    S("interaction.faces-colour-save", "Faces", "interaction", "faces-colour", interaction),
    S("interaction.faces-slots-select", "Faces", "interaction", "faces-slot-provider", interaction),
    S("interaction.home-light-toggle", "Home", "interaction", "home-brightness", interaction),
    S("interaction.home-light-dial", "Home", "interaction", "home-brightness", interaction),
    S("interaction.home-switch-toggle", "Home", "interaction", "none", interaction),
    S("interaction.home-outlet-toggle", "Home", "interaction", "none", interaction),
    S("interaction.home-scene", "Home", "interaction", "home-scenes", interaction),
    S("interaction.home-thermostat-mode", "Home", "interaction", "home-mode", interaction),
    S("interaction.timer-start-pause", "Time", "interaction", "time-timer", interaction),
    S("interaction.timer-quick-preset", "Time", "interaction", "time-timer", interaction),
    S("interaction.stopwatch-lap-pause", "Time", "interaction", "none", interaction),
    S("interaction.alarm-toggle", "Time", "interaction", "time-alarm", interaction),
    S("interaction.alarm-weekday", "Time", "interaction", "time-alarm", interaction),
    S("interaction.reminder-done", "Time", "interaction", "time-reminders", interaction),
    S("interaction.game-marble", "Play", "interaction", "none", interaction),
    S("interaction.game-beads", "Play", "interaction", "none", interaction),
    S("interaction.game-orbit", "Play", "interaction", "none", interaction),
    S("interaction.game-snake", "Play", "interaction", "none", interaction),
    S("interaction.game-pop", "Play", "interaction", "none", interaction),
    W("weather.current", "current"), W("weather.current-long-place", "current-long-place"),
    W("weather.current-long-place-end", "current-long-place-scrolled"),
    W("weather.stale", "stale"), W("weather.stale-long-place", "stale-long-place"),
    W("weather.offline.retained", "offline-retained-stale"), W("weather.offline.empty", "offline"),
    W("weather.loading", "loading"), W("weather.loading-long-place", "loading-long-place"),
    W("weather.unavailable", "unavailable"), W("weather.unavailable-long-place", "unavailable-long-place"),
    W("weather.unsupported", "unsupported"), W("weather.not-configured", "not-configured"),
    W("weather.cold-24h", "negative-24h-two-hours"),
    W("weather.from-face", "face-complication-route"), W("weather.from-face-back", "face-complication-back"),
    W("weather.from-face-iris", "face-complication-iris-sample"),
};
const unsigned parity_app_count = sizeof(parity_app_scenarios) / sizeof(parity_app_scenarios[0]);

/* Public owner queries keep the trace independent of private view topology. */
bool parity_apps_rim(char *out, size_t capacity)
{
    if (s_faces) {
        int value = 0, maximum = 0;
        bool active = orb_faces_rotary(s_faces, &value, &maximum);
        snprintf(out, capacity, "faces:%s value=%d maximum=%d", active ? "selection" : "none", value, maximum);
        return true;
    }
    if (s_home) {
        float value = 0, step = 0;
        bool active = orb_home_view_dial(s_home, &value, &step);
        lv_obj_t *scroll = orb_home_view_scroll(s_home);
        snprintf(out, capacity, "home:%s value=%.6f step=%.6f writes=%d scroll=%d",
            active ? "dial" : scroll ? "list" : "none", (double)value, (double)step,
            orb_home_view_dial_writes(s_home), scroll ? lv_obj_get_scroll_y(scroll) : 0);
        return true;
    }
    if (s_time) {
        orb_time_rotary_t owner = {0};
        bool active = orb_time_view_rotary(s_time, &owner);
        snprintf(out, capacity, "time:%s value=%.6f minimum=%.6f maximum=%.6f detent=%.6f units=%.6f",
            active ? "rotary" : "none", (double)owner.value, (double)owner.minimum,
            (double)owner.maximum, (double)owner.detent, (double)owner.units_per_turn);
        return true;
    }
    if (s_game_view) { snprintf(out, capacity, "game:none"); return true; }
    return false;
}

bool parity_apps_semantic(char *out, size_t capacity)
{
    if (s_faces) {
        orb_faces_prefs_t prefs; orb_faces_preferences(s_faces, &prefs);
        snprintf(out, capacity, "faces mode=%d face=%u colour=%u slots=%u,%u,%u meridian=%u,%u,%u,%u activity=%u,%u,%u save_error=%d",
            orb_faces_mode(s_faces), prefs.face, prefs.colour, prefs.slot[0], prefs.slot[1], prefs.slot[2],
            prefs.meridian_slot[0], prefs.meridian_slot[1], prefs.meridian_slot[2], prefs.meridian_slot[3],
            prefs.activity_slot[0], prefs.activity_slot[1], prefs.activity_slot[2], orb_faces_save_error(s_faces));
        return true;
    }
    if (s_home) {
        orb_home_view_state_t context; orb_home_view_save(s_home, &context);
        orb_home_snapshot_t *snapshot = malloc(sizeof(*snapshot));
        parity_check(snapshot && orb_home_snapshot(snapshot), "observe Home service snapshot");
        unsigned pending = 0;
        for (unsigned i = 0; i < snapshot->accessory_count; ++i) pending += snapshot->accessories[i].pending;
        snprintf(out, capacity, "home revision=%" PRIu64 " online=%d synced=%d available=%d pending=%u scene_pending=%d error=%s error_id=%s surface=%d room=%s selected=%s page=%u sections=%d sensors=%d scenes=%d scroll=%d",
            snapshot->revision, snapshot->online, snapshot->synced, snapshot->available, pending,
            snapshot->scene_pending, snapshot->error, snapshot->error_id, context.surface,
            context.room, context.selected, context.detail_page, context.sections, context.sensors,
            context.scene_page, context.scroll);
        free(snapshot); return true;
    }
    if (s_time) {
        orb_time_snapshot_t data; orb_time_snapshot(orb_time_now_us(), &data);
        orb_time_view_context_t context; orb_time_view_save(s_time, &context);
        snprintf(out, capacity, "time generation=%u timer=%d duration=%" PRId64 " remaining=%" PRId64 " stopwatch=%d elapsed=%" PRId64 " laps=%u alarm=%u:%u repeat=%d days=%u enabled=%d ringing=%d next=%" PRId64 " reminders=%u due=%d saving=%d save_error=%d page=%d sheet=%u field=%u detail=%d selected=%u clock_valid=%d clock_now=%" PRId64 " scroll=%d,%d,%d,%d",
            data.generation, data.timer_state, data.timer_duration_us, data.timer_remaining_us,
            data.stopwatch_state, data.stopwatch_elapsed_us, data.lap_count,
            data.alarm_hour, data.alarm_minute, data.alarm_repeat, data.alarm_days,
            data.alarm_enabled, data.alarm_ringing, (int64_t)data.alarm_next,
            data.reminder_count, data.reminder_due, data.saving, data.save_error,
            context.page, context.sheet, context.field, context.reminder_detail, context.reminder_id,
            data.clock_valid, (int64_t)data.clock_now, context.scroll[0], context.scroll[1], context.scroll[2], context.scroll[3]);
        return true;
    }
    if (s_game_view) {
        snprintf(out, capacity, "game kind=%d seed=%u ticks=%u started=%d paused=%d ended=%d controls=%d score=%u save_error=%d calibrating=%d motion_unavailable=%d clear=%d garden=%d rewind=%d",
            s_game.kind, s_game.random, s_game.ticks, s_game.started, s_game.user_paused,
            s_game.game_over, s_game.controls, s_game.score, s_game_save_error,
            s_game.tilt_calibrating, s_game.motion_unavailable,
            s_game.kind == ORB_GAME_MARBLE && s_game.game.marble.stage_cleared,
            s_game.kind == ORB_GAME_MARBLE && s_game.game.marble.garden_open,
            s_game.kind == ORB_GAME_MARBLE && s_game.game.marble.rewind_active);
        return true;
    }
    return false;
}
