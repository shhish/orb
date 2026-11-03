/* Exercise the actual progressive descriptor and its opaque context owner. */
#include "apps_screen.h"
#include "orb_app.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_memory.h"
#include "orb_screen_lifecycle.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_now = 100000;
static unsigned s_commands, s_timer_baseline;
static size_t s_workspace_baseline;
static orb_intent_t s_last;
static lv_indev_data_t s_pointer;
static const unsigned s_actions[] = {ORB_SHELL_AGENTS, ORB_SHELL_DOT, ORB_SHELL_HOME,
    ORB_APPS_TIME, ORB_SHELL_PLAY, ORB_SHELL_UPDATES, ORB_SHELL_SETTINGS};

static uint32_t tick(void) { return s_now; }
static void advance(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 16) { s_now += 16; lv_timer_handler(); }
}
static size_t used(void)
{
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats); return stats.used;
}
static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}
static void quiescent(void)
{
    unsigned commands = s_commands;
    advance(1600);
    assert(used() == s_workspace_baseline && timers() == s_timer_baseline && commands == s_commands);
}
static lv_obj_t *page(void)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root); lv_obj_set_size(root, 466, 466); lv_obj_set_scrollable(root, false);
    return root;
}
static lv_obj_t *action(lv_obj_t *root, unsigned id)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == id) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = action(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static bool record(void *context, const orb_intent_t *intent)
{ (void)context; s_last = *intent; s_commands++; return true; }
static bool command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{ assert(orb_screen_alive(instance)); return record(context, intent); }
static bool allow(void *context, const orb_intent_t *intent)
{ (void)context; return intent->kind == ORB_INTENT_APP_COMMAND; }

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    /* Equal/changed projections never construct a disc or overwrite context. */
    assert(!action(orb_screen_root(instance), ORB_SHELL_AGENTS));
    unsigned char bytes[64] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    assert(orb_screen_save(instance, context, sizeof(bytes)));
    int32_t focused;
    memcpy(&focused, context->payload, sizeof(focused));
    assert(focused == (phase < ORB_LIFECYCLE_RESTORED ? ORB_SHELL_HOME : ORB_SHELL_DOT));
    orb_rim_binding_t binding;
    assert(!orb_apps_descriptor()->ops->rim_binding(instance, &binding));
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
}
static void exercise(orb_screen_instance_t *instance)
{
    assert(orb_apps_screen_build(instance, false, true));
    lv_obj_send_event(action(orb_screen_root(instance), ORB_SHELL_DOT), LV_EVENT_PRESSED, NULL);
    assert(s_last.value.command.action == ORB_APPS_FOCUS && s_last.value.command.extra == ORB_SHELL_DOT);
}
static void lifecycle(void)
{
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.intent = record,
        .allow_synthetic = allow, .defer_rim = true});
    orb_screen_update_t initial = {.revisions.device = 1};
    orb_screen_update_t changed = {.revisions.device = 2};
    orb_screen_update_t removed = {.revisions.device = 3};
    orb_screen_update_t empty = {.revisions.device = 4};
    orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){.descriptor = orb_apps_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &removed, .empty = &empty,
        .verify = verify, .exercise = exercise, .quiescent = quiescent});
}

