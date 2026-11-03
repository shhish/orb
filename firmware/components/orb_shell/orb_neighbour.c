#include "orb_neighbour.h"
#include "orb_home_host.h"
#include "orb_time_host.h"
#include "orb_faces_host.h"
#include "orb_time.h"
#include "control_screen.h"
#include "system/charging_screen.h"
#include "system/system_views.h"
#include "orb_place_view.h"
#include "orb_page.h"
#include "orb_ui.h"
#include "orb_sound.h"
#include "orb_apps.h"
#include "stack_screen.h"

#include <stddef.h>
#include <stdio.h>

struct orb_neighbour {
    workspace_snapshots_t snapshots;
    orb_app_host_t *host;
    orb_apps_view_t *apps;
    orb_system_views_t *system;
    orb_screen_id_t route;
    lv_obj_t *root, *page;
    _Alignas(max_align_t) unsigned char state[];
};

typedef struct {
    orb_stack_read_t read;
    orb_time_snapshot_t time;
    char clock_time[16], clock_date[32], card[ORB_MODEL_CARD_ID_MAX + 1];
} stack_neighbour_t;

static orb_app_host_t *home_create(void)
{
    return orb_home_host_create(true, NULL);
}

static orb_app_host_t *time_create(void)
{
    return orb_time_host_create(true);
}

static void home_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    (void)orb_home_host_attach(scene->host, scene->page, &(orb_app_home_model_t){
        .hour = source->hour, .contrast = source->contrast,
        .fixture = &scene->snapshots.home->value});
    if (source->home_fresh_entry) orb_app_home_enter(orb_app_host_current(scene->host));
}

static void time_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    (void)orb_time_host_attach(scene->host, scene->page, &(orb_app_time_model_t){.contrast = source->contrast},
        source->alarm_ringing, source->reminder_due);
}

static orb_app_host_t *charging_create(void)
{
    return orb_charging_host_create(true);
}

static orb_app_host_t *control_create(void)
{
    return orb_control_host_create(true, NULL, NULL);
}

static void charging_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    orb_charging_model_t model = orb_charging_project(source->status, source->contrast);
    (void)orb_charging_host_attach(scene->host, scene->page, &model);
}

static void control_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    /* Cold previews omitted the live shell policy and torch state. Only the
     * selected property and destination page cross this context boundary. */
    orb_control_model_t model = orb_control_project(source->status, orb_sound_volume(),
        source->silent, false, false, source->control_volume);
    orb_route_location_t place = orb_route_context_read(source->place);
    if (place.valid && place.page == 1)
        orb_control_home_project(&model, &scene->snapshots.home->value, source->hour);
    (void)orb_control_host_attach(scene->host, scene->page, &model);
}

static bool apps_admit(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    (void)source;
    /* This owner was mandatory in the cold workspace constructor, before
     * source attribution. Its later descriptor admission was optional. */
    scene->apps = orb_apps_view_create(true, NULL, NULL, NULL);
    return scene->apps != NULL;
}

static void apps_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    orb_route_location_t place = orb_route_context_read(source->place);
    /* A preview inherits focus alone. The fresh owner's cancellation focus
     * remains zero; copying the full live context would change that contract. */
    orb_apps_view_select(scene->apps, place.valid ? place.focus : orb_apps_view_focus(source->apps));
    (void)orb_apps_view_build(scene->apps, scene->page, false, orb_apps_find("time") != NULL);
}

static void restore_scroll(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    lv_obj_t *list = orb_screen_page_viewport(orb_neighbour_screen(scene));
    orb_route_location_t place = orb_route_context_read(source->place);
    if (list && place.valid) {
        lv_obj_scroll_to_y(list, place.scroll, LV_ANIM_OFF);
        orb_round_list_update(list);
    }
}

static void system_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    /* A cold owner samples current tutorial saving/error state, but never
     * inherits the live save observer or pending update-check latch. */
    scene->system = orb_system_views_create(true, NULL, NULL);
    if (!orb_system_views_attach(scene->system, scene->page, scene->route, source->status)) return;
    restore_scroll(scene, source);
}

