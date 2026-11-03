/* Real Agents descriptors and compatibility host. Behavioral assertions use
 * public screen/input/viewport seams, never presenter child order or fields. */
#include "orb_app_agents.h"
#include "orb_app_agents_registry.h"
#include "orb_agents_host.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACTION_OPTION = 200, ACTION_MORE = 272, ACTION_FOCUS_OPEN = 492 };
static const orb_screen_id_t s_routes[] = {ORB_APP_AGENTS_SCREEN, ORB_APP_AGENTS_SESSION,
    ORB_APP_AGENTS_CARD, ORB_APP_AGENTS_DOT, ORB_APP_AGENTS_IDLE,
    ORB_APP_AGENTS_USAGE, ORB_APP_AGENTS_SESSIONS};

typedef struct {
    orb_model_snapshot_t *model;
    orb_idle_page_t idle;
    orb_notification_record_t receipt;
    char session[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
    orb_app_agents_model_t projection;
    orb_screen_model_t view;
    uint64_t revision;
    unsigned projects, accepted, refused;
    orb_intent_t last;
    bool fail_project, interactive, destroy_on_accept;
    orb_agents_host_t *destroy_host;
    lv_obj_t *destroy_page;
} fixture_t;

static fixture_t s_lifecycle;
static orb_model_snapshot_t *s_generations[4];
static size_t s_workspace_baseline;
static unsigned s_timer_baseline;
static lv_obj_t *s_initial_root;
static orb_screen_context_t *s_initial_context;
static size_t s_context_size;
static int s_exercised_scroll;

static size_t used(orb_memory_owner_t owner)
{
    orb_memory_stats_t stats;
    orb_memory_get(owner, &stats);
    return stats.used;
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

static orb_model_snapshot_t *model_create(const char *prefix)
{
    orb_model_snapshot_t *model = calloc(1, sizeof(*model));
    assert(model);
    model->online = model->synced = model->prefs_known = true;
    model->prefs.sources[0] = model->prefs.sources[1] = model->prefs.sources[2] = true;
    model->session_count = 4;
    model->idle_count = ORB_MODEL_MAX_IDLE;
    for (unsigned i = 0; i < model->session_count; i++) {
        orb_session_t *session = &model->sessions[i];
        snprintf(session->id, sizeof(session->id), "%s-%u", prefix, i);
        snprintf(session->title, sizeof(session->title), "Session %u", i);
        snprintf(session->project, sizeof(session->project), "Orb");
        session->agent = i == 3 ? ORB_AGENT_DOT : i % 2;
        session->state = ORB_SESSION_BUSY;
        session->can_message = true;
    }
    model->card_count = 1;
    orb_card_t *card = &model->cards[0];
    snprintf(card->id, sizeof(card->id), "approval-a");
    snprintf(card->session, sizeof(card->session), "%s", model->sessions[0].id);
    snprintf(card->title, sizeof(card->title), "Review the complete operation before allowing it");
    snprintf(card->body, sizeof(card->body),
        "test-runner --include every-long-named-suite --directory local-fixtures "
        "--report all-results --verify every-output --preserve original-inputs "
        "--mode isolated --description a-long-request-that-remains-readable-in-full");
    snprintf(card->detail, sizeof(card->detail),
        "The request has enough context to overflow the reading viewport. "
        "Every line must remain reachable by scrolling, with the decision controls still explicit.");
    card->kind = ORB_CARD_APPROVAL;
    card->options_count = 4;
    static const char *const options[] = {"Deny", "Allow", "Allow this turn", "Allow once"};
    for (unsigned i = 0; i < card->options_count; i++) {
        snprintf(card->options[i].id, sizeof(card->options[i].id), "option-%u", i);
        snprintf(card->options[i].label, sizeof(card->options[i].label), "%s", options[i]);
    }
    card->options[0].role = ORB_OPTION_CANCEL;
    card->options[1].role = ORB_OPTION_PRIMARY;
    model->usage.count = 2;
    for (unsigned i = 0; i < 2; i++) {
        model->usage.windows[i].provider = i;
        snprintf(model->usage.windows[i].label, sizeof(model->usage.windows[i].label), "5 hours");
        model->usage.windows[i].used_percent = 20 + 10 * i;
    }
    return model;
}

static void fixture_init(fixture_t *fixture, orb_model_snapshot_t *model)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->model = model;
    fixture->interactive = true;
    snprintf(fixture->session, sizeof(fixture->session), "%s", model->sessions[0].id);
    snprintf(fixture->card, sizeof(fixture->card), "approval-a");
    fixture->idle.count = ORB_MODEL_MAX_IDLE;
    snprintf(fixture->idle.cursor, sizeof(fixture->idle.cursor), "next-page");
    for (unsigned i = 0; i < fixture->idle.count; i++) {
        orb_session_t *session = &fixture->idle.sessions[i];
        snprintf(session->id, sizeof(session->id), "idle-%u", i);
        snprintf(session->title, sizeof(session->title), "Idle session %u", i);
        session->agent = i % 2;
        session->state = ORB_SESSION_IDLE;
    }
}

static bool project_value(void *context, orb_screen_id_t screen, orb_app_agents_model_t *model)
{
    fixture_t *fixture = context;
    assert(orb_app_agents_screen(screen));
    fixture->projects++;
    if (fixture->fail_project) return false;
    *model = (orb_app_agents_model_t){.model = fixture->model, .idle = &fixture->idle,
        .receipt = &fixture->receipt, .net_connected = true, .link_online = true,
        .interactive = fixture->interactive};
    snprintf(model->session, sizeof(model->session), "%s", fixture->session);
    snprintf(model->card, sizeof(model->card), "%s", fixture->card);
    return true;
}

static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    fixture_t *fixture = context;
    if (!project_value(context, screen, &fixture->projection)) return false;
    fixture->view = (orb_screen_model_t){&fixture->projection, sizeof(fixture->projection)};
    *update = (orb_screen_update_t){.model = &fixture->view,
        .revisions.model = fixture->revision, .revisions.device = fixture->revision};
    return true;
}

