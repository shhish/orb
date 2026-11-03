/* Compile-time symbol adapters observe production requests without replacing
 * their implementation. The unmodified sources are separate CMake objects. */
#include "parity.h"
#include "orb_sound.h"
#include "orb_nav.h"
#include "orb_home.h"
#include <stdio.h>
#include <string.h>

bool parity_backend_sound_play(orb_sound_cue_t cue);
bool parity_backend_sound_preview(orb_sound_cue_t cue);
void parity_backend_sound_stop(orb_sound_cue_t cue);
bool orb_sound_play(orb_sound_cue_t cue)
{
    bool ok = parity_backend_sound_play(cue);
    parity_sound_event("play", cue, ok);
    return ok;
}
bool orb_sound_preview(orb_sound_cue_t cue)
{
    bool ok = parity_backend_sound_preview(cue);
    parity_sound_event("preview", cue, ok);
    return ok;
}
void orb_sound_stop(orb_sound_cue_t cue)
{
    parity_backend_sound_stop(cue);
    parity_sound_event("stop", cue, true);
}

/* Public read-model fixture for ESP-only preference persistence outcomes.
 * All ordinary reads use the real host service unchanged. No static fields,
 * filesystem calls or production error semantics are modified. */
void parity_backend_sound_preferences(orb_sound_preferences_t *preferences);
static bool s_sound_saving, s_sound_error;
void parity_sound_storage_state(bool saving, bool error)
{
    s_sound_saving=saving;s_sound_error=error;
    parity_event("fixture.sound-storage",error?"error":saving?"saving":"saved");
}
void orb_sound_preferences(orb_sound_preferences_t *preferences)
{
    parity_backend_sound_preferences(preferences);
    if(preferences) {
        preferences->saving=preferences->saving || s_sound_saving;
        preferences->save_error=preferences->save_error || s_sound_error;
    }
}

bool parity_backend_answer(const orb_answer_t *v);
bool parity_backend_message(const orb_message_t *v);
bool parity_backend_voice_request(orb_voice_action_t action, const char *target, const char *card);
bool parity_backend_prefs_request(const orb_prefs_patch_t *v);
bool parity_backend_nowplaying_request(orb_media_action_t action, int value);
static void request(const char *kind, const char *a, const char *b, const char *c, bool ok)
{
    char detail[1400];
    snprintf(detail, sizeof(detail), "accepted=%d; target=%s; choice=%s; text=%s", ok,
        a ? a : "", b ? b : "", c ? c : "");
    parity_event(kind, detail);
}
bool orb_model_answer(const orb_answer_t *v)
{
    bool ok = parity_backend_answer(v);
    request("answer", v ? v->card : "", v ? v->option : "", v ? v->text : "", ok);
    return ok;
}
bool orb_model_message(const orb_message_t *v)
{
    bool ok = parity_backend_message(v);
    request("message", v ? v->target : "", "", v ? v->text : "", ok);
    return ok;
}
bool orb_model_voice_request(orb_voice_action_t action, const char *target, const char *card)
{
    bool ok = parity_backend_voice_request(action, target, card);
    char name[40]; snprintf(name, sizeof(name), "voice.%u", action);
    request(name, target, card, "", ok);
    return ok;
}
bool orb_model_prefs_request(const orb_prefs_patch_t *v)
{
    bool ok = parity_backend_prefs_request(v);
    char detail[240];
    snprintf(detail, sizeof(detail), "accepted=%d; mask=%u; sources=%d,%d,%d; idle=%d:%u; temperature=%d:%u",
        ok, v ? v->sources_mask : 0, v ? v->sources[0] : 0, v ? v->sources[1] : 0,
        v ? v->sources[2] : 0, v ? v->has_idle_minutes : 0, v ? v->idle_minutes : 0,
        v ? v->has_temperature : 0, v ? v->temperature : 0);
    parity_event("preferences", detail);
    return ok;
}
bool orb_model_nowplaying_request(orb_media_action_t action, int value)
{
    bool ok = parity_backend_nowplaying_request(action, value);
    char detail[100]; snprintf(detail, sizeof(detail), "accepted=%d; action=%u; value=%d", ok, action, value);
    parity_event("media", detail);
    return ok;
}

void parity_backend_rotary_bind(orb_nav_t *nav, const orb_nav_rotary_owner_t *owner);
void parity_backend_rotary_unbind(orb_nav_t *nav, orb_nav_id_t owner);
void parity_backend_rotary_set(orb_nav_t *nav, float value);
static char s_rim[320] = "unbound";
/* The active workspace ticks; hidden neighbor previews only construct/bind.
 * Retain an identity for comparison only, never dereference a retired nav. */
