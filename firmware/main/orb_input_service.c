#include "orb_input_service.h"

#include <string.h>
#include <math.h>
#include "esp_timer.h"
#include "orb_apps.h"
#include "orb_display.h"
#include "orb_shell_services.h"
#if CONFIG_ORB_DEVTOOLS
#include "orb_devtools.h"
#endif

static orb_senses_t s_senses;
static uint32_t s_generation;

static void deliver_power_imu(orb_shell_t *shell, const orb_input_motion_t *input, bool minimal)
{
    static int64_t previous_us;
    if (!shell || (input->motion && input->at_us / 50000 == previous_us / 50000)) return;
    /* The display can block while dark, but pick-up/face-down still need each
     * scheduled sample. Serialize with shell/UI mutations instead of dropping
     * every other sample in the display mailbox. Only a state change wakes UI. */
    lv_lock();
    uint32_t revision = orb_shell_power_snapshot(shell).revision;
    void (*sample)(orb_shell_t *, float, float, float, uint64_t) =
        minimal ? orb_shell_power_imu_sleep : orb_shell_power_imu;
    sample(shell, input->motion ? input->gravity.x : NAN,
        input->motion ? input->gravity.y : NAN, input->motion ? input->gravity.z : NAN, 0);
    if (orb_shell_power_snapshot(shell).revision != revision) orb_display_wake();
    lv_unlock();
    previous_us = input->at_us;
}

orb_input_motion_t orb_input_motion(orb_idle_policy_t policy, bool screen_on, orb_shell_t *shell)
{
    (void)orb_board_motion_configure(policy.gyro, policy.taps);
    uint32_t generation = orb_board_motion_generation();
    if (generation != s_generation) {
        /* Never carry the zero gyro of accel-only mode into the bias learner,
         * or integrate a stale pre-suspend sample into a game or detent. */
        memset(&s_senses, 0, sizeof(s_senses));
        s_generation = generation;
    }
    orb_motion_sample_t sample = {0};
    orb_input_motion_t result = {.motion = orb_board_motion(&sample) == ESP_OK};
    result.at_us = esp_timer_get_time();
    if (result.motion) {
        result.gravity = (orb_accel_t){sample.ax, sample.ay, sample.az};
    }
    result.next_poll_ms = orb_idle_motion_wait(policy, result.motion,
        result.gravity.x, result.gravity.y, result.gravity.z);
    deliver_power_imu(shell, &result, policy.minimal);
    /* Eligible Sleep samples only physical wake evidence. Avoid touch reads,
     * gesture/bias learning, devtools snapshots and the app publish path. */
    if (policy.minimal) return result;
    if (result.motion) {
        result.senses = orb_senses_update(&s_senses, &sample, orb_display_touch_pressed(),
                                          (uint64_t)(result.at_us / 1000));
        if (!policy.gyro) result.senses.detents = 0;
        if (!policy.taps) result.senses.double_tap = false;
#if CONFIG_ORB_DEVTOOLS
        orb_devtools_publish_senses(&sample, &result.senses, (uint64_t)(result.at_us / 1000));
#endif
    }
    orb_shell_apps_service();
    orb_app_input_t input = {
        .now_us = result.at_us, .motion_sample_us = result.at_us,
        .accel_x = result.gravity.x, .accel_y = result.gravity.y, .accel_z = result.gravity.z,
        .motion_valid = result.motion && policy.gyro,
        .gyro_x = sample.gx - s_senses.bias_x,
        .gyro_y = sample.gy - s_senses.bias_y,
        .gyro_z = sample.gz - s_senses.bias_z,
        .gyro_valid = result.motion && policy.gyro && s_senses.bias_valid,
    };
#if CONFIG_ORB_DEVTOOLS
    /* Synthetic motion remains exclusively game input, never physical wake,
     * taps, twist, face-down, or an approval/send/unlock action. */
    if (orb_devtools_motion_override(result.at_us, &input.accel_x, &input.accel_y, &input.accel_z)) {
        input.motion_valid = policy.gyro;
        input.gyro_valid = false;
    }
#endif
    orb_apps_poll_input(&input, false, screen_on && policy.gyro);
    orb_shell_apps_publish();
    return result;
}
