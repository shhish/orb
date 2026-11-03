#include "orb_neighbour.h"
#include "orb_home_host.h"
#include "orb_time_host.h"
#include "orb_faces_host.h"
#include "orb_time.h"
#include "orb_memory.h"
#include "orb_system_copy.h"
#include "orb_ui.h"
#include "control_screen.h"
#include "stack_screen.h"
#include "system/charging_screen.h"
#include "system/system_views.h"
#include "orb_system_state.h"
#include "orb_page.h"
#include "orb_sound.h"
#include "orb_apps.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t tick(void) { return 1000; }

static size_t used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static void publish(void)
{
    orb_home_room_t room = {.id = "living", .name = "Living Room"};
    orb_home_accessory_t light = {.id = "lamp", .name = "Lamp", .room = "living", .kind = ORB_HOME_LIGHT,
        .reachable = true, .writable = ORB_HOME_ON | ORB_HOME_BRIGHTNESS,
        .state = {.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS, .on = true, .brightness = 40}};
    assert(orb_home_snapshot_chunk_available(&room, 1, &light, 1, NULL, 0, true, true, false));
    orb_home_set_connection(true, true);
    orb_model_set_connection(true, true);
}

static workspace_snapshots_t snapshots(void)
{
    orb_model_snapshot_t model;
    orb_home_snapshot_t home;
    assert(orb_model_snapshot(&model));
    orb_home_snapshot(&home);
    workspace_snapshots_t value;
    assert(orb_workspace_snapshots_init(&value, NULL, &model, &home));
    return value;
}

static lv_obj_t *page(void)
{
    lv_obj_t *object = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, 466, 466);
    return object;
}

static void round_trip(orb_screen_id_t route, bool source_first)
{
    publish();
    size_t before = used();
    unsigned before_timers = timers();
    workspace_snapshots_t source = snapshots();
    workspace_model_t *model = source.model;
    workspace_home_t *home = source.home;
    lv_obj_t *parent = page();
    orb_app_host_t *host = route == ORB_APP_HOME_SCREEN_ID ?
        orb_home_host_create(false, NULL) : orb_time_host_create(false);
    assert(host);
    if (route == ORB_APP_HOME_SCREEN_ID) {
        assert(orb_home_host_attach(host, parent, &(orb_app_home_model_t){.hour = 19, .fixture = &home->value}));
        orb_app_home_show_scenes(orb_app_host_current(host));
    } else {
        assert(orb_time_host_attach(host, parent, &(orb_app_time_model_t){0}, false, false));
        /* Select a remembered utility page through its published value schema. */
        _Alignas(max_align_t) unsigned char bytes[ORB_SCREEN_CONTEXT_MAX];
        orb_screen_context_t *context = (orb_screen_context_t *)bytes;
        orb_screen_instance_t *screen = orb_app_host_current(host);
        assert(orb_screen_save(screen, context, sizeof(bytes)));
        const orb_context_field_t *field = orb_screen_descriptor(screen)->context_fields;
        assert(field->kind == ORB_CONTEXT_U32);
        uint32_t selection = ORB_TIME_PAGE_STOPWATCH;
        memcpy(context->payload + field->offset, &selection, sizeof(selection));
        assert(orb_screen_restore(screen, context, sizeof(bytes)));
    }
    _Alignas(max_align_t) unsigned char saved[ORB_SCREEN_CONTEXT_MAX], copied[ORB_SCREEN_CONTEXT_MAX];
    assert(orb_screen_save(orb_app_host_current(host), (orb_screen_context_t *)saved, sizeof(saved)));
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source, .context = host, .hour = 19});
    assert(scene && orb_neighbour_page(scene) && orb_neighbour_screen(scene));
    assert(model->references == 2 && home->references == 2);
    assert(orb_screen_save(orb_neighbour_screen(scene), (orb_screen_context_t *)copied, sizeof(copied)));
    assert(!memcmp(saved, copied, ((orb_screen_context_t *)saved)->size));
    assert(orb_screen_host(orb_neighbour_screen(scene))->config.preview);
    assert(!orb_screen_emit(orb_neighbour_screen(scene), &(orb_intent_t){
        .kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH, .physical = true}));
    orb_time_snapshot_t time_before, time_after;
    orb_time_snapshot(orb_time_now_us(), &time_before);
    if (route == ORB_APP_TIME_SCREEN_ID) {
        assert(orb_app_time_page(orb_neighbour_screen(scene)) == ORB_TIME_PAGE_STOPWATCH);
        orb_time_rotary_t binding;
        if (orb_app_time_rotary(orb_neighbour_screen(scene), &binding))
            orb_app_time_rotate(orb_neighbour_screen(scene), binding.value + binding.detent);
        orb_app_time_rotary_finish(orb_neighbour_screen(scene), true);
    }
    orb_time_snapshot(orb_time_now_us(), &time_after);
    assert(time_before.generation == time_after.generation && time_before.timer_state == time_after.timer_state &&
        time_before.stopwatch_state == time_after.stopwatch_state);
    orb_home_request_t request;
    assert(!orb_home_take_request(&request));
    if (source_first) {
        const orb_model_snapshot_t *model_view = &source.model->value;
        const orb_home_snapshot_t *home_view = &source.home->value;
        orb_model_set_connection(false, false);
        orb_home_set_connection(false, false);
        assert(orb_workspace_model_refresh(&source.model, &model_view));
        assert(orb_workspace_home_refresh(&source.home, &home_view));
        assert(source.model != model && source.home != home);
        assert(model->references == 1 && home->references == 1);
        orb_app_host_destroy(host);
        lv_obj_delete(parent);
        orb_workspace_snapshots_release(&source);
        lv_obj_update_layout(orb_neighbour_page(scene));
        assert(orb_screen_save(orb_neighbour_screen(scene), (orb_screen_context_t *)copied, sizeof(copied)));
        orb_neighbour_destroy(scene);
    } else {
        /* Parent deletion retires the descriptor before later explicit close. */
        lv_obj_delete(lv_obj_get_parent(orb_neighbour_page(scene)));
        assert(!orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
        orb_neighbour_destroy(scene);
        assert(model->references == 1 && home->references == 1);
        orb_app_host_destroy(host);
        lv_obj_delete(parent);
        orb_workspace_snapshots_release(&source);
    }
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
}

