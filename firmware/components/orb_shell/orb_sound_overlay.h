#pragma once

#include "orb_shell.h"
#include "orb_app_voice_spoken.h"

/* Small retained handles only. Captions belong to LVGL, with no raster buffer. */
typedef struct {
    lv_obj_t *volume, *volume_arc, *volume_hint;
    orb_app_voice_spoken_t *spoken;
    uint32_t volume_at;
    unsigned seen_volume;
    bool control_visible, volume_presented, motion_active;
    orb_shell_speech_stop_cb_t stop;
    void *stop_ctx;
} orb_sound_overlay_t;

void orb_sound_overlay_init(orb_sound_overlay_t *view);
void orb_sound_overlay_set_control_visible(orb_sound_overlay_t *view, bool visible);
void orb_sound_overlay_set_motion_active(orb_sound_overlay_t *view, bool active);
void orb_sound_overlay_poll(orb_sound_overlay_t *view, lv_display_t *display, bool visible);
void orb_sound_overlay_speech(orb_sound_overlay_t *view, lv_display_t *display,
                              bool active, bool buffering, const char *caption, int agent);
void orb_sound_overlay_destroy(orb_sound_overlay_t *view);
bool orb_sound_overlay_stop(orb_sound_overlay_t *view);
size_t orb_sound_overlay_size(void);

void orb_sound_overlay_media(orb_sound_overlay_t *view, lv_display_t *display, unsigned volume);
void orb_sound_overlay_volume(orb_sound_overlay_t *view, lv_display_t *display);

/* Root existence, including covered captions, preserves shell precedence. */
bool orb_sound_overlay_has_speech(const orb_sound_overlay_t *view);