static bool accept(void *context, const orb_intent_t *intent)
{
    fixture_t *fixture = context;
    fixture->accepted++;
    fixture->last = *intent;
    if (fixture->destroy_on_accept) {
        /* The compatibility shell can materialize a committed pending Back
         * while handling an action. No presenter state may be used afterward. */
        assert(orb_agents_host_detach_page(fixture->destroy_host, fixture->destroy_page));
        lv_obj_delete(fixture->destroy_page);
        fixture->destroy_page = NULL;
    }
    return true;
}

static bool allow_synthetic(void *context, const orb_intent_t *intent)
{
    (void)context;
    return orb_app_agents_allow_synthetic(intent);
}

static void refused(void *context, const orb_intent_t *intent)
{
    fixture_t *fixture = context;
    assert(!intent->physical);
    fixture->refused++;
}

static orb_agents_host_t *host_create(fixture_t *fixture, bool preview)
{
    orb_agents_host_t *host = orb_agents_host_create(preview, &(orb_agents_host_config_t){
        .context = fixture, .project = project_value, .accept = accept, .synthetic_refused = refused});
    assert(host);
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

static int restore_scroll(orb_screen_instance_t *instance)
{
    int top = 0;
    lv_obj_t *viewport = orb_screen_page_viewport(instance);
    if (viewport && orb_app_agents_restore_scroll(instance, &top)) {
        lv_obj_update_layout(viewport);
        lv_obj_scroll_to_y(viewport, top, LV_ANIM_OFF);
    }
    return viewport ? lv_obj_get_scroll_y(viewport) : 0;
}

static int scroll_to(orb_screen_instance_t *instance, int wanted)
{
    lv_obj_t *viewport = orb_screen_page_viewport(instance);
    assert(viewport);
    lv_obj_update_layout(viewport);
    int maximum = lv_obj_get_scroll_y(viewport) + lv_obj_get_scroll_bottom(viewport);
    assert(maximum > 0);
    lv_obj_scroll_to_y(viewport, LV_MIN(wanted, maximum), LV_ANIM_OFF);
    return lv_obj_get_scroll_y(viewport);
}

static orb_screen_context_t *context_copy(orb_screen_instance_t *instance)
{
    size_t size = orb_screen_descriptor(instance)->context_bytes;
    orb_screen_context_t *context = calloc(1, size);
    assert(context && orb_screen_save(instance, context, size));
    return context;
}

static void quiescent(void)
{
    settle();
    assert(used(ORB_MEMORY_WORKSPACE) == s_workspace_baseline);
    assert(timers() == s_timer_baseline);
}

static void exercise(orb_screen_instance_t *instance)
{
    orb_screen_id_t route = orb_screen_descriptor(instance)->id;
    s_exercised_scroll = 0;
    if (route == ORB_APP_AGENTS_USAGE) assert(orb_app_agents_usage_change(instance, 1));
    if (route == ORB_APP_AGENTS_SESSIONS || route == ORB_APP_AGENTS_IDLE || route == ORB_APP_AGENTS_CARD)
        s_exercised_scroll = scroll_to(instance, 96);
    assert(!s_lifecycle.accepted);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    orb_screen_id_t route = orb_screen_descriptor(instance)->id;
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    assert(!s_lifecycle.accepted);
    if (phase == ORB_LIFECYCLE_INITIAL) {
        s_initial_root = orb_screen_root(instance);
        (void)restore_scroll(instance);
        s_initial_context = context_copy(instance);
        s_context_size = orb_screen_descriptor(instance)->context_bytes;
    } else if (phase == ORB_LIFECYCLE_IDEMPOTENT) {
        orb_screen_context_t *same = context_copy(instance);
        assert(orb_screen_root(instance) == s_initial_root);
        assert(!memcmp(same, s_initial_context, s_context_size));
        free(same);
        free(s_initial_context);
        s_initial_context = NULL;
        s_lifecycle.model = s_generations[1];
    } else if (phase == ORB_LIFECYCLE_CHANGED) {
        assert(orb_screen_root(instance) == s_initial_root);
    } else if (phase == ORB_LIFECYCLE_RESTORED) {
        if (route == ORB_APP_AGENTS_USAGE) assert(orb_app_agents_usage_page(instance) == 1);
        if (s_exercised_scroll) assert(restore_scroll(instance) == s_exercised_scroll);
        s_lifecycle.model = s_generations[2];
    } else if (phase == ORB_LIFECYCLE_REMOVED_ID) {
        (void)restore_scroll(instance);
        s_lifecycle.model = s_generations[3];
        s_lifecycle.idle.count = 0;
        s_lifecycle.idle.cursor[0] = '\0';
    } else {
        (void)restore_scroll(instance);
        assert(phase == ORB_LIFECYCLE_EMPTY);
    }
}

static void lifecycle(void)
{
    for (unsigned i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        fixture_init(&s_lifecycle, s_generations[0]);
        orb_app_agents_model_t models[4];
        orb_screen_model_t views[4];
        orb_screen_update_t updates[4];
        for (unsigned j = 0; j < 4; j++) {
            assert(project_value(&s_lifecycle, s_routes[i], &models[j]));
            models[j].model = s_generations[j];
            views[j] = (orb_screen_model_t){&models[j], sizeof(models[j])};
            updates[j] = (orb_screen_update_t){.model = &views[j], .revisions.model = j + 1};
        }
        orb_nav_t nav;
        orb_nav_init(&nav, s_routes[i]);
        orb_screen_host_t host;
        orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav,
            .context = &s_lifecycle, .project = project, .intent = accept,
            .allow_synthetic = allow_synthetic, .synthetic_refused = refused});
        orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){
            .descriptor = orb_app_agents_descriptor(s_routes[i]),
            .initial = &updates[0], .changed = &updates[1], .removed = &updates[2], .empty = &updates[3],
            .exercise = exercise, .verify = verify, .quiescent = quiescent});
    }
}

