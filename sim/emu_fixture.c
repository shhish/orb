#include "orb_app_play.h"
/* Real firmware controllers over synthetic model data and bounded RAM storage.
 * The SDL frontend owns the clock and calls the same lifecycle order as main.c. */
#include "emu_fixture.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "orb_apps.h"
#include "orb_buttons.h"
#include "orb_fs.h"
#include "orb_game_logic.h"
#include "orb_games.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell_services.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "voice_core.h"

static orb_shell_t *s_shell;
static orb_shell_snapshot_t s_status;
static uint32_t s_now, s_clock_second, s_actions;
static bool s_face_down, s_screen_on, s_navigation_pending, s_design_fixture, s_design_silent_pending;
static bool s_design_wake_pending, s_design_power;
static uint32_t s_design_started;
static int s_awake_brightness;
static orb_buttons_t s_buttons;
static orb_shell_surface_t s_destination;
static voice_core_t s_voice;
static struct {
    voice_request_kind_t kind;
    uint16_t stream;
    uint32_t due;
    bool pending;
} s_voice_reply[4];
static uint16_t s_result_stream;
static uint32_t s_result_due;

/* Games use the production score/keepsake codecs. Two bounded records per app
 * are enough for those namespaces, and are discarded on process exit. */
static struct {
    char id[ORB_FS_APP_ID_MAX + 1];
    struct {
        char key[ORB_FS_KEY_MAX + 1];
        uint8_t bytes[ORB_KEEPSAKE_RECORD_SIZE];
        size_t size;
    } records[2];
} s_storage[ORB_FS_MAX_APPS];

bool orb_fs_valid_app_id(const char *id)
{
    if (!id || !*id || strlen(id) > ORB_FS_APP_ID_MAX) return false;
    for (const char *p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return false;
    }
    return true;
}

bool orb_fs_ready(void) { return true; }

