#include "orb_screen_private.h"

#include <string.h>

static orb_screen_instance_t *candidate_slot(orb_router_t *router)
{
    return router->current == &router->instances[0] ? &router->instances[1] : &router->instances[0];
}

static bool prepare(orb_router_t *router, orb_screen_id_t id, const orb_screen_args_t *args,
                     orb_screen_instance_t **candidate)
{
    const orb_screen_descriptor_t *descriptor = orb_screen_registry_find(router->registry, id);
    if (!descriptor) return false;
    *candidate = candidate_slot(router);
    if (!orb_screen_create(*candidate, router->host, descriptor, router->parent, args, router->host->config.preview)) return false;
    /* Creation and restoration are silent, and the current screen keeps its
     * lease until candidate admission succeeds. No domain command can leak. */
    orb_screen_visibility(*candidate, ORB_VISIBILITY_COVERED);
    if (router->host->config.project) {
        orb_screen_update_t update = {0};
        if (!router->host->config.project(router->host->config.context, id, &update) ||
            !orb_screen_update(*candidate, &update)) {
            orb_screen_destroy(*candidate);
            return false;
        }
    }
    orb_screen_args_t defaults = {0};
    orb_context_store_restore_route(&router->contexts, *candidate, args ? args : &defaults);
    if (!orb_screen_ui_admitted(orb_screen_data(*candidate))) {
        orb_screen_destroy(*candidate);
        return false;
    }
    return true;
}

static void commit_candidate(orb_router_t *router, orb_screen_instance_t *candidate,
                              const orb_route_entry_t *route)
{
    if (router->current) {
        orb_screen_visibility(router->current, ORB_VISIBILITY_COVERED);
        orb_screen_destroy(router->current);
    }
    router->current = candidate;
    router->route = *route;
    orb_screen_visibility(candidate, ORB_VISIBILITY_ACTIVE);
    if (!router->host->config.defer_rim) orb_screen_rim_acquire(candidate);
}

bool orb_router_init(orb_router_t *router, orb_screen_registry_t *registry, orb_screen_host_t *host,
                     lv_obj_t *parent, orb_screen_id_t initial, const orb_screen_args_t *args)
{
    if (!router || !registry || !host || !host->config.nav || !parent ||
        !orb_screen_registry_find(registry, initial)) return false;
    memset(router, 0, sizeof(*router));
    router->registry = registry;
    router->host = host;
    router->parent = parent;
    orb_context_store_init(&router->contexts);
    orb_nav_t proposed;
    orb_nav_init(&proposed, initial);
    for (size_t i = 0; i < registry->screen_count; i++) {
        const orb_screen_descriptor_t *descriptor = registry->screens[i];
        if (!orb_nav_register(&proposed, (orb_nav_place_t){descriptor->id, descriptor->nav_role, false})) return false;
    }
    orb_screen_instance_t *candidate;
    if (!prepare(router, initial, args, &candidate)) return false;
    *host->config.nav = proposed;
    orb_route_entry_t route = {.id = initial};
    if (args) route.args = *args;
    commit_candidate(router, candidate, &route);
    router->initialized = true;
    return true;
}

void orb_router_destroy(orb_router_t *router)
{
    if (!router) return;
    orb_router_cancel_staged(router);
    orb_screen_destroy(&router->instances[0]);
    orb_screen_destroy(&router->instances[1]);
    orb_context_store_clear(&router->contexts);
    memset(router, 0, sizeof(*router));
}

static void push_history(orb_router_t *router, const orb_launch_token_t *launch)
{
    if (router->history_count == ORB_NAV_BACK_CAPACITY) {
        memmove(router->history, router->history + 1,
                (ORB_NAV_BACK_CAPACITY - 1) * sizeof(router->history[0]));
        router->history_count--;
    }
    orb_route_entry_t entry = router->route;
    entry.launch = launch ? *launch : (orb_launch_token_t){0};
    router->history[router->history_count++] = entry;
}

