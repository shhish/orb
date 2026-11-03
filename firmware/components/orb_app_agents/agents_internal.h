#pragma once

#include "orb_app_agents.h"
#include "orb_agents_copy.h"
#include "orb_usage_copy.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_kit_effects.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_motion.h"
#include "orb_ui.h"
#include "orb_page.h"
#include "orb_input.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Compatible route/action numbers remain value contracts during migration. */
enum {
    ORB_SHELL_AGENTS = ORB_APP_AGENTS_SCREEN, ORB_SHELL_SESSION = ORB_APP_AGENTS_SESSION,
    ORB_SHELL_CARD = ORB_APP_AGENTS_CARD, ORB_SHELL_DOT = ORB_APP_AGENTS_DOT,
    ORB_SHELL_IDLE_SESSIONS = ORB_APP_AGENTS_IDLE, ORB_SHELL_USAGE = ORB_APP_AGENTS_USAGE,
    ORB_SHELL_SESSIONS = ORB_APP_AGENTS_SESSIONS,
    ACT_SESSION = 120, ACT_OPTION = 200, ACT_REPLY = 212, ACT_IDLE_MORE = 272,
    ACT_IDLE_SESSION = 320, ACT_FOCUS_NEXT = 490, ACT_FOCUS_PREVIOUS = 491,
    ACT_FOCUS_OPEN = 492, ACT_APPROVAL_NEXT = 493, ACT_RESULT_RETRY = 494,
    ACT_RESULT_DONE = 495, ACT_FOCUS_SELECT = 520,
};
#define AGENTS_TALK_Y 352

typedef struct { uint32_t kind, started; } agents_wait_t;

