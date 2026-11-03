#pragma once

#include "orb_light_sleep_policy.h"

typedef enum {
    ORB_LIGHT_SLEEP_FAULT_NONE, ORB_LIGHT_SLEEP_FAULT_LOCK,
    ORB_LIGHT_SLEEP_FAULT_CALLBACK, ORB_LIGHT_SLEEP_FAULT_CONFIG,
    ORB_LIGHT_SLEEP_FAULT_WAKE, ORB_LIGHT_SLEEP_FAULT_RESTORE,
} orb_light_sleep_fault_t;

typedef struct {
    bool configured, wake_ready, eligible, usb_override;
    orb_light_sleep_reason_t reason;
    uint64_t entries, slept_us;
    orb_light_sleep_fault_t fault;
    int32_t last_error;
    uint32_t failures, retries, recoveries, retry_delay_ms;
    int64_t retry_at_us;
} orb_light_sleep_status_t;

/* Boot creates and takes the policy lock before allowing IDF automatic sleep.
 * GPIO setup is deferred until both existing physical input paths are ready. */
void orb_light_sleep_init(void);
/* IDF 6.1 S3 calls wake_inputs in the idle task with caches available, IRQs
 * masked and scheduling suspended. Only bounded sampling/FromISR notification
 * is allowed: no blocking, allocation, task API yielding or portYIELD_FROM_ISR.
 * Sample before AND after edge restoration, including an already idle level.
 * Setup/recovery also samples from the service owner with the same contract.
 * Keep flash/PSRAM/CPU power-down off; this is not a cache-off sleep hook. */
void orb_light_sleep_arm(bool inputs_ready, void (*wake_inputs)(void));
/* Service owner only. The panel_off input must prove the completed physical
 * panel transition, rather than repeat the controller's requested state. */
bool orb_light_sleep_update(orb_light_sleep_inputs_t inputs);
void orb_light_sleep_status(orb_light_sleep_status_t *out);
const char *orb_light_sleep_reason_name(orb_light_sleep_reason_t reason);
const char *orb_light_sleep_fault_name(orb_light_sleep_fault_t fault);
#if CONFIG_ORB_DEVTOOLS
/* RAM only; revoking the exception immediately takes the policy lock. */
bool orb_light_sleep_set_usb_override(bool allowed);
#endif
