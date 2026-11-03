#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_apps.h"
#include "orb_board.h"
#include "orb_buttons.h"
#include "orb_display.h"
#include "orb_fs.h"
#include "orb_fonts.h"
#include "orb_games.h"
#include "orb_link.h"
#include "orb_mem.h"
#include "orb_model.h"
#include "orb_home.h"
#include "orb_voice.h"
#include "orb_power_services.h"
#include "orb_panel_policy.h"
#include "orb_light_sleep.h"
#include "orb_net.h"
#include "orb_power_board.h"
#include "orb_ota.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "orb_local_services.h"
#include "orb_boot.h"
#include "orb_service_projection.h"
#include "orb_input_service.h"
#include "orb_time.h"
#include "orb_shell_services.h"
#include "orb_ui.h"
#if CONFIG_ORB_DEVTOOLS
#include "orb_devtools.h"
#endif

static const char *TAG = "main";
#define TICK_MS 20
#define GAME_INPUT_US 10000
#define STATUS_US 1000000
#define POWER_US 1000000
#define STATS_US 2000000
#define IDLE_MS (5 * 60 * 1000)
/* Callbacks only wake this task: it reads current service state, avoiding stale
 * OTA callback snapshots. RSSI and the clock also get a one-second refresh.
 * The UI consumes changed snapshots independently of the 50/100 Hz motion. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    orb_shell_snapshot_t view;
    orb_link_power_t power;
    orb_link_display_state_t display_state;
    orb_battery_detail_t battery_detail;
    bool battery_setting_error;
    orb_accel_t gravity;
    int64_t gravity_at;
    int64_t identify_until;
    uint32_t actions;
    uint32_t inactive_ms;
    int brightness;
    int actual_brightness;
    int owner_brightness;
    bool gravity_fresh;
    bool gravity_failed;
    bool status_dirty;
    bool view_dirty;
    bool ui_ready;
    bool hello_accepted;
    bool power_valid;
    bool button_activity;
    int twist_detents;
    bool double_tap;
    orb_idle_policy_t idle_policy;
    bool consume_pwr;
    orb_button_event_t button_events[ORB_BUTTON_EVENTS];
    unsigned button_count;
    orb_button_edge_t button_edges[ORB_BUTTON_HISTORY];
    size_t edge_count;
    struct { orb_button_t button; orb_button_gesture_t gesture; } injections[8];
    unsigned injection_count;
    char configured_name[32];
} s_box = { .brightness = -1, .actual_brightness = 0, .owner_brightness = 70, .power = { .battery_pct = -1 },
            .status_dirty = true, .view_dirty = true, .consume_pwr = true,
            .idle_policy = {.poll_ms = 50, .ui_ms = 20, .taps = true} };
static TaskHandle_t s_main_task;
static orb_shell_t *s_shell;
static char s_service_error[64];
static char s_action_error[64];
static int64_t s_action_error_until;
static orb_buttons_t s_buttons;
static lv_timer_t *s_ui_timer;
static bool s_resting;
static bool stop_speech(void *ctx)
{
    (void)ctx;
    orb_speech_status_t speech;
    orb_voice_speech_status(&speech);
    if (!speech.active) return false;
    orb_voice_speech_stop(ORB_SPEECH_DOUBLE_TAP);
    return true;
}
static void apply_panel_policy(void)
{
    orb_power_preferences_t preferences = orb_shell_power_preferences(s_shell);
    portENTER_CRITICAL(&s_lock);
    if (s_box.owner_brightness != preferences.brightness_pct) {
        s_box.owner_brightness = preferences.brightness_pct;
        s_box.status_dirty = true;
    }
    portEXIT_CRITICAL(&s_lock);
    int brightness = orb_panel_policy_apply(s_shell);
    portENTER_CRITICAL(&s_lock);
    if (s_box.actual_brightness != brightness) {
        s_box.actual_brightness = brightness;
        s_box.status_dirty = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

#if CONFIG_ORB_DEVTOOLS
static size_t button_history(orb_button_edge_t out[ORB_BUTTON_HISTORY], void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
    size_t count = s_box.edge_count;
    memcpy(out, s_box.button_edges, count * sizeof(*out));
    portEXIT_CRITICAL(&s_lock);
    return count;
}

static bool inject_button(orb_button_t button, orb_button_gesture_t gesture, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_box.injection_count < 8;
    if (accepted) {
        unsigned i = s_box.injection_count++;
        s_box.injections[i].button = button;
        s_box.injections[i].gesture = gesture;
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted && s_main_task) xTaskNotifyGive(s_main_task);
    return accepted;
}
#endif

static bool demo_preview(void)
{
#if CONFIG_ORB_MODEL_DEMO
    return true;
#else
    return false;
#endif
}

static void *ui_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void wake_main(void)
{
    if (s_main_task) xTaskNotifyGive(s_main_task);
}

static void status_changed(void)
{
    portENTER_CRITICAL(&s_lock);
    s_box.status_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    wake_main();
}

static void on_net(const orb_net_status_t *status, void *ctx)
{
    status_changed();
}

static void on_ota(const orb_ota_status_t *status, void *ctx)
{
    status_changed();
}

static void on_action(orb_shell_action_t action, void *ctx)
{
    /* Shell callbacks hold LVGL; copy the value and let the main task queue it
     * behind display transfers. No service/display work runs in this callback. */
    int brightness = action == ORB_SHELL_ACTION_BRIGHTNESS ? orb_shell_brightness(s_shell) : -1;
    portENTER_CRITICAL(&s_lock);
    s_box.actions |= 1u << (unsigned)action;
    if (brightness >= 0) s_box.brightness = brightness;
    portEXIT_CRITICAL(&s_lock);
    wake_main();
}

