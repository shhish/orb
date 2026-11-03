#pragma once
#include "esp_err.h"
#include <stddef.h>

esp_err_t orb_fs_port_mount(void);
const char *orb_fs_port_base(void);
void orb_fs_port_lock(void);
void orb_fs_port_unlock(void);
esp_err_t orb_fs_port_usage(size_t *total, size_t *used);