esp_err_t orb_fs_app_prepare(const char *id)
{
    if (!orb_fs_valid_app_id(id)) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < ORB_FS_MAX_APPS; ++i) {
        if (!strcmp(s_storage[i].id, id)) return ESP_OK;
        if (!s_storage[i].id[0]) {
            snprintf(s_storage[i].id, sizeof(s_storage[i].id), "%s", id);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    if (!orb_fs_valid_app_id(id) || !key || !data || !size) return ESP_ERR_INVALID_ARG;
    *size = 0;
    for (size_t i = 0; i < ORB_FS_MAX_APPS; ++i) {
        if (strcmp(s_storage[i].id, id)) continue;
        for (size_t r = 0; r < 2; ++r) {
            if (strcmp(s_storage[i].records[r].key, key) || !s_storage[i].records[r].size) continue;
            *size = s_storage[i].records[r].size;
            if (capacity < *size) return ESP_ERR_INVALID_SIZE;
            memcpy(data, s_storage[i].records[r].bytes, *size);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    if (!orb_fs_valid_app_id(id) || !key || !*key || strlen(key) > ORB_FS_KEY_MAX || !data ||
        !size || size > ORB_KEEPSAKE_RECORD_SIZE) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < ORB_FS_MAX_APPS; ++i) {
        if (strcmp(s_storage[i].id, id)) continue;
        for (size_t r = 0; r < 2; ++r) {
            if (s_storage[i].records[r].key[0] && strcmp(s_storage[i].records[r].key, key)) continue;
            snprintf(s_storage[i].records[r].key, sizeof(s_storage[i].records[r].key), "%s", key);
            memcpy(s_storage[i].records[r].bytes, data, size);
            s_storage[i].records[r].size = size;
            return ESP_OK;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_NOT_FOUND;
}

static bool voice_open(void *ctx) { (void)ctx; return true; }
static void voice_close(void *ctx) { (void)ctx; }
static size_t voice_read(void *ctx, uint8_t *pcm, size_t capacity)
{
    (void)ctx;
    /* Silence exercises the actual warm-up/capture framing without requesting
     * microphone permissions or presenting a fabricated transcript. */
    memset(pcm, 0, capacity);
    return capacity;
}
static bool voice_audio(void *ctx, const uint8_t *frame, size_t bytes)
{
    (void)ctx; (void)frame; (void)bytes;
    return true;
}
static bool voice_request(void *ctx, voice_request_kind_t kind, const orb_voice_state_t *state)
{
    (void)ctx;
    for (size_t i = 0; i < 4; ++i) {
        if (s_voice_reply[i].pending) continue;
        s_voice_reply[i].kind = kind;
        s_voice_reply[i].stream = state->stream;
        s_voice_reply[i].due = s_now + 32;
        s_voice_reply[i].pending = true;
        if (kind == VOICE_REQ_END) {
            s_result_stream = state->stream;
            s_result_due = s_now + 350;
        }
        if (kind == VOICE_REQ_CANCEL && state->stream == s_result_stream) s_result_stream = 0;
        return true;
    }
    return false;
}
static bool voice_answer(void *ctx, const orb_voice_state_t *state)
{
    (void)ctx;
    orb_answer_t answer = {0};
    snprintf(answer.card, sizeof(answer.card), "%s", state->card);
    snprintf(answer.text, sizeof(answer.text), "%s", state->text);
    return orb_model_answer(&answer);
}
static void voice_publish(void *ctx, const orb_voice_state_t *state)
{
    (void)ctx;
    (void)orb_model_set_voice(state);
}
static bool voice_focus(void *ctx, char target[49], char card[65])
{
    (void)ctx;
    return orb_model_get_voice_focus(target, card);
}

static void service_voice(void)
{
    /* main.c routes BOOT through orb_shell_button: its HOLD/UP enqueue the
     * production START/STOP requests. A second raw voice timer would race it. */
    orb_voice_request_t request;
    while (orb_model_take_voice_request(&request)) voice_core_action(&s_voice, &request, s_now);
    for (size_t i = 0; i < 4; ++i) {
        if (!s_voice_reply[i].pending || (int32_t)(s_now - s_voice_reply[i].due) < 0) continue;
        s_voice_reply[i].pending = false;
        voice_core_reply(&s_voice, s_voice_reply[i].kind, s_voice_reply[i].stream,
                         1, true, NULL, s_now);
    }
    if (s_result_stream && (int32_t)(s_now - s_result_due) >= 0) {
        uint16_t stream = s_result_stream;
        s_result_stream = 0;
        voice_core_error(&s_voice, stream, "no_speech", s_now);
    }
    voice_core_tick(&s_voice, s_now);
}

static void service_model(void)
{
    /* These acknowledgments are confined to explicitly loaded demo cards. An
     * answer is only consumed after a real pointer/keyboard UI interaction. */
    orb_answer_t answer;
    while (orb_model_peek_answer(&answer)) {
        (void)orb_model_cancel_card(answer.card);
        if (!orb_model_complete_answer(answer.card, &answer, true, NULL)) break;
    }
    orb_message_t message;
    while (orb_model_take_message(&message)) orb_model_complete_message(&message, true, NULL);
    orb_prefs_patch_t patch;
    if (orb_model_take_prefs_request(&patch)) {
        orb_model_snapshot_t model;
        if (orb_model_snapshot(&model)) {
            for (unsigned i = 0; i < 3; ++i)
                if (patch.sources_mask & (1u << i)) model.prefs.sources[i] = patch.sources[i];
            if (patch.has_idle_minutes) model.prefs.idle_minutes = patch.idle_minutes;
            if (patch.has_temperature) {
                model.prefs.temperature = patch.temperature == ORB_TEMPERATURE_UNKNOWN ?
                    ORB_TEMPERATURE_F : patch.temperature;
                model.prefs.temperature_owner = patch.temperature != ORB_TEMPERATURE_UNKNOWN;
            }
            orb_model_complete_prefs(&model.prefs, NULL);
        }
    }
    orb_idle_request_t idle;
    if (orb_model_take_idle_request(&idle)) {
        static const orb_session_t sessions[] = {
            {.id = "emu-idle-docs", .agent = ORB_AGENT_CLAUDE, .title = "Interaction notes",
             .project = "Orb", .state = ORB_SESSION_IDLE, .reason = "Ready for your reply",
             .summary = "The desktop emulator uses the firmware UI.", .can_message = true},
            {.id = "emu-idle-tests", .agent = ORB_AGENT_CODEX, .title = "Simulator tests",
             .project = "Orb", .state = ORB_SESSION_DONE, .reason = "Checks complete",
             .summary = "Open the Apps place to try Time and the games."},
        };
        (void)orb_model_complete_idle(&idle, sessions, 2, NULL, NULL);
    }
}

static void clock_status(void)
{
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local)) {
        strftime(s_status.clock_time, sizeof(s_status.clock_time), "%H:%M", &local);
        strftime(s_status.clock_date, sizeof(s_status.clock_date), "%A, %B %e", &local);
    }
}

static void shell_action(orb_shell_action_t action, void *ctx)
{
    (void)ctx;
    /* Match the firmware's UI callback boundary: only enqueue while LVGL is
     * delivering an event. The next main iteration publishes the result. */
    s_actions |= 1u << action;
}

static void service_actions(void)
{
    uint32_t actions = s_actions;
    s_actions = 0;
    if (!actions) return;
    if (actions & (1u << ORB_SHELL_ACTION_BRIGHTNESS)) {
        s_status.brightness = orb_shell_brightness(s_shell);
        if (s_status.brightness > 10) s_awake_brightness = s_status.brightness;
    }
    if (actions & (1u << ORB_SHELL_ACTION_SLEEP)) {
        s_status.brightness = 0;
        orb_shell_power_sleep(s_shell);
    }
    if (actions & (1u << ORB_SHELL_ACTION_CHECK_UPDATE)) {
        ++s_status.update_check_revision;
        snprintf(s_status.update_check_result, sizeof(s_status.update_check_result), "No emulator updates");
    }
    /* Wi-Fi/pairing/OTA actions have no host side effects. The current demo
     * status remains observable instead of pretending a real service changed. */
    orb_shell_update(s_shell, &s_status);
}

bool emu_fixture_init(lv_display_t *display)
{
    if (!display || s_shell || !orb_model_init() || !orb_home_init()) return false;
    memset(s_storage, 0, sizeof(s_storage));
    memset(s_voice_reply, 0, sizeof(s_voice_reply));
    s_now = lv_tick_get();
    s_clock_second = s_now / 1000;
    s_actions = 0;
    s_face_down = s_navigation_pending = s_design_fixture = s_design_silent_pending = s_design_wake_pending = s_design_power = false;
    s_screen_on = true;
    s_awake_brightness = 70;
    orb_buttons_init(&s_buttons);
    s_result_stream = 0;
    orb_ui_init(NULL);
    orb_model_load_demo();
    orb_home_load_demo();
    orb_model_set_connection(true, true);
    orb_prefs_t prefs;
    orb_model_prefs_defaults(&prefs);
    prefs.temperature = ORB_TEMPERATURE_F;
    (void)orb_model_set_prefs(&prefs);
    const uint32_t hidden[3] = {0};
    (void)orb_model_set_summary(2, hidden);
    orb_time_init();
    (void)orb_sound_init();
    (void)orb_app_play_init();
    orb_apps_init(ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_AUDIO | ORB_APP_NEEDS_STORAGE);
    /* A strong production provider must have replaced the registry's proof
     * fallback. Fail visibly if the target forgot to link orb_games.c. */
    if (!orb_apps_find("time") || !orb_apps_find("orbit") || !orb_apps_find("marble")) return false;
    const voice_ops_t ops = {voice_open, voice_close, voice_read, voice_audio,
        voice_request, voice_answer, voice_publish, voice_focus};
    voice_core_init(&s_voice, &ops, NULL);
    voice_core_connection(&s_voice, true, 1, s_now);
    s_shell = orb_shell_create(lv_display_get_screen_active(display), shell_action, NULL);
    if (!s_shell) return false;
    s_status = (orb_shell_snapshot_t){
        .net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70, .demo = true,
        .device_name = "Orb emulator", .device_id = "orb-000000000000",
        .hub_name = "Fixture Mac mini", .ssid = "Demo Wi-Fi", .firmware_version = "desktop emulator",
    };
    clock_status();
    orb_shell_update(s_shell, &s_status);
    return true;
}

orb_shell_t *emu_fixture_shell(void) { return s_shell; }

static void debug_destination(orb_shell_surface_t surface)
{
    orb_shell_app_state_t app;
    orb_shell_apps_snapshot(&app);
    if (orb_apps_active() || app.pending || s_navigation_pending) {
        s_destination = surface;
        s_navigation_pending = true;
        /* A queued launch may already own the shell mailbox. Registry exit
         * has priority and runs after that launch in this same main iteration. */
        if (!orb_shell_app_request(NULL)) orb_apps_request_exit();
    } else orb_shell_navigate(s_shell, surface);
}

void emu_fixture_input(emu_input_action_t action, int value)
{
    if (!s_shell) return;
    static const orb_shell_surface_t places[] = {
        ORB_SHELL_FACE, ORB_SHELL_AGENTS, ORB_SHELL_HOME,
        ORB_SHELL_APPS, ORB_SHELL_STACK, ORB_SHELL_NOTIFICATIONS,
    };
    switch (action) {
    case EMU_INPUT_NAVIGATE:
        if (value >= 1 && value <= 6) debug_destination(places[value - 1]);
        break;
    case EMU_INPUT_FACE_DOWN: emu_fixture_face_down(value != 0); break;
    default: break;
    }
}

static lv_obj_t *design_control(lv_obj_t *root, const char *glyph)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), glyph)) {
        for (lv_obj_t *obj = lv_obj_get_parent(root); obj; obj = lv_obj_get_parent(obj))
            if (lv_obj_check_type(obj, &lv_button_class)) return obj;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = design_control(lv_obj_get_child(root, i), glyph);
        if (found) return found;
    }
    return NULL;
}

