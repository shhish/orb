#pragma once

#include "orb_app_voice.h"
#include "orb_receipt.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_kit_effects.h"
#include "orb_ui.h"
#include <stdio.h>
#include <string.h>

struct orb_voice_flow {
    bool mic_held, voice_pending, voice_touch, voice_retry;
    orb_voice_phase_t command_phase, voice_seen_phase;
    uint32_t voice_sent_at, wait_started;
};

enum { ORB_SHELL_VOICE = ORB_APP_VOICE_SCREEN, ACT_OPTION = 200, ACT_REPLY = 212,
    ACT_VOICE_STOP = 268, ACT_VOICE_CANCEL, ACT_VOICE_SEND, ACT_VOICE_HOLD,
    ACT_RESULT_RETRY = 494, ACT_RESULT_DONE };
#define AGENTS_TALK_Y 352

typedef struct {
    orb_screen_instance_t *instance;
    lv_obj_t *page, *heading, *list, *meter;
    lv_obj_t *voice_bars[9], *voice_status, *voice_name, *voice_quote, *voice_talk;
    char voice_status_text[120], voice_preview_text[281], voice_source_text[281];
    const orb_model_snapshot_t *model;
    const orb_session_t *selection;
    orb_voice_flow_t *flow;
    const char *notice;
    bool has_selection, selection_external, fixture, materialise, interactive, touch_allowed;
    lv_color_t capture_colour;
    uint32_t reading_identity;
    int32_t reading_scroll;
    bool restore_pending, outcome_live, result_changed;
    unsigned result_kind;
    lv_obj_t *result_symbol;
} voice_state_t;

bool voice_projection_current(voice_state_t *state);
void voice_projection_end(voice_state_t *state);
void voice_render(voice_state_t *state);
void voice_meter(voice_state_t *state);
void voice_clicked(lv_event_t *event);
void voice_hold(lv_event_t *event);
lv_obj_t *voice_plain(lv_obj_t *parent);
lv_obj_t *voice_label(lv_obj_t *parent, const char *text, orb_text_style_t style,
    bool bold, int x, int y, int width, int lines, lv_color_t colour);
bool voice_ready(const voice_state_t *state);
lv_obj_t *voice_action(voice_state_t *state, const char *text, int action,
    int x, int y, bool primary, bool glyph, lv_color_t colour);
lv_obj_t *voice_capsule(voice_state_t *state, const char *text, int action,
    int x, int y, bool primary, lv_color_t colour);
lv_obj_t *voice_reading(voice_state_t *state, int x, int y, int width, int height);
void voice_reading_restore(voice_state_t *state, lv_obj_t *area, unsigned surface, const char *identity);
int voice_paragraph(lv_obj_t *parent, const char *text, orb_text_style_t style,
    bool bold, int y, lv_color_t colour, bool centred);
lv_obj_t *voice_secondary_text(voice_state_t *state, const char *text, int y, int width, int lines);
void voice_result(voice_state_t *state, const orb_result_copy_t *copy, const char *decision,
    const char *body, unsigned surface, const char *identity, bool retry);
bool voice_set(lv_obj_t *obj, char *buffer, size_t capacity, const char *value);
void voice_fit(lv_obj_t *obj, char *buffer, size_t capacity, const char *value);
