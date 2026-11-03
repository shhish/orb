#include "orb_screen_private.h"

#include <string.h>

bool orb_screen_context_valid(const orb_screen_descriptor_t *descriptor,
                               const orb_screen_context_t *context, size_t capacity)
{
    if (!orb_screen_descriptor_valid(descriptor, NULL) || !context || capacity < sizeof(*context) ||
        context->version != descriptor->context_version || context->size != descriptor->context_bytes ||
        capacity < context->size) return false;
    size_t payload = context->size - sizeof(*context);
    for (size_t offset = 0; offset < payload; offset++) {
        bool covered = false;
        for (size_t i = 0; i < descriptor->context_field_count; i++) {
            const orb_context_field_t *field = &descriptor->context_fields[i];
            if (offset < field->offset || offset >= field->offset + field->size) continue;
            covered = true;
            if (field->kind == ORB_CONTEXT_BOOL && context->payload[offset] > 1) return false;
            if (field->kind == ORB_CONTEXT_TEXT && offset == field->offset &&
                !memchr(context->payload + offset, 0, field->size)) return false;
        }
        if (!covered && context->payload[offset]) return false;
    }
    return true;
}

bool orb_screen_save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || !context || data->destroying || capacity < data->descriptor->context_bytes) return false;
    memset(context, 0, data->descriptor->context_bytes);
    context->version = data->descriptor->context_version;
    context->size = data->descriptor->context_bytes;
    data->silent++;
    bool saved = data->descriptor->ops->save_context(instance, context, capacity);
    data->silent--;
    return saved && orb_screen_context_valid(data->descriptor, context, capacity);
}

bool orb_screen_restore(orb_screen_instance_t *instance, const orb_screen_context_t *context, size_t capacity)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying || !orb_screen_context_valid(data->descriptor, context, capacity)) return false;
    if (data->host->lease.instance == instance) orb_screen_rim_release(data->host);
    data->silent++;
    orb_screen_scope_enter(data);
    bool restored = data->descriptor->ops->restore_context(instance, context);
    orb_screen_scope_leave(data);
    data->silent--;
    return restored && orb_screen_ui_admitted(data);
}

uint64_t orb_screen_resolve_id(uint64_t requested, const uint64_t *ids, size_t count)
{
    if (!ids || !count) return 0;
    for (size_t i = 0; i < count; i++) if (ids[i] == requested) return requested;
    return ids[0];
}

int32_t orb_screen_clamp_scroll(int32_t requested, int32_t maximum)
{
    if (maximum < 0) maximum = 0;
    return requested < 0 ? 0 : requested > maximum ? maximum : requested;
}

void orb_context_store_init(orb_context_store_t *store)
{
    if (store) memset(store, 0, sizeof(*store));
}

void orb_context_store_clear(orb_context_store_t *store)
{
    if (!store) return;
    for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
        orb_memory_free(store->entries[i].context);
    }
    memset(store, 0, sizeof(*store));
}

static orb_context_entry_t *find_entry(const orb_context_store_t *store,
                                      orb_screen_id_t id, uint64_t entity_id, uint32_t page)
{
    if (!store) return NULL;
    for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
        orb_context_entry_t *entry = (orb_context_entry_t *)&store->entries[i];
        if (entry->context && entry->id == id && entry->entity_id == entity_id && entry->page == page) return entry;
    }
    return NULL;
}

bool orb_context_store_save_route(orb_context_store_t *store, const orb_screen_instance_t *instance,
                                  const orb_screen_args_t *args)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!store || !data || !args || !orb_screen_save(instance, data->context, data->descriptor->context_bytes)) return false;
    orb_context_entry_t *entry = find_entry(store, data->descriptor->id, args->entity_id, args->page);
    if (!entry) {
        for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
            if (!store->entries[i].context) { entry = &store->entries[i]; break; }
        }
    }
    if (!entry) return false;
    size_t bytes = data->descriptor->context_bytes;
    orb_screen_context_t *copy = orb_memory_alloc(data->descriptor->memory.owner, bytes);
    if (!copy) return false;
    memcpy(copy, data->context, bytes);
    orb_memory_free(entry->context);
    *entry = (orb_context_entry_t){data->descriptor->id, args->entity_id, args->page, copy, data->descriptor->memory.owner};
    return true;
}

bool orb_context_store_restore_route(const orb_context_store_t *store, orb_screen_instance_t *instance,
                                     const orb_screen_args_t *args)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || !args) return false;
    const orb_context_entry_t *entry = find_entry(store, data->descriptor->id, args->entity_id, args->page);
    return entry && orb_screen_restore(instance, entry->context, orb_memory_size(entry->context));
}

bool orb_context_store_save(orb_context_store_t *store, const orb_screen_instance_t *instance,
                            uint64_t entity_id)
{
    return orb_context_store_save_route(store, instance, &(orb_screen_args_t){.entity_id = entity_id});
}

bool orb_context_store_restore(const orb_context_store_t *store, orb_screen_instance_t *instance,
                               uint64_t entity_id)
{
    return orb_context_store_restore_route(store, instance, &(orb_screen_args_t){.entity_id = entity_id});
}

void orb_context_store_remove(orb_context_store_t *store, orb_screen_id_t id, uint64_t entity_id)
{
    orb_context_entry_t *entry = find_entry(store, id, entity_id, 0);
    if (!entry) return;
    orb_memory_free(entry->context);
    memset(entry, 0, sizeof(*entry));
}

bool orb_context_store_close_route(orb_context_store_t *store, orb_screen_instance_t *instance,
                                   const orb_screen_args_t *args)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!store || !data || !args || !orb_screen_save(instance, data->context, data->descriptor->context_bytes)) return false;
    orb_context_entry_t *entry = find_entry(store, data->descriptor->id, args->entity_id, args->page);
    if (!entry) for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
        if (!store->entries[i].context) { entry = &store->entries[i]; break; }
    }
    if (!entry) return false;
    orb_memory_free(entry->context);
    *entry = (orb_context_entry_t){data->descriptor->id, args->entity_id, args->page,
        data->context, data->descriptor->memory.owner};
    data->context = NULL;
    orb_screen_destroy(instance);
    return true;
}

bool orb_context_store_copy(orb_context_store_t *destination, const orb_context_store_t *source)
{
    if (!destination || !source) return false;
    if (destination == source) return true;
    orb_context_store_t copy = {0};
    for (size_t i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
        const orb_context_entry_t *entry = &source->entries[i];
        if (!entry->context) continue;
        orb_screen_context_t *context = orb_memory_alloc(entry->owner, entry->context->size);
        if (!context) { orb_context_store_clear(&copy); return false; }
        memcpy(context, entry->context, entry->context->size);
        copy.entries[i] = *entry;
        copy.entries[i].context = context;
    }
    orb_context_store_clear(destination);
    *destination = copy;
    return true;
}
