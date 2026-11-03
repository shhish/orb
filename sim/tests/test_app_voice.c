/* Production Voice descriptor, its workspace-lifetime flow and borrowed model
 * bridge. Assertions use semantic actions and reading/context APIs. */
#include "orb_app_voice.h"
#include "orb_app_voice_registry.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "orb_voice_host.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACTION_STOP = 268, ACTION_CANCEL = 269, ACTION_SEND = 270,
    ACTION_RETRY = 494, ACTION_DONE = 495 };

typedef struct {
    orb_model_snapshot_t *model;
    orb_voice_flow_t *flow;
    orb_session_t selection;
    orb_app_voice_model_t projection;
    orb_screen_model_t view;
    unsigned projects, accepted, refused, requests;
    bool interactive, touch_allowed, physical, request_allowed, external, selected, fail_project;
    char notice[64];
    orb_intent_t last;
    orb_voice_action_t last_request;
} fixture_t;

static unsigned s_checks, s_timer_baseline;
static size_t s_workspace_baseline;
static uint64_t s_revision;
static fixture_t s_lifecycle;
static orb_model_snapshot_t *s_generations[4];
static lv_obj_t *s_initial_root;

#define CHECK(value) do { ++s_checks; assert(value); } while (0)

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static size_t workspace_used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static void settle(void)
{
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
}

static void quiescent(void)
{
    settle();
    CHECK(workspace_used() == s_workspace_baseline);
    CHECK(timers() == s_timer_baseline);
}

/* Text search is independent of the kit's private child positions. */
static bool contains(lv_obj_t *root, const char *value)
{
    if (lv_obj_check_type(root, &lv_label_class) && strstr(lv_label_get_text(root), value)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (contains(lv_obj_get_child(root, i), value)) return true;
    return false;
}

static orb_model_snapshot_t *model_create(void)
{
    orb_model_snapshot_t *model = calloc(1, sizeof(*model));
    CHECK(model);
    model->online = model->synced = model->prefs_known = true;
    model->prefs.sources[0] = model->prefs.sources[1] = model->prefs.sources[2] = true;
    model->session_count = 1;
    model->sessions[0] = (orb_session_t){.id = "session-a", .title = "Voice target",
        .agent = ORB_AGENT_CODEX, .can_message = true, .state = ORB_SESSION_IDLE};
    model->voice.phase = ORB_VOICE_CONFIRMING;
    model->voice.stream = 17;
    snprintf(model->voice.target, sizeof(model->voice.target), "session-a");
    snprintf(model->voice.text, sizeof(model->voice.text),
        "Please review every boundary in the local fixture and keep the original behavior. "
        "The request stays on Orb until an explicit physical Send. "
        "This longer transcript also exercises the saved reading position across page lifetimes.");
    return model;
}

static void fixture_init(fixture_t *fixture, orb_model_snapshot_t *model)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->model = model;
    fixture->interactive = fixture->touch_allowed = fixture->physical = fixture->request_allowed = true;
    fixture->flow = orb_voice_flow_create();
    CHECK(fixture->flow);
}

static bool project_value(void *context, orb_screen_id_t screen, orb_app_voice_model_t *model)
{
    fixture_t *fixture = context;
    CHECK(screen == ORB_APP_VOICE_SCREEN);
    fixture->projects++;
    if (fixture->fail_project) return false;
    *model = (orb_app_voice_model_t){.model = fixture->model, .flow = fixture->flow,
        .selection = fixture->selected ? &fixture->selection : NULL,
        .selection_external = fixture->external, .notice = fixture->notice,
        .interactive = fixture->interactive, .touch_allowed = fixture->touch_allowed,
        .capture_colour = ORB_RED};
    return true;
}

static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    fixture_t *fixture = context;
    if (!project_value(context, screen, &fixture->projection)) return false;
    fixture->view = (orb_screen_model_t){&fixture->projection, sizeof(fixture->projection)};
    *update = (orb_screen_update_t){.model = &fixture->view,
        .revisions.model = s_revision, .revisions.device = s_revision};
    return true;
}

static bool physical(void *context) { return ((fixture_t *)context)->physical; }
static bool request(void *context, orb_voice_action_t action)
{
    fixture_t *fixture = context;
    fixture->requests++;
    fixture->last_request = action;
    return fixture->request_allowed;
}
static void notice(void *context, const char *text)
{
    fixture_t *fixture = context;
    snprintf(fixture->notice, sizeof(fixture->notice), "%s", text);
}

