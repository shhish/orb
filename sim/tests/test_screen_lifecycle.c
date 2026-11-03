/* Public-contract fixtures: no existing app, child topology or live service. */
#include "orb_app.h"
#include "orb_screen_lifecycle.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void orb_test_screen_parent_delete(void);

typedef struct {
    uint64_t ids[3];
    size_t count;
    int32_t maximum_scroll;
} list_model_t;
typedef struct {
    uint64_t selected, focus;
    int32_t scroll;
    uint32_t page;
} list_context_t;
typedef struct {
    list_model_t model;
    list_context_t context;
    lv_timer_t *timer;
    uint64_t entity_id;
    unsigned updates, observer, asynchronous;
    int32_t animated;
} list_state_t;
typedef struct {
    unsigned cues[ORB_CUE_ERROR + 1], intents;
    orb_intent_t last_intent;
    size_t ui_bytes;
} host_events_t;

static unsigned s_stateless_updates, s_destroyed, s_observers, s_async, s_animations;
static unsigned s_begins, s_previews, s_commits, s_cancels, s_pauses;
static unsigned s_timer_baseline, s_animation_baseline;
static uint32_t s_now;
static bool s_fail_create, s_fail_restore, s_switch_property, s_attempt_rim_send, s_home_commit;
static orb_back_result_t s_back = ORB_BACK_LEAVE_SCREEN;
static orb_screen_descriptor_t s_stateless, s_list, s_layer;
static const list_model_t s_full = {{11, 22, 33}, 3, 100};
static const list_model_t s_removed = {{11, 33, 0}, 2, 40};
static const list_model_t s_empty = {{0}, 0, 0};

static uint32_t tick(void) { return s_now; }

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static size_t owned_memory(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static void quiescent(void)
{
    assert(s_observers == 0 && s_async == 0 && s_animations == 0);
    assert(timer_count() == s_timer_baseline);
    assert(lv_anim_count_running() == s_animation_baseline);
}

static lv_mem_monitor_t lvgl_memory(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor;
}

static bool intent(void *context, const orb_intent_t *value)
{
    host_events_t *events = context;
    events->intents++;
    events->last_intent = *value;
    return true;
}

static void cue(void *context, orb_feedback_role_t role)
{
    host_events_t *events = context;
    assert(role >= ORB_CUE_OPEN && role <= ORB_CUE_ERROR);
    events->cues[role]++;
}

static void ui_enter(void *context, orb_screen_instance_t *instance)
{
    (void)context; (void)instance;
}

static void ui_leave(void *context, orb_screen_instance_t *instance)
{
    (void)context; (void)instance;
}

static size_t ui_used(void *context, const orb_screen_instance_t *instance)
{
    (void)instance;
    return ((host_events_t *)context)->ui_bytes;
}

static void host_init(orb_screen_host_t *host, orb_nav_t *nav, host_events_t *events)
{
    orb_nav_init(nav, 1);
    orb_screen_host_init(host, &(orb_screen_host_config_t){
        .nav = nav, .intent = intent, .cue = cue, .context = events,
        .ui_scope = {ui_enter, ui_leave, ui_used},
    });
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context; (void)id;
    static const orb_screen_model_t model = {&s_full, sizeof(s_full)};
    *update = (orb_screen_update_t){.revisions.model = 1, .model = &model};
    return true;
}

static void assert_silent(orb_screen_instance_t *instance)
{
    orb_intent_t request = {.kind = ORB_INTENT_SEND, .origin = ORB_INPUT_TOUCH, .physical = true};
    assert(!orb_screen_emit(instance, &request));
    uint64_t event = orb_screen_event(orb_screen_host(instance));
    assert(event && !orb_screen_feedback(instance, event, ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
}

static bool stateless_create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)instance; (void)args;
    assert_silent(instance);
    s_stateless_updates = 0;
    return true;
}

static void stateless_update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    (void)instance; (void)update;
    assert_silent(instance);
    s_stateless_updates++;
}

static void destroyed(orb_screen_instance_t *instance)
{
    (void)instance;
    s_destroyed++;
}

static bool stateless_save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t size)
{
    assert(size >= sizeof(*context));
    *context = (orb_screen_context_t){orb_screen_descriptor(instance)->context_version, sizeof(*context)};
    return true;
}

static bool stateless_restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    (void)instance;
    return context->size == sizeof(*context);
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    return s_back;
}

static bool no_rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
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
    *value = (orb_screen_chrome_t){.title = "Fixture", .accent_token = 1, .title_role = 1};
}

static void timer_tick(lv_timer_t *timer) { (void)timer; }

static void animated(void *context, int32_t value)
{
    ((list_state_t *)context)->animated = value;
}