static void refusal(orb_screen_id_t route)
{
    publish();
    workspace_snapshots_t source = snapshots();
    orb_app_host_t *host = route == ORB_APP_HOME_SCREEN_ID ?
        orb_home_host_create(true, NULL) : orb_time_host_create(true);
    assert(host);
    size_t before = used();
    /* Snapshot payload telemetry is tracked without reservation on this host.
     * Exhaust actual admission rather than subtracting tracked used bytes. */
    size_t reserved = 0;
    while (orb_memory_reserve(ORB_MEMORY_WORKSPACE, 4096)) reserved += 4096;
    while (orb_memory_reserve(ORB_MEMORY_WORKSPACE, 1)) reserved++;
    /* A refused opaque clone releases both retained generations. */
    assert(!orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source, .context = host}));
    assert(source.model->references == 1 && source.home->references == 1);
    /* With no prior host the old caller ignored initial host/attach refusal.
     * Preserve its black-page presentation, rather than fix it structurally. */
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source});
    assert(scene && orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
    orb_neighbour_destroy(scene);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserved);
    assert(used() == before && source.model->references == 1 && source.home->references == 1);
    scene = orb_neighbour_create(lv_screen_active(), route, &(orb_neighbour_source_t){.snapshots = &source});
    assert(scene && orb_neighbour_screen(scene));
    orb_neighbour_destroy(scene);
    assert(used() == before);
    orb_app_host_destroy(host);
    orb_workspace_snapshots_release(&source);
}

static orb_screen_update_t projection(orb_screen_instance_t *screen)
{
    orb_screen_host_t *host = orb_screen_host(screen);
    orb_screen_update_t value = {0};
    assert(host->config.project(host->config.context, orb_screen_descriptor(screen)->id, &value));
    assert(value.model && value.model->data);
    return value;
}

