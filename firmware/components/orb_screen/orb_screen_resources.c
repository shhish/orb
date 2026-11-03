#include "orb_screen_private.h"

#include <stdint.h>
#include <string.h>

void *orb_screen_alloc(orb_screen_instance_t *instance, size_t bytes)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying || !bytes || bytes > SIZE_MAX - sizeof(orb_screen_payload_t) ||
        data->payload_used > data->descriptor->memory.payload_bytes ||
        bytes > data->descriptor->memory.payload_bytes - data->payload_used) return NULL;
    orb_screen_payload_t *payload = orb_memory_calloc(data->descriptor->memory.owner, 1, sizeof(*payload) + bytes);
    if (!payload) return NULL;
    payload->next = data->payloads;
    payload->bytes = bytes;
    data->payloads = payload;
    data->payload_used += bytes;
    if (data->payload_peak < data->payload_used) data->payload_peak = data->payload_used;
    return payload->data;
}

bool orb_screen_free(orb_screen_instance_t *instance, void *pointer)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || !pointer) return false;
    orb_screen_payload_t **link = &data->payloads;
    while (*link && (*link)->data != pointer) link = &(*link)->next;
    if (!*link) return false;
    orb_screen_payload_t *payload = *link;
    *link = payload->next;
    data->payload_used -= payload->bytes;
    orb_memory_free(payload);
    return true;
}

bool orb_screen_track(orb_screen_instance_t *instance, orb_screen_resource_kind_t kind,
                      void *resource, orb_screen_cleanup_cb_t cleanup, orb_screen_pause_cb_t pause)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying || !resource || !cleanup ||
        kind < ORB_RESOURCE_TIMER || kind > ORB_RESOURCE_ASYNC) return false;
    orb_screen_resource_t *empty = NULL;
    for (size_t i = 0; i < ORB_SCREEN_RESOURCE_LIMIT; i++) {
        if (data->resources[i].resource == resource) return false;
        if (!data->resources[i].resource && !empty) empty = &data->resources[i];
    }
    if (!empty) return false;
    *empty = (orb_screen_resource_t){kind, resource, cleanup, pause};
    if (pause && data->visibility != ORB_VISIBILITY_ACTIVE) pause(resource, true);
    return true;
}

bool orb_screen_untrack(orb_screen_instance_t *instance, void *resource, bool release)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || !resource) return false;
    for (size_t i = 0; i < ORB_SCREEN_RESOURCE_LIMIT; i++) {
        orb_screen_resource_t owned = data->resources[i];
        if (owned.resource != resource) continue;
        data->resources[i] = (orb_screen_resource_t){0};
        if (release) owned.cleanup(resource);
        return true;
    }
    return false;
}

void orb_screen_release_resources(orb_screen_data_t *data)
{
    for (size_t i = ORB_SCREEN_RESOURCE_LIMIT; i > 0; i--) {
        orb_screen_resource_t owned = data->resources[i - 1];
        data->resources[i - 1] = (orb_screen_resource_t){0};
        if (owned.resource) owned.cleanup(owned.resource);
    }
}

void orb_screen_pause_resources(orb_screen_data_t *data, bool paused)
{
    for (size_t i = 0; i < ORB_SCREEN_RESOURCE_LIMIT; i++) {
        orb_screen_resource_t *resource = &data->resources[i];
        if (resource->resource && resource->pause) resource->pause(resource->resource, paused);
    }
}

static void timer_cleanup(void *timer)
{
    lv_timer_delete(timer);
}

static void timer_pause(void *timer, bool paused)
{
    if (paused) lv_timer_pause(timer);
    else lv_timer_resume(timer);
}

lv_timer_t *orb_screen_timer_create(orb_screen_instance_t *instance, lv_timer_cb_t callback,
                                   uint32_t period, void *user_data)
{
    if (!orb_screen_alive(instance) || !callback || !period) return NULL;
    lv_timer_t *timer = lv_timer_create(callback, period, user_data);
    if (!timer) return NULL;
    /* Natural one-shot completion cannot leave a dangling tracked timer. */
    lv_timer_set_auto_delete(timer, false);
    if (!orb_screen_track(instance, ORB_RESOURCE_TIMER, timer, timer_cleanup, timer_pause)) {
        lv_timer_delete(timer);
        return NULL;
    }
    return timer;
}

void orb_screen_usage(const orb_screen_instance_t *instance, orb_screen_usage_t *usage)
{
    if (!usage) return;
    memset(usage, 0, sizeof(*usage));
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data) return;
    usage->state = data->descriptor->state_bytes;
    usage->context = data->descriptor->context_bytes;
    usage->payload = data->payload_used;
    usage->payload_peak = data->payload_peak;
    usage->allocations = 2 + (data->state != NULL);
    if (data->host->config.ui_scope.used)
        usage->ui = data->host->config.ui_scope.used(data->host->config.context, instance);
    for (orb_screen_payload_t *payload = data->payloads; payload; payload = payload->next)
        usage->allocations++;
    for (size_t i = 0; i < ORB_SCREEN_RESOURCE_LIMIT; i++) {
        if (data->resources[i].resource) usage->resources[data->resources[i].kind - 1]++;
    }
}
