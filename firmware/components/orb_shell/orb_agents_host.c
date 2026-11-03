#include "orb_agents_host.h"
#include "orb_app_agents_registry.h"

#include <string.h>

struct orb_agents_host {
    orb_agents_host_config_t config;
    orb_screen_registry_t registry;
    orb_screen_host_t host;
    /* The shell keeps the real navigation graph and its rotary trace policy. */
    orb_nav_t nav;
    orb_router_t router;
    orb_context_store_t saved;
    orb_app_agents_model_t projection;
    orb_screen_model_t model_view;
    orb_ui_environment_t environment;
    uint64_t revision;
};

static bool intent(void *context, const orb_intent_t *value)
{
    orb_agents_host_t *host = context;
    return !host->host.config.preview && host->config.accept &&
        host->config.accept(host->config.context, value);
}

static bool allow_synthetic(void *context, const orb_intent_t *value)
{
    (void)context;
    return orb_app_agents_allow_synthetic(value);
}

static void synthetic_refused(void *context, const orb_intent_t *value)
{
    orb_agents_host_t *host = context;
    if (!host->host.config.preview && host->config.synthetic_refused)
        host->config.synthetic_refused(host->config.context, value);
}

static void release_projection(orb_agents_host_t *host)
{
    /* Descriptors only inspect the borrowed fields during their callback. The
     * next update/input obtains the workspace's current COW generation anew. */
    memset(&host->projection, 0, sizeof(host->projection));
    host->model_view = (orb_screen_model_t){0};
}

static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    orb_agents_host_t *host = context;
    release_projection(host);
    if (!orb_app_agents_screen(screen) ||
        !host->config.project(host->config.context, screen, &host->projection)) return false;
    host->model_view = (orb_screen_model_t){&host->projection, sizeof(host->projection)};
    *update = (orb_screen_update_t){.revisions.model = host->revision,
        .revisions.time = host->revision, .revisions.device = host->revision,
        .model = &host->model_view, .environment = &host->environment};
    return true;
}

orb_agents_host_t *orb_agents_host_create(bool preview, const orb_agents_host_config_t *config)
{
    if (!config || !config->project) return NULL;
    orb_agents_host_t *host = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*host));
    if (!host) return NULL;
    host->config = *config;
    orb_screen_registry_init(&host->registry);
    if (!orb_screen_registry_add(&host->registry, orb_app_agents_manifest(), NULL)) {
        orb_memory_free(host);
        return NULL;
    }
    orb_screen_host_init(&host->host, &(orb_screen_host_config_t){
        .nav = &host->nav, .project = project, .intent = intent, .context = host,
        .preview = preview, .defer_rim = true, .allow_synthetic = allow_synthetic,
        .synthetic_refused = synthetic_refused});
    return host;
}

static bool detach(orb_agents_host_t *host)
{
    if (!host->router.registry) return true;
    orb_router_cancel_staged(&host->router);
    /* Closing transfers the already reserved context without an allocation.
     * A failed save leaves the live source available to the caller. */
    if (orb_screen_alive(host->router.current) &&
        !orb_context_store_close_route(&host->router.contexts, host->router.current,
            &host->router.route.args)) {
        release_projection(host);
        return false;
    }
    orb_context_store_clear(&host->saved);
    host->saved = host->router.contexts;
    orb_context_store_init(&host->router.contexts);
    orb_router_destroy(&host->router);
    release_projection(host);
    return true;
}

void orb_agents_host_destroy(orb_agents_host_t *host)
{
    if (!host) return;
    /* Final destruction needs no saved return record and cannot be refused. */
    orb_router_destroy(&host->router);
    orb_context_store_clear(&host->saved);
    orb_memory_free(host);
}

orb_screen_instance_t *orb_agents_host_current(orb_agents_host_t *host)
{
    return host && orb_screen_alive(host->router.current) ? host->router.current : NULL;
}

bool orb_agents_host_attach(orb_agents_host_t *host, lv_obj_t *page, orb_screen_id_t screen)
{
    if (!host || !page || !orb_app_agents_screen(screen)) return false;
    if (!host->router.registry) {
        host->router.registry = &host->registry;
        host->router.host = &host->host;
        host->router.contexts = host->saved;
        orb_context_store_init(&host->saved);
    }
    host->revision++;
    host->environment.revision++;
    /* Stage is synchronous and covered: failure releases only the candidate,
     * leaving the current instance and its opaque return contexts untouched. */
    bool attached = orb_router_stage(&host->router, &host->registry, &host->host,
        page, screen, &(orb_screen_args_t){0}, true) &&
        orb_router_commit_staged(&host->router, page);
    if (!attached) orb_router_cancel_staged(&host->router);
    release_projection(host);
    return attached;
}

bool orb_agents_host_detach_page(orb_agents_host_t *host, lv_obj_t *page)
{
    if (!host || !orb_agents_host_current(host)) return true;
    for (lv_obj_t *ancestor = orb_screen_root(host->router.current); ancestor; ancestor = lv_obj_get_parent(ancestor))
        if (ancestor == page) return detach(host);
    return true;
}

bool orb_agents_host_refresh(orb_agents_host_t *host)
{
    orb_screen_instance_t *current = orb_agents_host_current(host);
    if (!current) return true;
    host->revision++;
    host->environment.revision++;
    orb_screen_update_t update = {0};
    bool refreshed = project(host, orb_screen_descriptor(current)->id, &update) &&
        orb_screen_update(current, &update) && orb_app_agents_refreshable(current);
    release_projection(host);
    return refreshed;
}

bool orb_agents_host_clone_contexts(orb_agents_host_t *destination, orb_agents_host_t *source)
{
    if (!destination) return false;
    if (destination == source) return true;
    if (source && orb_agents_host_current(source)) {
        bool saved = orb_context_store_save_route(&source->router.contexts, source->router.current,
            &source->router.route.args);
        release_projection(source);
        if (!saved) return false;
    }
    orb_context_store_t copy = {0};
    if (source) {
        const orb_context_store_t *store = source->router.registry ? &source->router.contexts : &source->saved;
        if (!orb_context_store_copy(&copy, store)) return false;
    }
    if (!detach(destination)) {
        orb_context_store_clear(&copy);
        return false;
    }
    orb_context_store_clear(&destination->saved);
    destination->saved = copy;
    return true;
}
