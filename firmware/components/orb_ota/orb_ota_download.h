#pragma once

#include "orb_ota.h"
#include "orb_ota_logic.h"
#include "esp_partition.h"

#define ORB_OTA_REPORT_STEP 10 /* ota.report every 10 % */

/* Private synchronous boundary. The download owns its HTTP/flash resources;
 * these calls preserve the service owner's original lock, report and NVS order. */
esp_err_t orb_ota_download_app(const orb_ota_offer_t *offer, char *error, size_t size);
esp_err_t orb_ota_download_assets(const orb_ota_offer_t *offer, char *error, size_t size);
void orb_ota_download_state(orb_ota_state_t state, int progress, const char *error);
void orb_ota_download_report(const orb_ota_offer_t *offer, const char *state, int progress, const char *error);
void orb_ota_download_progress(const orb_ota_offer_t *offer, int percent, int *reported);
void orb_ota_download_record_attempt(const orb_ota_offer_t *offer, const esp_partition_t *slot);
void orb_ota_download_clear_attempt(void);
void orb_ota_download_mark_bad(const char *sha);
void orb_ota_download_assets_result(const orb_ota_offer_t *offer, esp_err_t result);