static void system_scene(orb_screen_id_t route, unsigned page_index, bool parent_first)
{
    publish();
    size_t before = used();
    unsigned before_timers = timers();
    workspace_snapshots_t source = snapshots();
    workspace_model_t *model = source.model;
    workspace_home_t *home = source.home;
    orb_shell_snapshot_t status = {.brightness = 71, .battery_pct = 62, .usb = true, .charging = true};
    orb_route_context_t place = {0};
    if (page_index != UINT32_MAX) orb_route_context_save(&place, 111, page_index, 480);
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source, .status = &status, .place = &place,
            .hour = 19, .contrast = true, .silent = true, .control_volume = true});
    assert(scene && orb_neighbour_screen(scene));
    orb_screen_instance_t *screen = orb_neighbour_screen(scene);
    assert(orb_screen_descriptor(screen)->id == route);
    assert(model->references == 2 && home->references == 2);
    assert(orb_screen_host(screen)->config.preview);
    assert(!orb_screen_page_viewport(screen));
    assert(lv_obj_get_parent(orb_screen_page_title(screen)) == orb_neighbour_page(scene));
    assert(!orb_screen_emit(screen, &(orb_intent_t){.kind = ORB_INTENT_APP_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = true,
        .value.command = {.target = route, .action = ORB_CONTROL_HOME_FIRST}}));
    orb_screen_context_t context;
    assert(orb_screen_save(screen, &context, sizeof(context)));
    assert(context.version == 1 && context.size == sizeof(context));
    orb_screen_update_t value = projection(screen);
    if (route == ORB_SHELL_CHARGING) {
        assert(value.model->size == sizeof(orb_charging_model_t));
        const orb_charging_model_t *power = value.model->data;
        assert(power->battery_pct == 62 && power->usb && power->charging && power->contrast);
    } else {
        assert(value.model->size == sizeof(orb_control_model_t));
        const orb_control_model_t *control = value.model->data;
        assert(control->brightness == 71 && control->battery_pct == 62 && control->charging);
        assert(control->volume == orb_sound_volume() && control->silent && control->control_volume);
        assert(!control->quiet && !control->flashlight);
        assert(control->home_panel == (page_index == 1));
        assert((orb_control_level(screen) != NULL) == (page_index != 1));
        if (page_index == 1) assert(control->home.ready);
    }
    /* Projections and the route record are call-scoped. Neither may be read
     * again after construction, including after source COW and destruction. */
    unsigned char saved[sizeof(orb_control_model_t)];
    assert(value.model->size <= sizeof(saved));
    memcpy(saved, value.model->data, value.model->size);
    memset(&status, 0, sizeof(status));
    memset(&place, 0, sizeof(place));
    const orb_model_snapshot_t *model_view = &source.model->value;
    const orb_home_snapshot_t *home_view = &source.home->value;
    orb_model_set_connection(false, false);
    orb_home_set_connection(false, false);
    assert(orb_workspace_model_refresh(&source.model, &model_view));
    assert(orb_workspace_home_refresh(&source.home, &home_view));
    assert(source.model != model && source.home != home);
    orb_workspace_snapshots_release(&source);
    assert(model->references == 1 && home->references == 1);
    lv_obj_update_layout(orb_neighbour_page(scene));
    value = projection(screen);
    assert(!memcmp(saved, value.model->data, value.model->size));
    orb_home_request_t request;
    assert(!orb_home_take_request(&request));
    if (parent_first) {
        lv_obj_delete(lv_obj_get_parent(orb_neighbour_page(scene)));
        assert(!orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
    }
    orb_neighbour_destroy(scene);
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
}

static void system_refusal(orb_screen_id_t route)
{
    publish();
    workspace_snapshots_t source = snapshots();
    orb_shell_snapshot_t status = {0};
    size_t before = used();
    unsigned before_timers = timers();
    size_t reserved = 0;
    while (orb_memory_reserve(ORB_MEMORY_WORKSPACE, 4096)) reserved += 4096;
    while (orb_memory_reserve(ORB_MEMORY_WORKSPACE, 1)) reserved++;
    /* These routes never inherited a live app context. Initial admission
     * failure retains the compatibility caller's captured black page. */
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source, .status = &status});
    assert(scene && orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
    orb_neighbour_destroy(scene);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserved);
    assert(used() == before && source.model->references == 1 && source.home->references == 1);
    scene = orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source, .status = &status});
    assert(scene && orb_neighbour_screen(scene));
    orb_neighbour_destroy(scene);
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
    orb_workspace_snapshots_release(&source);
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

static void prepared(void *context)
{
    unsigned *count = context;
    (*count)++;
}

static void scrolled(lv_event_t *event)
{
    unsigned *count = lv_event_get_user_data(event);
    (*count)++;
}

