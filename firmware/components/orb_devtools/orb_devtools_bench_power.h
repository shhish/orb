#pragma once

#include "orb_shell.h"

/* Benchmark ownership suspends ordinary shell polling. Take it only after
 * the guarded wake retires its public Face on the top layer; otherwise that
 * frozen overlay, including its clock, is rendered over every fixture. */
static inline bool orb_devtools_bench_power_settled(orb_shell_t *shell)
{
    if (!shell) return false;
    orb_power_snapshot_t power = orb_shell_power_snapshot(shell);
    return power.state == ORB_POWER_ACTIVE && !power.redact && power.panel_on &&
           power.moment == ORB_POWER_MOMENT_NONE;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline bool orb_devtools_bench_power_ready(orb_shell_t *shell, bool (*wake)(void))
{
    if (!shell || !wake || !wake()) return false;
    int64_t deadline = esp_timer_get_time() + 2000000;
    do {
        lv_lock();
        bool ready = orb_devtools_bench_power_settled(shell);
        lv_unlock();
        if (ready) return true;
        /* Keep the UI owner running its normal transition and presentation
         * fence. No synthetic contact, unlock or power-state shortcut. */
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);
    ESP_LOGW("devtools", "Benchmark wake did not settle");
    return false;
}
#endif
