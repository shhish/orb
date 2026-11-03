#include "system_views.h"
#include "orb_system_state.h"
#include "orb_page.h"
#include "orb_memory.h"

#include <string.h>

struct orb_system_views {
    orb_app_host_t *onboarding, *updates;
    orb_onboarding_port_t port;
    orb_updates_model_t model;
    unsigned save_state;
    uint32_t onboarding_wait_started;
    bool onboarding_waiting;
    bool preview;
    bool (*event)(void *, orb_system_view_event_t);
    void *context;
};

static unsigned onboarding_state(const orb_system_views_t *views)
{
    return (views->port.saving() ? 1u : 0u) | (views->port.error() ? 2u : 0u);
}

static bool event(orb_system_views_t *views, orb_system_view_event_t value)
{
    return views->event && views->event(views->context, value);
}

static bool command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    orb_system_views_t *views = context;
    (void)instance;
    if (views->preview || intent->kind != ORB_INTENT_APP_COMMAND || !event(views, ORB_SYSTEM_VIEW_INPUT)) return false;
    if (intent->value.command.target == ORB_SHELL_UPDATES && intent->value.command.action == ORB_SYSTEM_CHECK_UPDATE) {
        if (views->model.pending || !views->model.online) return false;
        views->model.pending = true;
        views->model.wait_started = lv_tick_get();
        event(views, ORB_SYSTEM_VIEW_CHECK);
        event(views, ORB_SYSTEM_VIEW_CHANGED);
        return true;
    }
    if (intent->value.command.target != ORB_SHELL_ONBOARDING || intent->value.command.action != ORB_SYSTEM_ONBOARD_DONE)
        return false;
    /* Acceptance is not completion. Only the service's durable result leaves
     * the tutorial; synthetic Done keeps its existing presentation-only exit. */
    if (event(views, ORB_SYSTEM_VIEW_PHYSICAL)) {
        (void)views->port.dismiss();
        views->save_state = onboarding_state(views);
        if (views->save_state & 1) views->onboarding_wait_started = lv_tick_get();
        if (!views->save_state && !views->port.needed()) event(views, ORB_SYSTEM_VIEW_DONE);
        else event(views, ORB_SYSTEM_VIEW_CHANGED);
    } else event(views, ORB_SYSTEM_VIEW_DONE);
    return true;
}

static bool allow_synthetic(const orb_intent_t *intent)
{
    return intent->kind == ORB_INTENT_APP_COMMAND &&
        ((intent->value.command.target == ORB_SHELL_ONBOARDING && intent->value.command.action == ORB_SYSTEM_ONBOARD_DONE) ||
         (intent->value.command.target == ORB_SHELL_UPDATES && intent->value.command.action == ORB_SYSTEM_CHECK_UPDATE));
}

static void rim_finish(orb_screen_instance_t *instance, bool commit)
{
    (void)instance; (void)commit;
}

orb_system_views_t *orb_system_views_create(bool preview,
    bool (*callback)(void *, orb_system_view_event_t), void *context)
{
    orb_system_views_t *views = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*views));
    if (!views) return NULL;
    views->preview = preview;
    views->event = callback;
    views->context = context;
    views->port = (orb_onboarding_port_t){orb_system_onboarding_needed, orb_system_onboarding_saving,
        orb_system_onboarding_save_error, orb_system_onboarding_dismiss};
    return views;
}

void orb_system_views_destroy(orb_system_views_t *views)
{
    if (!views) return;
    orb_app_host_destroy(views->onboarding);
    orb_app_host_destroy(views->updates);
    orb_memory_free(views);
}

void orb_system_views_onboarding_port(orb_system_views_t *views, const orb_onboarding_port_t *port)
{
    if (views && port && port->needed && port->saving && port->error && port->dismiss) views->port = *port;
}

