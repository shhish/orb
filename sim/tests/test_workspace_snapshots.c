/* Include the production workspace to inspect snapshot generations and inject
 * failure only into its own allocations; the other host providers stay real. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

static size_t s_fail_size;
static unsigned s_fail_allocations, s_fail_timer, s_failures;

static void *snapshot_calloc(size_t count, size_t size)
{
    if (s_fail_allocations && count == 1 && size == s_fail_size) {
        s_fail_allocations--;
        s_failures++;
        return NULL;
    }
    return calloc(count, size);
}

static lv_timer_t *snapshot_timer_create(lv_timer_cb_t callback, uint32_t period, void *context)
{
    if (s_fail_timer && !--s_fail_timer) {
        s_failures++;
        return NULL;
    }
    return lv_timer_create(callback, period, context);
}

#define calloc snapshot_calloc
#define lv_timer_create snapshot_timer_create
#include "../../firmware/components/orb_shell/orb_workspace_snapshot.c"
#include "../../firmware/components/orb_shell/orb_workspace.c"
#undef lv_timer_create
#undef calloc

#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case;
static lv_display_t *s_display;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "workspace snapshots %s:%d: %s\n", s_case, __LINE__, #value); \
    exit(EXIT_FAILURE); \
} } while (0)

static uint32_t tick(void) { return s_now; }

static size_t workspace_bytes(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static void advance(orb_workspace_t *live, orb_workspace_t *preview, unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 16) {
        s_now += 16;
        if (live) orb_workspace_poll(live);
        if (preview) orb_workspace_poll(preview);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}

static orb_workspace_t *create_live(void)
{
    orb_workspace_t *live = orb_workspace_create(lv_screen_active(), NULL, NULL, NULL);
    CHECK(live);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED,
        .link = ORB_SHELL_LINK_ONLINE, .battery_pct = 80, .brightness = 70,
        .clock_time = "10:09", .clock_date = "Thursday 29"};
    orb_workspace_update(live, &status);
    advance(live, NULL, 1200);
    CHECK(live->snapshots.model->references == 1 && live->snapshots.home->references == 1);
    return live;
}

static orb_workspace_t *create_preview(orb_workspace_t *live)
{
    size_t before = workspace_bytes();
    orb_workspace_t *preview = workspace_create_preview(lv_screen_active(), live);
    CHECK(preview && preview->fixture);
    CHECK(preview->model == live->model && preview->home == live->home);
    CHECK(preview->snapshots.model == live->snapshots.model && preview->snapshots.home == live->snapshots.home);
    CHECK(live->snapshots.model->references == 2 && live->snapshots.home->references == 2);
    CHECK(workspace_bytes() == before + sizeof(*preview) +
        orb_notification_history_bytes() + orb_notifications_host_bytes() + orb_voice_flow_bytes() + orb_apps_view_size());
    return preview;
}

static void sharing_and_updates(void)
{
    orb_workspace_t *live = create_live(), *preview = create_preview(live);
    orb_model_snapshot_t old_model = *preview->model;
    orb_home_snapshot_t old_home = *preview->home;
    workspace_model_t *model = live->snapshots.model;
    workspace_home_t *home = live->snapshots.home;
    size_t before = workspace_bytes();
    for (unsigned i = 0; i < 32; i++) {
        orb_workspace_poll(live);
        orb_workspace_poll(preview);
    }
    CHECK(live->snapshots.model == model && live->snapshots.home == home);
    CHECK(workspace_bytes() == before);
    orb_model_set_connection(false, false);
    orb_workspace_poll(live);
    CHECK(live->snapshots.model != model && live->model != preview->model);
    CHECK(live->snapshots.home == home && live->home == preview->home);
    CHECK(model->references == 1 && live->snapshots.model->references == 1 && home->references == 2);
    CHECK(!live->model->online && preview->model->online);
    CHECK(!memcmp(preview->model, &old_model, sizeof(old_model)));
    orb_home_set_connection(false, false);
    orb_workspace_poll(live);
    CHECK(live->snapshots.home != home && live->home != preview->home);
    CHECK(home->references == 1 && live->snapshots.home->references == 1);
    CHECK(!live->home->online && preview->home->online);
    CHECK(!memcmp(preview->home, &old_home, sizeof(old_home)));
    CHECK(!memcmp(preview->model, &old_model, sizeof(old_model)));
    orb_workspace_destroy(live);
    orb_workspace_destroy(preview);
}

static void live_destroyed_first(void)
{
    orb_workspace_t *live = create_live(), *preview = create_preview(live);
    orb_model_snapshot_t old_model = *preview->model;
    orb_home_snapshot_t old_home = *preview->home;
    orb_workspace_destroy(live);
    CHECK(preview->snapshots.model->references == 1 && preview->snapshots.home->references == 1);
    /* The old generation remains renderable after its source and the live
     * stores change; private navigation still cannot become a live update. */
    orb_model_reset(); orb_home_reset();
    orb_workspace_navigate(preview, ORB_SHELL_HOME);
    advance(NULL, preview, 1200);
    CHECK(orb_workspace_surface(preview) == ORB_SHELL_HOME && home_screen(preview));
    orb_workspace_navigate(preview, ORB_SHELL_SESSIONS);
    advance(NULL, preview, 1200);
    CHECK(orb_workspace_surface(preview) == ORB_SHELL_SESSIONS);
    CHECK(!memcmp(preview->model, &old_model, sizeof(old_model)));
    CHECK(!memcmp(preview->home, &old_home, sizeof(old_home)));
    orb_workspace_destroy(preview);
}