static void progressive(void)
{
    orb_apps_view_t *view = orb_apps_view_create(false, command, NULL, NULL);
    lv_obj_t *parent = page();
    orb_screen_instance_t *instance = NULL;
    for (unsigned i = 0; i < 7; i++) {
        /* Time becomes available after attachment but before its own slice. */
        assert(orb_apps_view_build(view, parent, true, i >= 3) == (i == 6));
        if (!instance) instance = orb_apps_view_current(view);
        assert(instance && instance == orb_apps_view_current(view));
        lv_obj_t *root = orb_screen_root(instance);
        for (unsigned j = 0; j < 7; j++) assert((action(root, s_actions[j]) != NULL) == (j <= i));
        assert((orb_apps_view_caption(view) != NULL) == (i == 6));
        orb_screen_update_t update = {.revisions.device = i + 20};
        assert(orb_screen_update(instance, &update));
        assert(orb_screen_update(instance, &update));
        if (i < 6) assert(!action(root, s_actions[i + 1]));
    }
    assert(!lv_obj_has_state(action(orb_screen_root(instance), ORB_APPS_TIME), LV_STATE_DISABLED));
    lv_obj_t *caption = orb_apps_view_caption(view);
    assert(orb_apps_view_build(view, parent, true, false) && orb_apps_view_caption(view) == caption);
    assert(!lv_obj_has_state(action(orb_screen_root(instance), ORB_APPS_TIME), LV_STATE_DISABLED));
    orb_apps_view_detach(view, parent); lv_obj_delete(parent);
    parent = page();
    assert(orb_apps_view_build(view, parent, false, false));
    instance = orb_apps_view_current(view);
    assert(lv_obj_has_state(action(orb_screen_root(instance), ORB_APPS_TIME), LV_STATE_DISABLED));
    lv_obj_delete(parent);
    assert(!orb_apps_view_current(view));
    orb_apps_view_destroy(view);
    quiescent();
}

static bool s_route_active = true;
static bool route_active(void *context) { (void)context; return s_route_active; }

static void context_and_actions(bool preview)
{
    orb_apps_view_t *view = orb_apps_view_create(preview, command, route_active, NULL);
    orb_apps_view_t *copy = orb_apps_view_create(true, NULL, NULL, NULL);
    lv_obj_t *parent = page();
    assert(view && copy && orb_apps_view_build(view, parent, false, true));
    orb_screen_instance_t *instance = orb_apps_view_current(view);
    lv_obj_t *disc = action(orb_screen_root(instance), ORB_SHELL_DOT);
    unsigned before = s_commands;
    lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
    if (preview) {
        lv_obj_send_event(disc, LV_EVENT_CLICKED, NULL);
        assert(s_commands == before && orb_apps_view_focus(view) == ORB_SHELL_HOME);
    } else {
        assert(orb_apps_view_focus(view) == ORB_SHELL_DOT);
        assert(orb_apps_view_copy(copy, view));
        s_route_active = false;
        before = s_commands;
        lv_obj_send_event(disc, LV_EVENT_PRESS_LOST, NULL);
        assert(s_commands == before && orb_apps_view_focus(view) == ORB_SHELL_DOT);
        s_route_active = true;
        lv_obj_send_event(disc, LV_EVENT_PRESS_LOST, NULL);
        assert(s_last.value.command.action == ORB_APPS_CANCEL && orb_apps_view_focus(view) == ORB_SHELL_HOME);
        assert(orb_apps_view_focus(copy) == ORB_SHELL_DOT);
        lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(disc, LV_EVENT_RELEASED, NULL);
        lv_obj_update_layout(parent);
        lv_area_t bounds; lv_obj_get_coords(disc, &bounds);
        lv_obj_send_event(disc, LV_EVENT_CLICKED, NULL);
        assert(s_last.value.command.action == ORB_SHELL_DOT && s_last.value.command.target == ORB_SHELL_APPS);
        assert(s_last.launch.valid && s_last.launch.rect.x == bounds.x1 && s_last.launch.rect.y == bounds.y1);
        assert(s_last.launch.rect.width == bounds.x2 - bounds.x1 + 1 && !s_last.physical);
        lv_obj_add_state(disc, LV_STATE_PRESSED);
        orb_apps_prepare_launch(instance, ORB_SHELL_DOT);
        assert(!lv_obj_has_state(disc, LV_STATE_PRESSED));
        orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
        before = s_commands;
        lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(disc, LV_EVENT_CLICKED, NULL);
        assert(s_commands == before);
        orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    }
    unsigned char bytes[64] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    assert(orb_screen_save(instance, context, sizeof(bytes)));
    context->version++;
    assert(!orb_screen_restore(instance, context, sizeof(bytes)));
    before = s_commands;
    orb_apps_view_detach(view, parent); lv_obj_delete(parent);
    parent = page();
    assert(orb_apps_view_build(view, parent, false, true));
    assert(orb_apps_view_focus(view) == (preview ? ORB_SHELL_HOME : ORB_SHELL_DOT));
    orb_apps_view_destroy(view); lv_obj_delete(parent);
    parent = page();
    assert(orb_apps_view_build(copy, parent, false, true));
    assert(orb_apps_view_focus(copy) == (preview ? ORB_SHELL_HOME : ORB_SHELL_DOT));
    orb_apps_view_destroy(copy); lv_obj_delete(parent);
    assert(s_commands == before);
    quiescent();
}

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_pointer; }
static void pointer_send(lv_indev_t *input, int x, int y, lv_indev_state_t state)
{
    s_pointer.point = (lv_point_t){x, y}; s_pointer.state = state;
    lv_indev_read(input); advance(16);
}
static void physical_pointer(bool synthetic)
{
    orb_apps_view_t *view = orb_apps_view_create(false, command, NULL, NULL);
    lv_obj_t *parent = page();
    assert(view && orb_apps_view_build(view, parent, false, true));
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(input, pointer_read);
    if (synthetic) orb_input_mark_synthetic(input);
    pointer_send(input, 320, 183, LV_INDEV_STATE_PRESSED);
    assert(orb_apps_view_focus(view) == ORB_SHELL_DOT && s_last.physical == !synthetic);
    pointer_send(input, 400, 400, LV_INDEV_STATE_RELEASED);
    assert(orb_apps_view_focus(view) == ORB_SHELL_HOME && s_last.value.command.action == ORB_APPS_CANCEL);
    pointer_send(input, 320, 183, LV_INDEV_STATE_PRESSED);
    pointer_send(input, 320, 183, LV_INDEV_STATE_RELEASED);
    assert(orb_apps_view_focus(view) == ORB_SHELL_DOT && s_last.value.command.action == ORB_SHELL_DOT);
    lv_indev_delete(input); orb_apps_view_destroy(view); lv_obj_delete(parent);
    quiescent();
}

