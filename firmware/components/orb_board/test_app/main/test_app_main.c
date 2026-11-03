/* orb_board on its own: buttons, battery and gravity once a second. Lying face up, z reads about +1 g. */
#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_board.h"

static const char *TAG = "test_app";

void app_main(void)
{
    ESP_ERROR_CHECK(orb_board_init());
    for (;;) {
        orb_accel_t g = { 0 };
        orb_power_t p = { 0 };
        orb_board_accel(&g);
        orb_board_power(&p);
        ESP_LOGI(TAG, "buttons %x, battery %d %% (%d mV)%s, gravity %.2f %.2f %.2f (|g| %.2f)", orb_board_buttons(),
                 p.battery_pct, p.battery_mv, p.charging ? " charging" : "", g.x, g.y, g.z,
                 sqrtf(g.x * g.x + g.y * g.y + g.z * g.z));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
