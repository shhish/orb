#pragma once

#include "orb_kit.h"

/* HIG §§1–4/7: page content is style-free; controls retain their kit material.
 * The returned viewport belongs to the caller, including all its descendants.
 * Geometry is explicit during migration so differing legacy viewports survive. */
typedef struct {
    int top, height, pad_bottom;
    bool reading;
} orb_kit_list_page_config_t;

/* Review #33: one rest position for every list. The first row starts just
 * below the 38 px top fade of a list under a rim title, never inside it and
 * never stranded at the equator below an empty band. */
#define ORB_KIT_LIST_INSET 40

lv_obj_t *orb_kit_list_page(lv_obj_t *parent,
                                  const orb_kit_list_page_config_t *config);

typedef struct {
    int top, height;
    bool system, arc;
} orb_kit_detail_page_config_t;

/* Caller owns the reading viewport and all descendants. No kit-private handle
 * is introduced: the caller may change viewport geometry for its legacy lane. */
lv_obj_t *orb_kit_detail_page(lv_obj_t *parent,
                               const orb_kit_detail_page_config_t *config);
int orb_kit_paragraph(lv_obj_t *parent, const char *text, orb_text_style_t style,
                       bool bold, int y, lv_color_t color, bool center);


typedef struct {
    const char *glyph, *title, *detail;
    lv_color_t secondary;
    lv_opa_t secondary_opa;
    /* Optional: symbol tint (grey unless accented) and the vertical band
     * the group centres in (0 = below the rim title, above the 6 o'clock
     * action lane: 88..378). Set a band or accent to opt into the symbol-disc
     * flow; an all-zero band retains the compact state composition. State is
     * never colour alone (HIG §5). */
    lv_color_t accent;
    bool accented;
    int32_t top, bottom;
} orb_kit_state_config_t;

lv_obj_t *orb_kit_state_create(lv_obj_t *parent,
                               const orb_kit_state_config_t *config);


typedef struct {
    const char *title, *decision, *answer, *explanation;
    lv_color_t secondary;
} orb_kit_receipt_config_t;

/* A delivered mark is prepared and painted before the reading flow so its
 * allocation and animation start order remain identical during extraction. */
lv_obj_t *orb_kit_receipt_mark(lv_obj_t *area);
void orb_kit_receipt_content(lv_obj_t *area, const orb_kit_receipt_config_t *config);

/* Caller/framework owns this full-screen sheet surface and its descendants. */
lv_obj_t *orb_kit_sheet_surface(lv_obj_t *parent);

/* The caller retains the existing reader's flags and lifecycle. Consequence
 * content and action layout never enqueue commands or choose a default. */
void orb_kit_confirm_content(lv_obj_t *reading, const char *consequences);
lv_obj_t *orb_kit_confirm_action(lv_obj_t *parent, const char *text, int center_y);

lv_obj_t *orb_kit_page_label(lv_obj_t *parent, const char *value, orb_text_style_t style,
    bool bold, int x, int y, int width, int lines, lv_color_t color);

void orb_kit_list_page_finish(lv_obj_t *list, int pad_bottom);