static void ui_tick(lv_timer_t *timer)
{
    if (orb_display_sleep_poll()) {
        orb_shell_snapshot_t status;
        portENTER_CRITICAL(&s_lock);
        bool dirty = s_box.view_dirty;
        bool buttons = s_box.button_count || s_box.button_activity;
        if (dirty) status = s_box.view;
        portEXIT_CRITICAL(&s_lock);
        /* Keep the full mailbox dirty until normal presentation resumes. */
        orb_shell_power_poll(s_shell, dirty ? &status : NULL);
        if (!buttons && orb_shell_power_snapshot(s_shell).state == ORB_POWER_SLEEP) return;
        orb_display_set_sleep_poll(false, NULL);
        wake_main();
    }
    static int64_t last_gravity;
    static uint32_t last_inactivity;
    bool benchmark = false;
#if CONFIG_ORB_DEVTOOLS
    benchmark = orb_devtools_benchmark_active();
#endif
    orb_shell_snapshot_t view;
    portENTER_CRITICAL(&s_lock);
    bool changed = s_box.view_dirty && !benchmark;
    if (changed) {
        view = s_box.view;
        s_box.view_dirty = false;
    }
    orb_accel_t gravity = s_box.gravity;
    int64_t at = s_box.gravity_at;
    bool fresh = s_box.gravity_fresh;
    bool failed_gravity = s_box.gravity_failed;
    bool activity = s_box.button_activity;
    int twist = s_box.twist_detents;
    bool double_tap = s_box.double_tap;
    orb_button_event_t button_events[ORB_BUTTON_EVENTS];
    unsigned button_count = s_box.button_count;
    memcpy(button_events, s_box.button_events, button_count * sizeof(*button_events));
    s_box.gravity_fresh = false;
    s_box.gravity_failed = false;
    s_box.button_activity = false;
    s_box.button_count = 0;
    s_box.twist_detents = 0;
    s_box.double_tap = false;
    portEXIT_CRITICAL(&s_lock);
    orb_power_snapshot_t power = orb_shell_power_snapshot(s_shell);
    if (!benchmark) orb_shell_display_active(s_shell, power.state == ORB_POWER_ACTIVE && !power.redact);
    if (benchmark) last_gravity = 0;
    else if (failed_gravity) {
        /* The service already broke power-IMU dwell on the failed read. Reset
         * only the presentation filter here, retaining latched face-down. */
        last_gravity = 0;
    } else if (fresh) {
        if (power.state == ORB_POWER_ACTIVE && !power.redact)
            orb_ui_set_gravity(gravity.x, gravity.y, gravity.z,
                               last_gravity ? (at - last_gravity) / 1e6f : TICK_MS / 1e3f);
        power = orb_shell_power_snapshot(s_shell);
        orb_shell_environment(s_shell, gravity.x, gravity.y, power.face_down ? -1 : 1,
                              power.state == ORB_POWER_ALWAYS_ON);
        last_gravity = at;
    }
    if (activity) lv_display_trigger_activity(NULL);
    if (changed) orb_shell_update(s_shell, &view);
    if (!benchmark) {
        for (unsigned i = 0; i < button_count; i++) orb_shell_button(s_shell, &button_events[i]);
        bool sensed_activity = double_tap && orb_shell_double_tap(s_shell);
        if (twist) sensed_activity = orb_shell_twist(s_shell, twist) || sensed_activity;
        if (sensed_activity) lv_display_trigger_activity(NULL);
        orb_speech_status_t speech;
        orb_voice_speech_status(&speech);
        orb_shell_spoken_reply(s_shell, speech.active, speech.buffering, speech.text);
        orb_shell_poll(s_shell);
        apply_panel_policy();
        power = orb_shell_power_snapshot(s_shell);
        portENTER_CRITICAL(&s_lock);
        uint32_t ui_ms = s_box.idle_policy.ui_ms;
        bool policy_changed = s_box.idle_policy.gyro != (power.state == ORB_POWER_ACTIVE && !power.redact);
        s_box.consume_pwr = power.state != ORB_POWER_ACTIVE || power.redact ||
            power.brightness_pct <= 10 || orb_panel_policy_acknowledged() != power.revision;
        portEXIT_CRITICAL(&s_lock);
        lv_timer_set_period(timer, ui_ms);
        if (policy_changed) wake_main();
    }
    /* Five-minute OTA-idle policy needs timely activity, not 50 copies/sec. */
    uint32_t tick = lv_tick_get();
    if (!last_inactivity || tick - last_inactivity >= 250) {
        uint32_t inactive = lv_display_get_inactive_time(NULL);
        portENTER_CRITICAL(&s_lock);
        s_box.inactive_ms = inactive;
        portEXIT_CRITICAL(&s_lock);
        last_inactivity = tick;
    }
    if (changed) {
        portENTER_CRITICAL(&s_lock);
        s_box.ui_ready = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void sleep_ui_poll(void) { ui_tick(s_ui_timer); }

static void build_ui(void *ctx)
{
    s_shell = orb_shell_create(lv_screen_active(), on_action, NULL);
    if (!s_shell) {
        ESP_LOGE(TAG, "could not create system UI");
        return; /* Never mark an image healthy without a working UI. */
    }
    orb_shell_set_speech_stop(s_shell, stop_speech, NULL);
#if CONFIG_ORB_DEVTOOLS
    orb_devtools_set_shell(s_shell);
    orb_devtools_set_buttons(button_history, inject_button, NULL);
#endif
    orb_panel_policy_init();
    orb_shell_power_enable(s_shell, true);
    orb_shell_power_display_ready(s_shell, 0, false);
    s_ui_timer = lv_timer_create(ui_tick, TICK_MS, NULL);
    orb_display_set_power_timer(s_ui_timer);
    if (!s_ui_timer) {
        ESP_LOGE(TAG, "could not create system UI timer");
    } /* The first applied snapshot, not allocation alone, proves UI readiness. */
}

static esp_err_t power_source(orb_link_power_t *out, void *ctx)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_box.power;
    /* The PMU detail and panel state can refresh between report slots; they
     * ride the existing report and never prompt one of their own. */
    out->battery_mv = s_box.battery_detail.battery_mv;
    out->display_state = s_box.display_state;
    bool valid = s_box.power_valid;
    portEXIT_CRITICAL(&s_lock);
    return valid ? ESP_OK : ESP_FAIL;
}

static void on_link(const orb_link_status_t *status, void *ctx)
{
    portENTER_CRITICAL(&s_lock);
    if (status->state == ORB_LINK_ONLINE) s_box.hello_accepted = true;
    /* A new hello is authoritative for the hub-owned name. */
    if (status->state == ORB_LINK_ONLINE) s_box.configured_name[0] = '\0';
    s_box.status_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    orb_voice_wake();
    wake_main();
}

static esp_err_t identify(const cJSON *params, cJSON *result, void *ctx)
{
    int64_t until = esp_timer_get_time() + 10000000;
    portENTER_CRITICAL(&s_lock);
    s_box.identify_until = until;
    s_box.status_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    wake_main();
    return ESP_OK;
}

static esp_err_t configure(const cJSON *params, cJSON *result, void *ctx)
{
    if (!cJSON_IsObject(params)) return ESP_ERR_INVALID_ARG;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(params, "channel");
    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(params, "brightness");
    if (name && (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) >= 32))
        return ESP_ERR_INVALID_ARG;
    if (channel && (!cJSON_IsString(channel) ||
        (strcmp(channel->valuestring, "dev") && strcmp(channel->valuestring, "stable"))))
        return ESP_ERR_INVALID_ARG;
    if (brightness && (!cJSON_IsNumber(brightness) || !isfinite(brightness->valuedouble) ||
        brightness->valuedouble < 0 || brightness->valuedouble > 100 ||
        brightness->valuedouble != brightness->valueint)) return ESP_ERR_INVALID_ARG;
    if (!name && !channel && !brightness) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (name) strlcpy(s_box.configured_name, name->valuestring, sizeof(s_box.configured_name));
    if (brightness) s_box.brightness = brightness->valueint;
    s_box.status_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    wake_main();
    /* Name/channel are persisted by the hub; hello refreshes them on reconnect. */
    return ESP_OK;
}

