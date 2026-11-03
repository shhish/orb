#pragma once

#include "lvgl.h"

typedef enum {
    ORB_DEMO_FIXTURE_QUIET,
    ORB_DEMO_FIXTURE_TRANSITION,
} orb_demo_fixture_t;

/* Optional benchmark-only synthetic surfaces. No timers/animations in quiet;
 * deleting the owner screen cancels the transition through LVGL ownership. */
void orb_demo_fixture_build(lv_obj_t *screen, orb_demo_fixture_t fixture);
