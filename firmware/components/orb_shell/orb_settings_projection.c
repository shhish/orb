#include "orb_settings_host.h"
#include "orb_app_settings.h"
#include "orb_app_settings_units.h"
#include "orb_app_settings_root.h"
#include "orb_app_settings_sound.h"
#include "orb_app_settings_accessibility.h"
#include "orb_app_settings_battery.h"
#include "orb_app_settings_display.h"
#include "orb_sound.h"
#include "orb_fonts.h"
#include "orb_ui.h"

#include <stdio.h>
#include <string.h>

bool orb_settings_host_stage_about(orb_settings_host_t *host, lv_obj_t *parent,
    const orb_shell_snapshot_t *status, const char *connection, bool ready)
{
    if (!status || !connection) return false;
    orb_settings_about_model_t model = {.ready = ready};
    snprintf(model.firmware_version, sizeof(model.firmware_version), "%s", status->firmware_version);
    snprintf(model.device_id, sizeof(model.device_id), "%s", status->device_id);
    snprintf(model.hub_name, sizeof(model.hub_name), "%s", status->hub_name);
    snprintf(model.connection_text, sizeof(model.connection_text), "%s", connection);
    return orb_settings_host_stage(host, parent, ORB_SETTINGS_ABOUT_SCREEN_ID,
        &(orb_screen_model_t){&model, sizeof(model)});
}

bool orb_settings_host_stage_units(orb_settings_host_t *host, lv_obj_t *parent,
    orb_screen_id_t screen, const orb_model_snapshot_t *model, const char *notice)
{
    if (!model || !notice) return false;
    orb_settings_units_model_t projection = {.temperature = model->prefs.temperature,
        .temperature_owner = model->prefs.temperature_owner, .prefs_pending = model->prefs_pending,
        .ready = model->online && model->synced,
        .known = model->prefs_known || (model->online && model->synced)};
    snprintf(projection.prefs_error, sizeof(projection.prefs_error), "%s", model->prefs_error);
    snprintf(projection.notice, sizeof(projection.notice), "%s", notice);
    return orb_settings_host_stage(host, parent, screen, &(orb_screen_model_t){&projection, sizeof(projection)});
}

bool orb_settings_host_stage_root(orb_settings_host_t *host, lv_obj_t *parent,
    const orb_shell_snapshot_t *status, bool silent)
{
    if (!status) return false;
    orb_sound_preferences_t sound;
    orb_sound_preferences(&sound);
    orb_settings_root_model_t model = {.glass_preference = orb_glass_preference(),
        .sound_on = !silent && sound.volume, .interactive = true};
    snprintf(model.hub_name, sizeof(model.hub_name), "%s", status->hub_name);
    snprintf(model.firmware_version, sizeof(model.firmware_version), "%s", status->firmware_version);
    return orb_settings_host_stage(host, parent, ORB_SETTINGS_ROOT_SCREEN_ID,
        &(orb_screen_model_t){&model, sizeof(model)});
}

bool orb_settings_host_stage_accessibility(orb_settings_host_t *host, lv_obj_t *parent,
    bool contrast, const char *notice)
{
    if (!host || !notice) return false;
    orb_settings_accessibility_model_t projection = {.content_size = orb_fonts_content_size(),
        .bold = orb_fonts_bold_text(), .reduce_motion = orb_ui_reduce_motion(),
        .reduce_transparency = orb_ui_reduce_transparency(), .contrast = contrast, .interactive = true};
    snprintf(projection.notice, sizeof(projection.notice), "%s", notice);
    return orb_settings_host_stage(host, parent, ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID,
        &(orb_screen_model_t){&projection, sizeof(projection)});
}

static orb_settings_battery_model_t battery_model(const orb_shell_snapshot_t *status)
{
    const orb_battery_detail_t *d = &status->battery_detail;
    return (orb_settings_battery_model_t){.present = d->present, .usb = d->usb, .charging = d->charging,
        .desk_limit_active = d->desk_limit_active, .health_valid = d->health_valid,
        .temperature_valid = d->temperature_valid, .battery_pct = d->battery_pct,
        .health_pct = d->health_pct, .temperature_c = d->temperature_c,
        .external_usb = status->usb, .desk_charge_limit = status->desk_charge_limit,
        .battery_setting_error = status->battery_setting_error,
        .fahrenheit = orb_model_temperature() != ORB_TEMPERATURE_C};
}

bool orb_settings_host_stage_battery(orb_settings_host_t *host, lv_obj_t *parent, const orb_shell_snapshot_t *status)
{
    orb_settings_battery_model_t model = battery_model(status);
    return orb_settings_host_stage(host, parent, ORB_SETTINGS_BATTERY_SCREEN_ID,
        &(orb_screen_model_t){&model, sizeof(model)});
}

bool orb_settings_host_update_battery(orb_settings_host_t *host, const orb_shell_snapshot_t *status)
{
    orb_settings_battery_model_t model = battery_model(status);
    return orb_settings_host_update(host, ORB_SETTINGS_BATTERY_SCREEN_ID, &(orb_screen_model_t){&model, sizeof(model)});
}

