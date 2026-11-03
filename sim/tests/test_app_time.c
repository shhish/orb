/* Production Time descriptor through the shared lifetime driver and the
 * shell's single-route host. Assertions read the value context and semantic
 * queries; service state is read from orb_time itself. */
#include "orb_app_time.h"
#include "orb_app_time_registry.h"
#include "orb_memory.h"
#include "orb_screen_lifecycle.h"
#include "orb_time.h"
#include "orb_time_command.h"
#include "orb_time_host.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t page;
    int32_t scroll[4], sheet_scroll[4], detail_scroll;
    uint32_t fields[4], sheet, field, reminder_id;
    bool reminder_detail;
} time_context_t; /* mirrors the descriptor's published value schema */

static unsigned s_timer_baseline, s_intents;
static size_t s_workspace_baseline;
static orb_app_time_model_t s_model = {.system_input = true};
static orb_screen_model_t s_model_view = {&s_model, sizeof(s_model)};
static uint64_t s_time;

static unsigned timer_count(void)
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

static time_context_t context_of(orb_screen_instance_t *instance)
{
    static unsigned char bytes[sizeof(orb_screen_context_t) + sizeof(time_context_t)];
    assert(orb_app_time_descriptor()->context_bytes == sizeof(bytes));
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    assert(orb_screen_save(instance, context, sizeof(bytes)));
    time_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    return value;
}

static void restore(orb_screen_instance_t *instance, const time_context_t *value)
{
    unsigned char bytes[sizeof(orb_screen_context_t) + sizeof(time_context_t)] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    context->version = orb_app_time_descriptor()->context_version;
    context->size = sizeof(bytes);
    memcpy(context->payload, value, sizeof(*value));
    assert(orb_screen_restore(instance, context, sizeof(bytes)));
    settle();
}

static orb_intent_t s_last;
static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    s_intents++;
    s_last = *value;
    return true;
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_APP_TIME_SCREEN_ID);
    *update = (orb_screen_update_t){.revisions.time = s_time, .model = &s_model_view};
    return true;
}

static void quiescent(void)
{
    settle();
    assert(workspace_used() == s_workspace_baseline);
    assert(timer_count() == s_timer_baseline);
}

static time_context_t s_editor;

static void exercise(orb_screen_instance_t *instance)
{
    /* Open the alarm editor sheet on its second field, then leave. */
    time_context_t value = context_of(instance);
    value.page = ORB_TIME_PAGE_ALARM;
    value.sheet = 1;
    value.field = 2;
    restore(instance, &value);
    s_editor = context_of(instance);
    assert(s_editor.page == ORB_TIME_PAGE_ALARM && s_editor.sheet == 1);
    assert(orb_app_time_local_back(instance));
    assert(!s_intents);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Time"));
    time_context_t value = context_of(instance);
    if (phase == ORB_LIFECYCLE_RESTORED || phase == ORB_LIFECYCLE_REMOVED_ID || phase == ORB_LIFECYCLE_EMPTY) {
        /* Editor context restores whole: page, sheet and its field. */
        assert(value.page == s_editor.page && value.sheet == s_editor.sheet && value.field == s_editor.field);
        assert(orb_app_time_page(instance) == ORB_TIME_PAGE_ALARM);
    } else {
        assert(value.page == ORB_TIME_PAGE_TIMER && !value.sheet);
    }
}