static uintptr_t s_active_nav;
const char *parity_rim_observed(void) { return s_rim; }
static void describe_rim(const orb_nav_t *nav,char *out,size_t capacity)
{
    if(!nav || !nav->rotary.bound) { snprintf(out,capacity,"unbound");return; }
    const orb_nav_rotary_t *r=&nav->rotary;
    snprintf(out,capacity,"owner=%u; min=%.5g; max=%.5g; value=%.5g; detent=%.5g; units=%.5g",
        r->owner.owner,r->owner.minimum,r->owner.maximum,r->owner.value,
        r->owner.detent,r->owner.units_per_turn);
}
static void rim(const orb_nav_t *nav)
{
    if(s_active_nav && s_active_nav!=(uintptr_t)nav)return;
    describe_rim(nav,s_rim,sizeof(s_rim));
}
static void rim_event(const char *kind,const orb_nav_t *nav)
{
    char detail[320];describe_rim(nav,detail,sizeof(detail));parity_event(kind,detail);
}
void orb_nav_rotary_bind(orb_nav_t *nav, const orb_nav_rotary_owner_t *owner)
{
    parity_backend_rotary_bind(nav, owner);
    rim(nav); rim_event("rim.bind",nav);
}
void orb_nav_rotary_unbind(orb_nav_t *nav, orb_nav_id_t owner)
{
    parity_backend_rotary_unbind(nav, owner);
    rim(nav); rim_event("rim.unbind",nav);
}
void orb_nav_rotary_set(orb_nav_t *nav, float value)
{
    parity_backend_rotary_set(nav, value);
    rim(nav);
}

/* Refresh after every public mutation path. Values are copied while the caller
 * still owns nav; retaining a workspace pointer would dangle after navigation. */
bool parity_backend_rotary_step(orb_nav_t *nav, int detents);
bool parity_backend_nav_tick(orb_nav_t *nav, uint32_t now_ms);
orb_nav_event_t parity_backend_nav_touch(orb_nav_t *nav, orb_nav_touch_phase_t phase,
    float x, float y, uint32_t now_ms, bool blank_start, bool scroll_at_top);
void parity_backend_nav_init(orb_nav_t *nav, orb_nav_id_t face);
void parity_backend_nav_reset(orb_nav_t *nav, orb_nav_id_t id);

bool orb_nav_rotary_step(orb_nav_t *nav, int detents)
{
    bool changed = parity_backend_rotary_step(nav, detents);
    rim(nav);
    return changed;
}
bool orb_nav_tick(orb_nav_t *nav, uint32_t now_ms)
{
    s_active_nav=(uintptr_t)nav;
    bool changed = parity_backend_nav_tick(nav, now_ms);
    rim(nav);
    return changed;
}
orb_nav_event_t orb_nav_touch(orb_nav_t *nav, orb_nav_touch_phase_t phase,
    float x, float y, uint32_t now_ms, bool blank_start, bool scroll_at_top)
{
    orb_nav_event_t event = parity_backend_nav_touch(nav, phase, x, y, now_ms,
        blank_start, scroll_at_top);
    rim(nav);
    return event;
}
void orb_nav_init(orb_nav_t *nav, orb_nav_id_t face)
{
    parity_backend_nav_init(nav, face);
    rim(nav);
}
void orb_nav_reset(orb_nav_t *nav, orb_nav_id_t id)
{
    parity_backend_nav_reset(nav, id);
    rim(nav);
}

/* Home intents are observed at the service boundary, including rejected ones.
 * The original bounded queue, validation and authority rules remain in use. */
bool parity_backend_home_set(const char *id,const orb_home_state_t *values,bool unlock);
bool parity_backend_home_toggle(const char *id);
bool parity_backend_home_scene(const char *id);
bool parity_backend_home_room_off(const char *id);
bool orb_home_set(const char *id,const orb_home_state_t *values,bool unlock)
{
    bool ok=parity_backend_home_set(id,values,unlock);
    char detail[512];
    snprintf(detail,sizeof(detail),"accepted=%d; id=%s; unlock=%d; present=%u; on=%d; brightness=%.6g; hue=%.6g; sat=%.6g; ct=%.6g; target=%.6g; mode=%u; locked=%d; speed=%.6g; position=%.6g",
        ok,id?id:"",unlock,values?values->present:0,values?values->on:0,
        values?(double)values->brightness:0,values?(double)values->hue:0,values?(double)values->sat:0,
        values?(double)values->ct:0,values?(double)values->target:0,values?values->mode:0,
        values?values->locked:0,values?(double)values->speed:0,values?(double)values->position:0);
    parity_event("home.set",detail);return ok;
}
bool orb_home_toggle(const char *id)
{
    bool ok=parity_backend_home_toggle(id);request("home.toggle",id,"","",ok);return ok;
}
bool orb_home_scene(const char *id)
{
    bool ok=parity_backend_home_scene(id);request("home.scene",id,"","",ok);return ok;
}
bool orb_home_room_off(const char *id)
{
    bool ok=parity_backend_home_room_off(id);request("home.room-off",id,"","",ok);return ok;
}

/* Explicit fixture for the public admission-failure outcome. This exercises
 * the UI's error projection without pressuring the host heap or altering the
 * production allocator. The normal service path always forwards unchanged. */
bool parity_backend_idle_open(void);
static bool s_idle_failure;
void parity_idle_failure(bool fail) { s_idle_failure=fail; }
bool orb_model_idle_open(void)
{
    if(!s_idle_failure)return parity_backend_idle_open();
    parity_event("fixture.idle-admission","allocation failure outcome");return false;
}