static void input_policy(void)
{
    fixture_t fixture;
    fixture_init(&fixture, s_generations[0]);
    orb_agents_host_t *host = host_create(&fixture, false);
    lv_obj_t *page = page_create();
    assert(orb_agents_host_attach(host, page, ORB_APP_AGENTS_CARD));
    orb_screen_instance_t *instance = orb_agents_host_current(host);
    assert(orb_app_agents_activate(instance, ORB_APP_AGENTS_SESSIONS, ORB_INPUT_TOUCH, false, NULL));
    assert(fixture.accepted == 1 && fixture.last.kind == ORB_INTENT_NAVIGATE);
    assert(!orb_app_agents_activate(instance, ACTION_OPTION, ORB_INPUT_TOUCH, false, NULL));
    assert(fixture.accepted == 1 && fixture.refused == 1);
    assert(!orb_app_agents_activate(instance, ACTION_OPTION, ORB_INPUT_CROWN, true, NULL));
    assert(fixture.accepted == 1);
    assert(orb_app_agents_activate(instance, ACTION_OPTION + 1, ORB_INPUT_TOUCH, true, NULL));
    assert(fixture.accepted == 2 && fixture.last.kind == ORB_INTENT_APPROVE && fixture.last.physical);
    orb_screen_context_t *context = context_copy(instance);
    size_t size = orb_screen_descriptor(instance)->context_bytes;
    assert(orb_screen_restore(instance, context, size));
    context->version++;
    assert(!orb_screen_restore(instance, context, size));
    free(context);
    orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    assert(!orb_app_agents_activate(instance, ORB_APP_AGENTS_USAGE, ORB_INPUT_TOUCH, true, NULL));
    assert(!orb_app_agents_activate(instance, ACTION_OPTION, ORB_INPUT_TOUCH, true, NULL));
    assert(fixture.accepted == 2);
    orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    assert(orb_agents_host_detach_page(host, page));
    lv_obj_delete(page);
    orb_agents_host_destroy(host);
    assert(fixture.accepted == 2);

    /* Every migrated route refuses direct preview activation before emitting
     * even a safe navigation, and never acquires an interactive rim lease. */
    unsigned before = fixture.accepted;
    for (unsigned i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        host = host_create(&fixture, true);
        page = page_create();
        assert(orb_agents_host_attach(host, page, s_routes[i]));
        instance = orb_agents_host_current(host);
        assert(!orb_app_agents_activate(instance, ORB_APP_AGENTS_USAGE, ORB_INPUT_TOUCH, true, NULL));
        assert(!orb_app_agents_activate(instance, ACTION_OPTION, ORB_INPUT_TOUCH, true, NULL));
        assert(!orb_screen_rim_acquire(instance));
        lv_obj_delete(page);
        assert(!orb_agents_host_current(host));
        orb_agents_host_destroy(host);
    }
    assert(fixture.accepted == before && fixture.refused == 1);
    quiescent();
}