void emu_fixture_step(uint32_t now_ms, unsigned pressed_mask, bool screen_on, float gx, float gy, float gz)
{
    if (!s_shell) return;
    s_now = now_ms;
    /* External power/face-down changes set fixture brightness once, so a
     * button wake remains awake on subsequent frames, just as on the board. */
    if (screen_on != s_screen_on) {
        s_screen_on = screen_on;
        s_status.brightness = screen_on && !s_face_down ? s_awake_brightness : 0;
        if (orb_shell_power_snapshot(s_shell).revision) {
            if (screen_on) (void)orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR);
            else orb_shell_power_sleep(s_shell);
        }
        orb_shell_update(s_shell, &s_status);
    }
    service_actions();
    service_voice();
    service_model();
    orb_buttons_poll(&s_buttons, s_now, pressed_mask);
    orb_button_event_t events[ORB_BUTTON_EVENTS];
    unsigned count = 0;
    while (orb_buttons_next(&s_buttons, &events[count])) {
        orb_button_event_t *event = &events[count++];
        if (!orb_shell_power_snapshot(s_shell).revision && event->gesture == ORB_BUTTON_DOWN &&
            !event->synthetic && s_status.brightness <= 10) {
            if (event->button == ORB_BUTTON_PWR) orb_buttons_consume_press(&s_buttons, event->button);
            s_status.brightness = s_awake_brightness;
            orb_shell_update(s_shell, &s_status);
        }
        if (count == ORB_BUTTON_EVENTS) break;
    }
    orb_shell_apps_service();
    /* The shell owns PWR gestures, including game pause and the power sheet.
     * Keep the raw three-axis sensor vector intact for captured game poses. */
    orb_power_snapshot_t power = orb_shell_power_snapshot(s_shell);
    bool active = power.revision ? power.state == ORB_POWER_ACTIVE && !power.redact : s_status.brightness > 0;
    orb_apps_poll((int64_t)s_now * 1000, false, active && !s_face_down, gx, gy, gz, true);
    orb_shell_apps_publish();
    if (power.revision) {
        /* The integrated controller owns visibility in power evidence. Feeding
         * the legacy70% fixture setting here would resume hidden work each
         * frame just before calm correctly pauses it again. */
        orb_shell_environment(s_shell, gx, gy, gz, power.state == ORB_POWER_ALWAYS_ON);
    } else {
        orb_shell_display_active(s_shell, s_status.brightness > 10);
        orb_shell_environment(s_shell, gx, gy, gz, s_status.brightness > 0 && s_status.brightness <= 10);
    }
    if (count) lv_display_trigger_activity(lv_display_get_default());
    if (s_now / 1000 != s_clock_second) {
        s_clock_second = s_now / 1000;
        if (!s_design_fixture) clock_status();
        orb_shell_update(s_shell, &s_status);
    }
    for (unsigned i = 0; i < count; ++i) orb_shell_button(s_shell, &events[i]);
    if (s_design_wake_pending && s_now - s_design_started >= 300) {
        s_design_wake_pending = false;
        (void)orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR);
    }
    if (s_design_fixture || orb_shell_power_snapshot(s_shell).revision) orb_shell_power_advance(s_shell, s_now);
    power = orb_shell_power_snapshot(s_shell);
    if (power.revision && s_status.brightness != power.brightness_pct) {
        /* Publish the controller's actual panel level, as main's board
         * snapshot does. A consumed wake must not leave the fixture at 0%. */
        s_status.brightness = power.brightness_pct;
        orb_shell_update(s_shell, &s_status);
    }
    orb_shell_poll(s_shell);
    if (s_design_silent_pending) {
        lv_obj_t *silent = design_control(orb_shell_root(s_shell), ORB_ICON_BELL);
        if (silent) {
            s_design_silent_pending = false;
            /* ControlCenter.dc.html explicitly depicts Silent On. This is
             * the same local control action, with no platform audio output. */
            lv_obj_send_event(silent, LV_EVENT_CLICKED, NULL);
        }
    }
    if (s_navigation_pending && !orb_apps_active()) {
        orb_shell_app_state_t app;
        orb_shell_apps_snapshot(&app);
        if (!app.pending) {
            s_navigation_pending = false;
            /* The production shell has consumed the exit revision and
             * restored its saved screen/Time fallback before this route. */
            orb_shell_navigate(s_shell, s_destination);
        }
    }
}

