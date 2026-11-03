#pragma once

#include "orb_screen.h"

typedef struct { int32_t scroll; } orb_page_context_t;

/* The screen owns its content; the framework exposes the viewport/chrome to
 * the shell transition adapter without revealing application state layouts. */
void orb_screen_page_bind(orb_screen_instance_t *instance, lv_obj_t *viewport, lv_obj_t *title);
lv_obj_t *orb_screen_page_viewport(const orb_screen_instance_t *instance);
lv_obj_t *orb_screen_page_title(const orb_screen_instance_t *instance);
orb_page_context_t orb_page_save(const lv_obj_t *viewport);
void orb_page_restore(lv_obj_t *viewport, const orb_page_context_t *context);

typedef struct {
    bool pause_page, pauses_runtime, runtime_was_paused;
} orb_sheet_lifecycle_t;

/* Runtime execution stays at its existing owner. The host queues the returned
 * pause/resume decision; replacing a sheet retains the original pause state. */
bool orb_sheet_begin(orb_sheet_lifecycle_t *sheet, bool pause_page,
    bool runtime_active, bool runtime_was_paused);
bool orb_sheet_end(orb_sheet_lifecycle_t *sheet, bool covered_by_app);
