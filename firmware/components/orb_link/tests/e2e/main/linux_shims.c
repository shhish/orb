/* TEST ONLY: ESP-IDF's linux port of esp_timer has the header but no body. */
#include <time.h>

#include "esp_timer.h"

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