static void animation_cleanup(void *context)
{
    lv_anim_delete(context, animated);
    s_animations--;
}

static void animation_pause(void *context, bool paused)
{
    lv_anim_t *animation = lv_anim_get(context, animated);
    assert(animation);
    if (paused) lv_anim_pause(animation);
    else lv_anim_resume(animation);
    s_pauses++;
}

static void observer_cleanup(void *context) { (void)context; s_observers--; }
static void async_cleanup(void *context) { (void)context; s_async--; }

static bool list_create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    list_state_t *state = orb_screen_state(instance);
    state->context.page = args ? args->page : 0;
    state->entity_id = args ? args->entity_id : 0;
    assert_silent(instance);
    state->timer = orb_screen_timer_create(instance, timer_tick, 10000, state);
    assert(state->timer);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, state);
    lv_anim_set_exec_cb(&animation, animated);
    lv_anim_set_values(&animation, 0, 100);
    lv_anim_set_duration(&animation, 1000);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    assert(lv_anim_start(&animation));
    s_animations++;
    assert(orb_screen_track(instance, ORB_RESOURCE_ANIMATION, state, animation_cleanup, animation_pause));
    s_observers++; s_async++;
    assert(orb_screen_track(instance, ORB_RESOURCE_OBSERVER, &state->observer, observer_cleanup, NULL));
    assert(orb_screen_track(instance, ORB_RESOURCE_ASYNC, &state->asynchronous, async_cleanup, NULL));
    assert(orb_screen_alloc(instance, 128));
    return !s_fail_create;
}

static void list_update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    list_state_t *state = orb_screen_state(instance);
    assert_silent(instance);
    state->updates++;
    state->model = (list_model_t){0};
    if (update->model && update->model->data && update->model->size == sizeof(list_model_t))
        state->model = *(const list_model_t *)update->model->data;
    state->context.selected = orb_screen_resolve_id(state->context.selected, state->model.ids, state->model.count);
    state->context.focus = orb_screen_resolve_id(state->context.focus, state->model.ids, state->model.count);
    state->context.scroll = orb_screen_clamp_scroll(state->context.scroll, state->model.maximum_scroll);
}

static bool list_save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t size)
{
    const list_state_t *state = orb_screen_state(instance);
    assert(size >= sizeof(*context) + sizeof(state->context));
    *context = (orb_screen_context_t){1, sizeof(*context) + sizeof(state->context)};
    memcpy(context->payload, &state->context, sizeof(state->context));
    return true;
}

static bool list_restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    list_state_t *state = orb_screen_state(instance);
    assert_silent(instance);
    if (s_fail_restore) return false;
    memcpy(&state->context, context->payload, sizeof(state->context));
    state->context.selected = orb_screen_resolve_id(state->context.selected, state->model.ids, state->model.count);
    state->context.focus = orb_screen_resolve_id(state->context.focus, state->model.ids, state->model.count);
    state->context.scroll = orb_screen_clamp_scroll(state->context.scroll, state->model.maximum_scroll);
    return true;
}

static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
    s_begins++;
}

static void rim_preview(orb_screen_instance_t *instance, float value)
{
    list_state_t *state = orb_screen_state(instance);
    state->context.scroll = (int32_t)value;
    s_previews++;
    if (s_attempt_rim_send) {
        orb_intent_t request = {.kind = ORB_INTENT_SEND, .origin = ORB_INPUT_TOUCH, .physical = true};
        assert(!orb_screen_emit(instance, &request));
    }
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
                       orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)value; (void)origin; (void)physical;
    if (commit) s_commits++;
    else s_cancels++;
    if (commit && s_home_commit) {
        orb_intent_t request = {.kind = ORB_INTENT_HOME_SET_LEVEL};
        assert(orb_screen_emit(instance, &request));
    }
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *value)
{
    const list_state_t *state = orb_screen_state(instance);
    *value = (orb_rim_binding_t){
        .subpage = state->context.page, .property = s_switch_property ? 2 : 1,
        .kind = ORB_RIM_VALUE, .minimum = 0, .maximum = 100,
        .value = state->context.scroll, .detent = 1, .units_per_turn = 100,
        .physical_required = true, .begin = rim_begin, .preview = rim_preview, .finish = rim_finish,
    };
    return true;
}

