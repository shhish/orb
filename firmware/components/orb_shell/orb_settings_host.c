#include "orb_settings_host.h"
#include "orb_app_settings_registry.h"
#include "orb_app_weather_registry.h"
#include "orb_app_settings.h"
#include "orb_app_settings_units.h"
#include "orb_app_settings_root.h"
#include "orb_app_settings_sound.h"
#include "orb_app_settings_display.h"
#include "orb_app_settings_battery.h"
#include "orb_app_settings_accessibility.h"
#include "orb_fonts.h"
#include "orb_app_settings_sources.h"
#include "orb_sound.h"
#include "orb_ui.h"
#include "orb_page.h"
#include "orb_kit.h"
#include "orb_kit_motion.h"

#include <stdio.h>
#include <string.h>

#define SETTINGS_MODEL_CAPACITY 1024

struct orb_settings_host {
    orb_screen_registry_t registry;
    orb_screen_host_t host;
    orb_nav_t nav;
    orb_router_t router;
    orb_context_store_t saved;
    orb_route_entry_t route, last_routes[ORB_SCREEN_LIMIT];
    orb_intent_t pending, frame_setting;
    bool frame_pending;
    _Alignas(orb_screen_context_t) unsigned char group_context[512];
    size_t group_size;
    orb_screen_id_t group_screen;
    orb_settings_host_changed_t changed;
    orb_settings_host_accept_t accept;
    void *context;
    unsigned char projection[SETTINGS_MODEL_CAPACITY];
    size_t projection_size;
    orb_screen_model_t model_view;
    uint64_t revision, hub_pref_operation;
    uint32_t hub_pref_key;
    lv_obj_t *provisional;
    bool preview, has_pending, prepared_fresh, snapshotting;
    bool snapshot_root_hidden, snapshot_title_hidden;
    bool local_arrival_pending, local_arrival_fresh;
};

static void discard_destination(orb_context_store_t *store, const orb_intent_t *pending);
static void cancel_canvas(orb_settings_host_t *host);

static orb_route_entry_t *saved_route(orb_settings_host_t *host, orb_screen_id_t screen)
{
    for (size_t i = 0; i < host->registry.screen_count; i++)
        if (host->registry.screens[i]->id == screen) return &host->last_routes[i];
    return NULL;
}

static bool hub_preference(const orb_intent_t *value)
{
    return value->kind == ORB_INTENT_SETTING_SET &&
        (value->value.setting.key == ORB_SETTINGS_SETTING_TEMPERATURE ||
         (value->value.setting.key >= ORB_SETTING_SOURCE_CLAUDE &&
          value->value.setting.key <= ORB_SETTING_SOURCE_RECENT_MINUTES));
}

static bool intent(void *context, const orb_intent_t *value)
{
    orb_settings_host_t *host = context;
    if (host->preview) return false;
    const orb_screen_descriptor_t *current = orb_screen_descriptor(host->router.current);
    if (value->kind != ORB_INTENT_NAVIGATE || !current || value->value.navigate.screen != current->id) {
        bool accepted = host->accept && host->accept(host->context, value);
        if (accepted && hub_preference(value)) {
            host->hub_pref_operation++;
            host->hub_pref_key = value->value.setting.key;
        }
        return accepted;
    }
    if (host->has_pending || (host->accept && !host->accept(host->context, value))) return false;
    /* Local navigation changes only value arguments here. The renderer is
     * recreated after dispatch; cross-screen routing stays at the outer host. */
    host->pending = *value;
    host->has_pending = true;
    orb_route_entry_t *route = saved_route(host, current->id);
    if (route) *route = (orb_route_entry_t){.id = current->id, .args = value->value.navigate.args};
    if (value->origin == ORB_INPUT_TOUCH) discard_destination(&host->router.contexts, value);
    if (host->changed) host->changed(host->context);
    return true;
}

