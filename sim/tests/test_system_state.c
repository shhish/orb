/* The real onboarding service over an isolated eight-byte filesystem fake. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "orb_fs.h"
#include "orb_system_state.h"

static bool s_ready = true, s_fail_write;
static unsigned char s_record[16];
static size_t s_size;
static unsigned s_reads, s_writes;

bool orb_fs_ready(void) { return s_ready; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    assert(!strcmp(id, "system"));
    return ESP_OK;
}
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    assert(!strcmp(id, "system") && !strcmp(key, "onboarding-v1"));
    ++s_reads;
    if (!s_size) return ESP_ERR_NOT_FOUND;
    *size = s_size;
    if (s_size > capacity) return ESP_ERR_INVALID_SIZE;
    memcpy(data, s_record, s_size);
    return ESP_OK;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    assert(!strcmp(id, "system") && !strcmp(key, "onboarding-v1"));
    assert(size == 8);
    assert(orb_system_onboarding_saving() && orb_system_onboarding_needed());
    ++s_writes;
    if (s_fail_write) return ESP_FAIL;
    memcpy(s_record, data, size);
    s_size = size;
    return ESP_OK;
}

int main(void)
{
    orb_system_state_init();
    assert(orb_system_onboarding_needed() && s_reads == 1 && s_writes == 0);
    assert(orb_system_onboarding_dismiss());
    assert(orb_system_onboarding_needed() && s_writes == 0); /* Pending is not durable. */
    assert(orb_system_onboarding_saving() && !orb_system_onboarding_dismiss());
    orb_system_state_service();
    assert(s_writes == 1 && s_size == 8 && !orb_system_onboarding_save_error());
    assert(!orb_system_onboarding_needed() && !orb_system_onboarding_saving());
    orb_system_state_service();
    assert(s_writes == 1); /* One write per acknowledgment. */
    orb_system_state_init();
    assert(!orb_system_onboarding_needed());

    s_record[4] = 0;
    orb_system_state_init();
    assert(orb_system_onboarding_needed()); /* Old/corrupt version. */
    s_size = 4;
    orb_system_state_init();
    assert(orb_system_onboarding_needed()); /* Truncated record. */
    s_size = 12;
    orb_system_state_init();
    assert(orb_system_onboarding_needed()); /* Oversized record. */
    s_size = 0;
    s_fail_write = true;
    orb_system_state_init();
    assert(orb_system_onboarding_dismiss());
    orb_system_state_service();
    assert(orb_system_onboarding_save_error() && orb_system_onboarding_needed());
    assert(!orb_system_onboarding_saving());
    s_fail_write = false;
    assert(orb_system_onboarding_dismiss());
    assert(orb_system_onboarding_saving() && !orb_system_onboarding_save_error());
    assert(orb_system_onboarding_needed());
    orb_system_state_service();
    assert(!orb_system_onboarding_needed() && !orb_system_onboarding_saving());
    s_size = 0;
    orb_system_state_init();
    assert(orb_system_onboarding_needed()); /* Failure never claims durability. */

    s_ready = false;
    unsigned reads = s_reads;
    orb_system_state_init();
    assert(orb_system_onboarding_needed() && reads == s_reads);
    assert(!orb_system_onboarding_dismiss());
    assert(orb_system_onboarding_needed() && orb_system_onboarding_save_error());
    assert(!orb_system_onboarding_saving());
    orb_system_state_service();
    puts("onboarding storage: read-once, queued write, reboot, corruption and failure pass");
    return 0;
}