static const orb_screen_ops_t s_stateless_ops = {
    stateless_create, stateless_update, destroyed, stateless_save, stateless_restore,
    back, no_rim, visibility, chrome,
};
static const orb_screen_ops_t s_list_ops = {
    list_create, list_update, destroyed, list_save, list_restore,
    back, rim, visibility, chrome,
};
static const orb_context_field_t s_fields[] = {
    {offsetof(list_context_t, selected), sizeof(uint64_t), ORB_CONTEXT_ID},
    {offsetof(list_context_t, focus), sizeof(uint64_t), ORB_CONTEXT_ID},
    {offsetof(list_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(list_context_t, page), sizeof(uint32_t), ORB_CONTEXT_U32},
};

static void descriptors_init(void)
{
    s_stateless = (orb_screen_descriptor_t){
        .id = 1, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_stateless_ops,
        .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
        .memory = {.owner = ORB_MEMORY_WORKSPACE, .context_bytes = sizeof(orb_screen_context_t),
                   .ui_bytes = 8192, .declared = true},
        .subscriptions = ORB_DOMAIN_MODEL, .privacy = ORB_PRIVACY_PUBLIC,
        .always_on = ORB_AOD_PUBLIC_SUMMARY, .feedback = ORB_FEEDBACK_STANDARD,
        .nav_role = ORB_NAV_ROLE_FACE,
    };
    s_list = s_stateless;
    s_list.id = 2; s_list.parent_id = 1; s_list.ops = &s_list_ops;
    s_list.state_bytes = sizeof(list_state_t);
    s_list.context_bytes += sizeof(list_context_t);
    s_list.context_fields = s_fields; s_list.context_field_count = 4;
    s_list.memory.state_bytes = s_list.state_bytes;
    s_list.memory.context_bytes = s_list.context_bytes;
    s_list.memory.payload_bytes = 256;
    s_list.memory.ui_bytes = 8192;
    s_list.nav_role = ORB_NAV_ROLE_SCREEN;
    s_list.privacy = ORB_PRIVACY_PRIVATE; s_list.always_on = ORB_AOD_HIDDEN;
    s_layer = s_stateless;
    s_layer.id = 3; s_layer.parent_id = 1; s_layer.nav_role = ORB_NAV_ROLE_CONTROL;
}

static void stateless_exercise(orb_screen_instance_t *instance) { (void)instance; }

static void stateless_verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_state(instance) == NULL);
    assert(s_stateless_updates == (phase == ORB_LIFECYCLE_CHANGED ? 2u : 1u));
}

static void list_exercise(orb_screen_instance_t *instance)
{
    list_state_t *state = orb_screen_state(instance);
    state->context = (list_context_t){22, 33, 75, 2};
}

static void list_verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    const list_state_t *state = orb_screen_state(instance);
    assert(state->updates == (phase == ORB_LIFECYCLE_CHANGED ? 2u : 1u));
    if (phase == ORB_LIFECYCLE_RESTORED) {
        assert(state->context.selected == 22 && state->context.focus == 33);
        assert(state->context.scroll == 75 && state->context.page == 2);
    } else if (phase == ORB_LIFECYCLE_REMOVED_ID) {
        assert(state->context.selected == 11 && state->context.focus == 33);
        assert(state->context.scroll == 40);
    } else if (phase == ORB_LIFECYCLE_EMPTY) {
        assert(!state->context.selected && !state->context.focus && !state->context.scroll);
    }
}

static void lifecycle_examples(orb_screen_host_t *host)
{
    orb_screen_model_t full = {&s_full, sizeof(s_full)};
    orb_screen_model_t removed = {&s_removed, sizeof(s_removed)};
    orb_screen_model_t empty = {&s_empty, sizeof(s_empty)};
    orb_screen_update_t initial = {.revisions.model = 1, .model = &full};
    orb_screen_update_t changed = {.revisions.model = 2, .model = &full};
    orb_screen_update_t missing = {.revisions.model = 3, .model = &removed};
    orb_screen_update_t none = {.revisions.model = 4, .model = &empty};
    orb_lifecycle_case_t test = {
        .descriptor = &s_stateless, .initial = &initial, .changed = &changed,
        .removed = &missing, .empty = &none, .exercise = stateless_exercise,
        .verify = stateless_verify, .quiescent = quiescent,
    };
    size_t before = owned_memory();
    orb_test_screen_lifecycle(host, &test);
    test.descriptor = &s_list; test.exercise = list_exercise; test.verify = list_verify;
    orb_test_screen_lifecycle(host, &test);
    assert(owned_memory() == before);
}