static void lifecycle(orb_screen_host_t *host)
{
    orb_screen_update_t initial = {.revisions.time = ++s_time, .model = &s_model_view};
    orb_screen_update_t changed = {.revisions.time = ++s_time, .model = &s_model_view};
    orb_screen_update_t removed = {.revisions.time = ++s_time, .model = &s_model_view};
    orb_screen_update_t empty = {.revisions.time = ++s_time};
    orb_lifecycle_case_t test = {.descriptor = orb_app_time_descriptor(), .initial = &initial,
        .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(host, &test);
}


/* A rim edit of the timer's minutes leaves the presenter as one typed,
 * value-only command intent; this host records it without executing. */
static void typed_commands(orb_screen_host_t *host)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_time_descriptor(), lv_screen_active(), NULL, false));
    orb_screen_update_t update = {.revisions.time = ++s_time, .model = &s_model_view};
    assert(orb_screen_update(&instance, &update));
    orb_time_snapshot_t before;
    orb_time_snapshot(orb_time_now_us(), &before);
    orb_time_rotary_t owner;
    assert(orb_app_time_rotary(&instance, &owner));
    unsigned count = s_intents;
    orb_app_time_rotate(&instance, owner.value + 2);
    assert(s_intents == count + 1 && s_last.kind == ORB_INTENT_APP_COMMAND && s_last.origin == ORB_INPUT_TOUCH);
    orb_time_command_t command;
    assert(orb_app_time_command_from_intent(&s_last, &command) && command.kind == ORB_TIME_COMMAND_TIMER_SET);
    assert(command.a >= 1 && command.a != before.timer_duration_us / 1000000);
    orb_time_snapshot_t after;
    orb_time_snapshot(orb_time_now_us(), &after);
    assert(after.timer_duration_us == before.timer_duration_us);
    orb_app_time_rotary_finish(&instance, true);
    orb_screen_destroy(&instance);
    assert(s_intents == count + 1);
    quiescent();
}

static void command_codec(void)
{
    for (unsigned kind = ORB_TIME_COMMAND_TIMER_SET; kind <= ORB_TIME_COMMAND_PERSIST_RETRY; kind++) {
        orb_time_command_t sent = {.kind = (orb_time_command_kind_t)kind, .at = INT64_C(1793178540123456),
            .a = 4000000000u, .b = 86400}, received;
        orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND};
        intent.value.command.target = (uint64_t)sent.at;
        intent.value.command.action = sent.kind;
        intent.value.command.value = (int32_t)sent.a;
        intent.value.command.extra = (int32_t)sent.b;
        assert(orb_app_time_command_from_intent(&intent, &received));
        assert(received.kind == sent.kind && received.at == sent.at && received.a == sent.a && received.b == sent.b);
    }
    orb_intent_t foreign = {.kind = ORB_INTENT_APP_COMMAND};
    foreign.value.command.action = ORB_TIME_COMMAND_PERSIST_RETRY + 1;
    orb_time_command_t command;
    assert(!orb_app_time_command_from_intent(&foreign, &command));
    foreign.kind = ORB_INTENT_SETTING_SET;
    foreign.value.command.action = ORB_TIME_COMMAND_TIMER_SET;
    assert(!orb_app_time_command_from_intent(&foreign, &command));
}

static lv_obj_t *page(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 466, 466);
    return parent;
}

/* Presentation lifetime is not runtime lifetime: a running timer and
 * stopwatch survive the screen's destruction, previews and restores. */