static void preview_destroyed_first(void)
{
    orb_workspace_t *live = create_live(), *preview = create_preview(live);
    workspace_model_t *model = live->snapshots.model;
    workspace_home_t *home = live->snapshots.home;
    orb_workspace_destroy(preview);
    CHECK(model->references == 1 && home->references == 1);
    orb_model_set_connection(false, false);
    orb_home_set_connection(false, false);
    orb_workspace_poll(live);
    CHECK(live->snapshots.model == model && live->snapshots.home == home);
    CHECK(!live->model->online && !live->home->online);
    orb_workspace_destroy(live);
}

static void external_inputs(void)
{
    orb_model_snapshot_t *model = malloc(sizeof(*model));
    orb_home_snapshot_t *home = malloc(sizeof(*home));
    CHECK(model && home && orb_model_snapshot(model) && orb_home_snapshot(home));
    orb_model_snapshot_t old_model = *model;
    orb_home_snapshot_t old_home = *home;
    orb_workspace_t *fixture = orb_workspace_create_fixture(lv_screen_active(), model, home);
    CHECK(fixture && fixture->model != model && fixture->home != home);
    CHECK(fixture->snapshots.model->references == 1 && fixture->snapshots.home->references == 1);
    memset(model, 0xa5, sizeof(*model)); memset(home, 0xa5, sizeof(*home));
    free(model); free(home);
    orb_workspace_navigate(fixture, ORB_SHELL_HOME);
    advance(NULL, fixture, 1200);
    CHECK(home_screen(fixture));
    CHECK(!memcmp(fixture->model, &old_model, sizeof(old_model)));
    CHECK(!memcmp(fixture->home, &old_home, sizeof(old_home)));
    orb_workspace_destroy(fixture);
}

static void failed_copy_on_write(bool model_change)
{
    orb_workspace_t *live = create_live(), *preview = create_preview(live);
    workspace_model_t *model = live->snapshots.model;
    workspace_home_t *home = live->snapshots.home;
    orb_model_snapshot_t old_model = *preview->model;
    orb_home_snapshot_t old_home = *preview->home;
    size_t before = workspace_bytes();
    if (model_change) orb_model_set_connection(false, false);
    else orb_home_set_connection(false, false);
    s_fail_size = sizeof(payload_header_t) +
        (model_change ? sizeof(workspace_model_t) : sizeof(workspace_home_t));
    s_fail_allocations = 1;
    orb_workspace_poll(live);
    CHECK(s_failures == 1 && s_fail_allocations == 0);
    CHECK(live->snapshots.model == model && live->snapshots.home == home);
    CHECK(model->references == 2 && home->references == 2);
    CHECK(workspace_bytes() == before);
    CHECK(!memcmp(live->model, &old_model, sizeof(old_model)));
    CHECK(!memcmp(live->home, &old_home, sizeof(old_home)));
    orb_workspace_poll(live);
    CHECK((live->snapshots.model != model) == model_change);
    CHECK((live->snapshots.home != home) == !model_change);
    CHECK(!memcmp(preview->model, &old_model, sizeof(old_model)));
    CHECK(!memcmp(preview->home, &old_home, sizeof(old_home)));
    orb_workspace_destroy(live);
    orb_workspace_destroy(preview);
}

static void failed_constructor(void)
{
    orb_workspace_t *live = create_live();
    size_t before = workspace_bytes();
    for (unsigned timer = 1; timer <= 2; timer++) {
        s_fail_timer = timer;
        CHECK(!workspace_create_preview(lv_screen_active(), live));
        CHECK(live->snapshots.model->references == 1 && live->snapshots.home->references == 1);
        CHECK(workspace_bytes() == before);
    }
    CHECK(s_failures == 2);
    s_fail_size = sizeof(payload_header_t) + sizeof(orb_workspace_t);
    s_fail_allocations = 1;
    CHECK(!workspace_create_preview(lv_screen_active(), live));
    CHECK(s_failures == 3 && workspace_bytes() == before);
    CHECK(live->snapshots.model->references == 1 && live->snapshots.home->references == 1);
    orb_workspace_t *preview = create_preview(live);
    orb_workspace_destroy(preview);
    orb_workspace_destroy(live);
}

int main(int argc, char **argv)
{
    s_case = argc > 1 ? argv[1] : "sharing";
    CHECK(argc == 2);
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL);
    CHECK(orb_model_init() && orb_home_init());
    orb_model_reset(); orb_home_reset();
    orb_model_load_demo(); orb_home_load_demo();
    orb_model_set_connection(true, true);
    orb_home_set_connection(true, true);
    size_t start = workspace_bytes();
    if (!strcmp(s_case, "sharing")) sharing_and_updates();
    else if (!strcmp(s_case, "live-first")) live_destroyed_first();
    else if (!strcmp(s_case, "preview-first")) preview_destroyed_first();
    else if (!strcmp(s_case, "external")) external_inputs();
    else if (!strcmp(s_case, "cow-model")) failed_copy_on_write(true);
    else if (!strcmp(s_case, "cow-home")) failed_copy_on_write(false);
    else if (!strcmp(s_case, "constructor")) failed_constructor();
    else CHECK(false);
    advance(NULL, NULL, 32);
    CHECK(workspace_bytes() == start);
    printf("workspace snapshots %s: %u checks passed, WORKSPACE returned to %zu B\n",
        s_case, s_checks, start);
    return EXIT_SUCCESS;
}