static void sample_power(void)
{
    orb_power_t power;
    bool valid = orb_board_power(&power) == ESP_OK;
    orb_link_power_t snapshot = { .battery_pct = -1 };
    if (valid) snapshot = (orb_link_power_t){
        .battery_pct = power.battery_pct, .charging = power.charging, .usb = power.usb };
    orb_battery_detail_t detail = {.battery_pct = -1, .battery_mv = -1};
    if (orb_board_battery_detail(&detail) != ESP_OK)
        detail = (orb_battery_detail_t){.battery_pct = -1, .battery_mv = -1};
    portENTER_CRITICAL(&s_lock);
    bool changed = memcmp(&s_box.battery_detail, &detail, sizeof(detail)) || s_box.power.battery_pct != snapshot.battery_pct ||
                   s_box.power.charging != snapshot.charging || s_box.power.usb != snapshot.usb ||
                   s_box.power_valid != valid;
    s_box.power = snapshot;
    s_box.battery_detail = detail;
    s_box.power_valid = valid;
    s_box.status_dirty |= changed;
    portEXIT_CRITICAL(&s_lock);
    static bool initialized;
    if (changed || !initialized) orb_ota_set_power(snapshot.battery_pct, snapshot.charging);
    initialized = true;
}

static void sample_status(int64_t now)
{
    orb_shell_snapshot_t view = { .battery_pct = -1 };
    orb_main_status_project(&view, now, s_service_error, s_action_error, s_action_error_until, demo_preview());
    portENTER_CRITICAL(&s_lock);
    view.battery_pct = s_box.power.battery_pct;
    view.battery_detail = s_box.battery_detail;
    view.battery_setting_error = s_box.battery_setting_error;
    view.charging = s_box.power.charging;
    view.usb = s_box.power.usb;
    view.brightness = s_box.owner_brightness;
    view.identify = now < s_box.identify_until;
    if (s_box.configured_name[0]) strlcpy(view.device_name, s_box.configured_name, sizeof(view.device_name));
    if (memcmp(&s_box.view, &view, sizeof(view)) != 0) {
        s_box.view = view;
        s_box.view_dirty = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool run_actions(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t actions = s_box.actions;
    int brightness = s_box.brightness;
    s_box.actions = 0;
    s_box.brightness = -1;
    portEXIT_CRITICAL(&s_lock);
    if (brightness >= 0 && s_shell) {
        lv_lock();
        orb_power_preferences_t preferences = orb_shell_power_preferences(s_shell);
        preferences.brightness_pct = (uint8_t)brightness;
        orb_shell_set_power_preferences(s_shell, &preferences);
        lv_unlock();
    }
    esp_err_t err = ESP_OK;
    if ((actions & (1u << ORB_SHELL_ACTION_SLEEP)) && s_shell) {
        lv_lock();
        orb_shell_power_sleep(s_shell);
        lv_unlock();
    }
    if (actions & (1u << ORB_SHELL_ACTION_RESTART)) esp_restart();
    uint32_t service_actions = actions & ~((1u << ORB_SHELL_ACTION_BRIGHTNESS) |
        (1u << ORB_SHELL_ACTION_SLEEP) | (1u << ORB_SHELL_ACTION_RESTART));
    if (demo_preview() && service_actions) {
        /* Previewing Connection/Updates must not erase real NVS or send work
         * to a service that is deliberately absent in this developer image. */
        snprintf(s_action_error, sizeof(s_action_error), "Unavailable in offline demo");
        s_action_error_until = esp_timer_get_time() + 5000000;
        return true;
    }
    if (actions & (1u << ORB_SHELL_ACTION_DESK_LIMIT)) {
        err = orb_board_set_desk_charge_limit(!orb_board_desk_charge_limit());
        portENTER_CRITICAL(&s_lock);
        s_box.battery_setting_error = err != ESP_OK;
        s_box.status_dirty = true;
        portEXIT_CRITICAL(&s_lock);
    }
    else if (actions & (1u << ORB_SHELL_ACTION_FORGET_NETWORK)) err = orb_net_forget();
    else if (actions & (1u << ORB_SHELL_ACTION_UNPAIR)) err = orb_link_unpair();
    else if (actions & (1u << ORB_SHELL_ACTION_UPDATE)) err = orb_ota_install();
    else if (actions & (1u << ORB_SHELL_ACTION_CHECK_UPDATE)) err = orb_ota_check_now();
    if (err != ESP_OK) {
        snprintf(s_action_error, sizeof(s_action_error), "Action failed: %s", esp_err_to_name(err));
        s_action_error_until = esp_timer_get_time() + 5000000;
        ESP_LOGW(TAG, "requested action: %s", esp_err_to_name(err));
    } else if (actions) s_action_error_until = 0;
    return actions != 0;
}

static void log_stats(void)
{
    orb_display_stats_t st;
    orb_display_stats(&st);
    multi_heap_info_t internal;
    orb_mem_heap_info(&internal, MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%.1f fps, frame %.1f ms (draw %.1f, wire %.1f), torn %u | internal free %u KB (min %u, largest %u)",
             st.fps, st.frame_ms, st.draw_ms, st.send_ms, (unsigned)st.torn,
             (unsigned)(internal.total_free_bytes / 1024),
             (unsigned)(internal.minimum_free_bytes / 1024),
             (unsigned)(internal.largest_free_block / 1024));
}

static void publish_button_events(bool consume_pwr)
{
    orb_button_event_t event;
    while (orb_buttons_next(&s_buttons, &event)) {
        if (event.button == ORB_BUTTON_PWR && event.gesture == ORB_BUTTON_DOWN &&
            !event.synthetic && consume_pwr) {
            /* The recognizer also consumes this press so release cannot pair
             * a waking contact with the next ordinary awake press. */
            orb_buttons_consume_press(&s_buttons, ORB_BUTTON_PWR);
        }
        portENTER_CRITICAL(&s_lock);
        s_box.button_activity = true;
        if (s_box.button_count < ORB_BUTTON_EVENTS)
            s_box.button_events[s_box.button_count++] = event;
        portEXIT_CRITICAL(&s_lock);
    }
}

static uint32_t poll_buttons(int64_t now)
{
    static unsigned mask;
    static uint64_t last_ms;
    if (!last_ms) mask = orb_board_buttons();
    portENTER_CRITICAL(&s_lock);
    bool consume_pwr = s_box.consume_pwr;
    for (unsigned i = 0; i < s_box.injection_count; i++)
        orb_buttons_inject(&s_buttons, s_box.injections[i].button,
                           s_box.injections[i].gesture, (uint64_t)(now / 1000));
    s_box.injection_count = 0;
    portEXIT_CRITICAL(&s_lock);
    orb_board_button_edge_t edge;
    while (orb_board_button_edge(&edge)) {
        uint64_t at = edge.at_us / 1000;
        if (at < last_ms) at = last_ms;
        /* Replay the held interval before its release. A delayed task cannot
         * turn a real >30 ms press into two unseen pin changes. */
        orb_buttons_poll(&s_buttons, at, mask);
        publish_button_events(consume_pwr);
        orb_buttons_poll(&s_buttons, at, edge.buttons);
        publish_button_events(consume_pwr);
        mask = edge.buttons;
        last_ms = at;
    }
    last_ms = (uint64_t)(esp_timer_get_time() / 1000);
    orb_buttons_poll(&s_buttons, last_ms, mask);
    publish_button_events(consume_pwr);
    portENTER_CRITICAL(&s_lock);
    s_box.edge_count = orb_buttons_history(&s_buttons, s_box.button_edges);
    bool activity = s_box.button_activity;
    portEXIT_CRITICAL(&s_lock);
    if (activity) {
        lv_lock();
        if (s_ui_timer) lv_timer_ready(s_ui_timer);
        lv_unlock();
        orb_display_wake();
    }
    portENTER_CRITICAL(&s_lock);
    /* GPIO edges notify main. Only a live contact's recognizer deadline needs
     * a timer in eligible Sleep; a second 100 ms poll drifts from the IMU. */
    uint32_t maximum = s_box.idle_policy.minimal ? UINT32_MAX : 50;
    portEXIT_CRITICAL(&s_lock);
    return orb_idle_button_wait_limit(&s_buttons, last_ms, maximum);
}

static void button_edge_isr(void *arg)
{
    BaseType_t higher_priority = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)arg, &higher_priority);
    if (higher_priority) portYIELD_FROM_ISR();
}