static void context_and_admission(void)
{
    fixture_t fixture;
    fixture_init(&fixture, s_generations[0]);
    orb_agents_host_t *host = host_create(&fixture, false);
    lv_obj_t *first = page_create();
    assert(orb_agents_host_attach(host, first, ORB_APP_AGENTS_CARD));
    orb_screen_instance_t *card = orb_agents_host_current(host);
    (void)restore_scroll(card);
    int saved = scroll_to(card, 120);
    assert(saved > 0);
    orb_screen_context_t *context = context_copy(card);

    /* Failed projection admission preserves source identity and context. */
    fixture.fail_project = true;
    lv_obj_t *failed = page_create();
    assert(!orb_agents_host_attach(host, failed, ORB_APP_AGENTS_USAGE));
    assert(orb_agents_host_current(host) == card && orb_screen_alive(card));
    orb_screen_context_t *same = context_copy(card);
    assert(!memcmp(context, same, orb_screen_descriptor(card)->context_bytes));
    free(same);
    free(context);
    lv_obj_delete(failed);
    fixture.fail_project = false;

    assert(orb_agents_host_detach_page(host, first));
    lv_obj_delete(first);
    lv_obj_t *second = page_create();
    assert(orb_agents_host_attach(host, second, ORB_APP_AGENTS_CARD));
    assert(restore_scroll(orb_agents_host_current(host)) == saved);
    assert(orb_agents_host_detach_page(host, second));
    lv_obj_delete(second);

    /* An otherwise identical card with a new stable identity starts at zero. */
    orb_model_snapshot_t *different = malloc(sizeof(*different));
    assert(different);
    memcpy(different, fixture.model, sizeof(*different));
    snprintf(different->cards[0].id, sizeof(different->cards[0].id), "approval-b");
    fixture.model = different;
    snprintf(fixture.card, sizeof(fixture.card), "approval-b");
    lv_obj_t *third = page_create();
    assert(orb_agents_host_attach(host, third, ORB_APP_AGENTS_CARD));
    assert(restore_scroll(orb_agents_host_current(host)) == 0);
    assert(orb_agents_host_detach_page(host, third));
    lv_obj_delete(third);

    lv_obj_t *usage = page_create();
    assert(orb_agents_host_attach(host, usage, ORB_APP_AGENTS_USAGE));
    assert(orb_app_agents_usage_change(orb_agents_host_current(host), 1));
    orb_agents_host_t *saved_host = host_create(&fixture, true);
    assert(orb_agents_host_clone_contexts(saved_host, host));
    assert(orb_app_agents_usage_change(orb_agents_host_current(host), 0));
    assert(orb_agents_host_clone_contexts(host, saved_host));
    assert(!orb_agents_host_current(host));
    assert(orb_agents_host_attach(host, usage, ORB_APP_AGENTS_USAGE));
    assert(orb_app_agents_usage_page(orb_agents_host_current(host)) == 1);
    orb_agents_host_destroy(saved_host);
    lv_obj_delete(usage);
    assert(!orb_agents_host_current(host));
    orb_agents_host_destroy(host);
    free(different);
    assert(!fixture.accepted);
    quiescent();
}

