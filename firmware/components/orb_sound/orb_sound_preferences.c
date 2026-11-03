#include "orb_sound.h"
#include "orb_sound_save.h"

#if defined(ESP_PLATFORM) || defined(ORB_SOUND_TEST_STORAGE)
#include "nvs.h"
static orb_sound_save_t s_save;
#endif

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_ORB_DEVTOOLS
#include <assert.h>
#include "esp_memory_utils.h"
#endif
static TaskHandle_t s_owner;
#endif

void orb_sound_save_notify(void)
{
#ifdef ESP_PLATFORM
    if (s_owner) xTaskNotifyGive(s_owner);
#endif
}

void orb_sound_preferences_init(void)
{
#ifdef ESP_PLATFORM
    s_owner = xTaskGetCurrentTaskHandle();
#if CONFIG_ORB_DEVTOOLS
    /* A stack-local address checks the executing stack without retaining any
     * diagnostic state or adding RAM to production builds. */
    unsigned marker;
    assert(esp_ptr_internal(&marker));
#endif
#endif
#if defined(ESP_PLATFORM) || defined(ORB_SOUND_TEST_STORAGE)
    nvs_handle_t h;
    if (nvs_open("orb_sound", NVS_READONLY, &h) == ESP_OK) {
        uint32_t packed;
        if (nvs_get_u32(h, "preferences", &packed) == ESP_OK &&
            (packed & 127u) <= 100 && ((packed >> 8) & 31u) <= 23 && ((packed >> 13) & 31u) <= 23)
            orb_sound_save_restore(packed);
        nvs_close(h);
    }
    orb_sound_save_snapshot_t snapshot;
    orb_sound_save_snapshot(&snapshot);
    s_save = (orb_sound_save_t){.observed = snapshot.generation};
#endif
}

int64_t orb_sound_preferences_service(int64_t now_us)
{
#if defined(ESP_PLATFORM) || defined(ORB_SOUND_TEST_STORAGE)
#if defined(ESP_PLATFORM) && CONFIG_ORB_DEVTOOLS
    unsigned marker;
    assert(xTaskGetCurrentTaskHandle() == s_owner && esp_ptr_internal(&marker));
#endif
    orb_sound_save_snapshot_t snapshot;
    orb_sound_save_snapshot(&snapshot);
    uint32_t wait = orb_sound_save_wait(&s_save, snapshot.generation, snapshot.completed, now_us);
    if (!wait) {
        /* Main already owns blocking local persistence. Never commit from the
         * PSRAM audio stack, nor hold the LVGL lock across cache suspension. */
        nvs_handle_t h;
        bool saved = false;
        if (nvs_open("orb_sound", NVS_READWRITE, &h) == ESP_OK) {
            saved = nvs_set_u32(h, "preferences", snapshot.packed) == ESP_OK && nvs_commit(h) == ESP_OK;
            nvs_close(h);
        }
        orb_sound_save_complete(snapshot.generation, saved);
        orb_sound_save_snapshot(&snapshot);
        /* A commit may block while another edit arrives. Leave that generation
         * unobserved until the next poll supplies a fresh monotonic timestamp;
         * backdating it to pre-commit time would shorten its quiet interval. */
        return snapshot.generation == snapshot.completed ? INT64_MAX : now_us;
    }
    return wait == UINT32_MAX ? INT64_MAX : s_save.changed_at_us + 500000;
#else
    (void)now_us;
    return INT64_MAX;
#endif
}
