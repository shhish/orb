#pragma once

#include "orb_shell.h"
#include "orb_fonts.h"
#include "orb_ui.h"

typedef enum { LINE_TITLE, LINE_SUBTITLE, LINE_CODE, LINE_BODY, LINE_COMMAND, LINE_DETAIL, LINE_COUNT } line_t;
typedef enum {
    VIEW_START, VIEW_SETUP, VIEW_PAIR, VIEW_HOME, VIEW_OFFER,
    VIEW_DOWNLOAD, VIEW_VERIFY, VIEW_READY, VIEW_FAILED, VIEW_SERVICE,
    VIEW_IDENTIFY, VIEW_CONNECTION, VIEW_MAINTENANCE, VIEW_FORGET, VIEW_UNPAIR, VIEW_UPDATES,
} view_t;

/* One shell-lifetime scaffold: setup and OTA deliberately reuse these exact
 * nodes and their historical styles. Descriptors borrow them synchronously;
 * deleting an instance must never delete or reinitialise the scaffold. */
typedef struct {
    lv_obj_t *system, *lines[LINE_COUNT], *wifi, *battery, *power, *ring;
    lv_obj_t *art, *system_title, *settings, *primary, *forget, *unpair, *cancel, *reading;
    unsigned max_lines[LINE_COUNT];
    /* Boot wait ownership survives disposable descriptor/menu views. */
    unsigned boot_wait_kind;
    uint32_t boot_wait_started;
} orb_system_scaffold_t;

void orb_system_scaffold_create(orb_system_scaffold_t *scaffold, lv_obj_t *system,
    lv_event_cb_t clicked, void *context);
void orb_system_scaffold_line(orb_system_scaffold_t *scaffold, view_t view, line_t which,
    const char *value, orb_text_style_t style, orb_text_variant_t variant, unsigned max_lines);
void orb_system_scaffold_primary(orb_system_scaffold_t *scaffold, const char *label, bool enabled);
int32_t orb_system_scaffold_design_line(orb_system_scaffold_t *scaffold, line_t which,
    int32_t y, int32_t width, lv_color_t color);
void orb_system_scaffold_layout(orb_system_scaffold_t *scaffold, view_t view);
void orb_system_scaffold_layout_status(orb_system_scaffold_t *scaffold, const orb_shell_snapshot_t *status);
void orb_system_scaffold_status(orb_system_scaffold_t *scaffold, const orb_shell_snapshot_t *status);
void orb_system_scaffold_identity(orb_system_scaffold_t *scaffold, bool shown);