static bool command(fixture_t *fixture, orb_voice_action_t action)
{
    orb_app_voice_model_t projection;
    CHECK(project_value(fixture, ORB_APP_VOICE_SCREEN, &projection));
    return orb_voice_flow_command(fixture->flow, &projection, action,
        &(orb_voice_command_port_t){.context = fixture, .physical = physical,
            .request = request, .notice = notice});
}

static bool accept(void *context, const orb_intent_t *intent)
{
    fixture_t *fixture = context;
    fixture->accepted++;
    fixture->last = *intent;
    if (intent->value.command.action == ACTION_SEND) return command(fixture, ORB_VOICE_CONFIRM);
    if (intent->value.command.action == ACTION_CANCEL) return command(fixture, ORB_VOICE_CANCEL);
    if (intent->value.command.action == ACTION_STOP) return command(fixture, ORB_VOICE_STOP);
    return true;
}

static bool allow_synthetic(void *context, const orb_intent_t *intent)
{ (void)context; return orb_app_voice_allow_synthetic(intent); }
static void refused(void *context, const orb_intent_t *intent)
{
    CHECK(!intent->physical && intent->kind == ORB_INTENT_SEND);
    ((fixture_t *)context)->refused++;
}

static orb_voice_host_t *host_create(fixture_t *fixture, bool preview)
{
    orb_voice_host_t *host = orb_voice_host_create(preview, &(orb_voice_host_config_t){
        .context = fixture, .project = project_value, .accept = accept, .synthetic_refused = refused});
    CHECK(host);
    return host;
}

static lv_obj_t *page_create(void)
{
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 466, 466);
    lv_obj_set_scrollable(page, false);
    return page;
}

static orb_screen_instance_t *attach(orb_voice_host_t *host, lv_obj_t *page)
{
    CHECK(orb_voice_host_attach(host, page, ORB_APP_VOICE_SCREEN));
    orb_screen_instance_t *instance = orb_voice_host_current(host);
    CHECK(instance && orb_screen_alive(instance));
    return instance;
}

static void close_page(orb_voice_host_t *host, lv_obj_t *page)
{
    CHECK(orb_voice_host_detach_page(host, page));
    lv_obj_delete(page);
    CHECK(!orb_voice_host_current(host));
}

static orb_screen_context_t *context_copy(orb_screen_instance_t *instance)
{
    size_t size = orb_screen_descriptor(instance)->context_bytes;
    orb_screen_context_t *context = calloc(1, size);
    CHECK(context && orb_screen_save(instance, context, size));
    return context;
}

static int restore_scroll(orb_screen_instance_t *instance)
{
    int scroll = 0;
    lv_obj_t *viewport = orb_screen_page_viewport(instance);
    if (viewport && orb_app_voice_restore_scroll(instance, &scroll)) {
        lv_obj_update_layout(viewport);
        lv_obj_scroll_to_y(viewport, scroll, LV_ANIM_OFF);
    }
    return viewport ? lv_obj_get_scroll_y(viewport) : 0;
}

static void exercise(orb_screen_instance_t *instance)
{
    orb_screen_context_t *context = context_copy(instance);
    size_t size = orb_screen_descriptor(instance)->context_bytes;
    CHECK(orb_screen_restore(instance, context, size));
    context->version++;
    CHECK(!orb_screen_restore(instance, context, size));
    free(context);
    CHECK(!orb_screen_rim_acquire(instance));
    CHECK(!s_lifecycle.requests && !s_lifecycle.accepted);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    CHECK(orb_screen_alive(instance));
    CHECK(!lv_obj_is_clickable(orb_screen_root(instance)));
    CHECK(lv_obj_is_event_bubble(orb_screen_root(instance)));
    CHECK(!s_lifecycle.accepted && !s_lifecycle.requests);
    if (phase == ORB_LIFECYCLE_INITIAL) s_initial_root = orb_screen_root(instance);
    if (phase == ORB_LIFECYCLE_IDEMPOTENT || phase == ORB_LIFECYCLE_CHANGED)
        CHECK(orb_screen_root(instance) == s_initial_root);
    if (phase == ORB_LIFECYCLE_IDEMPOTENT) s_lifecycle.model = s_generations[1];
    if (phase == ORB_LIFECYCLE_RESTORED) s_lifecycle.model = s_generations[2];
    if (phase == ORB_LIFECYCLE_REMOVED_ID) s_lifecycle.model = s_generations[3];
}

