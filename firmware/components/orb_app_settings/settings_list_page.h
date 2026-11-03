#pragma once

#include "lvgl.h"

typedef struct {
    lv_obj_t *heading, *list;
} orb_settings_list_page_t;

/* Existing Settings list composition, without routing, service access or state
 * beyond its owned view pointers. The screen framework owns the root lifetime. */
void orb_settings_list_page_begin(lv_obj_t *root, const char *title, orb_settings_list_page_t *page);
void orb_settings_list_page_end(const orb_settings_list_page_t *page);
/* Choice lists open on the selected row at the equator. */
void orb_settings_list_page_focus(lv_obj_t *list, lv_obj_t *row);
void orb_settings_list_page_reveal(lv_obj_t *list, lv_obj_t *row);
/* Polish review #12: the shared unavailable caption where the list began.
 * The list then starts below it, so no row ever rests under the caption. */
lv_obj_t *orb_settings_list_page_status(lv_obj_t *root, lv_obj_t *list, const char *text);
int32_t orb_settings_list_page_maximum(const orb_settings_list_page_t *page);
void orb_settings_list_page_scroll(const orb_settings_list_page_t *page, int32_t value);