static void live_projection(void)
{
    fixture_t live, preview;
    fixture_init(&live, model_create("live"));
    fixture_init(&preview, model_create("preview"));
    orb_agents_host_t *host = host_create(&live, false), *clone = host_create(&preview, true);
    lv_obj_t *page = page_create(), *copy = page_create();
    assert(orb_agents_host_attach(host, page, ORB_APP_AGENTS_SCREEN));
    assert(orb_agents_host_clone_contexts(clone, host));
    assert(orb_agents_host_attach(clone, copy, ORB_APP_AGENTS_SCREEN));
    assert(!strcmp(orb_app_agents_session(orb_agents_host_current(clone)), "preview-0"));
    orb_model_snapshot_t *previous = live.model;
    live.model = model_create("new-generation");
    free(previous);
    snprintf(live.session, sizeof(live.session), "new-generation-0");
    /* Input must reacquire the new generation even before the next UI poll. */
    orb_app_agents_select_focus(orb_agents_host_current(host), 1);
    assert(!strcmp(orb_app_agents_session(orb_agents_host_current(host)), "new-generation-1"));
    assert(!strcmp(orb_app_agents_session(orb_agents_host_current(clone)), "preview-0"));
    assert(orb_app_agents_activate(orb_agents_host_current(host), ACTION_FOCUS_OPEN, ORB_INPUT_TOUCH, false, NULL));
    assert(live.accepted == 1 && !preview.accepted);
    assert(!orb_app_agents_activate(orb_agents_host_current(clone), ACTION_FOCUS_OPEN, ORB_INPUT_TOUCH, true, NULL));
    lv_obj_delete(page);
    orb_agents_host_destroy(host);
    /* The surviving preview keeps its own callback and independent source. */
    assert(orb_agents_host_refresh(clone));
    assert(!strcmp(orb_app_agents_session(orb_agents_host_current(clone)), "preview-0"));
    lv_obj_delete(copy);
    orb_agents_host_destroy(clone);
    free(live.model);
    free(preview.model);
    quiescent();
}

static void membership_and_paging(void)
{
    fixture_t fixture;
    fixture_init(&fixture, model_create("membership"));
    orb_agents_host_t *host = host_create(&fixture, false);
    lv_obj_t *page = page_create();
    assert(orb_agents_host_attach(host, page, ORB_APP_AGENTS_SESSIONS));
    orb_screen_instance_t *instance = orb_agents_host_current(host);
    (void)restore_scroll(instance);
    int top = scroll_to(instance, 150);
    lv_obj_t *viewport = orb_screen_page_viewport(instance);
    snprintf(fixture.model->sessions[0].title, sizeof(fixture.model->sessions[0].title),
        "A substantially longer live session title still updates this row");
    assert(orb_agents_host_refresh(host));
    assert(orb_agents_host_current(host) == instance && orb_screen_page_viewport(instance) == viewport);
    assert(lv_obj_get_scroll_y(viewport) == top);
    fixture.model->prefs.sources[ORB_AGENT_CODEX] = false;
    assert(!orb_agents_host_refresh(host));
    assert(orb_agents_host_current(host) == instance);
    fixture.model->prefs.sources[ORB_AGENT_CODEX] = true;
    assert(orb_agents_host_refresh(host));
    fixture.model->session_count--;
    assert(!orb_agents_host_refresh(host));
    fixture.model->session_count++;
    fixture.idle.pending = true;
    assert(!orb_agents_host_refresh(host));
    assert(orb_app_agents_activate(instance, ACTION_MORE, ORB_INPUT_TOUCH, false, NULL));
    assert(fixture.accepted == 1 && fixture.last.value.command.action == ACTION_MORE);
    assert(orb_agents_host_detach_page(host, page));
    lv_obj_delete(page);
    orb_agents_host_destroy(host);
    free(fixture.model);
    assert(fixture.accepted == 1);
    quiescent();
}

