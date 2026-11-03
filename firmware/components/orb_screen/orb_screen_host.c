#include "orb_screen_private.h"

#include <string.h>

void orb_screen_host_init(orb_screen_host_t *host, const orb_screen_host_config_t *config)
{
    if (!host) return;
    memset(host, 0, sizeof(*host));
    if (config) host->config = *config;
}

void orb_screen_scope_enter(orb_screen_data_t *data)
{
    if (data->host->config.ui_scope.enter)
        data->host->config.ui_scope.enter(data->host->config.context, data->handle);
}

void orb_screen_scope_leave(orb_screen_data_t *data)
{
    if (data->host->config.ui_scope.leave)
        data->host->config.ui_scope.leave(data->host->config.context, data->handle);
}

bool orb_screen_ui_admitted(const orb_screen_data_t *data)
{
    if (!data->host->config.ui_scope.used) return true;
    return data->host->config.ui_scope.used(data->host->config.context, data->handle) <=
        data->descriptor->memory.ui_bytes;
}

static void dispose(orb_screen_instance_t *instance, bool parent_deleting)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying) return;
    data->destroying = true;
    data->silent++;
    if (data->host->lease.instance == instance) orb_screen_rim_release(data->host);
    orb_screen_release_resources(data);
    /* Even a failed create owns its partial state and runs the same teardown. */
    orb_screen_scope_enter(data);
    if (data->create_entered) data->descriptor->ops->destroy(instance);
    orb_screen_page_cleanup(data);
    orb_screen_scope_leave(data);
    lv_obj_t *root = data->root;
    /* LVGL sends the root DELETE before recursively deleting its children.
     * Finish that subtree now while descendant callbacks still have state. */
    if (root && parent_deleting) lv_obj_clean(root);
    else if (root) lv_obj_delete(root);
    data->root = NULL;
    while (data->payloads) orb_screen_free(instance, data->payloads->data);
    orb_memory_free(data->context);
    orb_memory_free(data->state);
    instance->framework = NULL;
    orb_memory_free(data);
}

static void root_deleted(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    dispose(lv_event_get_user_data(event), true);
}

bool orb_screen_create(orb_screen_instance_t *instance, orb_screen_host_t *host,
                       const orb_screen_descriptor_t *descriptor, lv_obj_t *parent,
                       const orb_screen_args_t *args, bool preview)
{
    if (!instance || instance->framework || !host || !parent ||
        !orb_screen_descriptor_valid(descriptor, NULL)) return false;
    orb_screen_ui_scope_t *scope = &host->config.ui_scope;
    if ((descriptor->memory.ui_bytes || scope->enter || scope->leave || scope->used) && (!scope->enter || !scope->leave || !scope->used))
        return false;
    orb_screen_data_t *data = orb_memory_calloc(descriptor->memory.owner, 1, sizeof(*data));
    if (!data) return false;
    instance->framework = data;
    data->handle = instance;
    data->host = host;
    data->descriptor = descriptor;
    data->preview = preview;
    data->visibility = preview ? ORB_VISIBILITY_SUSPENDED : ORB_VISIBILITY_ACTIVE;
    data->silent = 1;
    if (descriptor->state_bytes) data->state = orb_memory_calloc(descriptor->memory.owner, 1, descriptor->state_bytes);
    data->context = orb_memory_calloc(descriptor->memory.owner, 1, descriptor->context_bytes);
    if ((descriptor->state_bytes && !data->state) || !data->context) {
        dispose(instance, false);
        return false;
    }
    orb_screen_scope_enter(data);
    data->root = lv_obj_create(parent);
    if (data->root) {
        lv_obj_remove_style_all(data->root);
        lv_obj_set_size(data->root, ORB_NAV_DIAMETER, ORB_NAV_DIAMETER);
        lv_obj_set_scrollable(data->root, false);
        lv_obj_add_event_cb(data->root, root_deleted, LV_EVENT_DELETE, instance);
    }
    orb_screen_args_t defaults = {0};
    data->create_entered = data->root != NULL;
    bool success = data->root && descriptor->ops->create(instance, args ? args : &defaults);
    orb_screen_scope_leave(data);
    if (!success || !orb_screen_ui_admitted(data)) {
        dispose(instance, false);
        return false;
    }
    data->silent--;
    orb_screen_visibility(instance, data->visibility);
    return true;
}