static void lifecycle(void)
{
    fixture_init(&s_lifecycle, s_generations[0]);
    size_t before = s_workspace_baseline;
    s_workspace_baseline += orb_voice_flow_bytes();
    orb_app_voice_model_t models[4];
    orb_screen_model_t views[4];
    orb_screen_update_t updates[4];
    for (unsigned i = 0; i < 4; i++) {
        CHECK(project_value(&s_lifecycle, ORB_APP_VOICE_SCREEN, &models[i]));
        models[i].model = s_generations[i];
        views[i] = (orb_screen_model_t){&models[i], sizeof(models[i])};
        updates[i] = (orb_screen_update_t){.model = &views[i], .revisions.model = i + 1};
    }
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_APP_VOICE_SCREEN);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav,
        .context = &s_lifecycle, .project = project, .intent = accept,
        .allow_synthetic = allow_synthetic, .synthetic_refused = refused});
    orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){
        .descriptor = orb_app_voice_descriptor(), .initial = &updates[0], .changed = &updates[1],
        .removed = &updates[2], .empty = &updates[3], .exercise = exercise,
        .verify = verify, .quiescent = quiescent});
    orb_voice_flow_destroy(s_lifecycle.flow);
    s_workspace_baseline = before;
    quiescent();
}

static void capability_and_commands(void)
{
    fixture_t fixture;
    fixture_init(&fixture, model_create());
    orb_app_voice_model_t projection;
    CHECK(project_value(&fixture, ORB_APP_VOICE_SCREEN, &projection));
    CHECK(orb_app_voice_supported(&projection, "time", NULL));
    CHECK(!orb_app_voice_supported(&projection, "dot", NULL));
    CHECK(orb_app_voice_supported(&projection, "session-a", NULL));
    fixture.model->prefs.sources[ORB_AGENT_CODEX] = false;
    CHECK(!orb_app_voice_supported(&projection, "session-a", NULL));
    fixture.model->card_count = 1;
    fixture.model->cards[0] = (orb_card_t){.id = "reply-card", .reply = true};
    /* Card text uses its explicit reply grant, independently of can_message. */
    fixture.model->sessions[0].can_message = false;
    CHECK(orb_app_voice_supported(&projection, "session-a", "reply-card"));
    fixture.model->cards[0].answered = true;
    CHECK(!orb_app_voice_supported(&projection, "session-a", "reply-card"));
    CHECK(!orb_app_voice_supported(&projection, "time", "missing-card"));
    fixture.model->prefs.sources[ORB_AGENT_CODEX] = true;
    fixture.selection = (orb_session_t){.id = "paged", .title = "An idle session",
        .agent = ORB_AGENT_CODEX, .can_message = true};
    fixture.selected = true;
    CHECK(project_value(&fixture, ORB_APP_VOICE_SCREEN, &projection));
    CHECK(!orb_app_voice_supported(&projection, "paged", NULL));
    fixture.external = true;
    CHECK(project_value(&fixture, ORB_APP_VOICE_SCREEN, &projection));
    CHECK(orb_app_voice_supported(&projection, "paged", NULL));
    fixture.model->prefs.sources[ORB_AGENT_CODEX] = false;
    CHECK(!orb_app_voice_supported(&projection, "paged", NULL));

    fixture.model->prefs.sources[ORB_AGENT_CODEX] = true;
    fixture.model->sessions[0].can_message = true;
    fixture.physical = false;
    CHECK(!command(&fixture, ORB_VOICE_CONFIRM) && !fixture.requests);
    fixture.physical = true;
    CHECK(command(&fixture, ORB_VOICE_START));
    CHECK(!orb_voice_flow_pending(fixture.flow));
    CHECK(command(&fixture, ORB_VOICE_STOP));
    CHECK(!orb_voice_flow_pending(fixture.flow));
    CHECK(command(&fixture, ORB_VOICE_CONFIRM));
    CHECK(orb_voice_flow_pending(fixture.flow));
    orb_voice_flow_poll(fixture.flow, ORB_VOICE_CONFIRMING);
    CHECK(orb_voice_flow_pending(fixture.flow));
    orb_voice_flow_poll(fixture.flow, ORB_VOICE_SENDING);
    CHECK(!orb_voice_flow_pending(fixture.flow));
    fixture.request_allowed = false;
    CHECK(!command(&fixture, ORB_VOICE_CONFIRM));
    CHECK(!strcmp(fixture.notice, "Voice queue full. Try again."));
    CHECK(!orb_voice_flow_pending(fixture.flow));
    fixture.model->synced = false;
    unsigned requests = fixture.requests;
    CHECK(!command(&fixture, ORB_VOICE_START) && fixture.requests == requests);
    CHECK(!strcmp(fixture.notice, "Waiting for Mac sync"));
    fixture.model->online = false;
    CHECK(!command(&fixture, ORB_VOICE_START));
    CHECK(!strcmp(fixture.notice, "Voice needs your Mac"));
    fixture.request_allowed = true;
    CHECK(command(&fixture, ORB_VOICE_CANCEL));
    CHECK(command(&fixture, ORB_VOICE_STOP));
    CHECK(!fixture.notice[0]);
    orb_voice_flow_touch(fixture.flow, true);
    CHECK(orb_voice_flow_held(fixture.flow));
    orb_voice_flow_boot(fixture.flow);
    CHECK(orb_voice_flow_held(fixture.flow));
    orb_voice_flow_touch(fixture.flow, false);
    CHECK(!orb_voice_flow_held(fixture.flow));
    orb_voice_flow_destroy(fixture.flow);
    free(fixture.model);
    quiescent();
}

