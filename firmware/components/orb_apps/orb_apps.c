#include "orb_apps.h"
#include "orb_fs.h"

#include <math.h>
#include <string.h>
#include <stdatomic.h>

extern const orb_app_manifest_t orb_marble_manifest;
/* Additive compiled-provider hook: the small proof controller remains the
 * fallback for builds that do not link the game component. */
__attribute__((weak)) bool orb_apps_register_builtins(void) { return false; }
/* Offline services are additive to the game provider and keep their deadlines
 * independently of the active app and the screen's power state. */
__attribute__((weak)) void orb_apps_register_utilities(void) {}
__attribute__((weak)) void orb_apps_poll_utilities(int64_t now_us) { (void)now_us; }
static atomic_int s_requested = ATOMIC_VAR_INIT(-1);
static atomic_int s_pause_requested = ATOMIC_VAR_INIT(-1);

static const orb_app_manifest_t *s_registry[ORB_APPS_MAX];
static size_t s_count;
static uint32_t s_available;
static _Atomic(const orb_app_manifest_t *) s_active;
static atomic_bool s_paused;
static bool s_manual_pause;
static int64_t s_power_since;

void orb_apps_exit(void)
{
    const orb_app_manifest_t *old = s_active;
    /* Revoke ownership before the module's cleanup, so there is always a
     * reliable OS-level way out, including paused apps (hig.md section 7). */
    s_active = NULL;
    s_paused = false;
    s_manual_pause = false;
    atomic_store(&s_pause_requested, -1);
    s_power_since = -1;
    if (old) old->entry->exit();
}

void orb_apps_init(uint32_t available)
{
    orb_apps_exit();
    memset(s_registry, 0, sizeof(s_registry));
    s_count = 0;
    s_available = available;
    atomic_store(&s_requested, -1);
    if (!orb_apps_register_builtins())
        (void)orb_apps_register(&orb_marble_manifest);
    orb_apps_register_utilities();
}

const orb_app_manifest_t *orb_apps_find(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < s_count; ++i) {
        if (!strcmp(s_registry[i]->id, id)) return s_registry[i];
    }
    return NULL;
}

esp_err_t orb_apps_register(const orb_app_manifest_t *manifest)
{
    const uint32_t needs = ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_AUDIO | ORB_APP_NEEDS_NET | ORB_APP_NEEDS_STORAGE;
    if (!manifest || !orb_fs_valid_app_id(manifest->id) || !manifest->name ||
        !manifest->name[0] || strlen(manifest->name) > 32 || !manifest->icon ||
        !manifest->icon[0] || strlen(manifest->icon) > 8 || manifest->colour > 0xffffff ||
        (manifest->needs & ~needs) || !manifest->entry || !manifest->entry->enter ||
        !manifest->entry->exit || !manifest->entry->pause || !manifest->entry->tick)
        return ESP_ERR_INVALID_ARG;
    if (orb_apps_find(manifest->id)) return ESP_ERR_INVALID_STATE;
    if (s_count >= ORB_APPS_MAX) return ESP_ERR_NO_MEM;
    s_registry[s_count++] = manifest;
    return ESP_OK;
}

size_t orb_apps_count(void) { return s_count; }
const orb_app_manifest_t *orb_apps_at(size_t index) { return index < s_count ? s_registry[index] : NULL; }
const orb_app_manifest_t *orb_apps_active(void) { return s_active; }
bool orb_apps_paused(void) { return s_paused; }

esp_err_t orb_apps_enter(const char *id)
{
    const orb_app_manifest_t *app = orb_apps_find(id);
    if (!app) return ESP_ERR_NOT_FOUND;
    if (s_active) return ESP_ERR_INVALID_STATE;
    if ((app->needs & s_available) != app->needs) return ESP_ERR_NOT_SUPPORTED;
    if (orb_fs_ready() || (app->needs & ORB_APP_NEEDS_STORAGE)) {
        esp_err_t err = orb_fs_app_prepare(app->id);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = app->entry->enter();
    if (err != ESP_OK) {
        app->entry->exit(); /* Failed partial entry relinquishes resources. */
        return err;
    }
    s_active = app;
    s_paused = false;
    s_manual_pause = false;
    atomic_store(&s_pause_requested, -1);
    s_power_since = -1;
    return ESP_OK;
}

esp_err_t orb_apps_request_enter(const char *id)
{
    const orb_app_manifest_t *app = orb_apps_find(id);
    if (!app) return ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < s_count; ++i) {
        if (s_registry[i] == app) {
            int empty = -1;
            return atomic_compare_exchange_strong(&s_requested, &empty, (int)i) ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void orb_apps_request_exit(void) { atomic_store(&s_requested, -2); }
void orb_apps_request_pause(bool paused) { atomic_store(&s_pause_requested, paused ? 1 : 0); }

void orb_apps_poll_input(const orb_app_input_t *input, bool pwr_held, bool screen_on)
{
    if (!input) return;
    int64_t now_us = input->now_us;
    orb_apps_poll_utilities(now_us);
    int requested = atomic_exchange(&s_requested, -1);
    if (requested == -2) orb_apps_exit();
    else if (requested >= 0 && (size_t)requested < s_count && !s_active)
        (void)orb_apps_enter(s_registry[requested]->id);
    if (!s_active) return;
    int pause_requested = atomic_exchange(&s_pause_requested, -1);
    if (pause_requested >= 0) s_manual_pause = pause_requested != 0;
    if (pwr_held) {
        if (s_power_since < 0 || now_us < s_power_since) s_power_since = now_us;
        if (now_us - s_power_since >= ORB_APPS_PWR_EXIT_US) {
            orb_apps_exit();
            return;
        }
    } else s_power_since = -1;
    bool paused = !screen_on || s_manual_pause;
    if (paused != s_paused) {
        s_paused = paused;
        s_active->entry->pause(paused);
    }
    if (!s_paused) {
        orb_app_input_t sanitized = *input;
        sanitized.motion_valid = input->motion_valid && isfinite(input->accel_x) &&
            isfinite(input->accel_y) && isfinite(input->accel_z);
        sanitized.accel_x = isfinite(input->accel_x) ? input->accel_x : 0;
        sanitized.accel_y = isfinite(input->accel_y) ? input->accel_y : 0;
        sanitized.accel_z = isfinite(input->accel_z) ? input->accel_z : 0;
        sanitized.gyro_valid = sanitized.motion_valid && input->gyro_valid &&
            isfinite(input->gyro_x) && isfinite(input->gyro_y) && isfinite(input->gyro_z);
        sanitized.gyro_x = sanitized.gyro_valid ? input->gyro_x : 0;
        sanitized.gyro_y = sanitized.gyro_valid ? input->gyro_y : 0;
        sanitized.gyro_z = sanitized.gyro_valid ? input->gyro_z : 0;
        s_active->entry->tick(&sanitized);
    }
}

void orb_apps_poll(int64_t now_us, bool pwr_held, bool screen_on,
                   float accel_x, float accel_y, float accel_z, bool motion_valid)
{
    const orb_app_input_t input = {
        .now_us = now_us,
        .motion_sample_us = now_us,
        .accel_x = accel_x,
        .accel_y = accel_y,
        .accel_z = accel_z,
        .motion_valid = motion_valid,
    };
    orb_apps_poll_input(&input, pwr_held, screen_on);
}
