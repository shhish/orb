#include "orb_power_board.h"
#include "orb_board.h"
#include <stddef.h>
#include <stdatomic.h>

static _Atomic uint32_t s_poll_period_ms = 20;

void orb_power_board_set_poll_period(uint32_t milliseconds)
{
    atomic_store_explicit(&s_poll_period_ms, milliseconds, memory_order_relaxed);
}

uint32_t orb_power_board_poll_period(void)
{
    return atomic_load_explicit(&s_poll_period_ms, memory_order_relaxed);
}

void orb_power_board_adapter_init(orb_power_board_adapter_t *adapter)
{
    if (adapter) *adapter = (orb_power_board_adapter_t){ .brightness_pct = -1 };
}

static bool brightness_request(orb_power_board_adapter_t *adapter,
    const orb_power_port_t *port, int pct)
{
    if (adapter->brightness_pct == pct) return true;
    if (port && port->brightness) {
        if (!port->brightness(port->context, pct)) {
            adapter->brightness_pct = -1;
            adapter->shown_safe = false;
            return false;
        }
    } else orb_board_panel_brightness(pct);
    adapter->brightness_pct = pct;
    return true;
}

static orb_power_apply_result_t panel_request(orb_power_board_adapter_t *adapter,
    const orb_power_port_t *port, bool awake)
{
    bool change = adapter->panel_pending ? adapter->panel_requested != awake :
        !adapter->panel_known || adapter->panel_on != awake;
    if (change) {
        if (!port->panel_awake(port->context, awake)) {
            /* A failed multi-command transition cannot prove either state. */
            adapter->panel_known = false;
            adapter->panel_pending = false;
            return ORB_POWER_PORT_FAILED;
        }
        adapter->panel_requested = awake;
        adapter->panel_pending = port->panel_ready != NULL;
    }
    if (adapter->panel_pending && port->panel_ready && !port->panel_ready(port->context, awake))
        return ORB_POWER_WAIT_PANEL_READY;
    adapter->panel_pending = false;
    adapter->panel_known = true;
    adapter->panel_on = awake;
    return ORB_POWER_APPLIED;
}

orb_power_apply_result_t orb_power_board_apply(const orb_power_controller_t *power,
    orb_power_board_adapter_t *adapter, const orb_power_port_t *port, uint32_t safe_frame_revision)
{
    if (!power) return ORB_POWER_PORT_FAILED;
    orb_power_snapshot_t snapshot = orb_power_get_snapshot(power);
    return orb_power_board_apply_snapshot(&snapshot, adapter, port, safe_frame_revision);
}

orb_power_apply_result_t orb_power_board_apply_snapshot(const orb_power_snapshot_t *value,
    orb_power_board_adapter_t *adapter, const orb_power_port_t *port, uint32_t safe_frame_revision)
{
    if (!value || !adapter) return ORB_POWER_PORT_FAILED;
    orb_power_snapshot_t snapshot = *value;
    bool asynchronous = port && port->panel_awake && port->panel_ready;
    bool writable = !asynchronous ||
        (adapter->panel_known && adapter->panel_on && !adapter->panel_pending);
    if (snapshot.panel_on && asynchronous && !writable) {
        if (!brightness_request(adapter, port, 0)) return ORB_POWER_PORT_FAILED;
        adapter->shown_safe = false;
        adapter->acknowledged_revision = 0;
        orb_power_apply_result_t result = panel_request(adapter, port, true);
        if (result != ORB_POWER_APPLIED) return result;
    }
    bool safe = writable && safe_frame_revision && safe_frame_revision == snapshot.revision;
    if (safe) adapter->acknowledged_revision = snapshot.revision;
    bool acknowledged = adapter->acknowledged_revision == snapshot.revision;
    /* panel_on is only tracked with a port; a lit brightness proves it here. */
    if (snapshot.panel_on && !acknowledged) {
        bool lit_safe = adapter->shown_safe && adapter->brightness_pct > 0;
        bool same_content = snapshot.state == adapter->shown_state && snapshot.redact == adapter->shown_redact;
        if (lit_safe) {
            /* Keep the already visible frame lit until its replacement drains.
             * Only brightness-only changes can brighten before that receipt.
             * A reveal at a lower target must dim first, so incoming private
             * pixels never inherit a brighter redacted surface's level. */
            if ((same_content || (!snapshot.redact && snapshot.brightness_pct < adapter->brightness_pct)) &&
                !brightness_request(adapter, port, snapshot.brightness_pct)) return ORB_POWER_PORT_FAILED;
        } else {
            adapter->shown_safe = false;
            /* A dark or unknown framebuffer still needs its first safe frame. */
            if (!brightness_request(adapter, port, 0)) return ORB_POWER_PORT_FAILED;
        }
        return ORB_POWER_WAIT_SAFE_FRAME;
    }
    if (!snapshot.panel_on) {
        if (!brightness_request(adapter, port, 0)) return ORB_POWER_PORT_FAILED;
        if (!port || !port->panel_awake) return ORB_POWER_PANEL_UNSUPPORTED;
        adapter->shown_safe = false;
        orb_power_apply_result_t result = panel_request(adapter, port, false);
        if (result != ORB_POWER_APPLIED) return result;
    } else {
        if (port && port->panel_awake && (!adapter->panel_known || !adapter->panel_on)) {
            if (!brightness_request(adapter, port, 0)) return ORB_POWER_PORT_FAILED;
            orb_power_apply_result_t result = panel_request(adapter, port, true);
            if (result != ORB_POWER_APPLIED) return result;
        }
        if (!brightness_request(adapter, port, snapshot.brightness_pct)) return ORB_POWER_PORT_FAILED;
        adapter->shown_state = snapshot.state;
        adapter->shown_redact = snapshot.redact;
        adapter->shown_safe = true;
    }
    if (!snapshot.panel_on) adapter->shown_safe = false;
    return ORB_POWER_APPLIED;
}

orb_power_apply_result_t orb_power_board_try_light_sleep(const orb_power_controller_t *power,
    const orb_power_board_adapter_t *adapter, const orb_power_port_t *port, uint32_t maximum_ms)
{
    if (!power || !adapter || !port || !port->panel_awake || !port->light_sleep || !maximum_ms)
        return ORB_POWER_SLEEP_DENIED;
    orb_power_snapshot_t snapshot = orb_power_get_snapshot(power);
    uint32_t required = ORB_POWER_WAKE_READY_TOUCH | ORB_POWER_WAKE_READY_PWR |
        ORB_POWER_WAKE_READY_TIMER | ORB_POWER_WAKE_READY_ATTENTION;
    if (power->preferences.motion_wake) required |= ORB_POWER_WAKE_READY_IMU;
    if (snapshot.state != ORB_POWER_SLEEP || snapshot.panel_on || power->inhibitors ||
        !adapter->panel_known || adapter->panel_on || adapter->panel_pending || adapter->brightness_pct != 0 ||
        adapter->acknowledged_revision != snapshot.revision || (port->wake_ready & required) != required)
        return ORB_POWER_SLEEP_DENIED;
    return port->light_sleep(port->context, maximum_ms) ? ORB_POWER_APPLIED : ORB_POWER_PORT_FAILED;
}
