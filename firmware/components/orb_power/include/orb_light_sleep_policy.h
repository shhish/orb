#pragma once

#include "orb_power.h"

typedef enum {
    ORB_LIGHT_SLEEP_ALLOWED,
    ORB_LIGHT_SLEEP_DISABLED,
    ORB_LIGHT_SLEEP_WAKE_UNREADY,
    ORB_LIGHT_SLEEP_WAKE_FAILED,
    ORB_LIGHT_SLEEP_POWER_UNKNOWN,
    ORB_LIGHT_SLEEP_NOT_SLEEP,
    ORB_LIGHT_SLEEP_PANEL_ON,
    ORB_LIGHT_SLEEP_INHIBITED,
    ORB_LIGHT_SLEEP_USB,
    ORB_LIGHT_SLEEP_VOICE,
    ORB_LIGHT_SLEEP_OTA,
    ORB_LIGHT_SLEEP_ALARM,
    ORB_LIGHT_SLEEP_BENCHMARK,
    ORB_LIGHT_SLEEP_GAME,
    ORB_LIGHT_SLEEP_CONTACT,
} orb_light_sleep_reason_t;

typedef struct {
    orb_power_snapshot_t power;
    uint32_t inhibitors;
    bool power_valid, panel_off, voice, ota, alarm, benchmark, game;
} orb_light_sleep_inputs_t;

static inline orb_light_sleep_reason_t orb_light_sleep_policy(orb_light_sleep_inputs_t in,
    bool configured, bool wake_ready, bool wake_failed, bool usb_override, bool contact)
{
    if (!configured) return ORB_LIGHT_SLEEP_DISABLED;
    if (wake_failed) return ORB_LIGHT_SLEEP_WAKE_FAILED;
    if (!wake_ready) return ORB_LIGHT_SLEEP_WAKE_UNREADY;
    if (!in.power_valid) return ORB_LIGHT_SLEEP_POWER_UNKNOWN;
    if (in.power.state != ORB_POWER_SLEEP) return ORB_LIGHT_SLEEP_NOT_SLEEP;
    if (in.power.panel_on || !in.panel_off) return ORB_LIGHT_SLEEP_PANEL_ON;
    if (in.inhibitors) return ORB_LIGHT_SLEEP_INHIBITED;
    if (in.power.usb && !usb_override) return ORB_LIGHT_SLEEP_USB;
    if (in.voice) return ORB_LIGHT_SLEEP_VOICE;
    if (in.ota) return ORB_LIGHT_SLEEP_OTA;
    if (in.alarm) return ORB_LIGHT_SLEEP_ALARM;
    if (in.benchmark) return ORB_LIGHT_SLEEP_BENCHMARK;
    if (in.game) return ORB_LIGHT_SLEEP_GAME;
    if (contact) return ORB_LIGHT_SLEEP_CONTACT;
    return ORB_LIGHT_SLEEP_ALLOWED;
}

/* One NO_LIGHT_SLEEP lock, never a recursive acquisition on each service poll. */
typedef struct { bool held; } orb_light_sleep_lock_t;
typedef enum { ORB_LIGHT_SLEEP_LOCK_KEEP, ORB_LIGHT_SLEEP_LOCK_ACQUIRE,
               ORB_LIGHT_SLEEP_LOCK_RELEASE } orb_light_sleep_lock_action_t;

static inline orb_light_sleep_lock_action_t orb_light_sleep_lock_step(
    orb_light_sleep_lock_t *lock, orb_light_sleep_reason_t reason)
{
    bool held = reason != ORB_LIGHT_SLEEP_ALLOWED;
    if (held == lock->held) return ORB_LIGHT_SLEEP_LOCK_KEEP;
    lock->held = held;
    return held ? ORB_LIGHT_SLEEP_LOCK_ACQUIRE : ORB_LIGHT_SLEEP_LOCK_RELEASE;
}
