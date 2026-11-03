/* Descendant callbacks run during LVGL deletion, before state can retire. */
#include "orb_screen.h"

#include <assert.h>

typedef struct { unsigned marker, updates; lv_timer_t *timer; } delete_state_t;
typedef struct { size_t ui_bytes; } delete_scope_t;

static unsigned s_children_deleted, s_descriptor_destroyed;
static bool s_fail_create;

static size_t workspace_used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static lv_mem_monitor_t lvgl_used(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor;
}

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static void child_deleted(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    delete_state_t *state = orb_screen_state(instance);
    assert(state && state->marker == 0xabc123);
    state->marker++;
    s_children_deleted++;
}

static void timer_tick(lv_timer_t *timer) { (void)timer; }

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    delete_state_t *state = orb_screen_state(instance);
    state->marker = 0xabc123;
    lv_obj_t *child = lv_obj_create(orb_screen_root(instance));
    lv_obj_add_event_cb(child, child_deleted, LV_EVENT_DELETE, instance);
    state->timer = orb_screen_timer_create(instance, timer_tick, 10000, state);
    assert(state->timer);
    return !s_fail_create;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    (void)update;
    ((delete_state_t *)orb_screen_state(instance))->updates++;
}

static void destroy(orb_screen_instance_t *instance)
{
    delete_state_t *state = orb_screen_state(instance);
    assert(state && state->marker == 0xabc123);
    s_descriptor_destroyed++;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    (void)instance; (void)context; (void)capacity;
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    (void)instance; (void)context;
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
    *value = (orb_screen_chrome_t){.title = "Delete fixture"};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = 17, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(delete_state_t), .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(delete_state_t),
               .context_bytes = sizeof(orb_screen_context_t), .ui_bytes = 128, .declared = true},
    .privacy = ORB_PRIVACY_PUBLIC, .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_SILENT,
    .subscriptions = ORB_DOMAIN_MODEL, .nav_role = ORB_NAV_ROLE_SCREEN,
};

static void scope_enter(void *context, orb_screen_instance_t *instance) { (void)context; (void)instance; }
static void scope_leave(void *context, orb_screen_instance_t *instance) { (void)context; (void)instance; }
static size_t scope_used(void *context, const orb_screen_instance_t *instance)
{
    (void)instance;
    return ((delete_scope_t *)context)->ui_bytes;
}

static void deletion_case(orb_screen_host_t *host, unsigned kind)
{
    size_t before = workspace_used();
    lv_mem_monitor_t lv_before = lvgl_used();
    unsigned timers = timer_count(), deleted = s_children_deleted, destroyed = s_descriptor_destroyed;
    orb_screen_instance_t instance = {0};
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    s_fail_create = kind == 2;
    bool created = orb_screen_create(&instance, host, &s_descriptor, parent, NULL, false);
    assert(created == !s_fail_create);
    s_fail_create = false;
    if (kind == 0) orb_screen_destroy(&instance);
    lv_obj_delete(parent);
    assert(!orb_screen_alive(&instance));
    orb_screen_destroy(&instance);
    assert(s_children_deleted == deleted + 1 && s_descriptor_destroyed == destroyed + 1);
    assert(timer_count() == timers && workspace_used() == before);
    lv_mem_monitor_t lv_after = lvgl_used();
    assert(lv_before.total_size == lv_after.total_size && lv_before.used_cnt == lv_after.used_cnt);
}

static void failed_update_is_not_success(orb_screen_host_t *host, delete_scope_t *scope)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, &s_descriptor, lv_screen_active(), NULL, false));
    orb_screen_update_t value = {.revisions.model = 1};
    scope->ui_bytes = s_descriptor.memory.ui_bytes + 1;
    assert(!orb_screen_update(&instance, &value));
    assert(!orb_screen_update(&instance, &value));
    orb_screen_context_t context = {.version = 1, .size = sizeof(context)};
    assert(!orb_screen_restore(&instance, &context, sizeof(context)));
    scope->ui_bytes = 64;
    assert(orb_screen_update(&instance, &value));
    unsigned updates = ((delete_state_t *)orb_screen_state(&instance))->updates;
    assert(orb_screen_update(&instance, &value));
    assert(((delete_state_t *)orb_screen_state(&instance))->updates == updates);
    assert(orb_screen_restore(&instance, &context, sizeof(context)));
    orb_screen_destroy(&instance);
}

void orb_test_screen_parent_delete(void)
{
    delete_scope_t scope = {.ui_bytes = 64};
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){
        .context = &scope, .ui_scope = {scope_enter, scope_leave, scope_used},
    });
    for (unsigned kind = 0; kind < 3; kind++) deletion_case(&host, kind);
    failed_update_is_not_success(&host, &scope);
}
