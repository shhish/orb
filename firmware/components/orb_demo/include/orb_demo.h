#pragma once

/*
 * The Sessions demo: a list of agent sessions scrolling under Liquid Glass
 * controls, laid out round-first per docs/hig.md, with parallax. It also
 * exercises the renderer: it alternates a full-screen scroll with a phase
 * where only the glass animates (the light sweeping round, a press), so frame
 * rates for both can be read off the log.
 *
 * Depends on LVGL and orb_ui only, so the desktop simulator builds it too.
 * Every call needs lv_lock() held (or the LVGL thread).
 */

#include "lvgl.h"

/* Builds the demo on `screen`. Call once, after orb_ui_init(). */
void orb_demo_build(lv_obj_t *screen);

/*
 * The accelerometer, in g on screen axes (+x right, +y up, +z out of the
 * screen), dt_s after the last call; 30-60 Hz.
 */
void orb_demo_tick(float gx, float gy, float gz, float dt_s);

/* What the demo is doing now, for logs: "scroll" or "glass". */
const char *orb_demo_phase(void);
