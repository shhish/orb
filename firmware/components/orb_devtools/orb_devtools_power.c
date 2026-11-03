#include "orb_devtools_power.h"
#include "orb_light_sleep.h"
#include "orb_power_board.h"
#include "orb_board.h"
#include "orb_display.h"
#include "esp_pm.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

void orb_devtools_power_report(FILE *out, orb_devtools_power_printf_fn output)
{
    static uint64_t last_frames;
    orb_display_counters_t counters;
    orb_display_counters(&counters);
    const char *panel = orb_display_panel_ready(true) ? "awake" : orb_display_panel_ready(false) ? "asleep" : "transition";
    output("\n@@ORBDEV POWER BEGIN\n");
    output("@@ORBDEV POWER panel=%s imu_mode=%u(0:unknown,1:gyro224,2:tap250,3:accel21) poll_ms=%u frames_since_report=%llu frames_total=%llu\n",
        panel, (unsigned)orb_board_motion_mode(),
        (unsigned)orb_power_board_poll_period(), (unsigned long long)(counters.frames - last_frames),
        (unsigned long long)counters.frames);
    last_frames = counters.frames;
    /* The 4 KiB command stack also carries the Wi-Fi output path. */
    orb_board_brightness_write_t *writes = heap_caps_malloc(48 * sizeof(*writes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = writes ? orb_board_brightness_history(writes, 48) : 0;
    output("@@ORBDEV POWER brightness_writes");
    for (size_t i = 0; i < n; i++) output(" %lld:%u", (long long)(writes[i].at_us / 1000), writes[i].pct);
    output("\n");
    heap_caps_free(writes);
    orb_light_sleep_status_t sleep;
    orb_light_sleep_status(&sleep);
    output("@@ORBDEV POWER sleep_eligible=%d sleep_block=%s wake_ready=%d usb_override=%d sleep_entries=%llu sleep_us=%llu\n",
        sleep.eligible, orb_light_sleep_reason_name(sleep.reason), sleep.wake_ready, sleep.usb_override,
        (unsigned long long)sleep.entries, (unsigned long long)sleep.slept_us);
    int64_t retry_us = sleep.retry_at_us ? sleep.retry_at_us - esp_timer_get_time() : 0;
    output("@@ORBDEV POWER sleep_fault=%s sleep_error=%ld sleep_failures=%lu sleep_retries=%lu sleep_recoveries=%lu sleep_retry_ms=%lld\n",
        orb_light_sleep_fault_name(sleep.fault), (long)sleep.last_error,
        (unsigned long)sleep.failures, (unsigned long)sleep.retries,
        (unsigned long)sleep.recoveries, (long long)(retry_us > 0 ? (retry_us + 999) / 1000 : 0));
#if CONFIG_PM_ENABLE
    esp_pm_config_t config;
    esp_err_t err = esp_pm_get_configuration(&config);
    if (err == ESP_OK)
        output("@@ORBDEV POWER min_mhz=%d max_mhz=%d light_sleep=%d\n",
               config.min_freq_mhz, config.max_freq_mhz, config.light_sleep_enable);
    else output("@@ORBDEV POWER configuration unavailable: %s\n", esp_err_to_name(err));
    err = esp_pm_dump_locks(out);
    if (err != ESP_OK) output("@@ORBDEV POWER locks unavailable: %s\n", esp_err_to_name(err));
#else
    output("@@ORBDEV POWER disabled: enable CONFIG_PM_ENABLE\n");
#endif
    output("@@ORBDEV POWER END\n");
}

bool orb_devtools_lightsleep_command(const char *cmd, orb_devtools_power_reply_fn output)
{
    bool allowed = !strcmp(cmd, "lightsleep usb on");
    if (!allowed && strcmp(cmd, "lightsleep usb off")) {
        output("@@ORBDEV ERROR lightsleep reason=invalid_arguments; usb on|off\n");
        return false;
    }
    if (!orb_light_sleep_set_usb_override(allowed)) {
        output("@@ORBDEV ERROR lightsleep reason=unavailable\n");
        return false;
    }
    output("@@ORBDEV LIGHTSLEEP usb_override=%d persistent=0\n", allowed);
    return true;
}
