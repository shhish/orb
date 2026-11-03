#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    ORB_SCROLL_AGENTS, ORB_SCROLL_ROOMS, ORB_SCROLL_SETTINGS,
    ORB_SCROLL_NOTIFICATIONS, ORB_SCROLL_FAVOURITES, ORB_SCROLL_SURFACE_COUNT
} orb_scroll_surface_t;
typedef struct orb_scroll_fixture orb_scroll_fixture_t;
typedef enum { ORB_SCROLL_INPUT, ORB_SCROLL_RELEASE, ORB_SCROLL_OFFSET } orb_scroll_event_t;
typedef void (*orb_scroll_observer_t)(orb_scroll_event_t event, int32_t x, int32_t y, void *ctx);

/* UI thread only. Private snapshots feed the production workspace/Home views;
 * no reset, demo load, answer, preference or Home request touches a live store. */
bool orb_scroll_surface_parse(const char *name, orb_scroll_surface_t *surface);
const char *orb_scroll_surface_name(orb_scroll_surface_t surface);
orb_scroll_fixture_t *orb_scroll_fixture_create(lv_obj_t *parent, orb_scroll_surface_t surface);
void orb_scroll_fixture_destroy(orb_scroll_fixture_t *fixture);
/* Prepare after its initial screen has settled. Large advances skip missed
 * samples rather than slowing the scripted gesture on an overloaded board. */
bool orb_scroll_fixture_prepare(orb_scroll_fixture_t *fixture);
/* Nominal active polling period of the production touch path, not a measured
 * CST9217 hardware report frequency. Missed reports are skipped, never replayed. */
#define ORB_SCROLL_REPORT_MS 16u
void orb_scroll_fixture_observe(orb_scroll_fixture_t *fixture, orb_scroll_observer_t observer, void *ctx);
typedef struct {
    uint32_t reports, releases, skipped_reports, report_gap_max_ms;
    uint32_t scroll_events, offset_changes, offset_gap_max_ms;
} orb_scroll_fixture_diagnostics_t;
void orb_scroll_fixture_diagnostics(const orb_scroll_fixture_t *fixture, orb_scroll_fixture_diagnostics_t *out);
void orb_scroll_fixture_tick(orb_scroll_fixture_t *fixture, uint32_t elapsed_ms);
bool orb_scroll_fixture_moving(const orb_scroll_fixture_t *fixture);
unsigned orb_scroll_fixture_gestures(const orb_scroll_fixture_t *fixture);
int32_t orb_scroll_fixture_position(const orb_scroll_fixture_t *fixture);
/* Rectangular content viewport clipped to the display, before round clipping. */
uint32_t orb_scroll_fixture_viewport_pixels(const orb_scroll_fixture_t *fixture);