static void host_policy_and_generation(void)
{
    fixture_t fixture;
    fixture_init(&fixture, model_create());
    orb_voice_host_t *host = host_create(&fixture, false);
    lv_obj_t *page = page_create();
    orb_screen_instance_t *instance = attach(host, page);
    CHECK(!orb_app_voice_activate(instance, ACTION_SEND, ORB_INPUT_TOUCH, false));
    CHECK(fixture.refused == 1 && !fixture.accepted && !fixture.requests);
    CHECK(!orb_app_voice_activate(instance, ACTION_SEND, ORB_INPUT_CROWN, true));
    CHECK(orb_app_voice_activate(instance, ACTION_SEND, ORB_INPUT_TOUCH, true));
    CHECK(fixture.requests == 1 && fixture.last.kind == ORB_INTENT_SEND);
    CHECK(fixture.last_request == ORB_VOICE_CONFIRM && orb_voice_flow_pending(fixture.flow));
    orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    CHECK(!orb_app_voice_activate(instance, ACTION_CANCEL, ORB_INPUT_TOUCH, true));
    orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    fixture.interactive = false;
    CHECK(!orb_app_voice_activate(instance, ACTION_CANCEL, ORB_INPUT_TOUCH, true));
    fixture.interactive = true;

    orb_model_snapshot_t *old = fixture.model;
    fixture.model = model_create();
    free(old);
    fixture.model->online = false;
    /* The old generation is gone before input. Service acceptance must use
     * the newly borrowed offline snapshot even before a presentation poll. */
    unsigned requests = fixture.requests, projects = fixture.projects;
    CHECK(!orb_app_voice_activate(instance, ACTION_SEND, ORB_INPUT_BUTTON, true));
    CHECK(fixture.projects > projects && fixture.requests == requests);
    CHECK(!strcmp(fixture.notice, "Voice needs your Mac"));
    CHECK(orb_app_voice_activate(instance, ACTION_CANCEL, ORB_INPUT_TOUCH, false));
    CHECK(fixture.requests == requests + 1 && fixture.last_request == ORB_VOICE_CANCEL);
    CHECK(!orb_voice_flow_pending(fixture.flow));

    orb_screen_context_t *context = context_copy(instance);
    fixture.fail_project = true;
    lv_obj_t *failed = page_create();
    CHECK(!orb_voice_host_attach(host, failed, ORB_APP_VOICE_SCREEN));
    CHECK(orb_voice_host_current(host) == instance && orb_screen_alive(instance));
    orb_screen_context_t *same = context_copy(instance);
    CHECK(!memcmp(context, same, orb_screen_descriptor(instance)->context_bytes));
    free(same); free(context);
    lv_obj_delete(failed);
    fixture.fail_project = false;
    close_page(host, page);
    orb_voice_host_destroy(host);

    requests = fixture.requests;
    unsigned accepted = fixture.accepted;
    host = host_create(&fixture, true);
    page = page_create();
    instance = attach(host, page);
    CHECK(!orb_app_voice_activate(instance, ACTION_SEND, ORB_INPUT_TOUCH, true));
    CHECK(!orb_app_voice_activate(instance, ACTION_CANCEL, ORB_INPUT_TOUCH, true));
    CHECK(!orb_app_voice_activate(instance, ORB_APP_VOICE_TOUCH_START, ORB_INPUT_TOUCH, true));
    CHECK(!orb_screen_rim_acquire(instance));
    lv_obj_delete(page);
    CHECK(!orb_voice_host_current(host));
    orb_voice_host_destroy(host);
    CHECK(fixture.requests == requests && fixture.accepted == accepted);
    orb_voice_flow_destroy(fixture.flow);
    free(fixture.model);
    quiescent();
}