static void ordinary_scene(orb_screen_id_t route, bool saved_place, bool parent_first)
{
    publish();
    size_t before = used();
    unsigned before_timers = timers(), prepare_count = 0, scroll_count = 0;
    workspace_snapshots_t source = snapshots();
    workspace_model_t *model = source.model;
    workspace_home_t *home = source.home;
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .firmware_version = "v0.5.2-preview",
        .update_check_result = "Update failed.\nTry again.\nMac offline.\nConnect.\nCheck again."};
    orb_route_context_t place = {0};
    int scroll = route == ORB_SHELL_ONBOARDING ? 120 : 20;
    if (saved_place) orb_route_context_save(&place, scroll, 9, 31);
    unsigned save_state = (orb_system_onboarding_saving() ? 1u : 0u) |
        (orb_system_onboarding_save_error() ? 2u : 0u);
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source, .status = &status, .place = &place,
            .prepare = prepared, .prepare_context = &prepare_count});
    assert(scene && orb_neighbour_screen(scene) && prepare_count == 1);
    assert(model->references == 2 && home->references == 2);
    orb_screen_instance_t *screen = orb_neighbour_screen(scene);
    assert(orb_screen_descriptor(screen)->id == route && orb_screen_host(screen)->config.preview);
    lv_obj_t *list = orb_screen_page_viewport(screen);
    assert(list && lv_obj_get_parent(orb_screen_page_title(screen)) == orb_neighbour_page(scene));
    assert(lv_obj_get_scroll_y(list) == (saved_place ? scroll : 0));
    lv_obj_add_event_cb(list, scrolled, LV_EVENT_SCROLL, &scroll_count);
    orb_neighbour_settle(scene);
    assert(scroll_count == 3);
    orb_screen_context_t context;
    assert(orb_screen_save(screen, &context, sizeof(context)) && context.version == 1 && context.size == sizeof(context));
    unsigned command = route == ORB_SHELL_ONBOARDING ? ORB_SYSTEM_ONBOARD_DONE : ORB_SYSTEM_CHECK_UPDATE;
    lv_obj_t *button = action(orb_neighbour_page(scene), command);
    assert(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    assert(!orb_screen_emit(screen, &(orb_intent_t){.kind = ORB_INTENT_APP_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = true,
        .value.command = {.target = route, .action = command}}));
    orb_screen_update_t value = projection(screen);
    if (route == ORB_SHELL_ONBOARDING) {
        assert(value.model->size == sizeof(orb_onboarding_model_t));
        assert(((const orb_onboarding_model_t *)value.model->data)->save_state == save_state);
        assert(save_state == ((orb_system_onboarding_saving() ? 1u : 0u) |
            (orb_system_onboarding_save_error() ? 2u : 0u)));
    } else {
        assert(value.model->size == sizeof(orb_updates_model_t));
        const orb_updates_model_t *update = value.model->data;
        assert(update->online && !update->pending && !strcmp(update->firmware_version, status.firmware_version));
        assert(!strcmp(update->result, status.update_check_result));
    }
    unsigned char saved[sizeof(orb_updates_model_t)];
    assert(value.model->size <= sizeof(saved));
    memcpy(saved, value.model->data, value.model->size);
    memset(&status, 0, sizeof(status));
    memset(&place, 0, sizeof(place));
    const orb_model_snapshot_t *model_view = &source.model->value;
    const orb_home_snapshot_t *home_view = &source.home->value;
    orb_model_set_connection(false, false);
    orb_home_set_connection(false, false);
    assert(orb_workspace_model_refresh(&source.model, &model_view));
    assert(orb_workspace_home_refresh(&source.home, &home_view));
    assert(source.model != model && source.home != home);
    orb_workspace_snapshots_release(&source);
    assert(model->references == 1 && home->references == 1);
    orb_neighbour_settle(scene);
    value = projection(screen);
    assert(!memcmp(saved, value.model->data, value.model->size));
    if (parent_first) {
        lv_obj_delete(lv_obj_get_parent(orb_neighbour_page(scene)));
        assert(!orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
        orb_neighbour_settle(scene);
    }
    orb_neighbour_destroy(scene);
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
}

static void apps_scene(bool saved_place, bool parent_first, bool time_available)
{
    publish();
    size_t before = used();
    unsigned before_timers = timers(), prepare_count = 0;
    workspace_snapshots_t source = snapshots();
    lv_obj_t *parent = page();
    orb_apps_view_t *apps = orb_apps_view_create(false, NULL, NULL, NULL);
    assert(apps && orb_apps_view_build(apps, parent, false, true));
    orb_screen_instance_t *live = orb_apps_view_current(apps);
    _Alignas(max_align_t) unsigned char bytes[ORB_SCREEN_CONTEXT_MAX];
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    assert(orb_screen_save(live, context, sizeof(bytes)));
    const orb_context_field_t *fields = orb_screen_descriptor(live)->context_fields;
    int32_t focused = ORB_SHELL_DOT, previous = ORB_SHELL_SETTINGS;
    assert(orb_screen_descriptor(live)->context_field_count == 2);
    assert(fields[0].kind == ORB_CONTEXT_I32 && fields[1].kind == ORB_CONTEXT_I32);
    memcpy(context->payload + fields[0].offset, &focused, sizeof(focused));
    memcpy(context->payload + fields[1].offset, &previous, sizeof(previous));
    assert(orb_screen_restore(live, context, sizeof(bytes)));
    orb_route_context_t place = {0};
    if (saved_place) orb_route_context_save(&place, 123, 9, ORB_SHELL_PLAY);
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), ORB_SHELL_APPS,
        &(orb_neighbour_source_t){.snapshots = &source, .apps = apps, .place = &place,
            .prepare = prepared, .prepare_context = &prepare_count});
    assert(scene && orb_neighbour_screen(scene) && prepare_count == 1);
    orb_screen_instance_t *screen = orb_neighbour_screen(scene);
    assert(orb_screen_descriptor(screen)->id == ORB_SHELL_APPS);
    assert(orb_screen_host(screen)->config.preview);
    assert(!orb_screen_page_title(screen) && !orb_screen_page_viewport(screen));
    assert(orb_screen_save(screen, context, sizeof(bytes)));
    memcpy(&focused, context->payload + fields[0].offset, sizeof(focused));
    memcpy(&previous, context->payload + fields[1].offset, sizeof(previous));
    assert(focused == (saved_place ? ORB_SHELL_PLAY : ORB_SHELL_DOT) && previous == 0);
    lv_obj_t *time = action(orb_neighbour_page(scene), ORB_APPS_TIME);
    assert(time && lv_obj_has_state(time, LV_STATE_DISABLED) == !time_available);
    lv_obj_t *dot = action(orb_neighbour_page(scene), ORB_SHELL_DOT);
    assert(dot);
    unsigned char saved[ORB_SCREEN_CONTEXT_MAX];
    memcpy(saved, bytes, context->size);
    lv_obj_send_event(dot, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(dot, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_send_event(dot, LV_EVENT_CLICKED, NULL);
    assert(!orb_screen_emit(screen, &(orb_intent_t){.kind = ORB_INTENT_APP_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = true,
        .value.command = {.target = ORB_SHELL_APPS, .action = ORB_SHELL_DOT}}));
    assert(orb_screen_save(screen, context, sizeof(bytes)) && !memcmp(saved, bytes, context->size));
    orb_apps_view_destroy(apps);
    lv_obj_delete(parent);
    orb_workspace_snapshots_release(&source);
    memset(&place, 0, sizeof(place));
    lv_obj_update_layout(orb_neighbour_page(scene));
    assert(orb_screen_save(screen, context, sizeof(bytes)) && !memcmp(saved, bytes, context->size));
    if (parent_first) {
        lv_obj_delete(lv_obj_get_parent(orb_neighbour_page(scene)));
        assert(!orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
    }
    orb_neighbour_destroy(scene);
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
}

static void apps_refusal(void)
{
    publish();
    workspace_snapshots_t source = snapshots();
    size_t before = used(), reserved = 0;
    unsigned before_timers = timers(), prepare_count = 0;
    while (orb_memory_reserve(ORB_MEMORY_WORKSPACE, 4096)) reserved += 4096;
    while (orb_memory_reserve(ORB_MEMORY_WORKSPACE, 1)) reserved++;
    orb_neighbour_source_t input = {.snapshots = &source,
        .prepare = prepared, .prepare_context = &prepare_count};
    assert(!orb_neighbour_create(lv_screen_active(), ORB_SHELL_APPS, &input));
    assert(!prepare_count && source.model->references == 1 && source.home->references == 1);
    /* Exactly the mandatory owner fits, but descriptor admission does not. */
    orb_memory_release(ORB_MEMORY_WORKSPACE, orb_apps_view_size());
    reserved -= orb_apps_view_size();
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), ORB_SHELL_APPS, &input);
    assert(scene && orb_neighbour_page(scene) && !orb_neighbour_screen(scene) && prepare_count == 1);
    orb_neighbour_destroy(scene);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserved);
    assert(used() == before && source.model->references == 1 && source.home->references == 1);
    scene = orb_neighbour_create(lv_screen_active(), ORB_SHELL_APPS, &input);
    assert(scene && orb_neighbour_screen(scene) && prepare_count == 2);
    orb_neighbour_destroy(scene);
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
    orb_workspace_snapshots_release(&source);
}

