#pragma once

#include <stdbool.h>

/* Main-task only, after orb_fs_mount(). Storage failure still permits the
 * tutorial for this boot; read errors never manufacture its completion. */
void orb_system_state_init(void);
void orb_system_state_service(void);

/* UI-safe reads and a bounded write request. Filesystem I/O never runs under
 * the LVGL lock. Needed stays true throughout saving and failure. Only a
 * durable write completes the tutorial; failure remains visible and retryable. */
bool orb_system_onboarding_needed(void);
bool orb_system_onboarding_dismiss(void);
bool orb_system_onboarding_save_error(void);
bool orb_system_onboarding_saving(void);
