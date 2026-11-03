#pragma once

#include "orb_faces.h"
#include "orb_ui.h"

/* Presenter-owned state stays private while the adapter stores only values. */
#define FACE_REFS 160
#define FACE_ACTIONS 24

typedef struct { struct orb_faces *faces; int action, value; } face_action_t;
typedef struct { lv_obj_t *object; const char *role; } face_ref_t;
typedef struct {
    lv_obj_t *root, *glyph, *value, *ring;
    char glyph_text[24], value_text[32];
    int target;
    bool known;
} face_form_t;
struct orb_faces {
    lv_timer_t *seconds;
    lv_obj_t *root, *content, *clock, *date, *day_label, *weather, *weather_icon, *weather_temp, *weather_label, *summary, *caption;
    lv_obj_t *disc[4], *value[4], *glyph[4], *ring[4], *activity[3], *legend[3], *hands[3], *offline;
    lv_point_precise_t ticks[60][2], hand_points[3][2];
    orb_faces_config_t config;
    orb_faces_snapshot_t snapshot;
    orb_faces_prefs_t prefs, save_candidate;
    uint64_t save_ticket;
    unsigned save_action;
    bool save_browsing;
    orb_faces_mode_t save_mode;
    orb_faces_mode_t mode;
    unsigned slot, ref_count, action_count;
    uint8_t face_before_gallery;
    int activity_target[3];
    int lit_activity[3], lit_forms[4][ORB_FACE_PROVIDER_COUNT];
    bool wake_values, covered, timer_tick;
    bool activity_known[3], lit_activity_known[3], lit_forms_known[4][ORB_FACE_PROVIDER_COUNT];
    face_ref_t refs[FACE_REFS];
    face_form_t forms[4][ORB_FACE_PROVIDER_COUNT];
    struct orb_faces *gallery[3], *colour_preview;
    lv_obj_t *swatches[9], *gallery_dots, *slot_position;
    face_action_t actions[FACE_ACTIONS];
    orb_content_size_t size;
    bool browsing, active, bold, opaque, always_on, save_error, failed_select, dirty, building, held, dragged;
    int drag_x, drag_y, artwork_scale;
    uint32_t sample_tick;
    char date_weekday[24], date_day[8];
    char summary_text[48], weather_text[48], legend_text[3][48];
};


void orb_faces_seconds_policy(orb_faces_t *v);
void orb_faces_rolling_text(orb_faces_t *v, lv_obj_t *label, const char *value);
void orb_faces_rolling_static(orb_faces_t *v, lv_obj_t *label, char *storage, size_t capacity, const char *value);
