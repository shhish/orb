#include "orb_voice_host.h"
#include "orb_app_voice_registry.h"

#include <string.h>

struct orb_voice_host {
    orb_voice_host_config_t config;
    orb_screen_registry_t registry;
    orb_screen_host_t host;
    /* The shell keeps the real navigation graph and its rotary trace policy. */
    orb_nav_t nav;
    orb_router_t router;
    orb_context_store_t saved;
    orb_app_voice_model_t projection;
    orb_screen_model_t model_view;
    orb_ui_environment_t environment;
    uint64_t revision;
};

static bool intent(void *context, const orb_intent_t *value)
{
    orb_voice_host_t *host = context;
    return !host->host.config.preview && host->config.accept &&
        host->config.accept(host->config.context, value);
}

static bool allow_synthetic(void *context, const orb_intent_t *value)
{
    (void)context;
    return orb_app_voice_allow_synthetic(value);
}

static void synthetic_refused(void *context, const orb_intent_t *value)
{
    orb_voice_host_t *host = context;
    if (!host->host.config.preview && host->config.synthetic_refused)
        host->config.synthetic_refused(host->config.context, value);
}

static void release_projection(orb_voice_host_t *host)
{
    /* Descriptors only inspect the borrowed fields during their callback. The
     * next update/input obtains the workspace's current COW generation anew. */
    memset(&host->projection, 0, sizeof(host->projection));
    host->model_view = (orb_screen_model_t){0};
}

static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    orb_voice_host_t *host = context;
    release_projection(host);
    if (screen != ORB_APP_VOICE_SCREEN ||
        !host->config.project(host->config.context, screen, &host->projection)) return false;
    host->model_view = (orb_screen_model_t){&host->projection, sizeof(host->projection)};
    *update = (orb_screen_update_t){.revisions.model = host->revision,
        .revisions.time = host->revision, .revisions.device = host->revision,
        .model = &host->model_view, .environment = &host->environment};
    return true;
}

orb_voice_host_t *orb_voice_host_create(bool preview, const orb_voice_host_config_t *config)
{
    if (!config || !config->project) return NULL;
    orb_voice_host_t *host = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*host));
    if (!host) return NULL;
    host->config = *config;
    orb_screen_registry_init(&host->registry);
    if (!orb_screen_registry_add(&host->registry, orb_app_voice_manifest(), NULL)) {
        orb_memory_free(host);
        return NULL;
    }
    orb_screen_host_init(&host->host, &(orb_screen_host_config_t){
        .nav = &host->nav, .project = project, .intent = intent, .context = host,
        .preview = preview, .defer_rim = true, .allow_synthetic = allow_synthetic,
        .synthetic_refused = synthetic_refused});
    return host;
}

static bool detach(orb_voice_host_t *host)
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

void orb_voice_host_destroy(orb_voice_host_t *host)
{
    if (!host) return;
    /* Final destruction needs no saved return record and cannot be refused. */
    orb_router_destroy(&host->router);
    orb_context_store_clear(&host->saved);
    orb_memory_free(host);
}

orb_screen_instance_t *orb_voice_host_current(orb_voice_host_t *host)
{
    return host && orb_screen_alive(host->router.current) ? host->router.current : NULL;
}

bool orb_voice_host_attach(orb_voice_host_t *host, lv_obj_t *page, orb_screen_id_t screen)
{
    if (!host || !page || screen != ORB_APP_VOICE_SCREEN) return false;
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

bool orb_voice_host_detach_page(orb_voice_host_t *host, lv_obj_t *page)
{
    if (!host || !orb_voice_host_current(host)) return true;
    for (lv_obj_t *ancestor = orb_screen_root(host->router.current); ancestor; ancestor = lv_obj_get_parent(ancestor))
        if (ancestor == page) return detach(host);
    return true;
}

void orb_voice_host_meter(orb_voice_host_t *host)
{
    if (!host) return;
    orb_app_voice_meter(orb_voice_host_current(host));
    release_projection(host);
}