static int64_t poll_input(void)
{
    portENTER_CRITICAL(&s_lock);
    orb_idle_policy_t policy = s_box.idle_policy;
    bool screen_on = s_box.actual_brightness > 0;
    portEXIT_CRITICAL(&s_lock);
    orb_input_motion_t input = orb_input_motion(policy, screen_on, s_shell);
    s_resting = s_resting && input.senses.resting;
    portENTER_CRITICAL(&s_lock);
    s_box.gravity_failed |= !input.motion;
    if (input.motion) {
        s_box.gravity = input.gravity;
        s_box.gravity_at = input.at_us;
        s_box.gravity_fresh = true;
        s_box.twist_detents = (int)fmaxf(-8, fminf(8, s_box.twist_detents + input.senses.detents));
        s_box.double_tap |= input.senses.double_tap;
    }
    portEXIT_CRITICAL(&s_lock);
    orb_power_board_set_poll_period(policy.gyro && orb_games_motion_active() ? 10 : input.next_poll_ms);
    return input.at_us + (policy.gyro && orb_games_motion_active() ? GAME_INPUT_US : input.next_poll_ms * 1000);
}

static void wake_inputs(void)
{
    orb_board_buttons_sleep_wake();
    orb_display_touch_sleep_wake();
    /* FromISR defers ready-list updates while tickless idle suspends scheduling; no yield. */
    if (s_main_task) vTaskNotifyGiveFromISR(s_main_task, NULL);
}

