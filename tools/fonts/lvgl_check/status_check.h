#ifndef ORB_STATUS_CHECK_H
#define ORB_STATUS_CHECK_H

#include "lvgl.h"

/* Both previews are produced by the same real LVGL drawing code as the firmware. */
int orb_status_host_check(lv_display_t *display);
int orb_status_host_previews(lv_display_t *display, const char *sheet_path, const char *face_path);

/* Host-only fake for exercising a cold pack-font load failure and recovery. */
void orb_host_fail_font_allocations(bool fail);
unsigned orb_host_font_allocation_failures(void);

#endif
