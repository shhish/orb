#pragma once

/* Simulator-only public-data stub. The shell branch owns the shipped face.
 * No string or model pointer enters this API: private text cannot be drawn. */
#include "lvgl.h"
#include "orb_power.h"

typedef struct {
    unsigned hour, minute;
    unsigned waiting;
} power_face_data_t;

typedef struct {
    float clock_x, clock_y, mark_x, mark_y;
    float clock_opa, mark_opa, scale;
} power_face_pose_t;

typedef struct {
    lv_obj_t *root, *clock, *hand, *mark;
    lv_obj_t *source_image;
    lv_draw_buf_t *source_buffer;
    bool initialized, reduced, crossfade, reflow;
    orb_power_snapshot_t previous;
    power_face_data_t data;
    power_face_pose_t pose, from;
    uint64_t painted_ms, transition_ms, reflow_ms;
    float progress;
} power_view_t;

void power_view_create(power_view_t *view, lv_obj_t *parent, bool increase_contrast);
/* True only when visible content changed; settled Always On coalesces to 1 Hz.
 * Transitions admit frames at the controller's 16 ms opportunity. */
bool power_view_update(power_view_t *view, const orb_power_snapshot_t *power,
                       power_face_data_t data, bool reduce_motion, uint64_t now_ms);

/* Compose with the controller cadence: an admitted attention-presence change
 * briefly needs frames even while the controller is in settled Always On. */
uint32_t power_view_frame_interval_ms(const power_view_t *view);
