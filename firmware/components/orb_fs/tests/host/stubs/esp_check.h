#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(value, error, tag, ...) do { (void)(tag); if (!(value)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(value, tag, ...) do { (void)(tag); esp_err_t stub_err = (value); if (stub_err != ESP_OK) return stub_err; } while (0)
