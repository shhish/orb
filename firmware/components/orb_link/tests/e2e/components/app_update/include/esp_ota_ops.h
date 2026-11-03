#pragma once

/* TEST ONLY: the one call orb_link makes, for the linux target. */

#include "esp_partition.h"

const esp_partition_t *esp_ota_get_running_partition(void);