static void faces_scene(orb_screen_id_t route, bool parent_first)
{
    publish();
    size_t before = used();
    unsigned before_timers = timers(), prepare_count = 0;
    workspace_snapshots_t source = snapshots();
    workspace_model_t *model = source.model;
    workspace_home_t *home = source.home;
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .clock_time = "7:05 PM", .clock_date = "Wed, Oct 28", .battery_pct = 62, .charging = true};
    lv_obj_t *parent = page();
    orb_app_host_t *live = orb_faces_host_create(false, NULL, NULL);
    assert(live && orb_faces_host_attach(live, parent));
    orb_app_faces_hold(orb_app_host_current(live));
    assert(orb_app_faces_mode(orb_app_host_current(live)) == ORB_FACES_GALLERY);
    _Alignas(max_align_t) unsigned char live_before[ORB_SCREEN_CONTEXT_MAX], live_after[ORB_SCREEN_CONTEXT_MAX];
    assert(orb_screen_save(orb_app_host_current(live), (orb_screen_context_t *)live_before, sizeof(live_before)));
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), route,
        &(orb_neighbour_source_t){.snapshots = &source, .status = &status,
            .prepare = prepared, .prepare_context = &prepare_count});
    assert(scene && orb_neighbour_screen(scene) && prepare_count == 1);
    orb_screen_instance_t *screen = orb_neighbour_screen(scene);
    assert(orb_screen_descriptor(screen)->id == ORB_APP_FACES_SCREEN_ID);
    assert(orb_screen_host(screen)->config.preview);
    assert(orb_app_faces_mode(screen) == ORB_FACES_FACE && !orb_app_faces_retained(screen));
    assert(!orb_screen_page_title(screen) && !orb_screen_page_viewport(screen));
    assert(model->references == 2 && home->references == 2);
    assert(!orb_screen_emit(screen, &(orb_intent_t){.kind = ORB_INTENT_APP_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = true,
        .value.command = {.action = ORB_APP_FACES_OPEN, .value = ORB_FACES_OPEN_HOME}}));
    assert(!orb_screen_emit(screen, &(orb_intent_t){.kind = ORB_INTENT_APP_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = true, .value.command.action = ORB_APP_FACES_PREFERENCES}));
    orb_screen_update_t value = projection(screen);
    assert(value.model->size == sizeof(orb_app_faces_model_t));
    const orb_faces_snapshot_t *face = &((const orb_app_faces_model_t *)value.model->data)->snapshot;
    assert(face->battery_percent == 62 && face->charging && face->online);
    assert(face->home_on == 1 && face->home_total == 1 && !face->home_stale);
    assert(!strcmp(face->weekday, "Wed") && !strcmp(face->day, "28") && face->clock[0]);
    orb_app_faces_model_t saved;
    memcpy(&saved, value.model->data, sizeof(saved));
    assert(orb_screen_save(orb_app_host_current(live), (orb_screen_context_t *)live_after, sizeof(live_after)));
    assert(!memcmp(live_before, live_after, ((orb_screen_context_t *)live_before)->size));
    orb_app_host_destroy(live);
    lv_obj_delete(parent);
    memset(&status, 0, sizeof(status));
    const orb_model_snapshot_t *model_view = &source.model->value;
    const orb_home_snapshot_t *home_view = &source.home->value;
    orb_model_set_connection(false, false);
    orb_home_set_connection(false, false);
    assert(orb_workspace_model_refresh(&source.model, &model_view));
    assert(orb_workspace_home_refresh(&source.home, &home_view));
    assert(source.model != model && source.home != home);
    orb_workspace_snapshots_release(&source);
    assert(model->references == 1 && home->references == 1);
    orb_neighbour_settle(scene);
    value = projection(screen);
    assert(!memcmp(&saved, value.model->data, sizeof(saved)));
    if (parent_first) {
        lv_obj_delete(lv_obj_get_parent(orb_neighbour_page(scene)));
        assert(!orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
    }
    orb_neighbour_destroy(scene);
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
}

