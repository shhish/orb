#pragma once

#include "esp_err.h"
#include "esp_partition.h"
#include "orb_assets_format.h"

/* Uses the running signed app's SBv2 key (or eFuse trust if enabled). Never burns fuses.
 * image_size=0 discovers the bounded size from the signed header. */
esp_err_t orb_assets_verify_partition(const esp_partition_t *partition, size_t image_size,
                                      orb_assets_info_t *info);

/* Initialize/verify at boot. Invalid/missing assets are safe: fonts use built-in rescue. */
esp_err_t orb_assets_init(void);
uint32_t orb_assets_version(void); /* 0 when unavailable or during an update */

/* Serialize font reads against a single-slot erase/write. Loaded PSRAM fonts remain
 * valid; new loads use the rescue face until the next boot verifies new contents.
 * Every successful begin must be paired with end, including failed downloads. */
esp_err_t orb_assets_begin_update(void);
void orb_assets_end_update(void);

/* Font port only: serialized, signature-checked payload copy. */
bool orb_assets_read_payload(uint32_t offset, void *dst, size_t size);

/* Worker-only signed sound-section copy; caller frees with heap_caps_free.
 * A PSRAM copy remains valid across a single-slot OTA assets update. */
esp_err_t orb_assets_copy_sounds(void **data, size_t *size);
