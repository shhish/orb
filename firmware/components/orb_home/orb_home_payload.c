#include "orb_home_payload.h"
#include "orb_memory.h"
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void *orb_home_payload_create(size_t view_bytes, size_t model_bytes)
{
#ifdef ESP_PLATFORM
    if (!orb_memory_reserve(ORB_MEMORY_MODEL, model_bytes)) return NULL;
    if (!orb_memory_reserve(ORB_MEMORY_WORKSPACE, view_bytes)) {
        orb_memory_release(ORB_MEMORY_MODEL, model_bytes);
        return NULL;
    }
    void *pointer = heap_caps_calloc(1, view_bytes + model_bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pointer) {
        orb_memory_release(ORB_MEMORY_MODEL, model_bytes);
        orb_memory_release(ORB_MEMORY_WORKSPACE, view_bytes);
    }
#else
    /* Desktop payloads retain their existing unbudgeted admission policy. */
    void *pointer = calloc(1, view_bytes + model_bytes);
    if (pointer) {
        orb_memory_track(ORB_MEMORY_MODEL, model_bytes);
        orb_memory_track(ORB_MEMORY_WORKSPACE, view_bytes);
    }
#endif
    return pointer;
}

void orb_home_payload_destroy(void *pointer, size_t view_bytes, size_t model_bytes)
{
    if (!pointer) return;
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_MODEL, model_bytes);
    orb_memory_release(ORB_MEMORY_WORKSPACE, view_bytes);
#else
    orb_memory_untrack(ORB_MEMORY_MODEL, model_bytes);
    orb_memory_untrack(ORB_MEMORY_WORKSPACE, view_bytes);
#endif
    free(pointer);
}