static void hidden_runtime(void)
{
    int64_t start = orb_time_now_us();
    assert(orb_time_timer_set(2) && orb_time_timer_start(start));
    assert(orb_time_stopwatch_start(start));
    orb_app_host_t *host = orb_time_host_create(false);
    assert(host);
    lv_obj_t *first = page();
    orb_app_time_model_t model = {.system_input = true};
    assert(orb_time_host_attach(host, first, &model, false, false));
    orb_screen_instance_t *time = orb_app_host_current(host);
    time_context_t value = context_of(time);
    value.page = ORB_TIME_PAGE_STOPWATCH;
    restore(time, &value);
    orb_app_host_detach_page(host, first);
    lv_obj_delete(first);
    assert(!orb_app_host_current(host));
    orb_time_snapshot_t state;
    orb_time_snapshot(start + 1000000, &state);
    assert(state.timer_state == ORB_TIME_RUNNING && state.stopwatch_state == ORB_TIME_RUNNING);
    orb_time_snapshot(start + 2500000, &state);
    assert(state.timer_state == ORB_TIME_FINISHED && state.stopwatch_elapsed_us >= 2500000);

    /* Return restores the remembered page; a preview clone sees it too. */
    lv_obj_t *second = page();
    assert(orb_time_host_attach(host, second, &model, false, false));
    assert(orb_app_time_page(orb_app_host_current(host)) == ORB_TIME_PAGE_STOPWATCH);
    assert(!orb_app_time_page_changed(orb_app_host_current(host)));
    orb_app_host_t *clone = NULL;
    assert(orb_app_host_clone(&clone, host, true));
    lv_obj_t *preview = page();
    assert(orb_time_host_attach(clone, preview, &model, false, false));
    assert(orb_app_time_page(orb_app_host_current(clone)) == ORB_TIME_PAGE_STOPWATCH);
    lv_obj_delete(preview);
    orb_app_host_destroy(clone);

    /* A ringing alarm takes the dial after the restored context, and its
     * presentation never stops, snoozes or saves the service state. */
    orb_app_host_detach_page(host, second);
    lv_obj_delete(second);
    lv_obj_t *third = page();
    assert(orb_time_host_attach(host, third, &model, true, false));
    assert(orb_app_time_page(orb_app_host_current(host)) == ORB_TIME_PAGE_ALARM);
    /* The forced page differs from the remembered one: cached planes retire
     * once, as the workspace's remembered page made them before. */
    assert(orb_app_time_page_changed(orb_app_host_current(host)));
    assert(!orb_app_time_page_changed(orb_app_host_current(host)));
    orb_time_snapshot(start + 2600000, &state);
    assert(state.timer_state == ORB_TIME_FINISHED && state.stopwatch_state == ORB_TIME_RUNNING);

    /* The rim edits values or scrolls only: no rim lift starts anything. */
    orb_time_timer_reset();
    orb_time_stopwatch_reset();
    orb_time_rotary_t owner;
    orb_screen_instance_t *current = orb_app_host_current(host);
    if (orb_app_time_rotary(current, &owner)) {
        orb_app_time_rotate(current, owner.maximum);
        orb_app_time_rotary_finish(current, true);
    }
    orb_time_snapshot(orb_time_now_us(), &state);
    assert(state.timer_state == ORB_TIME_IDLE && state.stopwatch_state != ORB_TIME_RUNNING);
    lv_obj_delete(third);
    assert(!orb_app_host_current(host));
    orb_app_host_destroy(host);
    quiescent();
}

/* The shell's alert layer presents a fresh, context-free Time screen each
 * time; replacing it never restores the previous presentation's page. */
static void alert_layer(void)
{
    orb_app_host_t *alert = NULL;
    assert(orb_time_host_present(&alert, lv_layer_top()));
    orb_screen_instance_t *first = orb_app_host_current(alert);
    assert(first && lv_obj_get_parent(orb_screen_root(first)) == lv_layer_top());
    time_context_t value = context_of(first);
    value.page = ORB_TIME_PAGE_STOPWATCH;
    restore(first, &value);
    assert(orb_app_time_page(first) == ORB_TIME_PAGE_STOPWATCH);
    assert(orb_time_host_present(&alert, lv_layer_top()));
    assert(orb_app_time_page(orb_app_host_current(alert)) == ORB_TIME_PAGE_TIMER);
    orb_app_host_poll(alert);
    orb_app_host_destroy(alert);
    quiescent();
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_time_init();
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_time_manifest(), NULL));
    assert(orb_screen_registry_find(&registry, ORB_APP_TIME_SCREEN_ID) == orb_app_time_descriptor());
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_APP_TIME_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent, .project = project});
    orb_screen_instance_t warm = {0};
    assert(orb_screen_create(&warm, &host, orb_app_time_descriptor(), lv_screen_active(), NULL, false));
    orb_screen_destroy(&warm);
    settle();
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();

    command_codec();
    lifecycle(&host);
    typed_commands(&host);
    hidden_runtime();
    alert_layer();
    printf("app_time: lifecycle, hidden runtime and compatibility host passed\n");
    return 0;
}