static bool allow_synthetic(void *context, const orb_intent_t *value)
{
    orb_settings_host_t *host = context;
    const orb_screen_descriptor_t *current = orb_screen_descriptor(host->router.current);
    if (!current || (value->origin != ORB_INPUT_TOUCH && value->origin != ORB_INPUT_BUTTON)) return false;
    if (current->id == ORB_SETTINGS_SOURCES_SCREEN_ID)
        return hub_preference(value) && value->value.setting.key != ORB_SETTINGS_SETTING_TEMPERATURE;
    if (current->id == ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID)
        return value->kind == ORB_INTENT_SETTING_SET && value->value.setting.key >= ORB_SETTING_ACCESS_TEXT_SIZE &&
            value->value.setting.key <= ORB_SETTING_ACCESS_CONTRAST;
    if (current->id == ORB_SETTINGS_DISPLAY_SCREEN_ID)
        return value->kind == ORB_INTENT_SETTING_SET && value->value.setting.key >= ORB_SETTING_DISPLAY_BRIGHTNESS &&
            value->value.setting.key <= ORB_SETTING_DISPLAY_QUIET;
    if (current->id == ORB_SETTINGS_SOUND_SCREEN_ID)
        return (value->kind == ORB_INTENT_SETTING_SET && value->value.setting.key >= ORB_SETTING_SOUND_VOLUME &&
            value->value.setting.key <= ORB_SETTING_SOUND_QUIET_END) ||
            (value->kind == ORB_INTENT_APP_COMMAND && value->value.command.target == ORB_SETTINGS_SOUND_SCREEN_ID &&
             value->value.command.action == ORB_SETTINGS_SOUND_COMMAND_PREVIEW);
    if (current->id == ORB_SETTINGS_ROOT_SCREEN_ID)
        return (value->kind == ORB_INTENT_SETTING_SET && value->value.setting.key == ORB_SETTINGS_SETTING_GLASS) ||
            (value->kind == ORB_INTENT_APP_COMMAND && value->value.command.target == 0 &&
             value->value.command.action == ORB_SETTINGS_COMMAND_CONNECTION);
    return current->id == ORB_SETTINGS_TEMPERATURE_SCREEN_ID && value->kind == ORB_INTENT_SETTING_SET &&
        value->value.setting.key == ORB_SETTINGS_SETTING_TEMPERATURE;
}

static void synthetic_refused(void *context, const orb_intent_t *value)
{
    orb_settings_host_t *host = context;
    const orb_screen_descriptor_t *current = orb_screen_descriptor(host->router.current);
    if (current && current->id == ORB_SETTINGS_BATTERY_SCREEN_ID &&
        value->kind == ORB_INTENT_SETTING_SET && value->value.setting.key == ORB_SETTINGS_KEY_DESK_LIMIT && host->accept)
        (void)host->accept(host->context, value);
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    orb_settings_host_t *host = context;
    (void)id;
    host->model_view = (orb_screen_model_t){host->projection, host->projection_size};
    *update = (orb_screen_update_t){.revisions.device = host->revision,
        .revisions.model = host->revision, .revisions.settings = host->revision,
        .model = &host->model_view};
    return true;
}

orb_settings_host_t *orb_settings_host_create(bool preview,
    orb_settings_host_changed_t changed, orb_settings_host_accept_t accept, void *context)
{
    orb_settings_host_t *host = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*host));
    if (!host) return NULL;
    host->preview = preview;
    host->changed = changed;
    host->accept = accept;
    host->context = context;
    orb_screen_registry_init(&host->registry);
    /* Weather shares this compatibility host: one admission, chrome and
     * context path for every migrated descriptor the shell still frames. */
    if (!orb_screen_registry_add(&host->registry, orb_app_settings_manifest(), NULL) ||
        !orb_screen_registry_add(&host->registry, orb_app_weather_manifest(), NULL)) {
        orb_memory_free(host);
        return NULL;
    }
    orb_screen_host_init(&host->host, &(orb_screen_host_config_t){
        .nav = &host->nav, .intent = intent, .project = project,
        .context = host, .defer_rim = true, .preview = preview, .allow_synthetic = allow_synthetic, .synthetic_refused = synthetic_refused,
    });
    return host;
}

