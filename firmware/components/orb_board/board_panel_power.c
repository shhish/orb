#include "orb_board.h"
#include "orb_panel_power.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdatomic.h>

/* All mutation and commands belong to lcd_send, just like pixel windows and
 * brightness. Readers see only the completed state, never half a timestamp. */
static orb_panel_power_t s_panel = { .requested = true, .display_on = true };
static atomic_bool s_ready_awake = true, s_ready_asleep;
static int64_t s_retry_us;

/* Private wire seam: drains DMA before and the command after transmission. */
esp_err_t orb_board_panel_command(uint8_t command);
esp_err_t orb_board_panel_bus_release(void);

void orb_board_panel_service(void)
{
    int64_t now = esp_timer_get_time();
    if (now < s_retry_us) return;
    for (unsigned i = 0; i < 2; i++) {
        uint8_t command = orb_panel_power_next(&s_panel, now);
        if (!command) break;
        esp_err_t err = orb_board_panel_command(command);
        now = esp_timer_get_time();
        if (err != ESP_OK) {
            ESP_LOGE("board", "panel power %02x: %s", command, esp_err_to_name(err));
            /* A failed completion may still have reached the controller. Do
             * not violate its command exclusion window when retrying. */
            if (command == 0x10 || command == 0x11) s_panel.ready_us = now + 120000;
            s_retry_us = now + 50000;
            break;
        }
        orb_panel_power_done(&s_panel, command, now);
    }
    bool asleep = orb_panel_power_ready(&s_panel, false);
    if (asleep && orb_board_panel_bus_release() != ESP_OK) {
        asleep = false;
        s_retry_us = now + 50000;
    }
    s_ready_awake = orb_panel_power_ready(&s_panel, true);
    s_ready_asleep = asleep;
}

esp_err_t orb_board_panel_awake(bool awake)
{
    s_panel.requested = awake;
    s_ready_awake = s_ready_asleep = false;
    orb_board_panel_service();
    return ESP_OK;
}

bool orb_board_panel_ready(bool awake)
{
    return awake ? s_ready_awake : s_ready_asleep;
}

uint32_t orb_board_panel_wait_ms(void)
{
    if (orb_board_panel_ready(s_panel.requested)) return UINT32_MAX;
    int64_t until = s_retry_us;
    if (!orb_panel_power_ready(&s_panel, s_panel.requested) && s_panel.ready_us > until)
        until = s_panel.ready_us;
    int64_t delay = until - esp_timer_get_time();
    return delay > 0 ? (uint32_t)((delay + 999) / 1000) : 1;
}
