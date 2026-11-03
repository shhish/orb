#include "orb_page.h"
#include "orb_app.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static unsigned s_deleted, s_destroyed;
static bool s_fail;

typedef struct { lv_obj_t *viewport; } page_state_t;

static void deleted(lv_event_t *event)
{
    (void)event;
    s_deleted++;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    page_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    state->viewport = lv_obj_create(root);
    lv_obj_remove_style_all(state->viewport);
    lv_obj_set_size(state->viewport, 300, 100);
    lv_obj_t *content = lv_obj_create(state->viewport);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 100, 600);
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "Page");
    lv_obj_add_event_cb(title, deleted, LV_EVENT_DELETE, NULL);
    orb_screen_page_bind(instance, state->viewport, title);
    /* The legacy host keeps chrome outside the body for iris/content motion.
     * Its instance must still clean that resource on all destruction paths. */
    lv_obj_set_parent(title, lv_obj_get_parent(root));
    return !s_fail;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    (void)instance; (void)value;
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
    s_destroyed++;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < context->size) return false;
    orb_page_context_t value = orb_page_save(orb_screen_page_viewport(instance));
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    orb_page_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    orb_page_restore(orb_screen_page_viewport(instance), &value);
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    return ORB_BACK_LEAVE_SCREEN;
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    (void)instance; (void)binding;
    return false;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Page"};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_context_field_t s_fields[] = {{0, sizeof(int32_t), ORB_CONTEXT_I32}};
static const orb_screen_descriptor_t s_descriptor = {
    .id = 1, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(page_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(orb_page_context_t),
    .context_version = 1, .context_fields = s_fields, .context_field_count = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(page_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(orb_page_context_t), .declared = true},
    .privacy = ORB_PRIVACY_PUBLIC, .always_on = ORB_AOD_HIDDEN,
    .feedback = ORB_FEEDBACK_SILENT, .nav_role = ORB_NAV_ROLE_SCREEN,
};

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(466, 466);
    assert(display);
    orb_screen_host_t host;
    orb_screen_host_init(&host, NULL);
    orb_screen_instance_t instance = {0};
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    assert(orb_screen_create(&instance, &host, &s_descriptor, parent, NULL, false));
    lv_obj_t *viewport = orb_screen_page_viewport(&instance);
    assert(viewport && orb_screen_page_title(&instance));
    lv_obj_t *title = orb_screen_page_title(&instance);
    orb_screen_visibility(&instance, ORB_VISIBILITY_COVERED);
    assert(lv_obj_is_hidden(title));
    orb_screen_visibility(&instance, ORB_VISIBILITY_ACTIVE);
    assert(!lv_obj_is_hidden(title));
    orb_page_restore(viewport, &(orb_page_context_t){.scroll = 5000});
    assert(orb_page_save(viewport).scroll == 500);
    orb_page_restore(viewport, &(orb_page_context_t){.scroll = -100});
    assert(orb_page_save(viewport).scroll == 0);
    orb_screen_destroy(&instance);
    assert(s_deleted == 1 && s_destroyed == 1);
    assert(!orb_screen_page_title(&instance) && !orb_screen_page_viewport(&instance));
    orb_screen_destroy(&instance);
    assert(s_deleted == 1 && s_destroyed == 1);

    assert(orb_screen_create(&instance, &host, &s_descriptor, parent, NULL, false));
    lv_obj_delete(parent);
    assert(!orb_screen_alive(&instance));
    assert(s_deleted == 2 && s_destroyed == 2);

    s_fail = true;
    assert(!orb_screen_create(&instance, &host, &s_descriptor, lv_screen_active(), NULL, false));
    assert(!orb_screen_alive(&instance));
    assert(s_deleted == 3 && s_destroyed == 3);
    s_fail = false;
    assert(orb_screen_create(&instance, &host, &s_descriptor, lv_screen_active(), NULL, false));
    lv_obj_delete(orb_screen_page_title(&instance));
    assert(!orb_screen_page_title(&instance));
    orb_screen_destroy(&instance);
    assert(s_deleted == 4 && s_destroyed == 4);
    orb_context_store_t store = {0}, copy = {0};
    assert(orb_screen_create(&instance, &host, &s_descriptor, lv_screen_active(), NULL, false));
    orb_page_restore(orb_screen_page_viewport(&instance), &(orb_page_context_t){.scroll = 123});
    assert(orb_context_store_close_route(&store, &instance, &(orb_screen_args_t){0}));
    assert(!orb_screen_alive(&instance));
    assert(orb_context_store_copy(&copy, &store));
    orb_context_store_clear(&store);
    assert(orb_screen_create(&instance, &host, &s_descriptor, lv_screen_active(), NULL, false));
    assert(orb_context_store_restore_route(&copy, &instance, &(orb_screen_args_t){0}));
    assert(orb_page_save(orb_screen_page_viewport(&instance)).scroll == 123);
    orb_context_store_clear(&copy);
    orb_screen_destroy(&instance);
    assert(s_deleted == 6 && s_destroyed == 6);
    /* Exhausted context storage refuses close without destroying the source. */
    for (unsigned i = 0; i < ORB_SCREEN_CONTEXT_LIMIT; i++) {
        store.entries[i] = (orb_context_entry_t){.id = i + 2, .owner = ORB_MEMORY_WORKSPACE,
            .context = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, s_descriptor.context_bytes)};
        assert(store.entries[i].context);
    }
    assert(orb_screen_create(&instance, &host, &s_descriptor, lv_screen_active(), NULL, false));
    assert(!orb_context_store_close_route(&store, &instance, &(orb_screen_args_t){0}));
    assert(orb_screen_alive(&instance));
    orb_context_store_clear(&store);
    orb_screen_destroy(&instance);
    assert(s_deleted == 7 && s_destroyed == 7);
    lv_display_delete(display);
    return 0;
}
