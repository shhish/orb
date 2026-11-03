#pragma once

#include <stdbool.h>

#include "orb_model.h"
#include "orb_receipt.h"
#include "lvgl.h"

/* Agent-surface copy that depends only on model facts, shared by the card,
 * approval, notification and voice views so each state has one wording
 * (docs/design/glossary.md). */

/* Review #12: why controls are unavailable, in the kit's shared words; NULL when ready. */
const char *orb_agents_unavailable(bool online, bool ready);

/* Review #13: NULL when Send can commit this transcript, else the reason. */
const char *orb_voice_review_blocked(bool ready, bool available, bool session, const char *target, bool text);

/* Retained agent text: update only when semantic content changes. */
bool agents_set(lv_obj_t *obj, char *buffer, size_t capacity, const char *value);
void agents_fit(lv_obj_t *obj, char *buffer, size_t capacity, const char *value, bool wrap);
void agents_line(lv_obj_t *obj, char *buffer, size_t capacity, const char *value);
int agents_ink(lv_obj_t *label, const char *text);
void agents_count(char *text, size_t capacity, const char *sign, uint32_t count);

/* Shared provider identity and compact step-list presentation. */
const char *agents_provider(orb_agent_t agent);
const char *agents_icon(orb_agent_t agent);
const char *agents_state_text(orb_session_state_t state);
lv_color_t agents_colour(orb_agent_t agent);
unsigned agents_priority(const orb_session_t *session);
unsigned agents_step_rows(void);

/* One shared row component for the Agents glance and the session stream. */
const char *agents_step_glyph(orb_step_state_t state);
int agents_step_gap(void);
void agents_step_bind(lv_obj_t *icon, lv_obj_t *text, char *buffer, size_t capacity,
                       orb_agent_t agent, orb_step_state_t state, const char *value,
                       int width, bool present);

unsigned agents_order(const orb_model_snapshot_t *model, unsigned *indices);

/* Idle pager footer vocabulary is shared with shell compatibility consumers. */
typedef struct { const char *title, *detail, *glyph; } orb_idle_more_copy_t;
orb_idle_more_copy_t orb_idle_more_copy(bool allocated, bool online, bool ready, bool pending, bool failed);