bool orb_settings_host_can_depart(orb_settings_host_t *host)
{
    return !host || (!host->snapshotting && orb_router_can_depart(&host->router) &&
        orb_settings_host_group_capture(host, ORB_SETTINGS_SOUND_SCREEN_ID));
}

bool orb_settings_host_detach(orb_settings_host_t *host)
{
    if (!host) return true;
    if (!orb_settings_host_group_capture(host, ORB_SETTINGS_SOUND_SCREEN_ID)) return false;
    /* Ordinary departure is separate from a staged commit. Closing a specific
     * old page uses detach_page so it cannot tear down the new candidate. */
    cancel_canvas(host);
    if (!host->router.registry) return true;
    if (orb_screen_alive(host->router.current) &&
        !orb_context_store_close_route(&host->router.contexts, host->router.current, &host->router.route.args)) return false;
    if (host->router.initialized) host->route = host->router.route;
    if (host->has_pending && host->pending.value.navigate.screen == host->route.id) {
        host->route.args = host->pending.value.navigate.args;
        host->has_pending = false;
    }
    orb_route_entry_t *route = saved_route(host, host->route.id);
    if (route) *route = host->route;
    orb_context_store_clear(&host->saved);
    host->saved = host->router.contexts;
    orb_context_store_init(&host->router.contexts);
    orb_router_destroy(&host->router);
    return true;
}

void orb_settings_host_destroy(orb_settings_host_t *host)
{
    if (!host) return;
    orb_settings_host_detach(host);
    orb_router_destroy(&host->router);
    orb_context_store_clear(&host->saved);
    orb_memory_free(host);
}

static void discard_destination(orb_context_store_t *store, const orb_intent_t *pending)
{
    for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
        orb_context_entry_t *entry = &store->entries[i];
        if (entry->context && entry->id == pending->value.navigate.screen &&
            entry->entity_id == pending->value.navigate.args.entity_id &&
            entry->page == pending->value.navigate.args.page) {
            orb_memory_free(entry->context);
            memset(entry, 0, sizeof(*entry));
        }
    }
}

uint32_t orb_settings_host_page_for(const orb_settings_host_t *host, orb_screen_id_t screen)
{
    if (!host) return 0;
    if (host->has_pending && host->pending.value.navigate.screen == screen)
        return host->pending.value.navigate.args.page;
    if (host->route.id == screen) return host->route.args.page;
    for (size_t i = 0; i < host->registry.screen_count; i++)
        if (host->last_routes[i].id == screen) return host->last_routes[i].args.page;
    return 0;
}

bool orb_settings_host_back_for(orb_settings_host_t *host, orb_screen_id_t screen)
{
    uint32_t page = orb_settings_host_page_for(host, screen);
    if (!page) return false;
    uint32_t parent = screen == ORB_SETTINGS_SOUND_SCREEN_ID &&
        (page == ORB_SETTINGS_SOUND_START_PAGE || page == ORB_SETTINGS_SOUND_END_PAGE) ? ORB_SETTINGS_SOUND_QUIET_PAGE : 0;
    host->pending = (orb_intent_t){.kind = ORB_INTENT_NAVIGATE, .origin = ORB_INPUT_BUTTON,
        .value.navigate = {.screen = screen, .args.page = parent}};
    host->has_pending = true;
    orb_route_entry_t *route = saved_route(host, screen);
    if (route) *route = (orb_route_entry_t){.id = screen, .args.page = parent};
    if (host->changed) host->changed(host->context);
    return true;
}

lv_obj_t *orb_settings_host_list(const orb_settings_host_t *host)
{
    return host ? orb_screen_page_viewport(host->router.current) : NULL;
}

lv_obj_t *orb_settings_host_title(const orb_settings_host_t *host)
{
    return host ? orb_screen_page_title(host->router.current) : NULL;
}

