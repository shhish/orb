/* orb_display on its own: a grey screen with a raster-cached rounded card, and the renderer's stats. */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_board.h"
#include "orb_display.h"

static const char *TAG = "test_app";

void app_main(void)
{
    ESP_ERROR_CHECK(orb_board_init());
    ESP_ERROR_CHECK(orb_display_start());

    lv_lock();
    lv_obj_t *card = lv_obj_create(lv_screen_active());
    lv_obj_set_size(card, 300, 120);
    lv_obj_center(card);
    lv_obj_set_raster_matte(card, lv_color_white());
    lv_unlock();
    orb_display_brightness(50);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        orb_display_stats_t st;
        orb_display_stats(&st);
        ESP_LOGI(TAG, "%.1f fps, frame %.1f ms, wire %.1f ms, torn %u", st.fps, st.frame_ms, st.send_ms,
                 (unsigned)st.torn);
    }
}
