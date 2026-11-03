#pragma once
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
/* IDF evaluates the call in release builds too. assert() would discard monitor
 * start/stop and callback registration under the simulator's NDEBUG. */
#define ESP_ERROR_CHECK(call) do { \
    esp_err_t error = (call); \
    if (error != ESP_OK) abort(); \
} while (0)
