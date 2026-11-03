#pragma once
#include "lvgl.h"
void orb_display_touch_start(lv_display_t *display, unsigned priority);
void orb_display_touch_sleep_changed(void);
/* UI owner only: read a pending real contact through the ordinary indev path. */
void orb_display_touch_service(void);