static void admission(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_apps_view_create(false, command, NULL, NULL));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    orb_apps_view_t *view = orb_apps_view_create(false, command, NULL, NULL);
    lv_obj_t *parent = page();
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats); reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    (void)orb_apps_view_build(view, parent, false, true);
    assert(!orb_apps_view_current(view));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_apps_view_build(view, parent, false, true) && orb_apps_view_current(view));
    orb_apps_view_detach(view, parent); lv_obj_delete(parent);
    parent = page();
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats); reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    (void)orb_apps_view_build(view, parent, false, true);
    assert(!orb_apps_view_current(view));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_apps_view_build(view, parent, false, true));
    orb_apps_view_destroy(view); lv_obj_delete(parent);
    quiescent();
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    const orb_screen_descriptor_t *descriptor = orb_apps_descriptor();
    assert(orb_screen_descriptor_valid(descriptor, NULL) && descriptor->id == ORB_SHELL_APPS);
    const orb_screen_descriptor_t *screens[] = {descriptor};
    orb_app_manifest_t manifest = {.id = descriptor->id, .name = "Apps", .screens = screens,
        .screen_count = 1, .entry = descriptor->id};
    orb_screen_registry_t registry; orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &manifest, NULL));
    orb_apps_view_t *warm = orb_apps_view_create(false, command, NULL, NULL);
    lv_obj_t *parent = page(); assert(orb_apps_view_build(warm, parent, false, true));
    orb_apps_view_destroy(warm); lv_obj_delete(parent); advance(1600);
    s_workspace_baseline = used(); s_timer_baseline = timers();
    lifecycle(); progressive(); context_and_actions(false); context_and_actions(true);
    physical_pointer(false); physical_pointer(true); admission();
    puts("apps_screen: lifecycle, progressive construction, focus/cancel, context, input, preview and admission passed");
    return 0;
}
