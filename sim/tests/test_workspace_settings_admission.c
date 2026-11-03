/* Production workspace admission through public UI-thread APIs. The scene is
 * synthetic and contains no board, service task, or owner configuration. */
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_memory.h"
#include "orb_model.h"
#include "orb_settings_host.h"
#include "orb_ui.h"
#include "orb_workspace.h"
#include "sim_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case;
static lv_display_t *s_display;
static lv_obj_t *s_scene;
static orb_workspace_t *s_workspace;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "workspace Settings admission %s:%d: %s\n", s_case, __LINE__, #value); \
    exit(EXIT_FAILURE); \
} } while (0)

static uint32_t tick(void) { return s_now; }

static void advance(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 8) {
        s_now += 8;
        orb_workspace_poll(s_workspace);
        lv_timer_handler();
        lv_refr_now(s_display);
    }
}

static void await_surface(orb_shell_surface_t surface)
{
    for (unsigned elapsed = 0; elapsed < 4000; elapsed += 8) {
        advance(8);
        orb_shell_open_metrics_t metrics;
        orb_workspace_open_metrics(s_workspace, &metrics);
        if (orb_workspace_surface(s_workspace) == surface &&
            orb_workspace_nav_current(s_workspace) == surface && !metrics.active &&
            metrics.content_ready && !metrics.retaining_snapshot &&
            !sim_display_snapshot_transition_active()) return;
    }
    CHECK(false);
}

static void route(orb_shell_surface_t surface)
{
    orb_workspace_navigate(s_workspace, surface);
    await_surface(surface);
}

static void back_to(orb_shell_surface_t surface)
{
    orb_workspace_back(s_workspace);
    await_surface(surface);
}