static void phase_retry_receipt(void)
{
    fixture_t fixture;
    fixture_init(&fixture, model_create());
    orb_model_voice_focus("session-a", NULL);
    orb_voice_host_t *host = host_create(&fixture, false);
    fixture.model->voice.phase = ORB_VOICE_RECORDING;
    orb_voice_flow_sent(fixture.flow, 77);
    orb_voice_flow_poll(fixture.flow, ORB_VOICE_RECORDING);
    CHECK(orb_voice_flow_sent_at(fixture.flow) == 77);
    lv_obj_t *page = page_create();
    (void)attach(host, page);
    CHECK(!orb_voice_flow_sent_at(fixture.flow));
    close_page(host, page);
    orb_voice_flow_sent(fixture.flow, 88);
    page = page_create();
    (void)attach(host, page);
    CHECK(orb_voice_flow_sent_at(fixture.flow) == 88);
    fixture.model->voice.phase = ORB_VOICE_ERROR;
    orb_voice_flow_poll(fixture.flow, ORB_VOICE_ERROR);
    orb_app_voice_meter(orb_voice_host_current(host));
    CHECK(orb_voice_flow_sent_at(fixture.flow) == 88);
    close_page(host, page);

    fixture.model->voice.phase = ORB_VOICE_IDLE;
    fixture.model->last_message.message.stream = 17;
    snprintf(fixture.model->last_message.message.target, sizeof(fixture.model->last_message.message.target), "session-a");
    snprintf(fixture.model->last_message.message.text, sizeof(fixture.model->last_message.message.text), "Kept transcript");
    snprintf(fixture.model->last_message.error, sizeof(fixture.model->last_message.error), "timeout");
    fixture.model->last_message.status = ORB_SEND_FAILED;
    page = page_create();
    orb_screen_instance_t *instance = attach(host, page);
    CHECK(contains(orb_screen_root(instance), "Unknown"));
    CHECK(contains(orb_screen_root(instance), "Check the session first"));
    CHECK(contains(orb_screen_root(instance), "Try again"));
    orb_screen_context_t *context = context_copy(instance);
    CHECK(orb_screen_restore(instance, context, orb_screen_descriptor(instance)->context_bytes));
    free(context);
    CHECK(!fixture.requests && !fixture.accepted);
    close_page(host, page);

    orb_voice_flow_retry(fixture.flow);
    orb_voice_flow_sent(fixture.flow, 99);
    orb_voice_flow_depart(fixture.flow);
    CHECK(!orb_voice_flow_sent_at(fixture.flow));
    page = page_create();
    instance = attach(host, page);
    CHECK(!contains(orb_screen_root(instance), "Unknown"));
    CHECK(contains(orb_screen_root(instance), "Hold BOOT to talk"));
    close_page(host, page);
    fixture.model->voice.phase = ORB_VOICE_STARTING;
    page = page_create();
    (void)attach(host, page);
    close_page(host, page);
    fixture.model->voice.phase = ORB_VOICE_IDLE;
    page = page_create();
    instance = attach(host, page);
    CHECK(contains(orb_screen_root(instance), "Unknown"));
    close_page(host, page);
    orb_voice_host_destroy(host);
    CHECK(!fixture.requests && !fixture.accepted);
    orb_voice_flow_destroy(fixture.flow);
    free(fixture.model);
    quiescent();
}