void emu_fixture_face_down(bool face_down)
{
    s_face_down = face_down;
    s_status.brightness = s_screen_on && !face_down ? s_awake_brightness : 0;
    if (s_shell) orb_shell_update(s_shell, &s_status);
    orb_sound_set_muted(false, face_down);
}

void emu_fixture_shutdown(void)
{
    if (!s_shell) return;
    orb_apps_exit();
    orb_shell_apps_publish();
    orb_shell_poll(s_shell);
    orb_shell_destroy(s_shell);
    s_shell = NULL;
}


/* Exported frames contain synthetic examples, never hub data. Keep fixture
 * substitution explicit so ordinary desktop runs still show their demo label
 * and the local clock. Both the SDL screenshots and geometry tests use this. */
bool emu_fixture_design_scene(const char *name)
{
    static const struct { const char *name; orb_shell_surface_t surface; } scenes[] = {
        {"Face", ORB_SHELL_FACE}, {"FaceWaiting", ORB_SHELL_FACE},
        {"AlwaysOn", ORB_SHELL_FACE}, {"AlwaysOnEmpty", ORB_SHELL_FACE},
        {"AlwaysOnShifted", ORB_SHELL_FACE}, {"Sleep", ORB_SHELL_FACE}, {"Wake", ORB_SHELL_FACE},
        {"Stack", ORB_SHELL_STACK},
        {"Notifications", ORB_SHELL_NOTIFICATIONS},
        {"NotificationDetail", ORB_SHELL_NOTIFICATION_DETAIL},
        {"Offline", ORB_SHELL_OFFLINE}, {"Charging", ORB_SHELL_CHARGING},
        {"Apps", ORB_SHELL_APPS}, {"ControlCenter", ORB_SHELL_CONTROL_CENTER},
        {"Compass", ORB_SHELL_COMPASS}, {"PowerSheet", ORB_SHELL_FACE},
        {"Settings", ORB_SHELL_SETTINGS}, {"Setup", ORB_SHELL_FACE},
        {"Updating", ORB_SHELL_UPDATES}, {"SoundSettings", ORB_SHELL_SOUND},
        {"VolumeHUD", ORB_SHELL_FACE}, {"SpokenReply", ORB_SHELL_FACE},
        {"Battery", ORB_SHELL_BATTERY}, {"Onboarding", ORB_SHELL_ONBOARDING},
        {"NowPlaying", ORB_SHELL_STACK}, {"AlarmRinging", ORB_SHELL_FACE},
    };
    size_t scene;
    for (scene = 0; scene < sizeof(scenes) / sizeof(scenes[0]); ++scene)
        if (name && !strcmp(name, scenes[scene].name)) break;
    if (!s_shell || scene == sizeof(scenes) / sizeof(scenes[0])) return false;
    s_design_fixture = true;
    orb_time_init();
    orb_sound_set_volume(60);
    s_design_silent_pending = !strcmp(name, "ControlCenter");
    s_design_wake_pending = false;
    s_design_power = !strncmp(name, "AlwaysOn", 8) || !strcmp(name, "Sleep") || !strcmp(name, "Wake");
    s_design_started = s_now;
    s_status = (orb_shell_snapshot_t){.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70};
    orb_model_reset();
    /* Explicit fake hub metadata, never a fallback Mac voice name. */
    if (!strcmp(name, "SoundSettings")) orb_model_set_speech_voice("Ava");
    orb_prefs_t preferences;
    orb_model_prefs_defaults(&preferences);
    preferences.temperature = ORB_TEMPERATURE_F;
    if (!orb_model_set_prefs(&preferences)) return false;
    const bool waiting = !strcmp(name, "FaceWaiting") || !strcmp(name, "AlwaysOn") ||
        !strcmp(name, "AlwaysOnShifted") || !strcmp(name, "Sleep") || !strcmp(name, "Wake");
    const orb_session_t sessions[] = {
        {.id = "design-claude", .agent = ORB_AGENT_CLAUDE, .title = "esp32s3-firmware",
         .project = "esp32s3-firmware", .state = waiting ? ORB_SESSION_WAITING : ORB_SESSION_DONE,
         .reason = "Build finished", .summary = "Firmware 0.4.1 built in 2 min 14 s. 638 tests passed. Ready to flash to the dev board.",
         .can_message = true},
        {.id = "design-dot", .agent = ORB_AGENT_DOT, .title = "Dot",
         .state = waiting ? ORB_SESSION_WAITING : ORB_SESSION_DONE, .reason = "Replied", .can_message = true},
        {.id = "design-codex", .agent = ORB_AGENT_CODEX, .title = "Updates", .state = ORB_SESSION_DONE},
    };
    if (!orb_model_sessions_chunk(sessions, sizeof(sessions) / sizeof(sessions[0]), true, false)) return false;
    if (!strcmp(name, "Notifications") || !strcmp(name, "NotificationDetail")) {
        const orb_card_t cards[] = {
            {.id = "system-update-design", .kind = ORB_CARD_ATTENTION,
             .title = "Updated to 0.4.0", .body = "Your Orb is up to date."},
            {.id = "home-design-door", .kind = ORB_CARD_ATTENTION,
             .title = "Front door unlocked", .body = "Front door unlocked"},
            {.id = "design-build", .session = "design-claude", .kind = ORB_CARD_ATTENTION,
             .title = "Build finished", .body = "Firmware 0.4.1 built in 2 min 14 s. 638 tests passed. Ready to flash to the dev board.", .reply = true},
            {.id = "design-dot-reply", .session = "design-dot", .kind = ORB_CARD_MESSAGE,
             .title = "Dot replied", .body = "Your answer is ready."},
        };
        for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); ++i)
            if (!orb_model_upsert_card(&cards[i])) return false;
    }
    if (waiting) {
        const orb_card_t decisions[] = {
            {.id = "design-wait-1", .session = "design-claude", .kind = ORB_CARD_APPROVAL,
             .title = "Run command?", .body = "Waiting for a decision", .options_count = 2,
             .options = {{.id = "deny", .label = "Deny"}, {.id = "allow", .label = "Allow"}}},
            {.id = "design-wait-2", .session = "design-dot", .kind = ORB_CARD_CHOICE,
             .title = "Choose next step", .body = "Waiting for a choice", .options_count = 2,
             .options = {{.id = "continue", .label = "Continue"}, {.id = "cancel", .label = "Cancel"}}},
        };
        for (size_t i = 0; i < sizeof(decisions) / sizeof(decisions[0]); ++i)
            if (!orb_model_upsert_card(&decisions[i])) return false;
    }
    orb_model_set_connection(true, true);
    orb_home_room_t rooms[] = {{.id = "Living Room", .name = "Living Room"}, {.id = "Office", .name = "Office"}};
    orb_home_accessory_t accessories[11] = {0};
    for (unsigned i = 0; i < 10; ++i) {
        snprintf(accessories[i].id, sizeof(accessories[i].id), "design-light-%u", i);
        snprintf(accessories[i].name, sizeof(accessories[i].name), "Light %u", i + 1);
        snprintf(accessories[i].room, sizeof(accessories[i].room), "%s", i < 3 ? "Living Room" : "Office");
        accessories[i].kind = ORB_HOME_LIGHT;
        accessories[i].reachable = true;
        accessories[i].writable = ORB_HOME_ON;
        /* The Stack frame is a night with only the Living Room left on. */
        accessories[i].state = (orb_home_state_t){.present = ORB_HOME_ON,
            .on = i < 3 || strcmp(name, "Stack")};
    }
    accessories[10] = (orb_home_accessory_t){.id = "design-temp", .name = "Temperature", .room = "Living Room",
        .kind = ORB_HOME_TEMPERATURE, .reachable = true,
        .state = {.present = ORB_HOME_TEMP, .temp = 21.666666f}};
    if (!orb_home_snapshot_chunk_available(rooms, 2, accessories, 11, NULL, 0, true, true, false)) return false;
    orb_home_set_connection(true, true);
    s_status.demo = false;
    snprintf(s_status.device_name, sizeof(s_status.device_name), "Orb");
    snprintf(s_status.clock_time, sizeof(s_status.clock_time), "%s", !strcmp(name, "Stack") ? "23:09" : "10:09");
    snprintf(s_status.clock_date, sizeof(s_status.clock_date), "Tuesday, October 27");
    snprintf(s_status.hub_name, sizeof(s_status.hub_name), "Alex’s Mac mini");
    snprintf(s_status.firmware_version, sizeof(s_status.firmware_version), "0.4.1");
    snprintf(s_status.ssid, sizeof(s_status.ssid), "Demo Home");
    if (!strcmp(name, "Stack")) {
        orb_time_timer_reset();
        if (!orb_time_timer_set(210) || !orb_time_timer_start((int64_t)s_now * 1000)) return false;
        orb_time_timer_pause((int64_t)s_now * 1000);
    }
    if (!strcmp(name, "Setup")) {
        s_status.link = ORB_SHELL_LINK_PAIRING;
        snprintf(s_status.pairing_code, sizeof(s_status.pairing_code), "482915");
    } else if (!strcmp(name, "Updating")) {
        s_status.ota = ORB_SHELL_OTA_DOWNLOADING;
        s_status.progress = 64;
        snprintf(s_status.version, sizeof(s_status.version), "0.4.1");
    } else if (!strcmp(name, "Offline")) {
        s_status.link = ORB_SHELL_LINK_OFFLINE;
        orb_model_set_connection(false, false);
        orb_home_set_connection(false, false);
    } else if (!strcmp(name, "Charging")) {
        s_status.battery_pct = 47;
        s_status.charging = s_status.usb = true;
    }
    if (!strcmp(name, "Battery")) {
        s_status.desk_charge_limit = true; s_status.charging = true;
        s_status.battery_detail = (orb_battery_detail_t){.present = true, .usb = true, .charging = true,
            .battery_pct = 80, .health_valid = true, .health_pct = 96,
            .temperature_valid = true, .temperature_c = 25};
    }
    if (!strcmp(name, "NowPlaying")) {
        /* Only this explicit offline fixture supplies sample media data. */
        orb_model_nowplaying_capability(true);
        const orb_nowplaying_t media = {.app = "Music", .title = "Midnight City", .artist = "M83",
            .playing = true, .volume = 60, .duration = 243, .position = 121};
        if (!orb_model_set_nowplaying(&media)) return false;
        orb_time_timer_reset(); orb_home_set_connection(false, false);
        snprintf(s_status.hub_name, sizeof(s_status.hub_name), "Mac mini");
    }
    if (!strcmp(name, "AlarmRinging")) {
        struct tm before = {.tm_year = 126, .tm_mon = 9, .tm_mday = 28, .tm_hour = 6, .tm_min = 59, .tm_isdst = -1};
        time_t wall = mktime(&before);
        orb_time_poll((int64_t)s_now * 1000, wall);
        if (!orb_time_alarm_set(7, 0) || !orb_time_alarm_enable(true, wall)) return false;
        orb_time_poll((int64_t)s_now * 1000, wall + 60);
    }
    /* Start the retained history from this exact snapshot, without importing
     * unrelated demo notifications from emu_fixture_init(). */
    orb_shell_destroy(s_shell);
    s_shell = orb_shell_create(lv_screen_active(), shell_action, NULL);
    if (!s_shell) return false;
    orb_shell_update(s_shell, &s_status);
    orb_shell_poll(s_shell);
    orb_shell_navigate(s_shell, scenes[scene].surface);
    if (!strcmp(name, "VolumeHUD")) {
        orb_sound_set_volume(59); orb_shell_poll(s_shell);
        orb_sound_set_volume(60); orb_shell_poll(s_shell);
    }
    if (!strcmp(name, "SpokenReply")) {
        const orb_voice_state_t origin = {.stream = 1, .target = "dot"};
        if (!orb_model_set_voice(&origin)) return false;
        orb_shell_spoken_reply(s_shell, true, false,
            "Standup at 9:30, then a free afternoon. Rain after 4, so take the umbrella.");
    }
    if (!strcmp(name, "Notifications") || !strcmp(name, "NotificationDetail")) {
        orb_shell_fixture_notification_age(s_shell, "design-build", 2 * 60 * 1000);
        orb_shell_fixture_notification_age(s_shell, "home-design-door", 9 * 60 * 1000);
        orb_shell_fixture_notification_age(s_shell, "system-update-design", 60 * 60 * 1000);
        orb_shell_fixture_notification_age(s_shell, "design-dot-reply", 0);
    }
    if (!strcmp(name, "NotificationDetail")) {
        orb_shell_select_card(s_shell, "design-build");
        orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATION_DETAIL);
    }
    if (s_design_power) {
        orb_shell_power_enable(s_shell, true);
        if (!strcmp(name, "Sleep")) orb_shell_power_sleep(s_shell);
        else orb_shell_set_quiet(s_shell, true);
        if (!strcmp(name, "AlwaysOnShifted")) {
            orb_power_snapshot_t power = orb_shell_power_snapshot(s_shell);
            orb_shell_power_advance(s_shell, power.now_ms + 60000);
        }
        s_design_wake_pending = !strcmp(name, "Wake");
    }
    if (!strcmp(name, "PowerSheet")) {
        const orb_button_event_t hold = {.button = ORB_BUTTON_PWR, .gesture = ORB_BUTTON_HOLD};
        orb_shell_button(s_shell, &hold);
    }
    return true;
}
