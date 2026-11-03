#pragma once

/* Home's single state vocabulary, string table and formatters (HIG §§1/4/8).
 * Every Home surface, Control Center's Home page included, words the same
 * state the same way: sentence case, US English, one term per state. */
#include "orb_home.h"
#include "orb_units.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ORB_HOME_STATUS_READY,
    ORB_HOME_STATUS_OFFLINE,     /* the Mac or link is offline */
    ORB_HOME_STATUS_LOADING,     /* connected, first snapshot not yet complete */
    ORB_HOME_STATUS_UNAVAILABLE, /* the Mac reports Home itself unavailable */
    ORB_HOME_STATUS_NO_RESPONSE, /* one accessory is unreachable */
    ORB_HOME_STATUS_UPDATING,    /* one write is pending */
    ORB_HOME_STATUS_FAILED,      /* the last write to this target failed */
    ORB_HOME_STATUS_BUSY,        /* Home refused the write while busy */
} orb_home_status_t;

#define ORB_HOME_TEXT_LAST_KNOWN "Last known"

/* Home-wide state only: offline, loading or unavailable, else ready. */
orb_home_status_t orb_home_status_home(const orb_home_snapshot_t *snapshot);
/* Home-wide state first, then reachability, a visible failure, pending. */
orb_home_status_t orb_home_status_accessory(const orb_home_snapshot_t *snapshot,
    const orb_home_accessory_t *accessory, bool failure_visible);
/* "" for READY; otherwise the one table entry for that state. */
const char *orb_home_status_text(orb_home_status_t status);
/* Values shown under these states are cached, never current. */
bool orb_home_status_stale(orb_home_status_t status);

/* Errors name their target: "Couldn’t run Evening". A busy Home keeps its
 * own reason. verb is lower case ("run", "update", "turn off"). */
void orb_home_error_text(char *out, size_t capacity, const char *verb,
    const char *target, const char *service_error);

/* The one temperature formatter: "22°", in the owner's unit, no unit suffix.
 * half_steps shows a Celsius target's .5 ("22.5°"). Live lanes reserve a
 * realistic three-figure template, never a hidden impossible reading. */
void orb_home_format_temperature(char *out, size_t capacity, float celsius,
    orb_temperature_t unit, bool half_steps);
/* Widest tabular reading (−100 °C is −148 °F). The kit keeps a lane's
 * reservation as hidden text, so the template sits just outside the real
 * domain and can never be mistaken for, or found as, a reported value. */
#define ORB_HOME_TEMPERATURE_TEMPLATE "\u2212188°"

/* "Updated 2 hr ago", or "" when the update time is unknown or in the future. */
void orb_home_format_age(char *out, size_t capacity, uint64_t updated);
/* Battery words: low only at or below 20% (or a bare low flag without a level). */
bool orb_home_battery_low(const orb_home_state_t *state);
void orb_home_format_battery(char *out, size_t capacity, const orb_home_state_t *state);

/* A sensor row's or detail's type name: "Temperature", "Humidity", ... */
const char *orb_home_kind_name(orb_home_kind_t kind);

/* Fit an accessory name to a fixed label lane: whole words over its lines,
 * else a middle elision that preserves a distinguishing suffix. */
void orb_home_fit_name(lv_obj_t *label, const char *name);
