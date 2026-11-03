#include "orb_app.h"

#include <string.h>

static bool reject(const char **reason, const char *message)
{
    if (reason) *reason = message;
    return false;
}

static bool valid_schema(const orb_screen_descriptor_t *descriptor)
{
    size_t payload = descriptor->context_bytes - sizeof(orb_screen_context_t);
    if (!payload) return descriptor->context_field_count == 0;
    if (!descriptor->context_fields || !descriptor->context_field_count || descriptor->context_field_count > 128) return false;
    for (size_t i = 0; i < descriptor->context_field_count; i++) {
        const orb_context_field_t *field = &descriptor->context_fields[i];
        size_t expected = field->kind == ORB_CONTEXT_BOOL ? 1 :
            field->kind == ORB_CONTEXT_U32 || field->kind == ORB_CONTEXT_I32 ? 4 : 8;
        if (field->kind < ORB_CONTEXT_U32 || field->kind > ORB_CONTEXT_TEXT ||
            !field->size || field->offset > payload || field->size > payload - field->offset ||
            (field->kind != ORB_CONTEXT_TEXT && field->size != expected)) return false;
        for (size_t j = 0; j < i; j++) {
            const orb_context_field_t *other = &descriptor->context_fields[j];
            if (field->offset < other->offset + other->size &&
                other->offset < field->offset + field->size) return false;
        }
    }
    return true;
}

bool orb_screen_descriptor_valid(const orb_screen_descriptor_t *descriptor, const char **reason)
{
    if (reason) *reason = NULL;
    if (!descriptor || descriptor->id == ORB_SCREEN_INVALID_ID)
        return reject(reason, "invalid screen ID");
    const orb_screen_ops_t *ops = descriptor->ops;
    if (!ops || !ops->create || !ops->update || !ops->destroy || !ops->save_context ||
        !ops->restore_context || !ops->back || !ops->rim_binding || !ops->visibility || !ops->chrome)
        return reject(reason, "all screen operations are required");
    if (!descriptor->context_version || descriptor->context_bytes < sizeof(orb_screen_context_t) ||
        descriptor->context_bytes > ORB_SCREEN_CONTEXT_MAX || !valid_schema(descriptor))
        return reject(reason, "invalid context version, size or value schema");
    if (descriptor->privacy <= ORB_PRIVACY_UNDECLARED || descriptor->privacy > ORB_PRIVACY_PRIVATE ||
        descriptor->always_on <= ORB_AOD_UNDECLARED || descriptor->always_on > ORB_AOD_PUBLIC_SUMMARY ||
        descriptor->feedback <= ORB_FEEDBACK_UNDECLARED || descriptor->feedback > ORB_FEEDBACK_STANDARD)
        return reject(reason, "privacy, AOD and feedback metadata are required");
    if (descriptor->privacy == ORB_PRIVACY_PRIVATE && descriptor->always_on != ORB_AOD_HIDDEN)
        return reject(reason, "private screens cannot expose arbitrary AOD content");
    const orb_memory_profile_t *memory = &descriptor->memory;
    if (!memory->declared || (unsigned)memory->owner >= ORB_MEMORY_COUNT ||
        memory->state_bytes != descriptor->state_bytes || memory->context_bytes != descriptor->context_bytes)
        return reject(reason, "exact state/context and resource budgets must be declared");
    if (descriptor->subscriptions & ~ORB_DOMAIN_ALL || (unsigned)descriptor->nav_role > ORB_NAV_ROLE_CONTROL)
        return reject(reason, "invalid domain or navigation metadata");
    return true;
}

void orb_screen_registry_init(orb_screen_registry_t *registry)
{
    if (registry) memset(registry, 0, sizeof(*registry));
}

const orb_screen_descriptor_t *orb_screen_registry_find(const orb_screen_registry_t *registry,
                                                        orb_screen_id_t id)
{
    if (!registry) return NULL;
    for (size_t i = 0; i < registry->screen_count; i++) {
        if (registry->screens[i]->id == id) return registry->screens[i];
    }
    return NULL;
}

bool orb_screen_registry_add(orb_screen_registry_t *registry, const orb_app_manifest_t *app,
                              const char **reason)
{
    if (!registry || !app || !app->name || !app->name[0] || !app->screens || !app->screen_count)
        return reject(reason, "invalid app manifest");
    if (registry->app_count >= ORB_APP_LIMIT || app->screen_count > ORB_SCREEN_LIMIT - registry->screen_count)
        return reject(reason, "registry capacity exceeded");
    for (size_t i = 0; i < registry->app_count; i++) {
        if (registry->apps[i]->id == app->id) return reject(reason, "duplicate app ID");
    }
    bool entry = false;
    for (size_t i = 0; i < app->screen_count; i++) {
        const orb_screen_descriptor_t *screen = app->screens[i];
        if (!orb_screen_descriptor_valid(screen, reason)) return false;
        if (orb_screen_registry_find(registry, screen->id)) return reject(reason, "duplicate screen ID");
        for (size_t j = 0; j < i; j++) {
            if (app->screens[j]->id == screen->id) return reject(reason, "duplicate screen ID");
        }
        entry |= screen->id == app->entry;
    }
    if (!entry) return reject(reason, "app entry screen is absent");
    for (size_t i = 0; i < app->screen_count; i++) registry->screens[registry->screen_count++] = app->screens[i];
    registry->apps[registry->app_count++] = app;
    if (reason) *reason = NULL;
    return true;
}