static lv_obj_t *text(lv_obj_t *root, const char *value)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}

typedef struct {
    orb_time_snapshot_t *time;
    orb_shell_snapshot_t *status;
    orb_route_context_t *place;
    char *card;
    unsigned count;
} stack_prepare_t;

static void stack_prepared(void *context)
{
    stack_prepare_t *prepare = context;
    prepare->count++;
    prepare->time->timer_remaining_us = 0;
    snprintf(prepare->status->clock_time, sizeof(prepare->status->clock_time), "7:05 PM");
    snprintf(prepare->card, ORB_MODEL_CARD_ID_MAX + 1, "second");
    orb_route_context_save(prepare->place, 20, 3, 99);
}

static void stack_scene(unsigned saved_page, const char *override, bool parent_first, bool ordered)
{
    publish();
    size_t before = used();
    unsigned before_timers = timers(), scroll_count = 0;
    workspace_snapshots_t source = snapshots();
    workspace_model_t *model = source.model;
    workspace_home_t *home = source.home;
    model->value = (orb_model_snapshot_t){.online = true, .synced = true, .card_count = 2,
        .cards = {{.id = "first", .title = "First card", .kind = ORB_CARD_MESSAGE, .priority = ORB_PRIORITY_HIGH},
                  {.id = "second", .title = "Second card", .kind = ORB_CARD_APPROVAL, .priority = ORB_PRIORITY_LOW}}};
    for (unsigned i = 0; i < 2; i++)
        snprintf(model->value.cards[i].body, sizeof(model->value.cards[i].body),
            "First line\nSecond line\nThird line\nFourth line\nFifth line\nSixth line\nSeventh line\nEighth line");
    orb_time_snapshot_t time = {.timer_state = ORB_TIME_RUNNING, .timer_remaining_us = 91000000};
    orb_shell_snapshot_t status = {.clock_time = "7:05 PM", .clock_date = "Wed, Oct 28"};
    char card[ORB_MODEL_CARD_ID_MAX + 1];
    snprintf(card, sizeof(card), "%s", override);
    orb_route_context_t place = {0};
    if (saved_page != UINT32_MAX) orb_route_context_save(&place, 20, saved_page, 99);
    stack_prepare_t prepare = {.time = &time, .status = &status, .place = &place, .card = card};
    if (ordered) snprintf(status.clock_time, sizeof(status.clock_time), "invalid");
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), ORB_SHELL_STACK,
        &(orb_neighbour_source_t){.snapshots = &source, .status = &status, .place = &place,
            .time = &time, .card = card, .prepare = ordered ? stack_prepared : NULL, .prepare_context = &prepare});
    assert(scene && orb_neighbour_screen(scene) && prepare.count == (ordered ? 1u : 0u));
    assert(model->references == 2 && home->references == 2);
    orb_screen_instance_t *screen = orb_neighbour_screen(scene);
    assert(orb_screen_descriptor(screen)->id == ORB_SHELL_STACK && orb_screen_host(screen)->config.preview);
    assert(lv_obj_get_parent(orb_screen_page_title(screen)) == orb_neighbour_page(scene));
    _Alignas(max_align_t) unsigned char context_bytes[ORB_SCREEN_CONTEXT_MAX];
    orb_screen_context_t *context = (orb_screen_context_t *)context_bytes;
    const orb_screen_descriptor_t *descriptor = orb_screen_descriptor(screen);
    assert(orb_screen_save(screen, context, sizeof(context_bytes)) &&
        context->version == descriptor->context_version && context->size == descriptor->context_bytes);
    orb_screen_update_t value = projection(screen);
    assert(value.model->size == sizeof(orb_stack_source_t));
    const orb_stack_source_t *port = value.model->data;
    orb_stack_read_t read;
    assert(port->read(port->context, &read));
    /* Two cards, a running timer, Home and the date floor. The saved focus is
     * irrelevant; page clamps independently of an explicit card override. */
    unsigned expected_page = ordered ? 3 : saved_page == UINT32_MAX ? 0 : saved_page > 4 ? 4 : saved_page;
    assert(read.page_index == expected_page && read.clock_hour == 19);
    assert(read.time->timer_remaining_us == 91000000 && read.time != &time);
    assert(read.model == &model->value && read.home == &home->value);
    assert(read.stack_surface && !read.compact_time && !read.notice[0]);
    assert(!strcmp(read.card, ordered ? "second" : override) && read.card != card);
    assert(!strcmp(read.clock_time, "7:05 PM") && read.clock_time != status.clock_time);
    assert(!strcmp(read.clock_date, "Wed, Oct 28") && read.clock_date != status.clock_date);
    bool missing = !strcmp(override, "missing") && !ordered;
    bool card_reader = !missing && (ordered || override[0] || expected_page < 2);
    lv_obj_t *list = orb_screen_page_viewport(screen);
    assert((list != NULL) == (card_reader || (!missing && expected_page == 3)));
    if (card_reader) {
        assert(text(orb_neighbour_page(scene), ordered || override[0] || expected_page == 1 ? "Second card" : "First card"));
        assert(lv_obj_get_scroll_y(list) == (saved_page != UINT32_MAX || ordered ? 20 : 0));
    }
    if (missing) assert(text(orb_neighbour_page(scene), "All caught up"));
    if (list) lv_obj_add_event_cb(list, scrolled, LV_EVENT_SCROLL, &scroll_count);
    orb_neighbour_settle(scene);
    assert(scroll_count == (list ? 3u : 0u));
    assert(!orb_screen_emit(screen, &(orb_intent_t){.kind = ORB_INTENT_APP_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = true,
        .value.command = {.target = ORB_SHELL_STACK, .action = 160}}));
    for (unsigned i = 160; i <= 161; i++) {
        lv_obj_t *button = action(orb_neighbour_page(scene), i);
        if (button) lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    }
    memset(&status, 0, sizeof(status)); memset(&time, 0, sizeof(time));
    memset(&place, 0, sizeof(place)); memset(card, 0, sizeof(card));
    const orb_model_snapshot_t *model_view = &source.model->value;
    const orb_home_snapshot_t *home_view = &source.home->value;
    orb_model_set_connection(false, false); orb_home_set_connection(false, false);
    assert(orb_workspace_model_refresh(&source.model, &model_view));
    assert(orb_workspace_home_refresh(&source.home, &home_view));
    assert(source.model != model && source.home != home);
    orb_workspace_snapshots_release(&source);
    assert(model->references == 1 && home->references == 1);
    assert(port->read(port->context, &read) && read.time->timer_remaining_us == 91000000);
    assert(read.page_index == expected_page && read.model->online && read.home->online);
    assert(!strcmp(read.clock_time, "7:05 PM") && !strcmp(read.clock_date, "Wed, Oct 28"));
    assert(!strcmp(read.card, ordered ? "second" : override));
    assert(orb_stack_screen_refresh(screen, ORB_STACK_REFRESH_CLOCK));
    orb_answer_t answer; orb_message_t message; orb_media_request_t media; orb_home_request_t request;
    assert(!orb_model_take_answer(&answer) && !orb_model_take_message(&message));
    assert(!orb_model_take_nowplaying_request(&media) && !orb_home_take_request(&request));
    if (parent_first) {
        lv_obj_delete(lv_obj_get_parent(orb_neighbour_page(scene)));
        assert(!orb_neighbour_page(scene) && !orb_neighbour_screen(scene));
        orb_neighbour_settle(scene);
    }
    orb_neighbour_destroy(scene);
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
    assert(used() == before && timers() == before_timers);
}

