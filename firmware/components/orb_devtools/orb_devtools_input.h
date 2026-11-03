#pragma once

#include "orb_shell.h"

/* Called with LVGL locked. Developer navigation uses the same power wake as
 * touch, and renews LVGL's separate inactivity clock used by OTA policy. */
static inline bool orb_devtools_input_wake(orb_shell_t *shell)
{
    if (!shell) return false;
    orb_power_input_result_t result = orb_shell_power_wake(shell, ORB_POWER_WAKE_TOUCH);
    if (result.accepted) lv_display_trigger_activity(lv_obj_get_display(orb_shell_root(shell)));
    return result.accepted;
}

static inline bool orb_devtools_input_goto(orb_shell_t *shell, orb_shell_surface_t surface)
{
    if (!orb_devtools_input_wake(shell)) return false;
    orb_shell_navigate(shell, surface);
    return true;
}