static void registry_validation(void)
{
    const char *reason = NULL;
    assert(orb_screen_descriptor_valid(&s_stateless, &reason));
    assert(orb_screen_descriptor_valid(&s_list, &reason));
    orb_screen_descriptor_t invalid = s_list;
    orb_screen_ops_t operations = s_list_ops;
    operations.chrome = NULL; invalid.ops = &operations;
    assert(!orb_screen_descriptor_valid(&invalid, &reason) && reason);
    invalid = s_list; invalid.context_version = 0;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    invalid = s_list; invalid.privacy = ORB_PRIVACY_UNDECLARED;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    invalid = s_list; invalid.always_on = ORB_AOD_UNDECLARED;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    invalid = s_list; invalid.feedback = ORB_FEEDBACK_UNDECLARED;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    invalid = s_list; invalid.memory.declared = false;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    invalid = s_list; invalid.memory.state_bytes++;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    invalid = s_list; invalid.memory.owner = (orb_memory_owner_t)-1;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    invalid = s_list; invalid.id = ORB_SCREEN_INVALID_ID;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    orb_context_field_t overlap[4];
    memcpy(overlap, s_fields, sizeof(overlap)); overlap[1].offset = 0;
    invalid = s_list; invalid.context_fields = overlap;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    overlap[1] = s_fields[1]; overlap[0].kind = (orb_context_kind_t)99;
    assert(!orb_screen_descriptor_valid(&invalid, NULL));
    const orb_screen_descriptor_t *screens[] = {&s_stateless, &s_list};
    orb_app_manifest_t app = {1, "Examples", screens, 2, 1};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &app, &reason));
    assert(orb_screen_registry_find(&registry, 2) == &s_list);
    assert(!orb_screen_registry_add(&registry, &app, &reason));
    assert(registry.app_count == 1 && registry.screen_count == 2);
}

static void failed_create_and_parent_delete(orb_screen_host_t *host)
{
    size_t before = owned_memory();
    orb_screen_instance_t instance = {0};
    s_fail_create = true;
    assert(!orb_screen_create(&instance, host, &s_list, lv_screen_active(), NULL, false));
    s_fail_create = false;
    assert(!orb_screen_alive(&instance));
    quiescent();
    assert(owned_memory() == before);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    assert(orb_screen_create(&instance, host, &s_list, parent, NULL, false));
    assert(orb_screen_rim_acquire(&instance));
    assert(!orb_screen_rim_begin(&instance, ORB_INPUT_TOUCH, false));
    assert(orb_screen_rim_begin(&instance, ORB_INPUT_TOUCH, true));
    unsigned cancelled = s_cancels, destroyed_count = s_destroyed;
    lv_obj_delete(parent);
    assert(!orb_screen_alive(&instance) && host->lease.instance == NULL);
    assert(s_cancels == cancelled + 1 && s_destroyed == destroyed_count + 1);
    orb_screen_destroy(&instance);
    assert(s_destroyed == destroyed_count + 1);
    quiescent();
    assert(owned_memory() == before);
}

static void budget_rejection(orb_screen_host_t *host)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, stats.budget - stats.used));
    orb_screen_instance_t instance = {0};
    assert(!orb_screen_create(&instance, host, &s_list, lv_screen_active(), NULL, false));
    orb_memory_release(ORB_MEMORY_WORKSPACE, stats.budget - stats.used);
    assert(owned_memory() == stats.used);
    assert(orb_screen_create(&instance, host, &s_list, lv_screen_active(), NULL, false));
    assert(orb_screen_alloc(&instance, 129) == NULL);
    void *payload = orb_screen_alloc(&instance, 128);
    assert(payload && orb_screen_free(&instance, payload));
    assert(!orb_screen_free(&instance, payload));
    orb_screen_destroy(&instance);
    quiescent();
    assert(owned_memory() == stats.used);
}

static void ui_budget_rejection(orb_screen_host_t *host, host_events_t *events)
{
    orb_screen_instance_t instance = {0};
    size_t before = owned_memory();
    events->ui_bytes = s_list.memory.ui_bytes + 1;
    assert(!orb_screen_create(&instance, host, &s_list, lv_screen_active(), NULL, false));
    events->ui_bytes = 256;
    quiescent();
    assert(owned_memory() == before);
    orb_screen_host_t unaccounted;
    orb_screen_host_config_t config = host->config;
    config.ui_scope = (orb_screen_ui_scope_t){0};
    orb_screen_host_init(&unaccounted, &config);
    assert(!orb_screen_create(&instance, &unaccounted, &s_list, lv_screen_active(), NULL, false));
    assert(owned_memory() == before);
}