orb_screen_instance_t *orb_system_views_current(orb_system_views_t *views, orb_shell_surface_t surface)
{
    return views ? orb_app_host_current(surface == ORB_SHELL_ONBOARDING ? views->onboarding : views->updates) : NULL;
}

static void status_model(orb_system_views_t *views, const orb_shell_snapshot_t *status)
{
    views->model.online = status->net == ORB_SHELL_NET_CONNECTED && status->link == ORB_SHELL_LINK_ONLINE;
    memcpy(views->model.firmware_version, status->firmware_version, sizeof(views->model.firmware_version));
    memcpy(views->model.result, status->update_check_result, sizeof(views->model.result));
}

bool orb_system_views_attach(orb_system_views_t *views, lv_obj_t *page,
    orb_shell_surface_t surface, const orb_shell_snapshot_t *status)
{
    if (!views || !status || (surface != ORB_SHELL_ONBOARDING && surface != ORB_SHELL_UPDATES)) return false;
    bool onboarding = surface == ORB_SHELL_ONBOARDING;
    orb_app_host_t **host = onboarding ? &views->onboarding : &views->updates;
    if (!*host) *host = orb_app_host_create(views->preview, &(orb_app_host_config_t){
        .descriptor = onboarding ? orb_onboarding_descriptor : orb_updates_descriptor,
        .name = onboarding ? "Onboarding" : "Updates", .forget_on_detach = true,
        .model_bytes = onboarding ? sizeof(orb_onboarding_model_t) : sizeof(orb_updates_model_t),
        .command = command, .allow_synthetic = allow_synthetic, .rim_finish = rim_finish, .context = views});
    status_model(views, status);
    unsigned saving = onboarding ? onboarding_state(views) : 0;
    if ((saving & 1) && !views->onboarding_waiting) views->onboarding_wait_started = lv_tick_get();
    if (onboarding) views->onboarding_waiting = (saving & 1) != 0;
    orb_onboarding_model_t model = {.save_state = saving, .wait_started = views->onboarding_wait_started};
    if (!orb_app_host_set_model(*host, onboarding ? (const void *)&model : &views->model,
        onboarding ? sizeof(model) : sizeof(views->model)) || !orb_app_host_attach(*host, page)) return false;
    /* Shell chrome remains outside its moving body, as in Charging. The
     * framework still owns and releases the reparented title exactly once. */
    lv_obj_t *title = orb_screen_page_title(orb_app_host_current(*host));
    if (title) lv_obj_set_parent(title, page);
    return true;
}

void orb_system_views_detach(orb_system_views_t *views, lv_obj_t *page)
{
    if (!views) return;
    orb_app_host_detach_page(views->onboarding, page);
    orb_app_host_detach_page(views->updates, page);
}

bool orb_system_views_update(orb_system_views_t *views,
    const orb_shell_snapshot_t *status, const orb_shell_snapshot_t *previous)
{
    bool changed = status->update_check_revision != previous->update_check_revision ||
        strcmp(status->update_check_result, previous->update_check_result) ||
        strcmp(status->firmware_version, previous->firmware_version);
    if (!views) return changed;
    if (status->update_check_revision != previous->update_check_revision || status->ota != ORB_SHELL_OTA_IDLE ||
        status->net != ORB_SHELL_NET_CONNECTED || status->link != ORB_SHELL_LINK_ONLINE)
        views->model.pending = false;
    status_model(views, status);
    return changed;
}

void orb_system_views_poll(orb_system_views_t *views, bool onboarding)
{
    if (!views || !onboarding) return;
    unsigned state = onboarding_state(views);
    if (views->save_state == 1 && !state && !views->port.needed()) {
        views->save_state = 0;
        event(views, ORB_SYSTEM_VIEW_DONE);
        return;
    }
    if (views->save_state != state) {
        if ((state & 1) && !(views->save_state & 1)) views->onboarding_wait_started = lv_tick_get();
        views->save_state = state; event(views, ORB_SYSTEM_VIEW_CHANGED);
    }
}
