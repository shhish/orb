#include "orb_app_host.h"
#include "orb_app.h"
#include "orb_input.h"
#include "orb_memory.h"
#include "orb_sound.h"

#include <string.h>

struct orb_app_host {
    orb_app_host_config_t config;
    orb_screen_id_t screen;
    const orb_screen_descriptor_t *screens[1];
    orb_app_manifest_t manifest;
    orb_screen_registry_t registry;
    orb_screen_host_t host;
    /* The router's own navigation value. The shell keeps the real graph. */
    orb_nav_t nav;
    orb_router_t router;
    orb_context_store_t saved;
    size_t projection_size;
    orb_screen_model_t model_view;
    orb_ui_environment_t environment;
    uint64_t poll_revision;
    size_t projection_capacity;
    _Alignas(max_align_t) unsigned char projection[];
};

static bool intent(void *context, const orb_intent_t *value)
{
    orb_app_host_t *host = context;
    if (value->kind != ORB_INTENT_NAVIGATE) {
        if (host->config.command) return host->config.command(host->config.context,
            orb_app_host_current(host), value);
        return host->config.intent && host->config.intent(value);
    }
    if (value->value.navigate.screen != host->screen || !value->value.navigate.launch.valid ||
        !host->config.transition) return false;
    /* Presentation only: source capture must precede the screen's local
     * change, so this runs synchronously. It never routes or destroys the
     * sender. */
    const orb_nav_token_t *token = &value->value.navigate.launch.rect;
    lv_area_t area = {token->x, token->y, token->x + token->width - 1, token->y + token->height - 1};
    host->config.transition(host->config.context, &area, value->value.navigate.reverse,
        value->value.navigate.prepare);
    return true;
}

static bool allow_synthetic(void *context, const orb_intent_t *value)
{
    orb_app_host_t *host = context;
    return host->config.allow_synthetic && host->config.allow_synthetic(value);
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    orb_app_host_t *host = context;
    (void)id;
    host->model_view = (orb_screen_model_t){host->projection, host->projection_size};
    *update = (orb_screen_update_t){.revisions.time = host->poll_revision,
        .model = &host->model_view, .environment = &host->environment};
    return true;
}

/* One detent cue per detent, the shell's existing rim tick. */
static void cue(void *context, orb_feedback_role_t role)
{
    (void)context;
    if (role == ORB_CUE_DETENT) orb_sound_play(ORB_SOUND_TICK);
}

/* The recogniser owns this contact after tangential arbitration; it is
 * sampled outside LVGL dispatch, so provenance is the activating pointer's. */
static bool rim_contact(void *context, bool *physical)
{
    orb_app_host_t *host = context;
    const orb_nav_t *nav = host->config.nav;
    if (!nav || !nav->touch.down || nav->touch.owner != ORB_NAV_GESTURE_ROTARY) return false;
    *physical = !host->config.input || !orb_input_is_synthetic(*host->config.input);
    return true;
}

orb_app_host_t *orb_app_host_create(bool preview, const orb_app_host_config_t *config)
{
    if (!config || !config->descriptor || !config->name || !config->rim_finish || !config->descriptor()) return NULL;
    size_t capacity = config->model_bytes ? config->model_bytes : ORB_APP_HOST_MODEL_MAX;
    if (capacity > SIZE_MAX - sizeof(orb_app_host_t)) return NULL;
    orb_app_host_t *host = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*host) + capacity);
    if (!host) return NULL;
    host->config = *config;
    host->projection_capacity = capacity;
    host->screens[0] = config->descriptor();
    host->screen = host->screens[0]->id;
    host->manifest = (orb_app_manifest_t){.id = host->screen, .name = config->name, .screens = host->screens,
        .screen_count = 1, .entry = host->screen};
    orb_screen_registry_init(&host->registry);
    if (!orb_screen_registry_add(&host->registry, &host->manifest, NULL)) {
        orb_memory_free(host);
        return NULL;
    }
    /* The shell decides when the rim is rebound (defer_rim); the lease then
     * binds the shell's own navigation for the screen's rim property. */
    orb_screen_host_init(&host->host, &(orb_screen_host_config_t){
        .nav = &host->nav, .intent = intent, .cue = cue, .project = project, .allow_synthetic = allow_synthetic,
        .context = host, .defer_rim = true, .preview = preview,
        .rim_nav = config->nav, .rim_contact = rim_contact,
    });
    return host;
}

static void detach(orb_app_host_t *host)
{
    if (!host->router.registry) return;
    /* Closing transfers the instance's reserved context without allocation.
     * Departure never waits on the app: an unsaved close keeps the older record. */
    if (!host->config.forget_on_detach && orb_screen_alive(host->router.current))
        (void)orb_context_store_close_route(&host->router.contexts, host->router.current, &host->router.route.args);
    orb_context_store_clear(&host->saved);
    host->saved = host->router.contexts;
    orb_context_store_init(&host->router.contexts);
    orb_router_destroy(&host->router);
}

void orb_app_host_destroy(orb_app_host_t *host)
{
    if (!host) return;
    detach(host);
    orb_context_store_clear(&host->saved);
    orb_memory_free(host);
}