static void context_schema_values(void)
{
    const orb_context_field_t fields[] = {{0, 1, ORB_CONTEXT_BOOL}, {2, 4, ORB_CONTEXT_TEXT}};
    orb_screen_descriptor_t descriptor = s_stateless;
    descriptor.context_fields = fields; descriptor.context_field_count = 2;
    descriptor.context_bytes = descriptor.memory.context_bytes = 12;
    unsigned char bytes[12] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    *context = (orb_screen_context_t){1, sizeof(bytes)};
    assert(orb_screen_context_valid(&descriptor, context, sizeof(bytes)));
    context->payload[0] = 2;
    assert(!orb_screen_context_valid(&descriptor, context, sizeof(bytes)));
    context->payload[0] = 1; context->payload[1] = 9;
    assert(!orb_screen_context_valid(&descriptor, context, sizeof(bytes)));
    context->payload[1] = 0; memset(context->payload + 2, 'X', 4);
    assert(!orb_screen_context_valid(&descriptor, context, sizeof(bytes)));
    context->payload[5] = 0;
    assert(orb_screen_context_valid(&descriptor, context, sizeof(bytes)));
    assert(orb_screen_resolve_id(999, s_full.ids, 3) == 11);
    assert(orb_screen_resolve_id(11, NULL, 0) == 0);
    assert(orb_screen_clamp_scroll(-10, 100) == 0 && orb_screen_clamp_scroll(10, -1) == 0);
}

static void context_store(orb_screen_host_t *host)
{
    orb_screen_instance_t instance = {0};
    orb_context_store_t store;
    orb_context_store_init(&store);
    size_t before = owned_memory();
    assert(orb_screen_create(&instance, host, &s_list, lv_screen_active(), NULL, false));
    orb_screen_model_t model = {&s_full, sizeof(s_full)};
    orb_screen_update_t update = {.revisions.model = 1, .model = &model};
    assert(orb_screen_update(&instance, &update));
    list_exercise(&instance);
    assert(orb_context_store_save(&store, &instance, 55));
    assert(!orb_context_store_restore(&store, &instance, 56));
    list_state_t *state = orb_screen_state(&instance);
    state->context.scroll = 0;
    assert(orb_context_store_restore(&store, &instance, 55));
    assert(state->context.scroll == 75);
    unsigned char bytes[sizeof(orb_screen_context_t) + sizeof(list_context_t)] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    assert(orb_screen_save(&instance, context, sizeof(bytes)));
    context->version++;
    assert(!orb_screen_restore(&instance, context, sizeof(bytes)));
    context->version--; context->size--;
    assert(!orb_screen_restore(&instance, context, sizeof(bytes)));
    context->size++;
    assert(!orb_screen_restore(&instance, context, sizeof(bytes) - 1));
    orb_context_store_remove(&store, 2, 55);
    assert(!orb_context_store_restore(&store, &instance, 55));
    orb_context_store_clear(&store);
    orb_screen_destroy(&instance);
    quiescent();
    assert(owned_memory() == before);
}

static void leases_and_visibility(orb_screen_host_t *host, host_events_t *events)
{
    orb_nav_set_current(host->config.nav, s_list.id);
    orb_screen_instance_t instance = {0}, passive = {0};
    assert(orb_screen_create(&instance, host, &s_list, lv_screen_active(), NULL, false));
    assert(orb_screen_create(&passive, host, &s_stateless, lv_screen_active(), NULL, false));
    assert(!orb_screen_rim_acquire(&passive));
    assert(orb_screen_rim_acquire(&instance));
    assert(orb_screen_rim_begin(&instance, ORB_INPUT_TOUCH, true));
    assert(!orb_screen_rim_begin(&instance, ORB_INPUT_CROWN, true));
    s_switch_property = true;
    assert(!orb_screen_rim_acquire(&instance));
    s_switch_property = false;
    unsigned detents = events->cues[ORB_CUE_DETENT];
    assert(!orb_screen_rim_set(&instance, 10, ORB_INPUT_PROGRAMMATIC));
    assert(events->cues[ORB_CUE_DETENT] == detents);
    assert(orb_screen_rim_finish(&instance, false));
    assert(orb_screen_rim_set(&instance, 10, ORB_INPUT_PROGRAMMATIC));
    assert(events->cues[ORB_CUE_DETENT] == detents);
    assert(orb_screen_rim_begin(&instance, ORB_INPUT_CROWN, true));
    s_attempt_rim_send = true;
    assert(orb_screen_rim_set(&instance, 11, ORB_INPUT_CROWN));
    s_attempt_rim_send = false;
    assert(orb_screen_rim_finish(&instance, true));
    assert(events->cues[ORB_CUE_DETENT] == detents + 1);
    s_home_commit = true;
    assert(orb_screen_rim_step(host, 1, true));
    s_home_commit = false;
    assert(events->last_intent.kind == ORB_INTENT_HOME_SET_LEVEL);
    assert(events->last_intent.origin == ORB_INPUT_CROWN && events->last_intent.physical);
    assert(events->cues[ORB_CUE_DETENT] == detents + 2);
    const orb_visibility_t states[] = {ORB_VISIBILITY_COVERED, ORB_VISIBILITY_REDACTED, ORB_VISIBILITY_SUSPENDED};
    for (size_t i = 0; i < 3; i++) {
        assert(orb_screen_rim_acquire(&instance));
        assert(orb_screen_rim_begin(&instance, ORB_INPUT_TOUCH, true));
        unsigned cancelled = s_cancels;
        orb_screen_visibility(&instance, states[i]);
        assert(orb_screen_visibility_get(&instance) == states[i]);
        assert(lv_timer_get_paused(((list_state_t *)orb_screen_state(&instance))->timer));
        assert(!host->lease.instance && s_cancels == cancelled + 1);
        assert(!orb_screen_rim_step(host, 1, true));
        orb_screen_visibility(&instance, ORB_VISIBILITY_ACTIVE);
        assert(!lv_timer_get_paused(((list_state_t *)orb_screen_state(&instance))->timer));
    }
    assert(s_pauses >= 6);
    orb_screen_destroy(&passive);
    orb_screen_destroy(&instance);
    orb_nav_set_current(host->config.nav, s_stateless.id);
    quiescent();
}

