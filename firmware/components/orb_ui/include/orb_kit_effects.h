#pragma once

#include "orb_kit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Alive presentation only (delight §6). These never dispatch an action or
 * mutate the caller's value. The shared kit clock stops when effects settle. */
void orb_kit_disc_light_up(lv_obj_t *disc, lv_color_t before, lv_color_t after);
void orb_kit_disc_light_up_to(lv_obj_t *disc, lv_color_t after, lv_opa_t opacity);
void orb_kit_disc_light_up_cancel(lv_obj_t *disc);
void orb_kit_pop_in(lv_obj_t *obj);
void orb_kit_pop_in_from(lv_obj_t *obj, int32_t origin_x, int32_t origin_y);
void orb_kit_pop_in_arrival(lv_obj_t *obj);
/* Absolute display-space origin; visual pivot only, behind the arrival fence. */
void orb_kit_pop_in_arrival_from(lv_obj_t *obj, int32_t origin_x, int32_t origin_y);
lv_obj_t *orb_kit_check_create(lv_obj_t *parent, lv_color_t colour, bool deny);
void orb_kit_check_arrival(lv_obj_t *obj);
/* Draw real completion over the existing glyph's unchanged text/geometry. */
void orb_kit_check_label_draw_on(lv_obj_t *label);
void orb_kit_check_label_arrival(lv_obj_t *label);
void orb_kit_live_ink_opa(lv_obj_t *label, lv_opa_t opacity);
/* Repeated true does not restart a wait or its 30 s budget. False restores
 * the original ink opacity; hiding/quiet presentation also ends the effect. */
void orb_kit_breathe(lv_obj_t *label, bool waiting);
void orb_kit_value_set_rolling(lv_obj_t *label, const char *text, const char *reserve_template);
void orb_kit_value_set_projection(lv_obj_t *label, const char *presented_text,
                                const char *target_text, const char *reserve_template);
bool orb_kit_value_projection_semantic_text(lv_obj_t *label, const char **out);
void orb_kit_row_set_live_text_rolling(lv_obj_t *row, orb_kit_row_text_role_t role,
                                      const char *text, const char *reserve_template);
void orb_kit_row_set_live_accent_animated(lv_obj_t *row, lv_color_t fill, lv_color_t ink);
void orb_kit_row_live_accent_cancel(lv_obj_t *row);
void orb_kit_row_set_waiting(lv_obj_t *row, orb_kit_row_text_role_t role, bool waiting);
void orb_kit_state_set_waiting(lv_obj_t *state, bool waiting);

/* Retire navigation material before snapshotting a settled surface. */
void orb_kit_material_finish(lv_obj_t *parent);

void orb_kit_row_set_live_glyph(lv_obj_t *row, const char *glyph);

/* Semantic traces retain the target while native ink is drawn per digit. */
bool orb_kit_value_roll_semantic_text(lv_obj_t *label, const char **out);

/* Retained/rebuilt waits carry one absolute budget across covers and context
 * restoration. Activation resumes only waits that are still within 30 s. */
void orb_kit_breathe_since(lv_obj_t *label, bool waiting, uint32_t started_ms);
void orb_kit_row_set_waiting_since(lv_obj_t *row, orb_kit_row_text_role_t role, bool waiting, uint32_t started_ms);
void orb_kit_breathe_resume(lv_obj_t *parent);
void orb_kit_state_set_waiting_since(lv_obj_t *state, bool waiting, uint32_t started_ms);

#ifdef __cplusplus
}
#endif
