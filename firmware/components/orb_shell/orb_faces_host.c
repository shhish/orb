#include "orb_kit_motion.h"
#include "orb_faces_host.h"
#include "orb_sound.h"

#include <math.h>

static bool allow_synthetic(const orb_intent_t *intent)
{
    orb_faces_prefs_t prefs;
    return orb_app_faces_preferences_from_intent(intent, &prefs) ||
        (intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.action == ORB_APP_FACES_OPEN);
}

orb_app_host_t *orb_faces_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){.descriptor = orb_app_faces_descriptor,
        .name = "Faces", .model_bytes = sizeof(orb_app_faces_model_t), .command = command, .context = context,
        .allow_synthetic = allow_synthetic, .rim_finish = orb_app_faces_rotary_finish, .forget_on_detach = true});
}

orb_app_host_t *orb_faces_host_public_create(lv_obj_t *parent)
{
    orb_app_host_t *host = orb_app_host_create(true, &(orb_app_host_config_t){
        .descriptor = orb_app_faces_public_descriptor, .name = "Public face",
        .model_bytes = sizeof(orb_app_faces_model_t), .rim_finish = orb_app_faces_rotary_finish});
    if (host && orb_app_host_attach(host, parent)) return host;
    orb_app_host_destroy(host);
    return NULL;
}

bool orb_faces_host_attach(orb_app_host_t *host, lv_obj_t *page)
{
    if (orb_app_faces_resume(orb_app_host_current(host), page)) return true;
    /* A fresh presenter starts from its own empty snapshot, then receives the
     * current summary at the original construction tail. Never seed it with
     * the host's summary from a previous lifetime. */
    return orb_app_host_set_model(host, NULL, 0) && orb_app_host_attach(host, page);
}

void orb_faces_host_update(orb_app_host_t *host, const orb_faces_snapshot_t *snapshot)
{
    if (!snapshot || !orb_app_host_current(host)) return;
    orb_app_faces_model_t model = {.snapshot = *snapshot};
    if (orb_screen_descriptor(orb_app_host_current(host))->id == ORB_APP_FACES_PUBLIC_SCREEN_ID)
        orb_app_faces_public_projection(&model.snapshot);
    if (orb_app_host_set_model(host, &model, sizeof(model))) orb_app_host_poll(host);
}

void orb_faces_host_project(orb_faces_snapshot_t *snapshot, const orb_model_snapshot_t *model,
    const orb_home_snapshot_t *home, const orb_shell_snapshot_t *status)
{
    orb_app_faces_snapshot(snapshot, model, home, status->clock_time, status->clock_date,
        status->battery_pct, status->charging, status->net == ORB_SHELL_NET_CONNECTED &&
        status->link == ORB_SHELL_LINK_ONLINE);
}

bool orb_faces_host_preferences(orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    orb_faces_prefs_t prefs;
    if (!orb_app_faces_preferences_from_intent(intent, &prefs)) return false;
    uint64_t ticket = orb_settings_faces_request(&prefs);
    orb_app_faces_request_result(instance, ticket);
    return ticket != 0;
}

orb_screen_instance_t *orb_faces_host_current(orb_app_host_t *host)
{
    orb_screen_instance_t *screen = orb_app_host_current(host);
    return orb_app_faces_retained(screen) ? NULL : screen;
}

static void changed(void *context, float value, float delta)
{
    (void)delta;
    orb_app_faces_rotate(context, (int)lrintf(value));
}

static void tick(void *context)
{
    (void)context;
    orb_sound_play(ORB_SOUND_TICK);
}

bool orb_faces_host_rim(orb_app_host_t *host, orb_nav_t *nav, orb_nav_id_t route)
{
    orb_screen_instance_t *screen = orb_faces_host_current(host);
    if (!screen) return false;
    int value, maximum;
    if (orb_app_faces_rotary(screen, &value, &maximum)) {
        /* Keep the gallery's wide circular counter and the original re-anchor
         * policy, including synthetic focus changes and a coasting contact. */
        bool gallery = orb_app_faces_mode(screen) == ORB_FACES_GALLERY;
        orb_nav_rotary_owner_t owner = {.owner = route, .ctx = screen, .change = changed, .tick = tick,
            .minimum = gallery ? -32768 : 0, .maximum = gallery ? 32767 : maximum,
            .value = value, .detent = 1, .units_per_turn = maximum + 1};
        if (nav->rotary.bound && nav->rotary.owner.owner == owner.owner &&
            nav->rotary.owner.maximum == owner.maximum) {
            if (!nav->touch.down) orb_nav_rotary_set(nav, value);
        } else orb_nav_rotary_bind(nav, &owner);
    } else if (nav->rotary.bound) orb_nav_rotary_unbind(nav, nav->rotary.owner.owner);
    return true;
}

void orb_faces_host_motion(lv_obj_t *root, orb_screen_instance_t *face, bool active, bool shown, bool save)
{
    if (!active) orb_app_faces_active(face, false, save);
    orb_kit_motion_set_active(root, active && shown);
    if (active) orb_app_faces_active(face, true, save);
}