static lv_obj_t *caption(lv_obj_t *object, const char *text)
{
    /* Hidden provisional parents must never count as the visible source.
     * A cached row can retain its native label as hidden semantic content. */
    bool label = lv_obj_check_type(object, &lv_label_class);
    const char *rim = orb_kit_rim_text_get(object);
    const char *button = orb_kit_button_get_text(object);
    if ((rim && !strcasecmp(rim, text)) || (button && !strcmp(button, text)) ||
        (label && !strcmp(lv_label_get_text(object), text))) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = caption(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *row(const char *text)
{
    lv_obj_t *object = caption(s_scene, text);
    CHECK(object);
    while (object && !lv_obj_is_clickable(object)) object = lv_obj_get_parent(object);
    CHECK(object);
    return object;
}

static lv_obj_t *row_list(lv_obj_t *object)
{
    for (object = lv_obj_get_parent(object); object; object = lv_obj_get_parent(object))
        if (lv_obj_is_scrollable(object) && lv_obj_get_height(object) < SIM_D) return object;
    CHECK(false);
    return NULL;
}

static void set_scroll(lv_obj_t *list, int32_t value)
{
    lv_obj_scroll_to_y(list, value, LV_ANIM_OFF);
    advance(800);
}

static size_t reserve_remaining(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    /* Telemetry also includes legacy tracked bytes. Reserve actual admission
     * headroom without changing the allocator or introducing a fault hook. */
    size_t chunk = 1, held = 0;
    while (chunk <= stats.budget / 2) chunk *= 2;
    for (; chunk; chunk /= 2)
        if (orb_memory_reserve(ORB_MEMORY_WORKSPACE, chunk)) held += chunk;
    return held;
}

static void source_retained(orb_shell_surface_t surface, const char *title,
    lv_obj_t *heading, lv_obj_t *list, int32_t scroll, const orb_shell_open_metrics_t *before)
{
    CHECK(orb_workspace_surface(s_workspace) == surface);
    CHECK(orb_workspace_nav_current(s_workspace) == surface);
    CHECK(caption(s_scene, title) == heading);
    CHECK(lv_obj_is_visible(heading) && lv_obj_is_visible(list));
    CHECK(lv_obj_get_scroll_y(list) == scroll);
    CHECK(!sim_display_snapshot_transition_active());
    orb_shell_open_metrics_t after;
    orb_workspace_open_metrics(s_workspace, &after);
    CHECK(after.sequence == before->sequence && after.surface == before->surface);
    CHECK(after.input_us == before->input_us && !after.active && after.content_ready);
}

static size_t workspace_used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static void partial_first_host_rejection(void)
{
    route(ORB_SHELL_APPS);
    route(ORB_SHELL_ONBOARDING);
    lv_obj_t *heading = caption(s_scene, "Welcome to Orb");
    lv_obj_t *list = row_list(caption(s_scene, "Notifications"));
    CHECK(heading);
    set_scroll(list, 24);
    int32_t scroll = lv_obj_get_scroll_y(list);
    CHECK(scroll > 0);
    size_t baseline = workspace_used();
    orb_settings_host_t *probe = orb_settings_host_create(false, NULL, NULL, NULL);
    CHECK(probe && workspace_used() > baseline);
    size_t host_bytes = workspace_used() - baseline;
    orb_settings_host_destroy(probe);
    CHECK(workspace_used() == baseline);
    orb_shell_open_metrics_t before;
    orb_workspace_open_metrics(s_workspace, &before);
    size_t reservation = reserve_remaining();
    CHECK(reservation >= host_bytes);
    orb_memory_release(ORB_MEMORY_WORKSPACE, host_bytes);
    reservation -= host_bytes;
    size_t attempted = workspace_used();
    /* The host fits exactly, but its first descriptor cannot. Its failed
     * first-use admission must release the newly allocated empty host. */
    orb_workspace_navigate(s_workspace, ORB_SHELL_SETTINGS);
    advance(1600);
    source_retained(ORB_SHELL_ONBOARDING, "Welcome to Orb", heading, list, scroll, &before);
    CHECK(workspace_used() == attempted);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reservation);
    CHECK(workspace_used() == baseline);
    route(ORB_SHELL_SETTINGS);
    back_to(ORB_SHELL_ONBOARDING);
    back_to(ORB_SHELL_APPS);
    back_to(ORB_SHELL_FACE);
}

static void fresh_host_rejection(void)
{
    /* Neither Apps nor Onboarding creates a migrated Settings host. These two
     * successful entries also establish an observable, nontrivial Back chain. */
    route(ORB_SHELL_APPS);
    route(ORB_SHELL_ONBOARDING);
    lv_obj_t *heading = caption(s_scene, "Welcome to Orb");
    lv_obj_t *list = row_list(caption(s_scene, "Notifications"));
    CHECK(heading);
    set_scroll(list, 24);
    int32_t scroll = lv_obj_get_scroll_y(list);
    CHECK(scroll > 0);
    orb_shell_open_metrics_t before;
    orb_workspace_open_metrics(s_workspace, &before);
    size_t reservation = reserve_remaining();
    orb_workspace_navigate(s_workspace, ORB_SHELL_SETTINGS);
    advance(1600);
    source_retained(ORB_SHELL_ONBOARDING, "Welcome to Orb", heading, list, scroll, &before);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reservation);

    /* Retry the same public action. A phantom push from the refusal would
     * make one of these Back destinations repeat instead of reaching Face. */
    route(ORB_SHELL_SETTINGS);
    CHECK(caption(s_scene, "Settings"));
    back_to(ORB_SHELL_ONBOARDING);
    back_to(ORB_SHELL_APPS);
    back_to(ORB_SHELL_FACE);
}