bool orb_settings_host_accessibility_apply(const orb_intent_t *intent, bool *contrast,
    char *notice, size_t notice_size, void *context)
{
    if (intent->kind != ORB_INTENT_SETTING_SET) return false;
    uint32_t key = intent->value.setting.key;
    if (key < ORB_SETTING_ACCESS_TEXT_SIZE || key > ORB_SETTING_ACCESS_CONTRAST) return false;
    if (key == ORB_SETTING_ACCESS_TEXT_SIZE && (intent->value.setting.value < 0 ||
        intent->value.setting.value >= ORB_CONTENT_SIZE_COUNT)) return false;
    orb_fonts_err_t result = ORB_FONTS_OK;
    switch (key) {
    case ORB_SETTING_ACCESS_TEXT_SIZE:
        result = orb_fonts_set_content_size((orb_content_size_t)intent->value.setting.value);
        break;
    case ORB_SETTING_ACCESS_BOLD:
        result = orb_fonts_set_bold_text(intent->value.setting.value != 0);
        break;
    case ORB_SETTING_ACCESS_MOTION: orb_ui_set_reduce_motion(!orb_ui_reduce_motion()); break;
    case ORB_SETTING_ACCESS_TRANSPARENCY: orb_ui_set_reduce_transparency(!orb_ui_reduce_transparency()); break;
    case ORB_SETTING_ACCESS_CONTRAST:
        *contrast = !*contrast;
        if (context) orb_shell_power_set_contrast(context, *contrast);
        break;
    }
    if (result != ORB_FONTS_OK) snprintf(notice, notice_size, "Not saved");
    else if (key > ORB_SETTING_ACCESS_BOLD || !orb_fonts_pending()) notice[0] = '\0';
    /* Font setters may notify observers synchronously; the legacy explicit
     * render still follows their return, including no-op size choices. */
    return true;
}

static orb_settings_display_model_t display_model(orb_settings_host_t *host, void *context, int32_t brightness)
{
    orb_power_preferences_t power = orb_shell_power_preferences(context);
    return (orb_settings_display_model_t){.brightness = brightness,
        .always_on_brightness = power.always_on_brightness_pct, .always_on = power.always_on,
        .motion_wake = power.motion_wake, .quiet = context && orb_shell_quiet(context),
        .has_host = context != NULL, .interactive = true,
        .level_pending = orb_settings_host_setting_pending(host, ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS)};
}

bool orb_settings_host_stage_display(orb_settings_host_t *host, lv_obj_t *parent, void *context, int32_t brightness)
{
    orb_settings_display_model_t model = display_model(host, context, brightness);
    return orb_settings_host_stage(host, parent, ORB_SETTINGS_DISPLAY_SCREEN_ID,
        &(orb_screen_model_t){&model, sizeof(model)});
}

bool orb_settings_host_update_display(orb_settings_host_t *host, void *context, int32_t brightness)
{
    orb_settings_display_model_t model = display_model(host, context, brightness);
    return orb_settings_host_update(host, ORB_SETTINGS_DISPLAY_SCREEN_ID, &(orb_screen_model_t){&model, sizeof(model)});
}

void orb_settings_host_flush_settings(orb_settings_host_t *host, void *context)
{
    int32_t value;
    if (!orb_settings_host_setting_take(host, ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS, &value)) return;
    orb_power_preferences_t power = orb_shell_power_preferences(context);
    power.always_on_brightness_pct = value;
    orb_shell_set_power_preferences(context, &power);
}

__attribute__((weak)) void orb_settings_sound_preferences(orb_sound_preferences_t *preferences)
{
    orb_sound_preferences(preferences);
}

static orb_settings_sound_model_t sound_model(bool silent, const char *voice, const char *notice, bool poll)
{
    orb_settings_sound_model_t model = {.silent = silent, .interactive = true, .defer_save_render = poll};
    orb_settings_sound_preferences(&model.prefs);
    snprintf(model.speech_voice, sizeof(model.speech_voice), "%s", voice);
    snprintf(model.notice, sizeof(model.notice), "%s", notice);
    return model;
}

bool orb_settings_host_stage_sound(orb_settings_host_t *host, lv_obj_t *parent, bool silent,
    const char *voice, const char *notice)
{
    if (!orb_settings_host_group_capture(host, ORB_SETTINGS_SOUND_SCREEN_ID)) return false;
    orb_settings_sound_model_t model = sound_model(silent, voice, notice, false);
    if (!orb_settings_host_stage(host, parent, ORB_SETTINGS_SOUND_SCREEN_ID,
        &(orb_screen_model_t){&model, sizeof(model)})) return false;
    size_t size;
    const orb_screen_context_t *group = orb_settings_host_group_context(host, ORB_SETTINGS_SOUND_SCREEN_ID, &size);
    if (!group || orb_app_settings_sound_restore_group(orb_settings_host_staged(host), group, size)) return true;
    orb_settings_host_cancel_staged(host);
    return false;
}

bool orb_settings_host_update_sound(orb_settings_host_t *host, bool silent, const char *voice, const char *notice)
{
    orb_settings_sound_model_t model = sound_model(silent, voice, notice, true);
    if (!orb_settings_host_update(host, ORB_SETTINGS_SOUND_SCREEN_ID, &(orb_screen_model_t){&model, sizeof(model)})) return false;
    orb_screen_instance_t *current = orb_settings_host_current(host);
    return !orb_screen_alive(current) || orb_screen_descriptor(current)->id != ORB_SETTINGS_SOUND_SCREEN_ID ||
        !orb_app_settings_sound_take_render_request(current);
}
