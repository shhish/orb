#include "orb_boot.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "orb_assets.h"
#include "orb_board.h"
#include "orb_fs.h"
#include "orb_mem.h"
#include "orb_nav_compositor.h"
#include "orb_ota.h"
#include "orb_light_sleep.h"

static const char *s_tag = "main";

void orb_boot_prepare(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(orb_ota_boot_check());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(s_tag, "%s %s, IDF %s", app->project_name, app->version, esp_get_idf_version());
    orb_light_sleep_init();
#if CONFIG_ORB_SMALL_ALLOCATIONS_PSRAM
    /* Apply before LVGL creates its small objects; changing this after the UI
     * exists would leave its internal allocations and fragmentation in place.
     * Capability-specific DMA and RTOS allocations keep their own policy. */
    heap_caps_malloc_extmem_enable(0);
    ESP_LOGI(s_tag, "experimental generic allocation preference: PSRAM for all nonzero sizes");
#endif
    orb_mem_begin(ORB_MEM_BOARD_INIT);
    ESP_ERROR_CHECK(orb_board_init());
    orb_mem_end(ORB_MEM_BOARD_INIT);
    /* Navigation never races an optional cache for a full-screen allocation. */
    orb_mem_begin(ORB_MEM_NAV_RESERVE);
    ESP_ERROR_CHECK(orb_nav_compositor_reserve_boot(466, 466) ? ESP_OK : ESP_ERR_NO_MEM);
    orb_mem_end(ORB_MEM_NAV_RESERVE);
    /* Both services fail closed while leaving recovery UI available. Erased
     * storage is initialized once; corrupt media is never autoformatted. */
    orb_mem_begin(ORB_MEM_ASSETS_VERIFY);
    err = orb_assets_init();
    orb_mem_end(ORB_MEM_ASSETS_VERIFY);
    if (err != ESP_OK) ESP_LOGW(s_tag, "assets unavailable; rescue typography: %s", esp_err_to_name(err));
    orb_mem_begin(ORB_MEM_STORAGE_MOUNT);
    err = orb_fs_mount();
    orb_mem_end(ORB_MEM_STORAGE_MOUNT);
    if (err != ESP_OK) ESP_LOGW(s_tag, "app storage unavailable: %s", esp_err_to_name(err));
}