static void warmed_descriptor_rejection(void)
{
    route(ORB_SHELL_APPS);
    route(ORB_SHELL_SETTINGS);
    lv_obj_t *units = row("Units");
    lv_obj_t *list = row_list(units);
    set_scroll(list, LV_MAX(0, lv_obj_get_y(units) - 120));
    CHECK(lv_obj_get_style_opa(units, 0) != LV_OPA_TRANSP);
    lv_obj_t *heading = caption(s_scene, "Settings");
    int32_t scroll = lv_obj_get_scroll_y(list);
    CHECK(heading && scroll > 0);
    orb_shell_open_metrics_t before;
    orb_workspace_open_metrics(s_workspace, &before);
    size_t reservation = reserve_remaining();
    /* This executes the production row handler, typed NAV intent, and outer
     * animated admission/fallback path rather than calling a private helper. */
    lv_obj_send_event(units, LV_EVENT_CLICKED, NULL);
    advance(1600);
    source_retained(ORB_SHELL_SETTINGS, "Settings", heading, list, scroll, &before);
    CHECK(row("Units") == units);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reservation);

    lv_obj_send_event(units, LV_EVENT_CLICKED, NULL);
    await_surface(ORB_SHELL_UNITS);
    CHECK(caption(s_scene, "Units") && caption(s_scene, "Temperature"));
    back_to(ORB_SHELL_SETTINGS);
    CHECK(lv_obj_get_scroll_y(row_list(row("Units"))) == scroll);
    back_to(ORB_SHELL_APPS);
    back_to(ORB_SHELL_FACE);
}

static void back_to_migrated_rejection(void)
{
    route(ORB_SHELL_APPS);
    route(ORB_SHELL_SETTINGS);
    route(ORB_SHELL_ONBOARDING);
    lv_obj_t *heading = caption(s_scene, "Welcome to Orb");
    lv_obj_t *list = row_list(caption(s_scene, "Notifications"));
    CHECK(heading);
    set_scroll(list, 24);
    int32_t scroll = lv_obj_get_scroll_y(list);
    CHECK(scroll > 0);
    orb_shell_open_metrics_t before;
    orb_workspace_open_metrics(s_workspace, &before);
    size_t reservation = reserve_remaining();
    /* Back's no-animation fallback must not pop history before the saved
     * migrated destination has been admitted. Visible source alone is not
     * sufficient: the next successful Back must still reach Settings. */
    orb_workspace_back(s_workspace);
    advance(1600);
    source_retained(ORB_SHELL_ONBOARDING, "Welcome to Orb", heading, list, scroll, &before);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reservation);
    back_to(ORB_SHELL_SETTINGS);
    back_to(ORB_SHELL_APPS);
    back_to(ORB_SHELL_FACE);
}

int main(int argc, char **argv)
{
    s_case = argc > 1 ? argv[1] : "fresh";
    CHECK(!strcmp(s_case, "fresh") || !strcmp(s_case, "warm") ||
        !strcmp(s_case, "back") || !strcmp(s_case, "partial"));
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display);
    orb_ui_init(NULL);
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    orb_home_load_demo();
    s_scene = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_scene);
    lv_obj_set_size(s_scene, SIM_D, SIM_D);
    lv_obj_set_scrollable(s_scene, false);
    s_workspace = orb_workspace_create(s_scene, NULL, NULL, NULL);
    CHECK(s_workspace);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED,
        .link = ORB_SHELL_LINK_ONLINE, .battery_pct = 80, .brightness = 70};
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    snprintf(status.hub_name, sizeof(status.hub_name), "Synthetic Mac");
    orb_workspace_update(s_workspace, &status);
    advance(1200);
    route(ORB_SHELL_FACE);
    if (!strcmp(s_case, "fresh")) fresh_host_rejection();
    else if (!strcmp(s_case, "warm")) warmed_descriptor_rejection();
    else if (!strcmp(s_case, "back")) back_to_migrated_rejection();
    else partial_first_host_rejection();
    orb_answer_t answer;
    orb_home_request_t home;
    CHECK(!orb_model_peek_answer(&answer) && !orb_home_take_request(&home));
    orb_workspace_destroy(s_workspace);
    s_workspace = NULL;
    lv_obj_delete(s_scene);
    printf("workspace Settings admission %s: %u checks passed\n", s_case, s_checks);
    return 0;
}