static void init_models(void)
{
    if (!orb_model_init()) {
        snprintf(s_service_error, sizeof(s_service_error), "Session storage unavailable");
        ESP_LOGE(TAG, "could not create session model");
    }
    if (!orb_home_init()) {
        snprintf(s_service_error, sizeof(s_service_error), "Home storage unavailable");
        ESP_LOGE(TAG, "could not create Home model");
    }
    if (demo_preview()) {
        /* Opt-in preview has a visible Demo identity and no network services;
         * fixture answers remain local even if NVS contains a real pairing. */
        orb_model_load_demo();
        orb_home_load_demo();
        ESP_LOGW(TAG, "offline shell demo enabled; networking and service actions disabled");
    }
}

static orb_link_display_state_t link_display_state(orb_power_state_t state)
{
    switch (state) {
    case ORB_POWER_DIMMED: return ORB_LINK_DISPLAY_DIMMED;
    case ORB_POWER_ALWAYS_ON: return ORB_LINK_DISPLAY_ALWAYS_ON;
    case ORB_POWER_SLEEP: return ORB_LINK_DISPLAY_SLEEP;
    default: return ORB_LINK_DISPLAY_ACTIVE;
    }
}

static orb_idle_policy_t update_idle_policy(bool *changed)
{
    portENTER_CRITICAL(&s_lock);
    bool power_valid = s_box.power_valid, usb = s_box.power.usb;
    orb_idle_policy_t previous = s_box.idle_policy;
    portEXIT_CRITICAL(&s_lock);
    orb_idle_policy_t policy = orb_power_services_policy(s_shell, power_valid, usb);
    lv_lock();
    orb_link_display_state_t display_state = link_display_state(orb_shell_power_snapshot(s_shell).state);
    orb_display_set_sleep_poll(policy.minimal, sleep_ui_poll);
    lv_unlock();
    portENTER_CRITICAL(&s_lock);
    s_box.idle_policy = policy;
    s_box.display_state = display_state;
    portEXIT_CRITICAL(&s_lock);
    *changed = previous.gyro != policy.gyro || previous.minimal != policy.minimal;
    return policy;
}