static orb_app_host_t *faces_create(void)
{
    return orb_faces_host_create(true, NULL, NULL);
}

static void faces_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    /* Ordinary cold Faces read current preferences, never an editor context.
     * Attach starts empty; activity precedes the first current projection. */
    (void)orb_faces_host_attach(scene->host, scene->page);
    orb_screen_instance_t *screen = orb_faces_host_current(scene->host);
    orb_app_faces_active(screen, true, false);
    if (!screen) return;
    orb_faces_snapshot_t snapshot;
    orb_faces_host_project(&snapshot, &scene->snapshots.model->value,
        &scene->snapshots.home->value, source->status);
    orb_faces_host_update(scene->host, &snapshot);
}

static bool stack_admit(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    /* Time was copied before the source's page-attribution callback. The
     * constructor's separate deadline sample above is still discarded. */
    stack_neighbour_t *stack = (stack_neighbour_t *)scene->state;
    if (source->time) stack->time = *source->time;
    return true;
}

static orb_app_host_t *stack_create(void)
{
    return orb_stack_host_create(true, NULL, NULL);
}

static bool stack_read(void *context, orb_stack_read_t *read)
{
    const stack_neighbour_t *stack = context;
    *read = stack->read;
    return true;
}

static void stack_compose(orb_neighbour_t *scene, const orb_neighbour_source_t *source)
{
    stack_neighbour_t *stack = (stack_neighbour_t *)scene->state;
    orb_route_location_t place = orb_route_context_read(source->place);
    snprintf(stack->card, sizeof(stack->card), "%s", source->card ? source->card : "");
    stack->read = orb_stack_project(&scene->snapshots.model->value, &scene->snapshots.home->value,
        &stack->time, source->status, stack->card, "", place.valid ? place.page : 0, ORB_SHELL_STACK);
    /* The descriptor retains its source port. Own the small value inputs and
     * borrow only this scene's retained generations; live notices were omitted. */
    snprintf(stack->clock_time, sizeof(stack->clock_time), "%s", stack->read.clock_time);
    snprintf(stack->clock_date, sizeof(stack->clock_date), "%s", stack->read.clock_date);
    stack->read.clock_time = stack->clock_time; stack->read.clock_date = stack->clock_date;
    unsigned count = orb_stack_screen_entries(&stack->read, NULL);
    stack->read.page_index = LV_MIN(stack->read.page_index, LV_MAX(1u, count) - 1);
    (void)orb_stack_host_attach(scene->host, scene->page,
        &(orb_stack_source_t){.read = stack_read, .context = stack});
    restore_scroll(scene, source);
}

typedef struct {
    orb_screen_id_t route;
    orb_app_host_t *(*create)(void);
    void (*compose)(orb_neighbour_t *, const orb_neighbour_source_t *);
    bool (*admit)(orb_neighbour_t *, const orb_neighbour_source_t *);
    size_t state_bytes;
} neighbour_descriptor_t;

/* The shared hosts register and validate each production screen descriptor.
 * This table selects only its preview projection, never another renderer. */
static const neighbour_descriptor_t s_registry[] = {
    {ORB_APP_HOME_SCREEN_ID, home_create, home_compose, NULL, 0},
    {ORB_APP_TIME_SCREEN_ID, time_create, time_compose, NULL, 0},
    {ORB_SHELL_CHARGING, charging_create, charging_compose, NULL, 0},
    {ORB_SHELL_CONTROL_CENTER, control_create, control_compose, NULL, 0},
    {ORB_SHELL_APPS, NULL, apps_compose, apps_admit, 0},
    {ORB_SHELL_ONBOARDING, NULL, system_compose, NULL, 0},
    {ORB_SHELL_UPDATES, NULL, system_compose, NULL, 0},
    {ORB_SHELL_FACE, faces_create, faces_compose, NULL, 0},
    {ORB_SHELL_OFFLINE, faces_create, faces_compose, NULL, 0},
    {ORB_SHELL_STACK, stack_create, stack_compose, stack_admit, sizeof(stack_neighbour_t)},
};

static const neighbour_descriptor_t *find(orb_screen_id_t route)
{
    for (unsigned i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); i++)
        if (s_registry[i].route == route) return &s_registry[i];
    return NULL;
}