void orb_screen_destroy(orb_screen_instance_t *instance)
{
    dispose(instance, false);
}

bool orb_screen_alive(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data && !data->destroying;
}

void *orb_screen_state(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data ? data->state : NULL;
}

lv_obj_t *orb_screen_root(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data ? data->root : NULL;
}

const orb_screen_descriptor_t *orb_screen_descriptor(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data ? data->descriptor : NULL;
}

orb_screen_host_t *orb_screen_host(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data ? data->host : NULL;
}

static bool revisions_equal(const orb_screen_data_t *data, const orb_screen_update_t *update)
{
    orb_model_domain_mask_t domains = data->descriptor->subscriptions;
    return (!(domains & ORB_DOMAIN_MODEL) || data->revisions.model == update->revisions.model) &&
        (!(domains & ORB_DOMAIN_HOME) || data->revisions.home == update->revisions.home) &&
        (!(domains & ORB_DOMAIN_TIME) || data->revisions.time == update->revisions.time) &&
        (!(domains & ORB_DOMAIN_SETTINGS) || data->revisions.settings == update->revisions.settings) &&
        (!(domains & ORB_DOMAIN_DEVICE) || data->revisions.device == update->revisions.device) &&
        data->environment_revision == (update->environment ? update->environment->revision : 0);
}

bool orb_screen_update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying || !update) return false;
    if (data->updated && revisions_equal(data, update)) return orb_screen_ui_admitted(data);
    data->silent++;
    orb_screen_scope_enter(data);
    data->descriptor->ops->update(instance, update);
    orb_screen_scope_leave(data);
    data->silent--;
    data->revisions = update->revisions;
    data->environment_revision = update->environment ? update->environment->revision : 0;
    data->updated = true;
    return orb_screen_ui_admitted(data);
}

void orb_screen_visibility(orb_screen_instance_t *instance, orb_visibility_t visibility)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying || (unsigned)visibility > ORB_VISIBILITY_SUSPENDED) return;
    if (data->preview) visibility = ORB_VISIBILITY_SUSPENDED;
    if (visibility != ORB_VISIBILITY_ACTIVE && data->host->lease.instance == instance)
        orb_screen_rim_release(data->host);
    data->visibility = visibility;
    lv_obj_set_hidden(data->root, !data->preview && visibility != ORB_VISIBILITY_ACTIVE);
    orb_screen_pause_resources(data, visibility != ORB_VISIBILITY_ACTIVE);
    orb_screen_page_visibility(data);
    data->silent++;
    orb_screen_scope_enter(data);
    data->descriptor->ops->visibility(instance, visibility);
    orb_screen_scope_leave(data);
    data->silent--;
}

orb_visibility_t orb_screen_visibility_get(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data ? data->visibility : ORB_VISIBILITY_SUSPENDED;
}

orb_back_result_t orb_screen_back(orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying || data->preview || data->visibility != ORB_VISIBILITY_ACTIVE)
        return ORB_BACK_CONSUMED;
    orb_back_result_t result = data->descriptor->ops->back(instance);
    return (unsigned)result <= ORB_BACK_LEAVE_SCREEN ? result : ORB_BACK_CONSUMED;
}

void orb_screen_chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *chrome)
{
    if (!chrome) return;
    *chrome = (orb_screen_chrome_t){0};
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying) return;
    data->silent++;
    data->descriptor->ops->chrome(instance, chrome);
    data->silent--;
}
