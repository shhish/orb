#include "orb_fs_port.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if !defined(CONFIG_LITTLEFS_MALLOC_STRATEGY_SPIRAM)
#error "Orb app storage must use PSRAM without an internal-RAM fallback"
#endif

static const char *TAG = "orb_fs";
static StaticSemaphore_t s_mutex_memory;
static SemaphoreHandle_t s_mutex;

const char *orb_fs_port_base(void) { return "/storage"; }
void orb_fs_port_lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void orb_fs_port_unlock(void) { xSemaphoreGive(s_mutex); }
esp_err_t orb_fs_port_usage(size_t *total, size_t *used) { return esp_littlefs_info("storage", total, used); }

static void log_boot_heap(const char *phase)
{
    /* These are snapshots and historical per-heap low-water marks, not a
     * resettable phase monitor. Leave the OTA/developer monitor untouched. */
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "boot %s heap: internal free=%u historical-min=%u largest=%u; "
             "DMA free=%u historical-min=%u largest=%u", phase,
             (unsigned)heap_caps_get_free_size(internal),
             (unsigned)heap_caps_get_minimum_free_size(internal),
             (unsigned)heap_caps_get_largest_free_block(internal),
             (unsigned)heap_caps_get_free_size(dma),
             (unsigned)heap_caps_get_minimum_free_size(dma),
             (unsigned)heap_caps_get_largest_free_block(dma));
}

static esp_err_t mount_storage(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_memory);
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "filesystem mutex unavailable");
    const esp_vfs_littlefs_conf_t config = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .grow_on_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err == ESP_OK) return ESP_OK;
    if (err != ESP_FAIL) return err;
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                               ESP_PARTITION_SUBTYPE_ANY, "storage");
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG, "storage partition missing");
    /* A damaged filesystem is never erased automatically. Migration erases the
     * new partition; verify every byte before first-boot-only formatting. */
    uint8_t block[256];
    for (size_t at = 0; at < partition->size; at += sizeof(block)) {
        size_t length = partition->size - at;
        if (length > sizeof(block)) length = sizeof(block);
        ESP_RETURN_ON_ERROR(esp_partition_read(partition, at, block, length), TAG, "blank check failed");
        for (size_t i = 0; i < length; ++i) {
            ESP_RETURN_ON_FALSE(block[i] == 0xff, ESP_ERR_INVALID_STATE, TAG,
                                "nonempty storage failed mount; preserving data");
        }
    }
    ESP_LOGI(TAG, "initializing erased app storage");
    ESP_RETURN_ON_ERROR(esp_littlefs_format("storage"), TAG, "initial format failed");
    return esp_vfs_littlefs_register(&config);
}

esp_err_t orb_fs_port_mount(void)
{
    log_boot_heap("before storage mount");
    esp_err_t err = mount_storage();
    log_boot_heap("after storage mount");
    return err;
}