bool orb_app_host_set_model(orb_app_host_t *host, const void *model, size_t size)
{
    if (!host || (!model && size) || size > host->projection_capacity) return false;
    if (size) memcpy(host->projection, model, size);
    host->projection_size = size;
    return true;
}

const void *orb_app_host_model(const orb_app_host_t *host, size_t *size)
{
    if (size) *size = host ? host->projection_size : 0;
    return host ? host->projection : NULL;
}

bool orb_app_host_attach(orb_app_host_t *host, lv_obj_t *page)
{
    if (!host || !page) return false;
    detach(host);
    host->environment.revision++;
    /* Move, never clone, the saved opaque contexts into this router. */
    host->router.registry = &host->registry;
    host->router.host = &host->host;
    host->router.contexts = host->saved;
    orb_context_store_init(&host->saved);
    bool attached = orb_router_stage(&host->router, &host->registry, &host->host, page,
        host->screen, &(orb_screen_args_t){0}, true) && orb_router_commit_staged(&host->router, page);
    if (!attached) detach(host);
    return attached;
}

void orb_app_host_detach_page(orb_app_host_t *host, lv_obj_t *page)
{
    if (!host || !orb_screen_alive(host->router.current)) return;
    for (lv_obj_t *ancestor = orb_screen_root(host->router.current); ancestor; ancestor = lv_obj_get_parent(ancestor))
        if (ancestor == page) { detach(host); return; }
}

orb_screen_instance_t *orb_app_host_current(orb_app_host_t *host)
{
    return host && orb_screen_alive(host->router.current) ? host->router.current : NULL;
}

static void refresh(orb_app_host_t *host)
{
    orb_screen_update_t update = {0};
    if (project(host, host->screen, &update)) (void)orb_screen_update(host->router.current, &update);
}

void orb_app_host_environment(orb_app_host_t *host)
{
    if (!orb_app_host_current(host)) return;
    host->environment.revision++;
    refresh(host);
}

void orb_app_host_poll(orb_app_host_t *host)
{
    if (!orb_app_host_current(host)) return;
    host->poll_revision++;
    refresh(host);
}

bool orb_app_host_rim(orb_app_host_t *host)
{
    orb_screen_instance_t *screen = orb_app_host_current(host);
    if (!screen) return false;
    /* A captured contact keeps its binding; otherwise bind or refresh. */
    return (host->host.lease.instance == screen && host->host.lease.contact) || orb_screen_rim_acquire(screen);
}

void orb_app_host_rim_finish(orb_app_host_t *host, bool commit)
{
    orb_screen_instance_t *screen = orb_app_host_current(host);
    if (!screen) return;
    if (host->host.lease.instance == screen && host->host.lease.contact) {
        if (orb_screen_rim_finish(screen, commit)) {
            if (commit) return;
        } else {
            /* The framework refused (covered, preview or mid-transition): the
             * presenter may only drop its preview, never write it. */
            commit = false;
        }
    }
    host->config.rim_finish(screen, commit);
}

bool orb_app_host_twist(orb_app_host_t *host, int detents, bool *changed)
{
    orb_screen_instance_t *screen = orb_app_host_current(host);
    orb_nav_t *nav = host ? host->config.nav : NULL;
    if (!screen || !nav) return false;
    bool leased = host->host.lease.instance == screen && !nav->touch.down;
    *changed = leased ? orb_screen_rim_step(&host->host, detents, true) : orb_nav_rotary_step(nav, detents);
    /* A changed leased step has already committed through the lease. */
    if (!leased || !*changed) orb_app_host_rim_finish(host, true);
    return true;
}

static bool copy_contexts(orb_app_host_t *destination, orb_app_host_t *source)
{
    if (orb_app_host_current(source) &&
        !orb_context_store_save_route(&source->router.contexts, source->router.current, &source->router.route.args))
        return false;
    const orb_context_store_t *store = source->router.registry ? &source->router.contexts : &source->saved;
    orb_context_store_t copy = {0};
    if (!orb_context_store_copy(&copy, store)) return false;
    detach(destination);
    orb_context_store_clear(&destination->saved);
    destination->saved = copy;
    return true;
}

bool orb_app_host_clone(orb_app_host_t **destination, orb_app_host_t *source, bool preview)
{
    if (!destination) return false;
    if (!source) {
        orb_app_host_destroy(*destination);
        *destination = NULL;
        return true;
    }
    /* A copy keeps the app's vocabulary but none of the shell's hooks. */
    if (!*destination) *destination = orb_app_host_create(preview, &(orb_app_host_config_t){
        .descriptor = source->config.descriptor, .name = source->config.name, .intent = source->config.intent,
        .model_bytes = source->config.model_bytes, .forget_on_detach = source->config.forget_on_detach,
        .allow_synthetic = source->config.allow_synthetic, .rim_finish = source->config.rim_finish});
    return *destination && copy_contexts(*destination, source);
}

bool orb_app_host_restore(orb_app_host_t **destination, orb_app_host_t **saved)
{
    if (!destination || !saved) return false;
    /* The live host keeps its shell hooks; only values move back. */
    if (*saved && *destination && !copy_contexts(*destination, *saved)) return false;
    orb_app_host_destroy(*saved);
    *saved = NULL;
    return true;
}