static void stack_empty(void)
{
    publish();
    workspace_snapshots_t source = snapshots();
    source.model->value = (orb_model_snapshot_t){0};
    source.home->value = (orb_home_snapshot_t){0};
    orb_route_context_t place = {0}; orb_route_context_save(&place, 400, 99, 2);
    orb_neighbour_t *scene = orb_neighbour_create(lv_screen_active(), ORB_SHELL_STACK,
        &(orb_neighbour_source_t){.snapshots = &source, .place = &place});
    assert(scene && orb_neighbour_screen(scene));
    orb_screen_update_t value = projection(orb_neighbour_screen(scene));
    const orb_stack_source_t *port = value.model->data;
    orb_stack_read_t read; assert(port->read(port->context, &read));
    assert(!read.page_index && read.clock_hour == -1 && !read.clock_time[0] && !read.clock_date[0]);
    assert(text(orb_neighbour_page(scene), "All caught up") && !orb_screen_page_viewport(orb_neighbour_screen(scene)));
    orb_neighbour_settle(scene);
    orb_neighbour_destroy(scene);
    orb_workspace_snapshots_release(&source);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    assert(display);
    orb_ui_init(NULL);
    orb_time_init();
    assert(orb_model_init() && orb_home_init());
    unsigned hour = 99, minute = 99;
    assert(orb_system_clock_parts("7:05 PM", &hour, &minute) && hour == 19 && minute == 5);
    assert(orb_system_clock_parts("12:00 AM", &hour, NULL) && hour == 0);
    assert(orb_system_clock_parts("23:59", &hour, &minute) && hour == 23 && minute == 59);
    assert(!orb_system_clock_parts("24:00", &hour, NULL));
    assert(!orb_system_clock_parts("7:99 PM", &hour, NULL));
    assert(!orb_system_clock_parts("7:05 PM extra", &hour, NULL));
    assert(orb_neighbour_supported(ORB_APP_HOME_SCREEN_ID) && orb_neighbour_supported(ORB_APP_TIME_SCREEN_ID));
    assert(orb_neighbour_supported(0) && !orb_neighbour_supported(31));
    assert(!orb_neighbour_create(lv_screen_active(), 0, NULL));
    assert(!orb_neighbour_create(lv_screen_active(), ORB_APP_HOME_SCREEN_ID,
        &(orb_neighbour_source_t){.snapshots = &(workspace_snapshots_t){0}}));
    for (unsigned i = 0; i < 2; i++) {
        round_trip(ORB_APP_HOME_SCREEN_ID, true);
        round_trip(ORB_APP_HOME_SCREEN_ID, false);
        round_trip(ORB_APP_TIME_SCREEN_ID, true);
        round_trip(ORB_APP_TIME_SCREEN_ID, false);
    }
    refusal(ORB_APP_HOME_SCREEN_ID);
    refusal(ORB_APP_TIME_SCREEN_ID);
    for (unsigned i = 0; i < 2; i++) {
        system_scene(ORB_SHELL_CHARGING, 1, i != 0);
        system_scene(ORB_SHELL_CONTROL_CENTER, 0, i != 0);
        system_scene(ORB_SHELL_CONTROL_CENTER, 1, i != 0);
        system_scene(ORB_SHELL_CONTROL_CENTER, 2, i != 0);
        system_scene(ORB_SHELL_CONTROL_CENTER, UINT32_MAX, i != 0);
    }
    system_refusal(ORB_SHELL_CHARGING);
    system_refusal(ORB_SHELL_CONTROL_CENTER);
    assert(!orb_apps_find("time"));
    apps_scene(false, false, false);
    apps_scene(true, true, false);
    orb_apps_init(0);
    assert(orb_apps_find("time"));
    apps_scene(false, true, true);
    apps_scene(true, false, true);
    apps_refusal();
    for (unsigned i = 0; i < 2; i++) {
        ordinary_scene(ORB_SHELL_ONBOARDING, false, i != 0);
        ordinary_scene(ORB_SHELL_ONBOARDING, true, i != 0);
        ordinary_scene(ORB_SHELL_UPDATES, false, i != 0);
        ordinary_scene(ORB_SHELL_UPDATES, true, i != 0);
    }
    system_refusal(ORB_SHELL_ONBOARDING);
    system_refusal(ORB_SHELL_UPDATES);
    for (unsigned i = 0; i < 2; i++) {
        faces_scene(ORB_SHELL_FACE, i != 0);
        faces_scene(ORB_SHELL_OFFLINE, i != 0);
    }
    system_refusal(ORB_SHELL_FACE);
    system_refusal(ORB_SHELL_OFFLINE);
    for (unsigned i = 0; i < 2; i++) {
        stack_scene(UINT32_MAX, "", i != 0, false);
        stack_scene(0, "", i != 0, false);
        stack_scene(2, "", i != 0, false);
        stack_scene(3, "", i != 0, false);
        stack_scene(99, "", i != 0, false);
        stack_scene(99, "second", i != 0, false);
        stack_scene(99, "missing", i != 0, false);
        stack_scene(0, "", i != 0, true);
    }
    stack_empty();
    system_refusal(ORB_SHELL_STACK);
    lv_display_delete(display);
    return 0;
}