static void reader_context(void)
{
    fixture_t fixture;
    fixture_init(&fixture, model_create());
    orb_voice_host_t *host = host_create(&fixture, false);
    lv_obj_t *page = page_create();
    orb_screen_instance_t *instance = attach(host, page);
    CHECK(restore_scroll(instance) == 0);
    lv_obj_t *viewport = orb_screen_page_viewport(instance);
    CHECK(viewport);
    lv_obj_update_layout(viewport);
    int maximum = lv_obj_get_scroll_y(viewport) + lv_obj_get_scroll_bottom(viewport);
    CHECK(maximum > 0);
    lv_obj_scroll_to_y(viewport, LV_MIN(80, maximum), LV_ANIM_OFF);
    int saved = lv_obj_get_scroll_y(viewport);
    CHECK(saved > 0);
    close_page(host, page);
    page = page_create();
    instance = attach(host, page);
    CHECK(restore_scroll(instance) == saved);
    /* Voice's identity-keyed reader survives ordinary outer departure and
     * intermediate presentations without a reader. Neither replays a send. */
    orb_voice_flow_depart(fixture.flow);
    close_page(host, page);
    fixture.model->voice.phase = ORB_VOICE_IDLE;
    page = page_create();
    instance = attach(host, page);
    CHECK(!orb_screen_page_viewport(instance));
    close_page(host, page);
    fixture.model->voice.phase = ORB_VOICE_ERROR;
    snprintf(fixture.model->voice.error_code, sizeof(fixture.model->voice.error_code), "no_speech");
    page = page_create();
    instance = attach(host, page);
    CHECK(!orb_screen_page_viewport(instance));
    close_page(host, page);
    fixture.model->voice.phase = ORB_VOICE_CONFIRMING;
    page = page_create();
    instance = attach(host, page);
    CHECK(restore_scroll(instance) == saved);
    close_page(host, page);
    snprintf(fixture.model->voice.target, sizeof(fixture.model->voice.target), "session-b");
    snprintf(fixture.model->sessions[0].id, sizeof(fixture.model->sessions[0].id), "session-b");
    page = page_create();
    instance = attach(host, page);
    CHECK(restore_scroll(instance) == 0);
    lv_obj_delete(page);
    orb_voice_host_destroy(host);
    CHECK(!fixture.accepted && !fixture.requests);
    orb_voice_flow_destroy(fixture.flow);
    free(fixture.model);
    quiescent();
}

int main(void)
{
    lv_init();
    CHECK(sim_display_create());
    orb_ui_init(NULL);
    CHECK(orb_model_init());
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    CHECK(orb_screen_registry_add(&registry, orb_app_voice_manifest(), NULL));
    CHECK(orb_app_voice_descriptor()->id == 13);
    CHECK(orb_screen_registry_find(&registry, 13) == orb_app_voice_descriptor());
    s_generations[0] = model_create();
    for (unsigned i = 1; i < 4; i++) {
        s_generations[i] = malloc(sizeof(*s_generations[i]));
        CHECK(s_generations[i]);
        memcpy(s_generations[i], s_generations[0], sizeof(*s_generations[i]));
    }
    snprintf(s_generations[1]->sessions[0].title, sizeof(s_generations[1]->sessions[0].title), "Changed target title");
    s_generations[2]->session_count = 0;
    s_generations[3]->session_count = 0;
    s_generations[3]->online = s_generations[3]->synced = false;

    fixture_t warm;
    fixture_init(&warm, s_generations[0]);
    orb_voice_host_t *host = host_create(&warm, false);
    lv_obj_t *page = page_create();
    (void)attach(host, page);
    lv_obj_delete(page);
    orb_voice_host_destroy(host);
    orb_voice_flow_destroy(warm.flow);
    settle();
    s_workspace_baseline = workspace_used();
    s_timer_baseline = timers();
    lifecycle();
    capability_and_commands();
    host_policy_and_generation();
    phase_retry_receipt();
    reader_context();
    for (unsigned i = 0; i < 4; i++) free(s_generations[i]);
    printf("app_voice: %u lifecycle, command, generation, receipt and ownership checks passed\n", s_checks);
    return 0;
}
