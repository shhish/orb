/* Real Play descriptors and framework ownership. The runtime port fixture
 * supplies timer/display resources; game_flow separately runs the real game
 * runtime through lock/wake, overlays, BOOT and all five picker returns. */
#include "orb_app_play.h"
#include "orb_app_play_registry.h"
#include "orb_games_presentation.h"
#include "orb_play_host.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const orb_games_presentation_t *s_port;
static unsigned s_intents, s_creates, s_destroys, s_refreshes;
static bool s_fail;
static lv_timer_t *s_timer;
static orb_app_play_model_t s_model = {.system_input = true};
static orb_screen_model_t s_projection = {&s_model, sizeof(s_model)};

bool orb_games_set_presentation(const orb_games_presentation_t *port)
{
    s_port = port;
    return true;
}

static size_t used(orb_memory_owner_t owner)
{
    orb_memory_stats_t value;
    orb_memory_get(owner, &value);
    return value.used;
}

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static void settle(void)
{
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    assert(value->kind == ORB_INTENT_APP_COMMAND);
    s_intents++;
    return true;
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_APP_PLAY_SCREEN_ID);
    *update = (orb_screen_update_t){.model = &s_projection};
    return true;
}

static void exercise(orb_screen_instance_t *instance)
{
    orb_app_play_select(instance, 3);
    assert(orb_app_play_selected(instance) == ORB_GAME_SNAKE);
    assert(orb_app_play_selection_changed(instance));
    assert(!orb_app_play_selection_changed(instance));
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_app_play_picker(instance));
    orb_rim_binding_t binding;
    assert(orb_app_play_descriptor()->ops->rim_binding(instance, &binding));
    assert(binding.kind == ORB_RIM_FOCUS && binding.value == orb_app_play_selected(instance));
    assert(orb_app_play_selected(instance) ==
        (phase >= ORB_LIFECYCLE_RESTORED ? ORB_GAME_SNAKE : ORB_GAME_MARBLE));
    assert(!s_intents);
}

static void quiescent(void) { settle(); }

static void picker(void)
{
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_APP_PLAY_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent, .project = project});
    orb_screen_update_t initial = {.model = &s_projection};
    orb_screen_update_t changed = {.model = &s_projection, .revisions.time = 1};
    orb_screen_update_t empty = {.revisions.time = 2};
    orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){.descriptor = orb_app_play_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &changed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent});

    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, orb_app_play_descriptor(), lv_screen_active(), NULL, true));
    orb_intent_t command = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH, .physical = true,
        .value.command = {.target = ORB_APP_PLAY_SCREEN_ID, .action = ORB_APP_PLAY_LAUNCH, .value = ORB_GAME_POP}};
    assert(!orb_screen_emit(&preview, &command) && !s_intents);
    orb_screen_destroy(&preview);

    orb_app_host_t *live = orb_play_host_create(false, NULL, NULL), *clone = NULL;
    assert(live);
    lv_obj_t *first = lv_obj_create(lv_screen_active());
    assert(orb_play_host_attach(live, first, true, NULL, NULL));
    orb_app_play_select(orb_app_host_current(live), 4);
    assert(orb_app_host_clone(&clone, live, true));
    orb_app_host_detach_page(live, first);
    lv_obj_delete(first);
    lv_obj_t *second = lv_obj_create(lv_screen_active());
    assert(orb_play_host_attach(live, second, true, NULL, NULL));
    assert(orb_app_play_selected(orb_app_host_current(live)) == ORB_GAME_POP);
    lv_obj_t *copy = lv_obj_create(lv_screen_active());
    assert(orb_play_host_attach(clone, copy, true, NULL, NULL));
    assert(orb_app_play_selected(orb_app_host_current(clone)) == ORB_GAME_POP);
    orb_app_play_select(orb_app_host_current(clone), 1);
    assert(orb_app_play_selected(orb_app_host_current(live)) == ORB_GAME_POP);
    assert(!orb_screen_emit(orb_app_host_current(clone), &command));
    lv_obj_delete(copy);
    lv_obj_delete(second);
    orb_app_host_destroy(clone);
    orb_app_host_destroy(live);
    assert(!s_intents);
}

static void tick(lv_timer_t *timer) { (void)timer; }
static void refresh(lv_event_t *event) { (void)event; s_refreshes++; }

static bool view_create(lv_obj_t *parent, void *context)
{
    assert(parent && context == &s_creates);
    s_creates++;
    s_timer = lv_timer_create(tick, 17, context);
    assert(s_timer);
    assert(lv_display_add_event_cb(lv_display_get_default(), refresh, LV_EVENT_REFR_START, context));
    assert(lv_display_add_event_cb(lv_display_get_default(), refresh, LV_EVENT_REFR_READY, context));
    return !s_fail;
}

static void view_destroy(void *context)
{
    assert(context == &s_creates && s_timer);
    s_destroys++;
    lv_timer_delete(s_timer);
    s_timer = NULL;
    assert(lv_display_remove_event_cb_with_user_data(lv_display_get_default(), refresh, context) == 2);
}

static void game(void)
{
    size_t baseline = used(ORB_MEMORY_GAMES);
    unsigned timer_baseline = timers();
    assert(orb_app_play_init() && s_port);
    orb_screen_host_t invalid_host;
    orb_screen_host_init(&invalid_host, &(orb_screen_host_config_t){0});
    orb_screen_instance_t invalid = {0};
    assert(!orb_screen_create(&invalid, &invalid_host, orb_app_play_game_descriptor(), lv_screen_active(), NULL, false));
    assert(!orb_screen_create(&invalid, &invalid_host, orb_app_play_game_descriptor(), lv_screen_active(), NULL, true));
    assert(!s_creates && !s_destroys && used(ORB_MEMORY_GAMES) == baseline);
    orb_games_view_port_t view = {.create = view_create, .destroy = view_destroy, .context = &s_creates};
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    void *presentation = s_port->open(parent, &view);
    assert(presentation && s_creates == 1 && s_destroys == 0);
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, NULL);
    assert(s_refreshes == 1);
    s_port->close(presentation);
    assert(s_destroys == 1 && timers() == timer_baseline && used(ORB_MEMORY_GAMES) == baseline);
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, NULL);
    assert(s_refreshes == 1);
    s_fail = true;
    assert(!s_port->open(parent, &view));
    assert(s_creates == 2 && s_destroys == 2 && timers() == timer_baseline);
    assert(used(ORB_MEMORY_GAMES) == baseline);
    s_fail = false;
    presentation = s_port->open(parent, &view);
    assert(presentation);
    lv_obj_delete(parent);
    assert(s_creates == 3 && s_destroys == 3 && timers() == timer_baseline);
    s_port->close(presentation);
    assert(s_destroys == 3 && used(ORB_MEMORY_GAMES) == baseline);
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_play_manifest(), NULL));
    assert(orb_screen_registry_find(&registry, ORB_APP_PLAY_GAME_SCREEN_ID) == orb_app_play_game_descriptor());
    /* Warm the existing global font/material caches before lifetime receipts. */
    picker();
    settle();
    size_t workspace = used(ORB_MEMORY_WORKSPACE);
    unsigned timer_baseline = timers();
    picker();
    game();
    settle();
    assert(used(ORB_MEMORY_WORKSPACE) == workspace && timers() == timer_baseline);
    puts("app_play: picker context/preview and game partial/parent/explicit destruction passed");
    return 0;
}