typedef struct {
    orb_screen_instance_t *instance;
    orb_screen_id_t surface;
    lv_obj_t *page, *heading, *heading_glyph, *navigation, *list, *meter;
    /* Set only while a callback holds its call-scoped projection. */
    const orb_model_snapshot_t *model;
    const orb_idle_page_t *idle;
    const orb_session_t *selection;
    const orb_notification_record_t *receipt;
    bool has_selection, selection_external, fixture, materialise, net_connected, link_online, interactive;
    bool curved, refreshable, restore_pending, usage_bound;
    unsigned usage_bound_page;
    agents_wait_t waits[4];
    lv_obj_t *wait_row, *wait_result, *result_symbol;
    unsigned result_kind;
    bool outcome_live, result_changed;
    orb_kit_row_text_role_t wait_row_role;
    uint32_t reading_identity;
    int32_t reading_scroll;
    char session[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
    unsigned focus_index, page_index, projected_page;
    uint64_t agent_membership, agent_idle_hash;
    lv_obj_t *agent_status, *agent_name, *agent_quote, *agent_talk, *agent_caption, *agent_empty;
    char agent_status_text[120], agent_name_text[65], agent_quote_text[200], agent_quote_source[161];
    lv_obj_t *agent_steps[3], *agent_step_icons[3], *agent_added[3], *agent_removed[3];
    char agent_step_text[3][96], agent_step_icon_text[3][12];
    lv_obj_t *session_list_rows[ORB_MODEL_MAX_SESSIONS];
    char session_list_names[ORB_MODEL_MAX_SESSIONS][65], session_list_details[ORB_MODEL_MAX_SESSIONS][128];
    lv_obj_t *stream_rows[ORB_MODEL_STREAM_ROWS], *stream_text[ORB_MODEL_STREAM_ROWS], *stream_icons[ORB_MODEL_STREAM_ROWS];
    char stream_buffers[ORB_MODEL_STREAM_ROWS][ORB_MODEL_TEXT_MAX + 32];
    orb_stream_log_t *stream_log;
    lv_obj_t *usage_rings[2], *usage_pages[2], *usage_providers[2], *usage_values[2], *usage_resets[2];
    lv_obj_t *usage_weeks[2][3], *usage_footer, *usage_marks[2][2];
    lv_obj_t *dot_bubble, *dot_question, *dot_answer;
    char dot_question_text[281], dot_answer_text[281];
} agents_state_t;

lv_obj_t *agents_plain(lv_obj_t *parent);
lv_obj_t *agents_label(lv_obj_t *parent, const char *value, orb_text_style_t style,
    bool bold, int x, int y, int width, int lines, lv_color_t color);
lv_obj_t *agents_prose(agents_state_t *w, int y, int h);
bool agents_ready(const agents_state_t *w);
const char *agents_connection_status(const agents_state_t *w);
const orb_session_t *agents_selected_session(agents_state_t *w);
const orb_card_t *agents_selected_card(agents_state_t *w);
bool agents_voice_supported(agents_state_t *w, const char *target, const char *card_id);
void agents_clicked(lv_event_t *event);
lv_obj_t *agents_kit_action(agents_state_t *w, const char *text, int action,
    int x, int y, bool primary, bool glyph, lv_color_t colour);
void agents_reading_restore(agents_state_t *w, lv_obj_t *area, unsigned surface, const char *identity);
lv_obj_t *agents_status(agents_state_t *w, const char *glyph, const char *title, const char *detail, bool action);
lv_obj_t *agents_action(agents_state_t *w, const char *text, int action,
    int x, int y, bool primary, bool glyph, lv_color_t colour);
void agents_chrome_clear(agents_state_t *w);
lv_obj_t *agents_text(agents_state_t *w, const char *text, orb_text_style_t style,
    bool bold, int y, int width, unsigned lines, bool secondary);
bool agents_talk_available(agents_state_t *w, const orb_session_t *s);
lv_obj_t *agents_row(agents_state_t *w, const char *title, const char *detail, orb_agent_t agent, int action);
const orb_session_t *agents_target(agents_state_t *w, const char *target);
lv_obj_t *agents_capsule(agents_state_t *w, const char *caption, int action,
    int cx, int cy, bool primary, lv_color_t accent_colour);
int agents_pair(lv_obj_t *cancel, lv_obj_t *other);
lv_obj_t *agents_secondary_text(agents_state_t *w, const char *text, int y, int width, int lines);
lv_obj_t *agents_reading(agents_state_t *w, int x, int y, int width, int height);
int agents_orb_kit_paragraph(lv_obj_t *parent, const char *text, orb_text_style_t style,
    bool bold, int y, lv_color_t colour, bool centred);
void agents_result(agents_state_t *w, const orb_result_copy_t *copy, const char *decision,
    const char *body, unsigned surface, const char *identity, bool retry);
const orb_session_t *agents_focused(agents_state_t *w);
bool agents_focus_change(agents_state_t *w, int delta);
const orb_agent_stream_t *agents_stream(agents_state_t *w, const char *id);
uint64_t agents_membership(const agents_state_t *w);
void agents_focus_update(agents_state_t *w);
void agents(agents_state_t *w);
uint64_t agents_idle_hash(const agents_state_t *w);
void agents_sessions(agents_state_t *w);
void agents_stream_update(agents_state_t *w);
void agents_session(agents_state_t *w);
void agents_idle(agents_state_t *w);
void agents_usage_page(agents_state_t *w);
void agents_usage_update(agents_state_t *w);
void agents_usage(agents_state_t *w);
void agents_dot_update(agents_state_t *w);
void agents_dot(agents_state_t *w);
bool agents_approval(agents_state_t *w, const orb_card_t *c);
bool agents_refresh(agents_state_t *w);
bool agents_projection_begin(agents_state_t *state, const orb_screen_update_t *update);
bool agents_projection_current(agents_state_t *state);
void agents_projection_end(agents_state_t *state);
bool agents_input_allowed(agents_state_t *state);
void agents_token(lv_obj_t *row, bool session, orb_launch_token_t *token);

void agents_wait(agents_state_t *w, lv_obj_t *label, const char *text, unsigned slot);
void agents_state_wait(agents_state_t *w, lv_obj_t *state, const char *text, unsigned slot);