bool orb_settings_host_copy(orb_settings_host_t *destination, orb_settings_host_t *source)
{
    if (!destination || !source || !orb_settings_host_group_capture(source, ORB_SETTINGS_SOUND_SCREEN_ID)) return false;
    if (source->router.current && orb_screen_alive(source->router.current) &&
        !orb_context_store_save_route(&source->router.contexts, source->router.current, &source->router.route.args)) return false;
    const orb_context_store_t *store = source->router.registry ? &source->router.contexts : &source->saved;
    orb_context_store_t copy = {0};
    if (!orb_context_store_copy(&copy, store)) return false;
    if (!orb_settings_host_detach(destination)) { orb_context_store_clear(&copy); return false; }
    orb_context_store_clear(&destination->saved);
    destination->saved = copy;
    destination->route = source->router.initialized ? source->router.route : source->route;
    memcpy(destination->last_routes, source->last_routes, sizeof(source->last_routes));
    destination->hub_pref_operation = source->hub_pref_operation;
    destination->hub_pref_key = source->hub_pref_key;
    memcpy(destination->group_context, source->group_context, sizeof(source->group_context));
    destination->group_size = source->group_size;
    destination->group_screen = source->group_screen;
    orb_route_entry_t *route = saved_route(destination, destination->route.id);
    if (source->has_pending) destination->route.args = source->pending.value.navigate.args;
    if (route) *route = destination->route;
    return true;
}

bool orb_settings_host_clone(orb_settings_host_t **destination, orb_settings_host_t *source, bool preview)
{
    if (!source) {
        orb_settings_host_destroy(*destination);
        *destination = NULL;
        return true;
    }
    if (!*destination) *destination = orb_settings_host_create(preview, NULL, NULL, NULL);
    return *destination && orb_settings_host_copy(*destination, source);
}

bool orb_settings_host_restore(orb_settings_host_t **destination, orb_settings_host_t **saved)
{
    if (!orb_settings_host_clone(destination, *saved, false)) return false;
    orb_settings_host_destroy(*saved);
    *saved = NULL;
    return true;
}

orb_screen_instance_t *orb_settings_host_current(orb_settings_host_t *host)
{
    return host ? host->router.current : NULL;
}

static bool args_equal(const orb_screen_args_t *a, const orb_screen_args_t *b)
{
    return a->entity_id == b->entity_id && a->page == b->page;
}

static void provisional_deleted(lv_event_t *event)
{
    orb_settings_host_t *host = lv_event_get_user_data(event);
    if (host->provisional == lv_event_get_current_target_obj(event)) host->provisional = NULL;
    host->snapshotting = false;
}