static void init_ui(void)
{
    ESP_ERROR_CHECK(orb_display_start());
    orb_display_set_band_guard(orb_glass_band_guard);
    s_box.view.net = ORB_SHELL_NET_STARTING;
    s_box.view.battery_pct = -1;
    lv_lock();
    /* Prepare only the saved pair before masks/radio compete for PSRAM and
     * before build_ui can expose a frame with the wrong accessibility set. */
    orb_mem_begin(ORB_MEM_FONT_PRELOAD);
    orb_fonts_set_cache_callbacks(orb_glass_cache_suspend, orb_glass_cache_resume);
    orb_fonts_err_t font_err = orb_fonts_preload_saved();
    orb_mem_end(ORB_MEM_FONT_PRELOAD);
    if (font_err != ORB_FONTS_OK)
        ESP_LOGW(TAG, "saved font preload: %s", orb_fonts_err_name(font_err));
#if CONFIG_ORB_GLASS_BACKDROP_CACHE
    /* Cache only byte-verified backdrop/result pairs. Explicit PSRAM allocation
     * keeps its bounded working set out of networking and audio's internal RAM. */
    const lv_draw_sw_blur_cache_config_t backdrop = {
        .budget_bytes = 192 * 1024,
        .max_entry_bytes = 96 * 1024,
        .alloc_cb = ui_alloc,
        .free_cb = heap_caps_free,
    };
    if (lv_draw_sw_blur_cache_configure(&backdrop) != LV_RESULT_OK)
        ESP_LOGW(TAG, "glass backdrop cache unavailable; using direct rendering");
#endif
    orb_ui_init(&(orb_ui_config_t){ .alloc = ui_alloc, .free = heap_caps_free });
    orb_mem_begin(ORB_MEM_GLASS_PREWARM);
    orb_glass_cache_prewarm();
    orb_mem_end(ORB_MEM_GLASS_PREWARM);
    /* Never expose an unacknowledged startup/task framebuffer. */
    orb_display_brightness(0);
    lv_async_call(build_ui, NULL);
    lv_unlock();
}