bool orb_router_open(orb_router_t *router, orb_screen_id_t id, const orb_screen_args_t *args,
                     const orb_launch_token_t *launch, bool committed)
{
    if (!router || !router->initialized || !committed || router->staged) return false;
    orb_screen_args_t defaults = {0};
    if (!args) args = &defaults;
    if (router->route.id == id && router->route.args.entity_id == args->entity_id &&
        router->route.args.page == args->page) return true;
    orb_nav_t proposed = *router->host->config.nav;
    const orb_nav_token_t *token = launch && launch->valid ? &launch->rect : NULL;
    if (proposed.current == id) {
        /* orb_nav identities name screens. Distinct entity/page routes also
         * retain their value arguments in framework history. */
        if (proposed.back_count == ORB_NAV_BACK_CAPACITY) {
            memmove(proposed.back, proposed.back + 1,
                    (ORB_NAV_BACK_CAPACITY - 1) * sizeof(proposed.back[0]));
            proposed.back_count--;
        }
        orb_nav_back_entry_t entry = {.id = id};
        if (token) { entry.token = *token; entry.iris = token->width > 0 && token->height > 0; }
        proposed.back[proposed.back_count++] = entry;
    } else if (!orb_nav_push(&proposed, id, token)) return false;
    orb_screen_instance_t *candidate;
    if (!prepare(router, id, args, &candidate)) return false;
    if (router->current && !orb_context_store_save_route(&router->contexts, router->current, &router->route.args)) {
        orb_screen_destroy(candidate);
        return false;
    }
    size_t previous_count = router->host->config.nav->back_count;
    if (proposed.back_count > previous_count || previous_count == ORB_NAV_BACK_CAPACITY) push_history(router, launch);
    else router->history_count = proposed.back_count;
    *router->host->config.nav = proposed;
    orb_route_entry_t route = {.id = id, .args = *args};
    if (launch) route.launch = *launch;
    commit_candidate(router, candidate, &route);
    orb_screen_feedback(candidate, orb_screen_event(router->host), ORB_CUE_OPEN, ORB_INPUT_TOUCH, true);
    return true;
}

orb_back_result_t orb_router_back(orb_router_t *router, bool committed)
{
    if (!router || !router->initialized || !committed || !router->current || router->staged) return ORB_BACK_CONSUMED;
    orb_back_result_t result = orb_screen_back(router->current);
    if (result == ORB_BACK_CONSUMED) return result;
    orb_nav_t proposed = *router->host->config.nav;
    orb_nav_back_entry_t nav_entry;
    orb_route_entry_t route = {0};
    bool history = orb_nav_back(&proposed, &nav_entry) && router->history_count;
    if (history) route = router->history[router->history_count - 1];
    else {
        const orb_screen_descriptor_t *descriptor = orb_screen_descriptor(router->current);
        route.id = descriptor->parent_id;
        if (route.id == descriptor->id || route.id == ORB_SCREEN_INVALID_ID) return result;
        orb_nav_reset(&proposed, route.id);
    }
    orb_screen_instance_t *candidate;
    if (!prepare(router, route.id, &route.args, &candidate)) return ORB_BACK_CONSUMED;
    if (!orb_context_store_save_route(&router->contexts, router->current, &router->route.args)) {
        orb_screen_destroy(candidate);
        return ORB_BACK_CONSUMED;
    }
    if (history) router->history_count--;
    *router->host->config.nav = proposed;
    commit_candidate(router, candidate, &route);
    orb_screen_feedback(candidate, orb_screen_event(router->host), ORB_CUE_CLOSE, ORB_INPUT_BUTTON, true);
    return result;
}

static bool route_matches(const orb_route_entry_t *route, orb_screen_id_t id,
    const orb_screen_args_t *args)
{
    return route->id == id && route->args.entity_id == args->entity_id && route->args.page == args->page;
}

static const orb_context_entry_t *context_entry(const orb_context_store_t *store,
    orb_screen_id_t id, const orb_screen_args_t *args)
{
    for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
        const orb_context_entry_t *entry = &store->entries[i];
        if (entry->context && entry->id == id && entry->entity_id == args->entity_id &&
            entry->page == args->page) return entry;
    }
    return NULL;
}

static bool source_can_close(const orb_router_t *router)
{
    if (!orb_screen_alive(router->current)) return true;
    orb_screen_data_t *source = orb_screen_data(router->current);
    if (!source->context) return false;
    if (context_entry(&router->contexts, source->descriptor->id, &router->route.args)) return true;
    for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++)
        if (!router->contexts.entries[i].context) return true;
    return false;
}

bool orb_router_can_depart(orb_router_t *router)
{
    if (!router) return false;
    if (!orb_screen_alive(router->current)) return true;
    if (!source_can_close(router)) return false;
    orb_screen_data_t *source = orb_screen_data(router->current);
    return orb_screen_save(router->current, source->context, source->descriptor->context_bytes);
}

static bool stage_update(orb_router_t *router, orb_screen_instance_t *candidate)
{
    if (!router->host->config.project) return true;
    orb_screen_update_t update = {0};
    return router->host->config.project(router->host->config.context,
        orb_screen_descriptor(candidate)->id, &update) && orb_screen_update(candidate, &update);
}

