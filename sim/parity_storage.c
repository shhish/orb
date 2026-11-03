/* Bounded RAM-only service adapter. The test executable cannot open a path. */
#include "orb_fs.h"

#include <string.h>

#define RECORDS 8
#define RECORD_BYTES 4096
static struct {
    char app[ORB_FS_APP_ID_MAX + 1], key[ORB_FS_KEY_MAX + 1];
    unsigned char bytes[RECORD_BYTES];
    size_t size;
    bool occupied;
} s_records[RECORDS];
static bool s_failed;

void parity_storage_reset(void) { memset(s_records, 0, sizeof(s_records)); s_failed = false; }
void parity_storage_fail(bool failed) { s_failed = failed; }

bool orb_fs_valid_app_id(const char *id)
{
    if (!id || !*id || strlen(id) > ORB_FS_APP_ID_MAX) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}
bool orb_fs_ready(void) { return !s_failed; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    return !orb_fs_valid_app_id(id) ? ESP_ERR_INVALID_ARG : s_failed ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t orb_fs_read(const char *app, const char *key, void *data, size_t capacity, size_t *size)
{
    if (size) *size = 0;
    if (s_failed) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < RECORDS; ++i) {
        if (!s_records[i].occupied || strcmp(s_records[i].app, app) || strcmp(s_records[i].key, key)) continue;
        if (capacity < s_records[i].size) return ESP_ERR_INVALID_SIZE;
        memcpy(data, s_records[i].bytes, s_records[i].size);
        if (size) *size = s_records[i].size;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t orb_fs_write(const char *app, const char *key, const void *data, size_t size)
{
    if (s_failed) return ESP_ERR_INVALID_STATE;
    if (!orb_fs_valid_app_id(app) || !key || strlen(key) > ORB_FS_KEY_MAX || size > RECORD_BYTES) return ESP_ERR_INVALID_ARG;
    unsigned slot = RECORDS;
    for (unsigned i = 0; i < RECORDS; ++i) {
        if (s_records[i].occupied && !strcmp(s_records[i].app, app) && !strcmp(s_records[i].key, key)) { slot = i; break; }
        if (!s_records[i].occupied && slot == RECORDS) slot = i;
    }
    if (slot == RECORDS) return ESP_ERR_NO_MEM;
    strcpy(s_records[slot].app, app); strcpy(s_records[slot].key, key);
    memcpy(s_records[slot].bytes, data, size); s_records[slot].size = size; s_records[slot].occupied = true;
    return ESP_OK;
}
esp_err_t orb_fs_remove(const char *app, const char *key)
{
    if (s_failed) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < RECORDS; ++i)
        if (s_records[i].occupied && !strcmp(s_records[i].app, app) && !strcmp(s_records[i].key, key)) {
            memset(&s_records[i], 0, sizeof(s_records[i])); return ESP_OK;
        }
    return ESP_ERR_NOT_FOUND;
}