static lv_obj_t *provisional_create(orb_settings_host_t *host, lv_obj_t *parent)
{
    lv_obj_t *canvas = lv_obj_create(parent);
    if (!canvas) return NULL;
    lv_obj_set_hidden(canvas, true);
    lv_obj_remove_style_all(canvas);
    lv_obj_set_size(canvas, 466, 466);
    lv_obj_set_scrollable(canvas, false);
    lv_obj_set_clickable(canvas, false);
    lv_obj_set_event_bubble(canvas, false);
    lv_obj_set_gesture_bubble(canvas, false);
    lv_obj_set_style_bg_color(canvas, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(canvas, provisional_deleted, LV_EVENT_DELETE, host);
    return canvas;
}

static void cancel_canvas(orb_settings_host_t *host)
{
    if (!host) return;
    if (host->provisional) lv_obj_set_hidden(host->provisional, true);
    host->snapshotting = false;
    orb_router_cancel_staged(&host->router);
    if (host->provisional) lv_obj_delete(host->provisional);
    host->prepared_fresh = false;
}

void orb_settings_host_cancel_staged(orb_settings_host_t *host)
{
    if (!host) return;
    cancel_canvas(host);
    host->has_pending = false;
    if (orb_screen_alive(host->router.current)) {
        host->route = host->router.route;
        orb_route_entry_t *route = saved_route(host, host->route.id);
        if (route) *route = host->route;
    }
}

static bool stage_chrome(orb_settings_host_t *host)
{
    orb_screen_instance_t *candidate = orb_router_staged(&host->router);
    if (!candidate || !host->provisional) return false;
    lv_obj_t *title = orb_screen_page_title(candidate);
    if (title) {
        lv_obj_set_parent(title, host->provisional);
        if (lv_obj_get_parent(title) != host->provisional) return false;
    }
    return true;
}

bool orb_settings_host_stage(orb_settings_host_t *host, lv_obj_t *parent,
    orb_screen_id_t screen, const orb_screen_model_t *model)
{
    if (!host || !parent || !model || (!model->data && model->size) ||
        model->size > sizeof(host->projection) || !orb_screen_registry_find(&host->registry, screen)) return false;
    if (host->snapshotting || !orb_settings_host_group_capture(host, ORB_SETTINGS_SOUND_SCREEN_ID)) return false;
    const orb_route_entry_t *previous = saved_route(host, screen);
    orb_screen_args_t args = previous && previous->id == screen ? previous->args : (orb_screen_args_t){0};
    if (host->has_pending && host->pending.value.navigate.screen == screen)
        args = host->pending.value.navigate.args;
    if (model->size) memcpy(host->projection, model->data, model->size);
    host->projection_size = model->size;
    host->revision++;
    orb_screen_instance_t *candidate = orb_router_staged(&host->router);
    if (candidate && host->router.staged_route.id == screen && args_equal(&host->router.staged_route.args, &args)) {
        orb_screen_update_t update = {0};
        if (project(host, screen, &update) && orb_screen_update(candidate, &update) && stage_chrome(host)) return true;
        orb_settings_host_cancel_staged(host);
        return false;
    }
    cancel_canvas(host);
    if (!host->router.registry) {
        /* Move, never clone, the existing opaque contexts into this router. */
        host->router.registry = &host->registry;
        host->router.host = &host->host;
        host->router.contexts = host->saved;
        orb_context_store_init(&host->saved);
    }
    host->provisional = provisional_create(host, parent);
    if (!host->provisional) {
        orb_settings_host_cancel_staged(host);
        return false;
    }
    host->prepared_fresh = host->has_pending && host->pending.origin == ORB_INPUT_TOUCH &&
        host->pending.value.navigate.screen == screen;
    if (!orb_router_stage(&host->router, &host->registry, &host->host, host->provisional,
        screen, &args, !host->prepared_fresh) || !stage_chrome(host)) {
        orb_settings_host_cancel_staged(host);
        return false;
    }
    return true;
}

bool orb_settings_host_commit_staged(orb_settings_host_t *host, lv_obj_t *parent)
{
    if (!host || !parent || host->snapshotting || !orb_router_staged(&host->router)) return false;
    orb_route_entry_t target = host->router.staged_route;
    if (host->has_pending && host->pending.value.navigate.screen == target.id &&
        !args_equal(&host->pending.value.navigate.args, &target.args)) return false;
    orb_route_entry_t source = host->router.route;
    bool had_source = orb_screen_alive(host->router.current);
    orb_route_entry_t remembered_source = source;
    if (had_source && host->has_pending && host->pending.value.navigate.screen == source.id)
        remembered_source.args = host->pending.value.navigate.args;
    if (!orb_router_commit_staged(&host->router, parent)) return false;
    if (had_source) {
        orb_route_entry_t *previous = saved_route(host, source.id);
        /* A local Back/entry may be queued when another outer route commits.
         * Store its logical args, while the actual source context was saved
         * under the actual instance route by the router above. */
        if (previous) *previous = remembered_source;
    }
    if (host->has_pending && host->pending.value.navigate.screen != target.id &&
        (!had_source || host->pending.value.navigate.screen != source.id)) {
        orb_route_entry_t *queued = saved_route(host, host->pending.value.navigate.screen);
        if (queued) {
            queued->id = host->pending.value.navigate.screen;
            queued->args = host->pending.value.navigate.args;
        }
    }
    host->route = host->router.route;
    orb_route_entry_t *route = saved_route(host, host->route.id);
    if (route) *route = host->route;
    if (host->prepared_fresh) {
        orb_intent_t fresh = {.kind = ORB_INTENT_NAVIGATE,
            .value.navigate = {.screen = target.id, .args = target.args}};
        discard_destination(&host->router.contexts, &fresh);
    }
    host->local_arrival_pending = host->has_pending && host->pending.value.navigate.screen == target.id;
    host->local_arrival_fresh = host->prepared_fresh;
    /* All pending local args are now reflected in route storage. Keeping an
     * old-source pending record would reject the new screen's next local tap. */
    host->has_pending = false;
    if (host->provisional) lv_obj_delete(host->provisional);
    host->prepared_fresh = false;
    return true;
}

bool orb_settings_host_arrive_local(orb_settings_host_t *host)
{
    if (!host || !host->local_arrival_pending) return false;
    host->local_arrival_pending = false;
    orb_kit_arrival_complete(orb_screen_root(host->router.current), host->local_arrival_fresh);
    return true;
}

orb_screen_instance_t *orb_settings_host_staged(orb_settings_host_t *host)
{
    return host ? orb_router_staged(&host->router) : NULL;
}

bool orb_settings_host_detach_page(orb_settings_host_t *host, lv_obj_t *page)
{
    if (!host || !orb_screen_alive(host->router.current)) return true;
    lv_obj_t *root = orb_screen_root(host->router.current);
    for (lv_obj_t *ancestor = root; ancestor; ancestor = lv_obj_get_parent(ancestor))
        if (ancestor == page) return orb_settings_host_detach(host);
    return true;
}

lv_obj_t *orb_settings_host_snapshot_begin(orb_settings_host_t *host)
{
    if (!host || host->snapshotting || !host->provisional || !orb_router_staged(&host->router)) return NULL;
    orb_screen_instance_t *candidate = host->router.staged;
    lv_obj_t *root = orb_screen_root(candidate);
    lv_obj_t *title = orb_screen_page_title(candidate);
    /* A Back/restore snapshot must freeze final values just like its live
     * destination. Only a fresh local choice captures the prepared minimum. */
    if (!host->prepared_fresh) orb_kit_arrival_complete(root, false);
    host->snapshot_root_hidden = lv_obj_is_hidden(root);
    host->snapshot_title_hidden = title && lv_obj_is_hidden(title);
    host->snapshotting = true;
    /* Drawing is synchronous under the UI lock. Framework visibility remains
     * COVERED/SUSPENDED, so even a direct synthetic action stays refused. */
    lv_obj_set_hidden(root, false);
    if (title) lv_obj_set_hidden(title, false);
    lv_obj_set_hidden(host->provisional, false);
    lv_obj_t *viewport = orb_screen_page_viewport(candidate);
    if (viewport) {
        lv_obj_update_layout(host->provisional);
        orb_kit_list_refresh(viewport);
    }
    return host->provisional;
}

void orb_settings_host_snapshot_end(orb_settings_host_t *host)
{
    if (!host || !host->snapshotting) return;
    if (host->provisional) lv_obj_set_hidden(host->provisional, true);
    orb_screen_instance_t *candidate = orb_router_staged(&host->router);
    if (candidate) {
        lv_obj_t *title = orb_screen_page_title(candidate);
        lv_obj_set_hidden(orb_screen_root(candidate), host->snapshot_root_hidden);
        if (title) lv_obj_set_hidden(title, host->snapshot_title_hidden);
    }
    host->snapshotting = false;
}

bool orb_settings_host_stage_sources(orb_settings_host_t *host, lv_obj_t *parent,
    const orb_model_snapshot_t *model, const char *notice)
{
    if (!host || !model || !notice) return false;
    orb_settings_sources_model_t projection = {.idle_minutes = model->prefs.idle_minutes,
        .pending = model->prefs_pending, .ready = model->online && model->synced, .interactive = true,
        .known = model->prefs_known || (model->online && model->synced),
        .hub_pref_operation = host->hub_pref_operation, .hub_pref_key = host->hub_pref_key};
    memcpy(projection.sources, model->prefs.sources, sizeof(projection.sources));
    snprintf(projection.error, sizeof(projection.error), "%s", model->prefs_error);
    snprintf(projection.notice, sizeof(projection.notice), "%s", notice);
    return orb_settings_host_stage(host, parent, ORB_SETTINGS_SOURCES_SCREEN_ID,
        &(orb_screen_model_t){&projection, sizeof(projection)});
}

bool orb_settings_host_update(orb_settings_host_t *host, orb_screen_id_t screen, const orb_screen_model_t *model)
{
    if (!host || !orb_screen_alive(host->router.current) || orb_screen_descriptor(host->router.current)->id != screen) return true;
    if (!model || !model->data || model->size > sizeof(host->projection) || host->snapshotting) return false;
    memcpy(host->projection, model->data, model->size);
    host->projection_size = model->size;
    host->revision++;
    orb_screen_update_t update = {0};
    return project(host, screen, &update) && orb_screen_update(host->router.current, &update);
}

void orb_settings_host_setting_queue(orb_settings_host_t *host, uint32_t key, int32_t value)
{
    if (!host) return;
    host->frame_setting = (orb_intent_t){.kind = ORB_INTENT_SETTING_SET, .value.setting = {.key = key, .value = value}};
    host->frame_pending = true;
}

bool orb_settings_host_setting_pending(const orb_settings_host_t *host, uint32_t key)
{
    return host && host->frame_pending && host->frame_setting.value.setting.key == key;
}

bool orb_settings_host_setting_take(orb_settings_host_t *host, uint32_t key, int32_t *value)
{
    if (!value || !orb_settings_host_setting_pending(host, key)) return false;
    *value = host->frame_setting.value.setting.value;
    host->frame_pending = false;
    return true;
}

lv_obj_t *orb_settings_host_display_level(orb_settings_host_t *host)
{
    orb_screen_instance_t *current = orb_settings_host_current(host);
    return orb_screen_alive(current) && orb_screen_descriptor(current)->id == ORB_SETTINGS_DISPLAY_SCREEN_ID ?
        orb_app_settings_display_level_control(current) : NULL;
}

bool orb_settings_host_display_twist(orb_settings_host_t *host, int detents)
{
    if (!orb_settings_host_display_level(host)) return false;
    return orb_app_settings_display_set_level(orb_settings_host_current(host),
        orb_app_settings_display_level(orb_settings_host_current(host)) + detents, ORB_INPUT_CROWN, true);
}

bool orb_settings_host_group_capture(orb_settings_host_t *host, orb_screen_id_t screen)
{
    if (!host) return true;
    orb_screen_instance_t *current = orb_settings_host_current(host);
    if (!orb_screen_alive(current) || orb_screen_descriptor(current)->id != screen) return true;
    size_t size = orb_screen_descriptor(current)->context_bytes;
    if (size > sizeof(host->group_context) || !orb_screen_save(current,
        (orb_screen_context_t *)host->group_context, sizeof(host->group_context))) return false;
    host->group_size = size;
    host->group_screen = screen;
    return true;
}

const orb_screen_context_t *orb_settings_host_group_context(const orb_settings_host_t *host,
    orb_screen_id_t screen, size_t *size)
{
    if (!host || host->group_screen != screen || !host->group_size || !size) return NULL;
    *size = host->group_size;
    return (const orb_screen_context_t *)host->group_context;
}

lv_obj_t *orb_settings_host_sound_level(orb_settings_host_t *host)
{
    orb_screen_instance_t *current = orb_settings_host_current(host);
    return orb_screen_alive(current) && orb_screen_descriptor(current)->id == ORB_SETTINGS_SOUND_SCREEN_ID ?
        orb_app_settings_sound_level_control(current) : NULL;
}

void orb_settings_host_sound_crown(orb_settings_host_t *host, int32_t value)
{
    orb_screen_instance_t *current = orb_settings_host_current(host);
    if (!orb_screen_alive(current) || orb_screen_descriptor(current)->id != ORB_SETTINGS_SOUND_SCREEN_ID) return;
    if (orb_app_settings_sound_page(current) == ORB_SETTINGS_SOUND_VOLUME_PAGE)
        (void)orb_app_settings_sound_set_volume(current, value, ORB_INPUT_CROWN, true);
    else (void)orb_app_settings_sound_set_hour(current, value, ORB_INPUT_CROWN, true);
}
