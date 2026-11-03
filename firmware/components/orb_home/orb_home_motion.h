#pragma once
#include "lvgl.h"

/* Shared Home-only outcome/wait composition; all animation lives in the kit. */
typedef struct { uint32_t started; unsigned kind; } orb_home_wait_t;
typedef struct { char id[33]; uint32_t started; } orb_home_pending_wait_t;
bool orb_home_waiting(const char *text, orb_home_wait_t *wait);
void orb_home_ui_wait(lv_obj_t *label, const char *text, orb_home_wait_t *wait);
void orb_home_ui_state_wait(lv_obj_t *state, const char *text, orb_home_wait_t *wait);
void orb_home_ui_check(lv_obj_t *parent, lv_obj_t **check, bool show, bool animate, int x, int y);
