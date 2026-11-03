#pragma once

void orb_test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(tag, ...) orb_test_log(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
