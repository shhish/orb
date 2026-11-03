#pragma once

#include "orb_app_host.h"
#include "orb_app_play.h"

orb_app_host_t *orb_play_host_create(bool preview, void *context,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *));
bool orb_play_host_attach(orb_app_host_t *host, lv_obj_t *page, bool system_input,
    bool (*input_allowed)(void *), void *context);