void app_main(void)
{
    orb_link_memory_init();
    orb_mem_init();
    s_main_task = xTaskGetCurrentTaskHandle();
    orb_boot_prepare();
    orb_local_services_init();
    orb_apps_init(ORB_APP_NEEDS_IMU | (demo_preview() ? 0 : ORB_APP_NEEDS_NET) |
                  (orb_fs_ready() ? ORB_APP_NEEDS_STORAGE : 0));
    init_models();
    init_ui();
    /* Register all providers before the link can send hello or receive an offer.
     * An explicit demo never starts radio/link/OTA or reads saved credentials. */
    if (!demo_preview()) ESP_ERROR_CHECK(orb_ota_start(on_ota, NULL));
    sample_power();
    if (!demo_preview()) {
        orb_mem_begin(ORB_MEM_VOICE_SERVICE);
        esp_err_t err = orb_voice_start();
        orb_mem_end(ORB_MEM_VOICE_SERVICE);
        if (err != ESP_OK) ESP_LOGE(TAG, "voice startup: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(orb_link_set_power_source(power_source, NULL));
        ESP_ERROR_CHECK(orb_link_on_method("device.identify", identify, NULL));
        ESP_ERROR_CHECK(orb_link_on_method("device.config", configure, NULL));
        orb_mem_begin(ORB_MEM_WIFI_START);
        err = orb_net_start(on_net, NULL);
        orb_mem_end(ORB_MEM_WIFI_START);
        if (err == ESP_OK) err = orb_link_start(on_link, NULL);
        if (err != ESP_OK) {
            snprintf(s_service_error, sizeof(s_service_error), "Connection unavailable: %s", esp_err_to_name(err));
            ESP_LOGE(TAG, "network startup: %s", esp_err_to_name(err));
        }
    }
    bool healthy = false, idle = false;
    orb_buttons_init(&s_buttons);
    ESP_ERROR_CHECK(orb_board_buttons_notify(button_edge_isr, s_main_task));
    orb_light_sleep_arm(orb_display_touch_ready(), wake_inputs);
    int64_t next_input = 0, next_buttons = 0, next_status = 0, next_stats = 0;
    int64_t next_power = esp_timer_get_time() + POWER_US, next_calm = 0;
    ESP_LOGI(TAG, "boot done: main stack free=%u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool was_minimal = orb_display_sleep_poll();
        int64_t deadline = was_minimal ? orb_power_services_deadline(now) : INT64_MAX;
        bool policy_changed;
        orb_idle_policy_t policy = update_idle_policy(&policy_changed);
        if (policy_changed) next_input = next_buttons = 0;
        if (policy.minimal && !was_minimal) deadline = orb_power_services_deadline(now);
        bool calm_due = policy.minimal && now >= next_calm;
        if (calm_due) { orb_display_wake(); next_calm = now + STATUS_US; }
        if (now >= next_buttons || ulTaskNotifyTake(pdTRUE, 0))
            next_buttons = now + (int64_t)poll_buttons(now) * 1000;
        if (now >= next_input) {
            next_input = poll_input();
        }
        if (now >= next_power) {
            sample_power();
            policy = update_idle_policy(&policy_changed);
            if (policy_changed) next_input = next_buttons = 0;
            orb_board_battery_desk_poll(s_resting, (uint64_t)(now / 1000));
            s_resting = true;
            next_power = now + POWER_US;
        }
        if (!policy.minimal || calm_due) orb_local_services_poll();
        bool action = run_actions();
        portENTER_CRITICAL(&s_lock);
        bool dirty = s_box.status_dirty;
        s_box.status_dirty = false;
        bool ready = s_box.ui_ready && s_box.hello_accepted;
        bool now_idle = s_box.ui_ready && s_box.power.charging && s_box.inactive_ms >= IDLE_MS;
        portEXIT_CRITICAL(&s_lock);
        if (ready && !healthy) {
            orb_ota_mark_healthy();
            healthy = true;
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
            /* Check steady-state headroom before exposing the early render arena
             * as two halves. Large refreshes still use the complete band. */
            orb_display_request_double_buffer();
#endif
        }
        if (now_idle != idle) {
            idle = now_idle;
            orb_ota_set_idle(idle);
        }
        if (dirty || action || now >= next_status) {
            sample_status(now);
            next_status = now + STATUS_US;
            portENTER_CRITICAL(&s_lock);
            int64_t identify_until = s_box.identify_until;
            portEXIT_CRITICAL(&s_lock);
            if (identify_until > now && identify_until < next_status) next_status = identify_until;
            if (s_action_error_until > now && s_action_error_until < next_status) next_status = s_action_error_until;
        }
        if (!policy.minimal && now >= next_stats) { log_stats(); next_stats = now + STATS_US; }
        /* Service/button edges interrupt this wait without extra I2C reads.
         * Sensor, game and debounce deadlines retain independent cadence. */
        orb_fonts_service();
        int64_t sound_deadline = orb_sound_preferences_service(esp_timer_get_time());
        int64_t next = next_input < next_buttons ? next_input : next_buttons;
        if (deadline < next) next = deadline;
        if (sound_deadline < next) next = sound_deadline;
        if (next_power < next) next = next_power;
        if (next_status < next) next = next_status;
        int64_t left = next - esp_timer_get_time();
        TickType_t wait = left > 0 ? pdMS_TO_TICKS((left + 999) / 1000) : 0;
        if (ulTaskNotifyTake(pdTRUE, wait ? wait : 1)) next_buttons = 0;
    }
}
