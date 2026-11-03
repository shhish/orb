#include "orb_panel_policy.h"
#include "orb_power_board.h"
#include "orb_display.h"
#include "orb_idle_policy.h"

static orb_power_board_adapter_t s_panel_policy;
static orb_shell_t *s_shell;
/* These generations belong to the display/UI task, never the service task. */
static uint32_t s_power_rendering_revision, s_power_safe_revision;
static uint64_t s_power_render_errors;

static bool panel_awake(void *ctx, bool awake)
{
    (void)ctx;
    return orb_display_panel_awake(awake);
}

static bool panel_ready(void *ctx, bool awake)
{
    (void)ctx;
    return orb_display_panel_ready(awake);
}

static bool panel_brightness(void *ctx, int pct)
{
    (void)ctx;
    return orb_display_set_brightness(pct);
}

static const orb_power_port_t s_panel_port = {
    .panel_awake = panel_awake, .panel_ready = panel_ready, .brightness = panel_brightness,
};

static void power_render_event(lv_event_t *event)
{
    if (!s_shell) return;
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        orb_display_counters_t counters;
        orb_display_counters(&counters);
        s_power_render_errors = counters.transfer_errors;
        s_power_rendering_revision = orb_shell_power_snapshot(s_shell).revision;
    } else {
        /* REFR_READY identifies the completed LVGL surface. The next UI turn
         * drains every queued DMA before acknowledging or changing brightness. */
        s_power_safe_revision = s_power_rendering_revision;
    }
}

void orb_panel_policy_init(void)
{
    orb_power_board_adapter_init(&s_panel_policy);
    lv_display_add_event_cb(lv_display_get_default(), power_render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), power_render_event, LV_EVENT_REFR_READY, NULL);
}

uint32_t orb_panel_policy_acknowledged(void) { return s_panel_policy.acknowledged_revision; }

int orb_panel_policy_apply(orb_shell_t *shell)
{
    s_shell = shell;
    orb_power_snapshot_t power = orb_shell_power_snapshot(shell);
    orb_display_set_panel_on(power.panel_on);
    if (!power.panel_on && s_panel_policy.brightness_pct == 0 && s_panel_policy.panel_known &&
        !s_panel_policy.panel_on && !s_panel_policy.panel_pending && orb_display_panel_ready(false)) return s_panel_policy.brightness_pct;
    if (s_panel_policy.acknowledged_revision == power.revision &&
        s_panel_policy.brightness_pct == power.brightness_pct &&
        orb_display_panel_ready(power.panel_on)) return s_panel_policy.brightness_pct;
    if (!orb_display_drain()) return s_panel_policy.brightness_pct;
    orb_display_counters_t counters;
    orb_display_counters(&counters);
    uint32_t safe = counters.transfer_errors == s_power_render_errors ? s_power_safe_revision : 0;
    /* A generation that needs no panel command can finish rendering at the
     * current level. Requiring a black command merely to renew its receipt
     * would pulse every transition endpoint and PMU status revision. */
    if (power.panel_on && orb_display_panel_ready(true) && safe != power.revision &&
        s_panel_policy.brightness_pct == power.brightness_pct) {
        if (orb_idle_frame_needed(power, safe) && !orb_display_refresh_pending())
            lv_obj_invalidate(lv_screen_active());
        return s_panel_policy.brightness_pct;
    }
    orb_power_apply_result_t applied = orb_power_board_apply_snapshot(&power, &s_panel_policy, &s_panel_port, safe);
    /* Complete a waiting handoff even if nothing else needs to draw: a
     * redacted face crossing Dimmed or Always On may have no invalidation. */
    if (applied == ORB_POWER_WAIT_SAFE_FRAME && power.panel_on && !orb_display_refresh_pending())
        lv_obj_invalidate(lv_screen_active());
    orb_shell_power_display_ready(shell, power.revision,
        applied == ORB_POWER_APPLIED && s_panel_policy.brightness_pct > 0);
    return s_panel_policy.brightness_pct;
}
