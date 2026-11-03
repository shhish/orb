/* Face Weather complication → Weather → Back, through the real shell, model,
 * compatibility host and pointer input. All data is synthetic and local. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_buttons.h"
#include "orb_faces.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;

#define CHECK(value) do { \
    s_checks++; \
    if (!(value)) { \
        fprintf(stderr, "weather route %s: %s:%d: %s\n", s_case, __FILE__, __LINE__, #value); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    do {
        s_now += LV_MIN(8, end - s_now);
        if (s_shell) orb_shell_poll(s_shell);
        lv_timer_handler();
        lv_refr_now(s_display);
    } while (s_now < end);
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    *data = s_input;
}

static void pointer(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    advance(16);
}

static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    const char *rim = orb_kit_rim_text_get(root);
    if (rim && !strcmp(rim, text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(root, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static orb_weather_snapshot_t reading(float temperature)
{
    orb_weather_snapshot_t weather = {.configured = true, .available = true, .unit = ORB_TEMPERATURE_F,
        .temperature = temperature, .high = 78, .low = 61, .condition = ORB_WEATHER_PARTLY_CLOUDY,
        .is_day = true, .updated_at = 1793194860, .hourly_count = 3};
    strcpy(weather.place, "Columbia, MD");
    strcpy(weather.label, orb_weather_condition_label(weather.condition, weather.is_day));
    for (unsigned i = 0; i < 3; i++)
        weather.hourly[i] = (orb_weather_hour_t){.hour = 1793199600 + 3600 * i,
            .temperature = 70.f - (float)i, .condition = ORB_WEATHER_CLOUDY, .is_day = true};
    return weather;
}

static bool persist(const orb_faces_prefs_t *preferences, void *context)
{
    (void)preferences; (void)context;
    return true;
}

static void seed_weather_slot(void)
{
    /* A live editor's accepted save publishes the slot the shell's Face reads. */
    orb_faces_t *editor = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.persist = persist});
    CHECK(editor);
    orb_faces_prefs_t prefs;
    orb_faces_preferences(editor, &prefs);
    prefs.face = ORB_FACE_ORB;
    prefs.slot[0] = ORB_FACE_WEATHER;
    orb_faces_select(editor, ORB_FACE_MERIDIAN);
    orb_faces_set_preferences(editor, &prefs);
    CHECK(orb_faces_save(editor));
    orb_faces_destroy(editor);
}

static orb_shell_t *create_shell(void)
{
    orb_shell_t *shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    CHECK(shell != NULL);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true};
    strcpy(status.clock_time, "10:09");
    strcpy(status.clock_date, "Wednesday, October 28");
    strcpy(status.hub_name, "Synthetic Mac");
    orb_shell_update(shell, &status);
    return shell;
}

static void open_from_face(void)
{
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    /* Slot 1 of the Orb face, the Weather complication's resting centre. */
    pointer(128, 316, true); advance(48); pointer(128, 316, false);
    advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_WEATHER);
    CHECK(find_text(orb_shell_root(s_shell), "WEATHER"));
    CHECK(find_text(orb_shell_root(s_shell), "Partly cloudy"));
    CHECK(find_text(orb_shell_root(s_shell), "Columbia, MD"));
}

static void route_and_back(void)
{
    s_case = "Weather complication opens Weather; Back returns to the Face";
    open_from_face();
    CHECK(find_text(orb_shell_root(s_shell), "72°"));
    orb_shell_back(s_shell); advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!find_text(orb_shell_root(s_shell), "Updated 13:41"));
}

static void route_and_boot(void)
{
    s_case = "BOOT returns from Weather to the Face like every app screen";
    open_from_face();
    orb_button_event_t event = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_DOWN, .at_ms = s_now};
    orb_shell_button(s_shell, &event);
    event.gesture = ORB_BUTTON_UP; orb_shell_button(s_shell, &event);
    event.gesture = ORB_BUTTON_SHORT; orb_shell_button(s_shell, &event);
    advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
}

static void live_update_in_place(void)
{
    s_case = "a new hub reading updates retained labels without a rebuild";
    open_from_face();
    lv_obj_t *number = find_text(orb_shell_root(s_shell), "72°");
    lv_obj_t *place = find_text(orb_shell_root(s_shell), "Columbia, MD");
    CHECK(number && place);
    orb_weather_snapshot_t next = reading(74);
    next.updated_at += 1200;
    CHECK(orb_model_set_weather(&next));
    advance(400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_WEATHER);
    CHECK(find_text(orb_shell_root(s_shell), "74°") == number);
    CHECK(find_text(orb_shell_root(s_shell), "Columbia, MD") == place);
    CHECK(find_text(orb_shell_root(s_shell), "Updated 14:01"));
    /* The hub refreshes; losing it keeps the values and says so in words. */
    orb_model_weather_capability(false);
    advance(400);
    CHECK(find_text(orb_shell_root(s_shell), "74°") == number);
    CHECK(find_text(orb_shell_root(s_shell), "Not updated since 14:01"));
    orb_shell_back(s_shell); advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    orb_model_weather_capability(true);
}

int main(void)
{
    s_case = "setup";
    setenv("TZ", "UTC", 1);
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display != NULL);
    orb_ui_init(NULL);
    CHECK(orb_model_init() && orb_home_init());
    orb_time_init();
    orb_model_set_connection(true, true);
    orb_model_weather_capability(true);
    orb_weather_snapshot_t weather = reading(72);
    CHECK(orb_model_set_weather(&weather));
    s_pointer = lv_indev_create(); CHECK(s_pointer != NULL);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    seed_weather_slot();
    s_shell = create_shell();
    advance(1200);
    route_and_back();
    route_and_boot();
    live_update_in_place();
    orb_shell_destroy(s_shell); s_shell = NULL;
    printf("weather route: %u checks passed\n", s_checks);
    return 0;
}
