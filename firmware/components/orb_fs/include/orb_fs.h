#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* All app writes go through this bounded API. No API accepts a raw path. */
#define ORB_FS_MAX_APPS 8
#define ORB_FS_APP_ID_MAX 24
#define ORB_FS_KEY_MAX 32
#define ORB_FS_MAX_FILES 32
#define ORB_FS_FILE_LIMIT (64u * 1024u)
#define ORB_FS_APP_QUOTA (256u * 1024u)
#define ORB_FS_RESERVE (128u * 1024u)

/* Boot-task only. A completely erased partition is formatted once. A failed
 * mount of nonempty media never formats it, preserving recoverable data. */
esp_err_t orb_fs_mount(void);
bool orb_fs_ready(void);
bool orb_fs_valid_app_id(const char *id);
esp_err_t orb_fs_app_prepare(const char *id);
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size);
/* fsync + close + atomic LittleFS rename: reboot sees old or complete new data.
 * One pending file/app bounds space used by interrupted writes. */
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size);
esp_err_t orb_fs_remove(const char *id, const char *key);
esp_err_t orb_fs_usage(size_t *total, size_t *used);