bool orb_router_stage(orb_router_t *router, orb_screen_registry_t *registry,
    orb_screen_host_t *host, lv_obj_t *parent, orb_screen_id_t id,
    const orb_screen_args_t *args, bool restore_context)
{
    if (!router || !registry || !host || !host->config.nav || !parent ||
        !orb_screen_registry_find(registry, id)) return false;
    if (router->registry && (router->registry != registry || router->host != host)) return false;
    router->registry = registry;
    router->host = host;
    orb_router_cancel_staged(router);
    if (!orb_router_can_depart(router)) return false;
    orb_screen_args_t defaults = {0};
    if (!args) args = &defaults;
    orb_screen_data_t *source = orb_screen_data(router->current);
    bool same = restore_context && source && route_matches(&router->route, id, args);
    /* can_depart already refreshed the source's reserved scratch record.
     * Preparing a replacement needs no extra context allocation or save. */
    orb_screen_instance_t *candidate = candidate_slot(router);
    const orb_screen_descriptor_t *descriptor = orb_screen_registry_find(registry, id);
    if (!orb_screen_create(candidate, host, descriptor, parent, args, host->config.preview)) return false;
    orb_screen_visibility(candidate, ORB_VISIBILITY_COVERED);
    bool ready = stage_update(router, candidate);
    if (ready && same)
        ready = orb_screen_restore(candidate, source->context, source->descriptor->context_bytes);
    else if (ready && restore_context) {
        const orb_context_entry_t *entry = context_entry(&router->contexts, id, args);
        if (entry) ready = orb_screen_restore(candidate, entry->context, orb_memory_size(entry->context));
    }
    if (!ready || !orb_screen_ui_admitted(orb_screen_data(candidate))) {
        orb_screen_destroy(candidate);
        return false;
    }
    router->staged = candidate;
    router->staged_parent = parent;
    router->staged_route = (orb_route_entry_t){.id = id, .args = *args};
    return true;
}

static bool stage_navigation(const orb_router_t *router, orb_nav_t *nav)
{
    orb_nav_init(nav, router->staged_route.id);
    for (size_t i = 0; i < router->registry->screen_count; i++) {
        const orb_screen_descriptor_t *descriptor = router->registry->screens[i];
        if (!orb_nav_register(nav, (orb_nav_place_t){descriptor->id, descriptor->nav_role, false})) return false;
    }
    return true;
}

static bool stage_reparent(orb_screen_instance_t *candidate, lv_obj_t *parent)
{
    orb_screen_data_t *data = orb_screen_data(candidate);
    if (!data || !data->root) return false;
    lv_obj_set_parent(data->root, parent);
    if (lv_obj_get_parent(data->root) != parent) return false;
    if (data->title) {
        lv_obj_set_parent(data->title, parent);
        if (lv_obj_get_parent(data->title) != parent) return false;
    }
    return true;
}

bool orb_router_commit_staged(orb_router_t *router, lv_obj_t *parent)
{
    if (!router || !parent || !orb_screen_alive(router->staged) || !source_can_close(router)) return false;
    orb_nav_t proposed;
    if (!stage_navigation(router, &proposed)) return false;
    orb_screen_instance_t *candidate = router->staged;
    lv_obj_t *provisional = router->staged_parent;
    /* Reparent while the candidate is covered. Even an LVGL child-list
     * admission failure leaves the source's complete presentation alive. */
    if (!stage_reparent(candidate, parent)) {
        (void)stage_reparent(candidate, provisional);
        return false;
    }
    if (orb_screen_alive(router->current) &&
        !orb_context_store_close_route(&router->contexts, router->current, &router->route.args)) {
        (void)stage_reparent(candidate, provisional);
        return false;
    }
    router->current = candidate;
    router->route = router->staged_route;
    router->parent = parent;
    router->history_count = 0;
    *router->host->config.nav = proposed;
    router->staged = NULL;
    router->staged_parent = NULL;
    router->staged_route = (orb_route_entry_t){0};
    router->initialized = true;
    orb_screen_visibility(candidate, ORB_VISIBILITY_ACTIVE);
    if (!router->host->config.defer_rim) orb_screen_rim_acquire(candidate);
    return true;
}

void orb_router_cancel_staged(orb_router_t *router)
{
    if (!router || !router->staged) return;
    orb_screen_instance_t *candidate = router->staged;
    router->staged = NULL;
    router->staged_parent = NULL;
    router->staged_route = (orb_route_entry_t){0};
    orb_screen_destroy(candidate);
}

orb_screen_instance_t *orb_router_staged(orb_router_t *router)
{
    return router && orb_screen_alive(router->staged) ? router->staged : NULL;
}
