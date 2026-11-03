/* Exercise the same admission transaction as the workspace. */
#pragma once
#include "orb_settings_host.h"

static inline lv_obj_t *settings_scene(lv_obj_t *parent)
{
    lv_obj_t *scene = lv_obj_get_parent(parent);
    return scene ? scene : parent;
}

static inline bool settings_show_about(orb_settings_host_t *host, lv_obj_t *parent,
    const orb_shell_snapshot_t *status, const char *connection, bool ready)
{
    return parent && orb_settings_host_stage_about(host, settings_scene(parent), status, connection, ready) &&
        orb_settings_host_commit_staged(host, parent);
}

static inline bool settings_show_units(orb_settings_host_t *host, lv_obj_t *parent,
    orb_screen_id_t screen, const orb_model_snapshot_t *model, const char *notice)
{
    return parent && orb_settings_host_stage_units(host, settings_scene(parent), screen, model, notice) &&
        orb_settings_host_commit_staged(host, parent);
}

static inline bool settings_show_root(orb_settings_host_t *host, lv_obj_t *parent,
    const orb_shell_snapshot_t *status, bool silent)
{
    return parent && orb_settings_host_stage_root(host, settings_scene(parent), status, silent) &&
        orb_settings_host_commit_staged(host, parent);
}