bool orb_neighbour_supported(orb_screen_id_t route)
{
    return find(route) != NULL;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_clickable(object, false);
    lv_obj_set_event_bubble(object, true);
    return object;
}

static void deleted(lv_event_t *event)
{
    orb_neighbour_t *scene = lv_event_get_user_data(event);
    scene->root = scene->page = NULL;
}

orb_neighbour_t *orb_neighbour_create(lv_obj_t *parent, orb_screen_id_t route,
    const orb_neighbour_source_t *source)
{
    const neighbour_descriptor_t *descriptor = find(route);
    if (!parent || !descriptor || !source || !source->snapshots ||
        !source->snapshots->model || !source->snapshots->home) return NULL;
    orb_neighbour_t *scene = orb_workspace_payload_alloc(sizeof(*scene) + descriptor->state_bytes);
    if (!scene) return NULL;
    scene->route = route;
    /* The old constructor sampled Time before copying the source projection.
     * Even its discarded value advances deadlines; retain that service tick. */
    orb_time_snapshot_t time;
    orb_time_snapshot(orb_time_now_us(), &time);
    if (!orb_workspace_snapshots_init(&scene->snapshots, source->snapshots, NULL, NULL)) {
        orb_neighbour_destroy(scene);
        return NULL;
    }
    if (descriptor->admit && !descriptor->admit(scene, source)) {
        orb_neighbour_destroy(scene);
        return NULL;
    }
    if (source->prepare) source->prepare(source->prepare_context);
    if (!orb_app_host_clone(&scene->host, source->context, true)) {
        orb_neighbour_destroy(scene);
        return NULL;
    }
    /* Cold neighbour workspaces never ran their queued scaffold. Keep exactly
     * their plain root and black page, without shell timers/input/listeners. */
    scene->root = plain(parent);
    lv_obj_set_size(scene->root, 466, 466);
    lv_obj_set_clickable(scene->root, true);
    lv_obj_add_event_cb(scene->root, deleted, LV_EVENT_DELETE, scene);
    scene->page = plain(scene->root);
    lv_obj_set_size(scene->page, 466, 466);
    lv_obj_set_style_bg_color(scene->page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scene->page, LV_OPA_COVER, 0);
    if (!scene->host && descriptor->create) scene->host = descriptor->create();
    /* Preserve the compatibility host's construction-failure presentation:
     * its caller previously ignored attach refusal and captured the page. */
    descriptor->compose(scene, source);
    return scene;
}

lv_obj_t *orb_neighbour_page(const orb_neighbour_t *scene)
{
    return scene ? scene->page : NULL;
}

orb_screen_instance_t *orb_neighbour_screen(const orb_neighbour_t *scene)
{
    if (scene && scene->system) return orb_system_views_current(scene->system, scene->route);
    return scene ? (scene->apps ? orb_apps_view_current(scene->apps) : orb_app_host_current(scene->host)) : NULL;
}

void orb_neighbour_settle(orb_neighbour_t *scene)
{
    if (!scene || (!scene->system && scene->route != ORB_SHELL_STACK) || !scene->page) return;
    orb_screen_instance_t *screen = orb_neighbour_screen(scene);
    orb_place_title_lane(orb_screen_page_title(screen), NULL);
    lv_obj_update_layout(scene->page);
    lv_obj_t *list = orb_screen_page_viewport(screen);
    /* Curved typography feeds measured sizes back into layout. Keep the old
     * bounded settlement after scroll restoration and shell contrast. */
    if (list) for (unsigned pass = 0; pass < 3; pass++) {
        lv_obj_update_layout(scene->page);
        lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
        orb_round_list_update(list);
    }
}

void orb_neighbour_destroy(orb_neighbour_t *scene)
{
    if (!scene) return;
    orb_system_views_destroy(scene->system);
    orb_apps_view_destroy(scene->apps);
    orb_app_host_destroy(scene->host);
    if (scene->root) lv_obj_delete(scene->root);
    orb_workspace_snapshots_release(&scene->snapshots);
    orb_workspace_payload_free(scene);
}
