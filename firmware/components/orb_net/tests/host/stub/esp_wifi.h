#pragma once

#include "esp_err.h"

#define ESP_OK 0
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
esp_err_t esp_wifi_set_ps(wifi_ps_type_t mode);
