#pragma once

#include "orb_screen.h"
#include "orb_shell.h"
#include "orb_model.h"
#include "orb_sound.h"

typedef struct orb_settings_host orb_settings_host_t;
typedef void (*orb_settings_host_changed_t)(void *context);
typedef bool (*orb_settings_host_accept_t)(void *context, const orb_intent_t *intent);

/* Compatibility hosting for migrated descriptors while the shell still owns
 * outer transitions. Context storage stays opaque to the workspace. */
orb_settings_host_t *orb_settings_host_create(bool preview,
    orb_settings_host_changed_t changed, orb_settings_host_accept_t accept, void *context);
void orb_settings_host_destroy(orb_settings_host_t *host);
bool orb_settings_host_detach(orb_settings_host_t *host);
/* Preflight every outer departure before mutating surface/history, including
 * destinations that are still legacy screens. No current instance is closed. */
bool orb_settings_host_can_depart(orb_settings_host_t *host);
lv_obj_t *orb_settings_host_list(const orb_settings_host_t *host);
lv_obj_t *orb_settings_host_title(const orb_settings_host_t *host);
bool orb_settings_host_copy(orb_settings_host_t *destination, orb_settings_host_t *source);

bool orb_settings_host_clone(orb_settings_host_t **destination, orb_settings_host_t *source, bool preview);
bool orb_settings_host_restore(orb_settings_host_t **destination, orb_settings_host_t **saved);

orb_screen_instance_t *orb_settings_host_current(orb_settings_host_t *host);


uint32_t orb_settings_host_page_for(const orb_settings_host_t *host, orb_screen_id_t screen);
bool orb_settings_host_back_for(orb_settings_host_t *host, orb_screen_id_t screen);

/* Admission transaction. scene_parent must be outside the current shell page.
 * Stage is side-effect-free presentation work and retains the live instance.
 * Repeating stage for the same route updates that candidate without recreating
 * it. Commit must precede deletion of the old canonical page. */
bool orb_settings_host_stage(orb_settings_host_t *, lv_obj_t *scene_parent,
    orb_screen_id_t, const orb_screen_model_t *);
bool orb_settings_host_commit_staged(orb_settings_host_t *, lv_obj_t *canonical_parent);
void orb_settings_host_cancel_staged(orb_settings_host_t *);
orb_screen_instance_t *orb_settings_host_staged(orb_settings_host_t *);
bool orb_settings_host_detach_page(orb_settings_host_t *, lv_obj_t *page);
/* Synchronous snapshot bracket: do not dispatch input/timers or yield between
 * begin/end. Framework visibility stays covered/read-only while ink is exposed. */
lv_obj_t *orb_settings_host_snapshot_begin(orb_settings_host_t *);
void orb_settings_host_snapshot_end(orb_settings_host_t *);
bool orb_settings_host_stage_about(orb_settings_host_t *, lv_obj_t *scene_parent,
    const orb_shell_snapshot_t *, const char *connection, bool ready);
bool orb_settings_host_stage_units(orb_settings_host_t *, lv_obj_t *scene_parent,
    orb_screen_id_t, const orb_model_snapshot_t *, const char *notice);
bool orb_settings_host_stage_root(orb_settings_host_t *, lv_obj_t *scene_parent,
    const orb_shell_snapshot_t *, bool silent);

bool orb_settings_host_stage_sources(orb_settings_host_t *, lv_obj_t *scene_parent,
    const orb_model_snapshot_t *, const char *notice);

bool orb_settings_host_stage_accessibility(orb_settings_host_t *, lv_obj_t *scene_parent,
    bool contrast, const char *notice);

bool orb_settings_host_stage_battery(orb_settings_host_t *, lv_obj_t *, const orb_shell_snapshot_t *);
bool orb_settings_host_update_battery(orb_settings_host_t *, const orb_shell_snapshot_t *);
/* Weather (orb_app_weather) shares this host; orb_weather_host.c projects it.
 * Update changes the visible instance in place, true when it is not current. */
bool orb_settings_host_stage_weather(orb_settings_host_t *, lv_obj_t *, const orb_model_snapshot_t *);
bool orb_settings_host_update_weather(orb_settings_host_t *, const orb_model_snapshot_t *);

bool orb_settings_host_accessibility_apply(const orb_intent_t *, bool *contrast,
    char *notice, size_t notice_size, void *context);

bool orb_settings_host_stage_display(orb_settings_host_t *, lv_obj_t *, void *context, int32_t brightness);
bool orb_settings_host_update_display(orb_settings_host_t *, void *context, int32_t brightness);
lv_obj_t *orb_settings_host_display_level(orb_settings_host_t *);
bool orb_settings_host_display_twist(orb_settings_host_t *, int detents);
void orb_settings_host_setting_queue(orb_settings_host_t *, uint32_t key, int32_t value);
bool orb_settings_host_setting_pending(const orb_settings_host_t *, uint32_t key);
bool orb_settings_host_setting_take(orb_settings_host_t *, uint32_t key, int32_t *value);
void orb_settings_host_flush_settings(orb_settings_host_t *, void *context);
bool orb_settings_host_update(orb_settings_host_t *, orb_screen_id_t, const orb_screen_model_t *);

bool orb_settings_host_stage_sound(orb_settings_host_t *, lv_obj_t *, bool silent, const char *voice, const char *notice);
bool orb_settings_host_update_sound(orb_settings_host_t *, bool silent, const char *voice, const char *notice);
lv_obj_t *orb_settings_host_sound_level(orb_settings_host_t *);
void orb_settings_host_sound_crown(orb_settings_host_t *, int32_t value);
bool orb_settings_host_group_capture(orb_settings_host_t *, orb_screen_id_t);
const orb_screen_context_t *orb_settings_host_group_context(const orb_settings_host_t *, orb_screen_id_t, size_t *size);
/* Value-only sound snapshot port; storage-result fixtures replace the port. */
void orb_settings_sound_preferences(orb_sound_preferences_t *);

/* Consume local argument navigation after the new page becomes visible. */
bool orb_settings_host_arrive_local(orb_settings_host_t *);
