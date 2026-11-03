#include "orb_system_state.h"

#include <stdatomic.h>
#include <string.h>

#if defined(ESP_PLATFORM) || defined(ORB_SYSTEM_TEST_STORAGE)
#include "orb_fs.h"
#endif

/* Eight bytes, versioned and compared in full: a truncated/corrupt record must
 * not suppress the instructions. No heap or UI object belongs to this service. */
static const unsigned char s_done_record[8] = {'O', 'R', 'B', 'O', 1, 1, 0xA5, 0x5A};
static atomic_bool s_ready, s_needed, s_pending, s_error, s_saving;

void orb_system_state_init(void)
{
    atomic_store(&s_ready, false);
    atomic_store(&s_needed, false);
    atomic_store(&s_pending, false);
    atomic_store(&s_error, false);
    atomic_store(&s_saving, false);
#if defined(ESP_PLATFORM) || defined(ORB_SYSTEM_TEST_STORAGE)
    atomic_store(&s_needed, true);
    if (!orb_fs_ready()) return;
    if (orb_fs_app_prepare("system") != ESP_OK) {
        atomic_store(&s_error, true);
        return;
    }
    unsigned char record[sizeof(s_done_record)] = {0};
    size_t size = 0;
    esp_err_t error = orb_fs_read("system", "onboarding-v1", record, sizeof(record), &size);
    bool done = error == ESP_OK && size == sizeof(record) && !memcmp(record, s_done_record, size);
    atomic_store(&s_needed, !done);
    atomic_store(&s_ready, true);
#else
    /* Ordinary image fixtures have no filesystem and must remain deterministic
     * across tests. Explicit Onboarding scenes still use the production view. */
    (void)s_done_record;
#endif
}

bool orb_system_onboarding_needed(void)
{
    return atomic_load(&s_needed);
}

bool orb_system_onboarding_save_error(void)
{
    return atomic_load(&s_error);
}

bool orb_system_onboarding_saving(void)
{
    return atomic_load(&s_saving);
}

bool orb_system_onboarding_dismiss(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_saving, &expected, true)) return false;
    atomic_store(&s_error, !atomic_load(&s_ready));
    if (atomic_load(&s_error)) {
        atomic_store(&s_saving, false);
        return false;
    }
    atomic_store(&s_pending, true);
    return true;
}

void orb_system_state_service(void)
{
    if (!atomic_exchange(&s_pending, false)) return;
#if defined(ESP_PLATFORM) || defined(ORB_SYSTEM_TEST_STORAGE)
    esp_err_t error = orb_fs_write("system", "onboarding-v1", s_done_record, sizeof(s_done_record));
    atomic_store(&s_error, error != ESP_OK);
    /* Publish durability before completion; a UI poll can never observe a
     * successful result while the tutorial is still needed. */
    if (error == ESP_OK) atomic_store(&s_needed, false);
#endif
    atomic_store(&s_saving, false);
}