static void direct_crown(void)
{
    fixture_t fixture;
    fixture_init(&fixture, s_generations[0]);
    orb_agents_host_t *host = host_create(&fixture, false);
    lv_obj_t *page = page_create();
    assert(orb_agents_host_attach(host, page, ORB_APP_AGENTS_SCREEN));
    orb_screen_instance_t *instance = orb_agents_host_current(host);
    fixture.interactive = false;
    /* suppress_click fences the release after crown ownership. The existing
     * direct crown callback must still apply all three accumulated detents. */
    orb_app_agents_focus_change(instance, 3);
    assert(orb_app_agents_focus_index(instance) == 3);
    assert(!strcmp(orb_app_agents_session(instance), "session-3"));
    snprintf(fixture.session, sizeof(fixture.session), "%s", orb_app_agents_session(instance));
    assert(!orb_app_agents_activate(instance, ACTION_FOCUS_OPEN, ORB_INPUT_TOUCH, true, NULL));
    orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    orb_app_agents_focus_change(instance, 1);
    assert(orb_app_agents_focus_index(instance) == 3);
    orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    assert(orb_agents_host_detach_page(host, page));
    lv_obj_delete(page);
    page = page_create();
    assert(orb_agents_host_attach(host, page, ORB_APP_AGENTS_USAGE));
    instance = orb_agents_host_current(host);
    assert(orb_app_agents_usage_change(instance, 1));
    assert(orb_app_agents_usage_page(instance) == 1);
    orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    assert(!orb_app_agents_usage_change(instance, 0));
    assert(orb_app_agents_usage_page(instance) == 1);
    lv_obj_delete(page);
    orb_agents_host_destroy(host);
    assert(!fixture.accepted);
    quiescent();
}

static void destruction_during_accept(void)
{
    fixture_t fixture;
    fixture_init(&fixture, s_generations[0]);
    fixture.destroy_host = host_create(&fixture, false);
    fixture.destroy_page = page_create();
    assert(orb_agents_host_attach(fixture.destroy_host, fixture.destroy_page, ORB_APP_AGENTS_CARD));
    orb_screen_instance_t *instance = orb_agents_host_current(fixture.destroy_host);
    fixture.destroy_on_accept = true;
    assert(orb_app_agents_activate(instance, ACTION_OPTION, ORB_INPUT_TOUCH, true, NULL));
    assert(fixture.accepted == 1 && !fixture.destroy_page);
    assert(!orb_agents_host_current(fixture.destroy_host));
    orb_agents_host_destroy(fixture.destroy_host);
    assert(fixture.accepted == 1);
    quiescent();
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    assert(orb_model_init());
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_agents_manifest(), NULL));
    assert(registry.screen_count == sizeof(s_routes) / sizeof(s_routes[0]));
    for (unsigned i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++)
        assert(orb_screen_registry_find(&registry, s_routes[i]) == orb_app_agents_descriptor(s_routes[i]));
    assert(!orb_app_agents_screen(0) && !orb_app_agents_descriptor(99));
    s_generations[0] = model_create("session");
    for (unsigned i = 1; i < 4; i++) {
        s_generations[i] = malloc(sizeof(*s_generations[i]));
        assert(s_generations[i]);
        memcpy(s_generations[i], s_generations[0], sizeof(*s_generations[i]));
    }
    snprintf(s_generations[1]->sessions[0].title, sizeof(s_generations[1]->sessions[0].title), "Live title changed");
    s_generations[2]->sessions[0] = s_generations[2]->sessions[1];
    s_generations[2]->session_count = 1;
    s_generations[2]->card_count = 0;
    s_generations[3]->session_count = s_generations[3]->card_count = s_generations[3]->idle_count = 0;

    /* Warm each composition's shared typography/material caches before exact
     * instance ownership and timer receipts. */
    fixture_t warm;
    fixture_init(&warm, s_generations[0]);
    for (unsigned i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        orb_agents_host_t *host = host_create(&warm, false);
        lv_obj_t *page = page_create();
        assert(orb_agents_host_attach(host, page, s_routes[i]));
        lv_obj_delete(page);
        orb_agents_host_destroy(host);
    }
    settle();
    s_workspace_baseline = used(ORB_MEMORY_WORKSPACE);
    s_timer_baseline = timers();
    lifecycle();
    input_policy();
    context_and_admission();
    live_projection();
    membership_and_paging();
    direct_crown();
    destruction_during_accept();
    for (unsigned i = 0; i < 4; i++) free(s_generations[i]);
    puts("app_agents: seven lifecycles, input policy, context, live generations and membership passed");
    return 0;
}