static void intents_and_feedback(orb_screen_host_t *host, host_events_t *events)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, &s_stateless, lv_screen_active(), NULL, false));
    orb_intent_t request = {.kind = ORB_INTENT_SEND, .origin = ORB_INPUT_TOUCH, .physical = false};
    unsigned sent = events->intents;
    assert(!orb_screen_emit(&instance, &request));
    request.physical = true; request.origin = ORB_INPUT_CROWN;
    assert(!orb_screen_emit(&instance, &request));
    request.origin = ORB_INPUT_TOUCH; request.correlation = 123;
    assert(orb_screen_emit(&instance, &request));
    assert(events->intents == sent + 1 && events->last_intent.correlation == 123);
    unsigned cues = events->cues[ORB_CUE_DONE];
    assert(!orb_screen_feedback(&instance, orb_screen_event(host), ORB_CUE_DONE, ORB_INPUT_PROGRAMMATIC, true));
    assert(!orb_screen_feedback(&instance, orb_screen_event(host), ORB_CUE_DONE, ORB_INPUT_RESTORE, true));
    uint64_t cancelled = orb_screen_event(host);
    assert(!orb_screen_feedback(&instance, cancelled, ORB_CUE_DONE, ORB_INPUT_TOUCH, false));
    assert(!orb_screen_feedback(&instance, cancelled, ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
    uint64_t event = orb_screen_event(host);
    assert(orb_screen_feedback(&instance, event, ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
    assert(!orb_screen_feedback(&instance, event, ORB_CUE_ERROR, ORB_INPUT_TOUCH, true));
    assert(events->cues[ORB_CUE_DONE] == cues + 1);
    orb_screen_visibility(&instance, ORB_VISIBILITY_COVERED);
    assert(!orb_screen_emit(&instance, &request));
    orb_screen_destroy(&instance);
    assert(orb_screen_create(&instance, host, &s_stateless, lv_screen_active(), NULL, true));
    assert(!orb_screen_emit(&instance, &request));
    assert(!orb_screen_feedback(&instance, orb_screen_event(host), ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
    assert(!orb_screen_rim_acquire(&instance));
    orb_screen_destroy(&instance);
}

static void feedback_out_of_order(orb_screen_host_t *host, host_events_t *events)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, &s_stateless, lv_screen_active(), NULL, false));
    unsigned before = events->cues[ORB_CUE_DONE];
    uint64_t earlier = orb_screen_event(host), later = orb_screen_event(host);
    assert(earlier && later);
    assert(orb_screen_feedback(&instance, later, ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
    assert(orb_screen_feedback(&instance, earlier, ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
    assert(!orb_screen_feedback(&instance, earlier, ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
    for (unsigned i = 0; i < ORB_SCREEN_FEEDBACK_HISTORY + 1; i++) {
        uint64_t event = orb_screen_event(host);
        assert(event && orb_screen_feedback(&instance, event, ORB_CUE_DONE, ORB_INPUT_BUTTON, true));
    }
    assert(!orb_screen_feedback(&instance, earlier, ORB_CUE_DONE, ORB_INPUT_TOUCH, true));
    assert(events->cues[ORB_CUE_DONE] == before + ORB_SCREEN_FEEDBACK_HISTORY + 3);
    uint64_t pending[ORB_SCREEN_FEEDBACK_HISTORY];
    for (unsigned i = 0; i < ORB_SCREEN_FEEDBACK_HISTORY; i++) assert((pending[i] = orb_screen_event(host)));
    assert(orb_screen_event(host) == 0);
    for (unsigned i = ORB_SCREEN_FEEDBACK_HISTORY; i > 0; i--)
        assert(orb_screen_feedback(&instance, pending[i - 1], ORB_CUE_DONE, ORB_INPUT_BUTTON, true));
    orb_screen_destroy(&instance);
}

static void router_history(orb_screen_host_t *host, host_events_t *events)
{
    const orb_screen_descriptor_t *screens[] = {&s_stateless, &s_list, &s_layer};
    orb_app_manifest_t app = {1, "Examples", screens, 3, 1};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &app, NULL));
    orb_router_t router;
    assert(orb_router_init(&router, &registry, host, lv_screen_active(), 1, NULL));
    orb_launch_token_t token = {.identity = 91, .rect = {17, 29, 80, 80}, .valid = true};
    unsigned opens = events->cues[ORB_CUE_OPEN], closes = events->cues[ORB_CUE_CLOSE];
    assert(!orb_router_open(&router, 2, NULL, &token, false));
    assert(router.route.id == 1 && router.history_count == 0);
    assert(orb_router_open(&router, 2, &(orb_screen_args_t){.entity_id = 55}, &token, true));
    assert(router.route.id == 2 && router.history_count == 1);
    assert(router.route.launch.identity == 91 && router.route.launch.rect.x == 17);
    assert(host->config.nav->back_count == 1);
    s_back = ORB_BACK_CONSUMED;
    assert(orb_router_back(&router, true) == ORB_BACK_CONSUMED && router.route.id == 2);
    s_back = ORB_BACK_LEAVE_SCREEN;
    s_fail_create = true;
    assert(!orb_router_open(&router, 2, &(orb_screen_args_t){.entity_id = 66}, NULL, true));
    s_fail_create = false;
    assert(router.route.id == 2 && orb_screen_alive(router.current));
    assert(orb_router_back(&router, true) == ORB_BACK_LEAVE_SCREEN);
    assert(router.route.id == 1 && router.history_count == 0);
    assert(events->cues[ORB_CUE_OPEN] == opens + 1 && events->cues[ORB_CUE_CLOSE] == closes + 1);
    assert(orb_router_open(&router, 3, NULL, NULL, true));
    s_back = ORB_BACK_CLOSE_LAYER;
    assert(orb_router_back(&router, true) == ORB_BACK_CLOSE_LAYER && router.route.id == 1);
    s_back = ORB_BACK_LEAVE_SCREEN;
    orb_router_destroy(&router);
    quiescent();
}

static void router_same_screen(orb_screen_host_t *host)
{
    const orb_screen_descriptor_t *screens[] = {&s_stateless, &s_list};
    orb_app_manifest_t app = {1, "Examples", screens, 2, 1};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &app, NULL));
    host->config.project = project;
    orb_router_t router;
    assert(orb_router_init(&router, &registry, host, lv_screen_active(), 1, NULL));
    assert(orb_router_open(&router, 2, &(orb_screen_args_t){.entity_id = 55, .page = 1}, NULL, true));
    list_exercise(router.current);
    assert(orb_router_open(&router, 2, &(orb_screen_args_t){.entity_id = 66, .page = 3}, NULL, true));
    list_state_t *state = orb_screen_state(router.current);
    assert(state->entity_id == 66 && state->context.page == 3);
    assert(orb_router_back(&router, true) == ORB_BACK_LEAVE_SCREEN);
    state = orb_screen_state(router.current);
    assert(router.route.args.entity_id == 55 && state->entity_id == 55);
    assert(state->context.selected == 22 && state->context.focus == 33 && state->context.scroll == 75);
    orb_router_destroy(&router);
    host->config.project = NULL;
    quiescent();
}

static void router_stage_refusal(orb_screen_host_t *host, host_events_t *events)
{
    const orb_screen_descriptor_t *screens[] = {&s_stateless, &s_list};
    orb_app_manifest_t app = {1, "Examples", screens, 2, 1};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &app, NULL));
    host->config.project = project;
    lv_obj_t *source_parent = lv_obj_create(lv_screen_active());
    lv_obj_t *provisional = lv_obj_create(lv_screen_active());
    lv_obj_set_hidden(provisional, true);
    orb_router_t router;
    assert(orb_router_init(&router, &registry, host, source_parent, 1, NULL));
    const orb_screen_args_t source_args = {.entity_id = 55, .page = 1};
    const orb_screen_args_t target_args = {.entity_id = 66, .page = 3};
    assert(orb_router_open(&router, 2, &source_args, NULL, true));
    orb_screen_instance_t *source = router.current;
    lv_obj_t *source_root = orb_screen_root(source);
    list_exercise(source);
    /* A saved destination forces the different-route restore path; the last
     * case below also exercises restoration from live source scratch. */
    assert(orb_context_store_save_route(&router.contexts, source, &target_args));
    orb_context_store_t contexts_before;
    orb_nav_t nav_before;
    memcpy(&contexts_before, &router.contexts, sizeof(contexts_before));
    memcpy(&nav_before, host->config.nav, sizeof(nav_before));
    size_t memory_before = owned_memory(), history_before = router.history_count;
    unsigned timers_before = timer_count(), animations_before = lv_anim_count_running();
    unsigned observers_before = s_observers, asynchronous_before = s_async;
    unsigned cues_before[ORB_CUE_ERROR + 1];
    memcpy(cues_before, events->cues, sizeof(cues_before));
    for (unsigned refusal = 0; refusal < 3; refusal++) {
        s_fail_create = refusal == 0;
        s_fail_restore = refusal != 0;
        const orb_screen_args_t *args = refusal == 2 ? &source_args : &target_args;
        assert(!orb_router_stage(&router, &registry, host, provisional, 2, args, true));
        s_fail_create = s_fail_restore = false;
        assert(router.current == source && orb_screen_alive(source));
        assert(orb_screen_root(source) == source_root && lv_obj_get_parent(source_root) == source_parent);
        assert(orb_screen_visibility_get(source) == ORB_VISIBILITY_ACTIVE);
        assert(!orb_router_staged(&router));
        assert(router.route.id == 2 && router.route.args.entity_id == source_args.entity_id);
        assert(router.route.args.page == source_args.page && router.history_count == history_before);
        assert(!memcmp(host->config.nav, &nav_before, sizeof(nav_before)));
        assert(!memcmp(&router.contexts, &contexts_before, sizeof(contexts_before)));
        assert(!memcmp(events->cues, cues_before, sizeof(cues_before)));
        list_state_t *state = orb_screen_state(source);
        assert(state->context.selected == 22 && state->context.focus == 33 && state->context.scroll == 75);
        assert(host->lease.instance == source && !lv_timer_get_paused(state->timer));
        assert(owned_memory() == memory_before && timer_count() == timers_before);
        assert(lv_anim_count_running() == animations_before);
        assert(s_observers == observers_before && s_async == asynchronous_before);
        orb_router_cancel_staged(&router);
    }
    /* Source interaction still owns the same lease after every refusal. */
    assert(orb_screen_rim_step(host, 1, true));
    orb_router_destroy(&router);
    lv_obj_delete(source_parent);
    lv_obj_delete(provisional);
    host->config.project = NULL;
    quiescent();
}

static void update_subscriptions(orb_screen_host_t *host)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, &s_list, lv_screen_active(), NULL, false));
    orb_screen_model_t model = {&s_full, sizeof(s_full)};
    orb_screen_update_t update = {.revisions.model = 1, .model = &model};
    assert(orb_screen_update(&instance, &update));
    list_state_t *state = orb_screen_state(&instance);
    update.revisions.home++;
    assert(orb_screen_update(&instance, &update) && state->updates == 1);
    orb_ui_environment_t environment = {.revision = 1};
    update.environment = &environment;
    assert(orb_screen_update(&instance, &update) && state->updates == 2);
    update.revisions.model++; update.model = NULL;
    assert(orb_screen_update(&instance, &update) && state->updates == 3);
    assert(!state->context.selected && !state->context.focus && !state->context.scroll);
    orb_screen_destroy(&instance);
    quiescent();
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    assert(lv_display_create(466, 466));
    /* LVGL retains first-child bookkeeping for the screen parent's lifetime.
     * Warm that shared allocation before measuring individual view cleanup. */
    lv_obj_t *warm = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(warm);
    lv_obj_set_size(warm, 466, 466);
    lv_obj_delete(warm);
    descriptors_init();
    s_timer_baseline = timer_count();
    s_animation_baseline = lv_anim_count_running();
    lv_mem_monitor_t baseline = lvgl_memory();
    orb_screen_host_t host;
    orb_nav_t nav;
    host_events_t events = {.ui_bytes = 256};
    host_init(&host, &nav, &events);
    registry_validation();
    lifecycle_examples(&host);
    failed_create_and_parent_delete(&host);
    budget_rejection(&host);
    ui_budget_rejection(&host, &events);
    context_schema_values();
    context_store(&host);
    leases_and_visibility(&host, &events);
    intents_and_feedback(&host, &events);
    feedback_out_of_order(&host, &events);
    router_history(&host, &events);
    router_same_screen(&host);
    router_stage_refusal(&host, &events);
    update_subscriptions(&host);
    orb_test_screen_parent_delete();
    assert(owned_memory() == 0);
    lv_mem_monitor_t after = lvgl_memory();
    assert(after.total_size == baseline.total_size && after.used_cnt == baseline.used_cnt);
    puts("screen lifecycle: stateless/list, context, admission, routing, leases and feedback passed");
    return 0;
}
