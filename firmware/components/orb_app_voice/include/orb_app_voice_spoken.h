#pragma once

#include "orb_screen.h"

/* Shell-managed spoken presentation. The framework root is the original
 * modal canvas; the shell still owns precedence and underlying-app policy. */
#define ORB_APP_VOICE_SPOKEN_SCREEN_ID 0xff02
#define ORB_APP_VOICE_SPOKEN_STOP 1

typedef struct orb_app_voice_spoken orb_app_voice_spoken_t;
typedef bool (*orb_app_voice_spoken_stop_cb_t)(void *context);

/* Borrowed only during create/update. LVGL owns its copy of the caption. */
typedef struct {
    const char *caption;
    int agent;
    bool buffering;
} orb_app_voice_spoken_model_t;

const orb_screen_descriptor_t *orb_app_voice_spoken_descriptor(void);

orb_app_voice_spoken_t *orb_app_voice_spoken_create(bool preview,
    orb_app_voice_spoken_stop_cb_t stop, void *context);
void orb_app_voice_spoken_destroy(orb_app_voice_spoken_t *view);
void orb_app_voice_spoken_set_stop(orb_app_voice_spoken_t *view,
    orb_app_voice_spoken_stop_cb_t stop, void *context);
bool orb_app_voice_spoken_present(orb_app_voice_spoken_t *view, lv_display_t *display,
    bool active, bool buffering, const char *caption, int agent);
void orb_app_voice_spoken_show(orb_app_voice_spoken_t *view, bool visible);
/* A retained root, including a temporarily hidden one, mirrors the shell's
 * former speech handle. An inactive presentation has no root. */
lv_obj_t *orb_app_voice_spoken_root(const orb_app_voice_spoken_t *view);
bool orb_app_voice_spoken_stop(orb_app_voice_spoken_t *view);
size_t orb_app_voice_spoken_size(void);
