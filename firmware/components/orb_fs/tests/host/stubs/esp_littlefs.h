#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct {
    const char *base_path;
    const char *partition_label;
    bool format_if_mount_failed;
    bool grow_on_mount;
} esp_vfs_littlefs_conf_t;
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config);
esp_err_t esp_littlefs_format(const char *label);
esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used);
