#pragma once
#include "orb_power.h"

enum {
    ORB_POWER_WAKE_READY_TOUCH = 1u << 0,
    ORB_POWER_WAKE_READY_PWR = 1u << 1,
    ORB_POWER_WAKE_READY_IMU = 1u << 2,
    ORB_POWER_WAKE_READY_TIMER = 1u << 3,
    ORB_POWER_WAKE_READY_ATTENTION = 1u << 4,
};

typedef enum {
    ORB_POWER_APPLIED,
    ORB_POWER_WAIT_SAFE_FRAME,
    ORB_POWER_PANEL_UNSUPPORTED,
    ORB_POWER_PORT_FAILED,
    ORB_POWER_SLEEP_DENIED,
    ORB_POWER_WAIT_PANEL_READY,
} orb_power_apply_result_t;

typedef struct {
    void *context;
    /* Called only at the display owner's drained DMA fence. A missing hook
     * means brightness-zero fallback, NOT physical panel off. Waking must
     * preserve brightness zero AND any acknowledged safe pixels. */
    bool (*panel_awake)(void *context, bool awake);
    /* Optional nonblocking transition completion. With this hook, awake is
     * requested dark before a safe frame exists, so sleeping RAM can become
     * writable first. The display must gate pixel writes until ready(true).
     * A frame acknowledged before that readiness is never reused for wake. */
    bool (*panel_ready)(void *context, bool awake);
    /* Optional serialized command transport; the board void API is the
     * fallback. Required when deferred panel commands have another owner. */
    bool (*brightness)(void *context, int pct);
    /* Platform owns GPIO/IMU/network wake setup and may still reject sleep. */
    bool (*light_sleep)(void *context, uint32_t maximum_ms);
    uint32_t wake_ready;
} orb_power_port_t;

typedef struct {
    int brightness_pct;
    bool panel_known, panel_on;
    /* Accepted direction is distinct from completed physical state: an input
     * may reverse a pending sleep before its command deadline. */
    bool panel_pending, panel_requested;
    uint32_t acknowledged_revision;
    /* The last acknowledged frame can remain lit during a content handoff;
     * brightness-only changes of that content can apply immediately. */
    orb_power_state_t shown_state;
    bool shown_redact, shown_safe;
} orb_power_board_adapter_t;

void orb_power_board_adapter_init(orb_power_board_adapter_t *adapter);
/* Actual main-service cadence, shared with the devtools power report. */
void orb_power_board_set_poll_period(uint32_t milliseconds);
uint32_t orb_power_board_poll_period(void);
/* DISPLAY OWNER ONLY, never shell/main concurrently with orb_display.
 * safe_frame_revision is the exact generation just rendered and DMA-drained.
 * Pass 0 when no such frame exists. An unmatched token cannot brighten
 * changed or unknown content; an already safe frame may remain lit. */
orb_power_apply_result_t orb_power_board_apply(const orb_power_controller_t *power,
    orb_power_board_adapter_t *adapter, const orb_power_port_t *port, uint32_t safe_frame_revision);
/* Snapshot counterpart for an LVGL-owned controller. The snapshot is copied
 * under the UI lock, then applied by that same display owner at its fence. */
orb_power_apply_result_t orb_power_board_apply_snapshot(const orb_power_snapshot_t *snapshot,
    orb_power_board_adapter_t *adapter, const orb_power_port_t *port, uint32_t safe_frame_revision);
/* Optional policy hook, never an automatic ESP-IDF sleep call. Requires known
 * physical panel off, current safe frame, all wake sources and no work. */
orb_power_apply_result_t orb_power_board_try_light_sleep(const orb_power_controller_t *power,
    const orb_power_board_adapter_t *adapter, const orb_power_port_t *port, uint32_t maximum_ms);
