/* Real non-Home presenters: Alive changes ink, never the service projection. */
#include "../../firmware/components/orb_shell/orb_agents_host.h"
#include "../../firmware/components/orb_shell/apps_screen.h"
#include "orb_home.h"
#include "orb_shell.h"
#include "orb_app_voice_spoken.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"
#include "orb_kit_motion.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_now = 100000, s_frames;
static orb_model_snapshot_t *s_model;
static orb_idle_page_t s_idle;
static orb_notification_record_t s_receipt;
static unsigned s_commands;
static uint32_t tick(void) { return s_now; }
static void check(bool good, const char *why)
{
    if (good) return;
    fprintf(stderr, "alive screens: %s\n", why);
    exit(EXIT_FAILURE);
}
static void advance(unsigned duration)
{
    for (unsigned i = 0; i < duration; i += 16) { s_now += 16; lv_timer_handler(); }
}
static void rendered(lv_event_t *event) { (void)event; s_frames++; }
static void quiet(void)
{
    advance(1600);
    check(!orb_kit_motion_timer_running(), "settled presentation kept the shared clock running");
    uint32_t frames = s_frames;
    advance(2000);
    check(frames == s_frames, "settled presentation rendered idle frames");
}
static bool project(void *context, orb_screen_id_t screen, orb_app_agents_model_t *out)
{
    (void)context; (void)screen;
    *out = (orb_app_agents_model_t){.model = s_model, .idle = &s_idle, .receipt = &s_receipt,
        .net_connected = true, .link_online = true, .interactive = true, .fixture = true, .outcome_live = true};
    snprintf(out->card, sizeof(out->card), "receipt-a");
    return true;
}
static bool accept(void *context, const orb_intent_t *intent)
{ (void)context; (void)intent; s_commands++; return true; }
static lv_obj_t *page(void)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root); lv_obj_set_size(root, 466, 466);
    return root;
}
static lv_obj_t *arc(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_arc_class)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = arc(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}
static void percent(float value, char *out, size_t size, void *context)
{
    (void)context;
    snprintf(out, size, "%d%%", (int)lroundf(value));
}
static void external_value(void)
{
    lv_obj_t *root = page();
    lv_obj_t *ring = orb_kit_ring_create(root, 180, 6, ORB_KIT_CODEX);
    lv_obj_t *label = lv_label_create(root);
    orb_text_apply(label, ORB_TEXT_TITLE1, ORB_TEXT_TABULAR, 0);
    lv_obj_set_width(label, 240); lv_obj_set_pos(label, 110, 190);
    lv_label_set_text(label, "73%");
    orb_kit_ring_set_formatter(ring, percent, NULL);
    orb_kit_ring_set_value_label(ring, label, "100%");
    orb_kit_ring_set_arrival(ring, 73);
    const char *semantic = NULL;
    check(orb_kit_ring_semantic_text(label, &semantic) && !strcmp(semantic, "73%"), "external counter exposed its sweep sample");
    check(lv_obj_get_x(label) == 110 && lv_obj_get_y(label) == 190, "ring binding moved an external label");
    orb_kit_arrival_complete(root, true); advance(80);
    check(atoi(lv_label_get_text(label)) == (int)lroundf(orb_kit_ring_get_presented_value(ring)), "external number did not count with its arc");
    lv_obj_delete(label); advance(80);
    label = lv_label_create(root); lv_label_set_text(label, "42%");
    orb_kit_ring_set_value_label(ring, label, "100%");
    lv_obj_delete(ring); lv_obj_delete(label);
    lv_obj_delete(root); quiet();
}
static void pop_check(void)
{
    lv_obj_t *root = page();
    lv_obj_t *label = lv_label_create(root);
    orb_icon_apply(label, ORB_TEXT_TITLE2, ORB_TEXT_EMPHASIZED, 0);
    lv_label_set_text(label, ORB_ICON_CHECK);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_size(label, 100, 80); lv_obj_set_pos(label, 180, 180);
    lv_obj_set_hidden(root, true);
    orb_kit_pop_in_arrival(label); orb_kit_check_label_arrival(label);
    advance(80);
    check(lv_obj_get_style_opa(label, 0) == LV_OPA_TRANSP, "hidden arrival drew before commit");
    lv_obj_set_hidden(root, false); orb_kit_arrival_complete(root, true);
    advance(80);
    check(lv_obj_get_style_opa(label, 0) > LV_OPA_TRANSP, "Pop and Draw did not compose on one glyph");
    check(lv_obj_get_style_text_opa(label, 0) == LV_OPA_TRANSP, "Draw exposed the native glyph before completion");
    orb_kit_live_ink_opa(label, LV_OPA_COVER);
    check(lv_obj_get_style_text_opa(label, 0) == LV_OPA_TRANSP, "model restatement interrupted Draw on");
    quiet();
    check(lv_obj_get_style_opa(label, 0) == LV_OPA_COVER && lv_obj_get_style_text_opa(label, 0) == LV_OPA_COVER,
        "composed arrival did not restore final ink");
    check(lv_obj_get_style_transform_scale_x(label, 0) == 256, "composed arrival stranded its scale");
    orb_kit_pop_in_arrival(label); orb_kit_check_label_arrival(label);
    orb_kit_arrival_complete(root, false);
    check(lv_obj_get_style_opa(label, 0) == LV_OPA_COVER && lv_obj_get_style_text_opa(label, 0) == LV_OPA_COVER,
        "Back replayed or hid receipt ink");
    lv_obj_delete(root); quiet();
}
static void usage(void)
{
    s_model->usage.count = 1;
    s_model->usage.windows[0] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE, .used_percent = 73};
    snprintf(s_model->usage.windows[0].label, sizeof(s_model->usage.windows[0].label), "5 hours");
    orb_agents_host_t *host = orb_agents_host_create(false, &(orb_agents_host_config_t){.project = project, .accept = accept});
    lv_obj_t *root = page();
    check(host && orb_agents_host_attach(host, root, ORB_APP_AGENTS_USAGE), "Usage attach failed");
    lv_obj_t *ring = arc(root);
    check(ring != NULL, "Usage ring missing");
    orb_kit_arrival_complete(root, true);
    int32_t target;
    check(orb_kit_ring_semantic_value(ring, &target) && target == 730, "arrival exposed a false Usage target");
    advance(80);
    float value = orb_kit_ring_get_presented_value(ring);
    check(value > 0 && value < 73, "Usage arrival did not sweep");
    check(orb_agents_host_refresh(host), "Usage refresh during arrival failed");
    check(fabsf(value - orb_kit_ring_get_presented_value(ring)) < .01f, "unchanged projection interrupted Usage arrival");
    quiet();
    s_model->usage.windows[0].used_percent = 42;
    check(orb_agents_host_refresh(host), "Usage telemetry refresh failed");
    check(orb_kit_ring_semantic_value(ring, &target) && target == 420, "Follow exposed a false Usage target");
    check(orb_kit_ring_get_presented_value(ring) > 42, "Usage Follow jumped to the endpoint");
    quiet();
    s_model->usage.unknown[0] = true;
    check(orb_agents_host_refresh(host), "unknown Usage refresh failed");
    check(lv_obj_get_style_arc_opa(ring, LV_PART_INDICATOR) == LV_OPA_TRANSP, "unknown Usage invented a gauge value");
    quiet();
    check(!s_commands, "presentation sent a command");
    orb_agents_host_destroy(host); lv_obj_delete(root); quiet();
}
static void dot_wait_restore(void)
{
    s_model->session_count = 1;
    s_model->sessions[0] = (orb_session_t){.agent = ORB_AGENT_DOT, .can_message = true};
    snprintf(s_model->sessions[0].id, sizeof(s_model->sessions[0].id), "dot");
    snprintf(s_model->sessions[0].title, sizeof(s_model->sessions[0].title), "Dot");
    s_model->last_message.accepted = true;
    snprintf(s_model->last_message.message.target, sizeof(s_model->last_message.message.target), "dot");
    snprintf(s_model->last_message.message.text, sizeof(s_model->last_message.message.text), "What changed?");
    orb_agents_host_t *host = orb_agents_host_create(false, &(orb_agents_host_config_t){.project = project, .accept = accept});
    lv_obj_t *root = page();
    check(host && orb_agents_host_attach(host, root, ORB_APP_AGENTS_DOT), "Dot attach failed");
    orb_kit_arrival_complete(root, true); advance(16000);
    check(orb_kit_motion_timer_running(), "named Dot reply wait did not breathe");
    orb_screen_visibility(orb_agents_host_current(host), ORB_VISIBILITY_COVERED);
    advance(16000);
    check(orb_agents_host_detach_page(host, root), "Dot wait context save failed");
    lv_obj_delete(root); root = page();
    check(orb_agents_host_attach(host, root, ORB_APP_AGENTS_DOT), "Dot wait restore failed");
    orb_kit_arrival_complete(root, false); quiet();
    check(!orb_kit_motion_timer_running(), "Dot reconstruction restarted an expired reply wait");
    orb_agents_host_destroy(host); lv_obj_delete(root); quiet();
    s_model->session_count = 0; s_model->last_message.accepted = false;
}
static void receipt_changed(void)
{
    s_receipt = (orb_notification_record_t){.queued = true};
    snprintf(s_receipt.id, sizeof(s_receipt.id), "receipt-a");
    orb_agents_host_t *host = orb_agents_host_create(false, &(orb_agents_host_config_t){.project = project, .accept = accept});
    lv_obj_t *root = page();
    check(host && orb_agents_host_attach(host, root, ORB_APP_AGENTS_CARD), "pending receipt attach failed");
    orb_kit_arrival_complete(root, true); advance(1600);
    check(orb_agents_host_detach_page(host, root), "pending receipt context failed");
    lv_obj_delete(root);
    s_receipt.queued = false; s_receipt.resolved = true;
    s_receipt.result.accepted = s_receipt.result.delivered = true;
    root = page();
    check(orb_agents_host_attach(host, root, ORB_APP_AGENTS_CARD), "delivered receipt rebuild failed");
    /* The shell's ordinary model-rebuild fence cannot cancel a real outcome. */
    orb_kit_arrival_complete(root, false);
    check(orb_kit_motion_settling(root), "real Delivered result did not Draw on after model rebuild");
    advance(80); check(orb_kit_motion_settling(root), "delivered receipt skipped its intermediate ink");
    quiet();
    check(orb_agents_host_detach_page(host, root), "delivered receipt save failed");
    lv_obj_delete(root); root = page();
    check(orb_agents_host_attach(host, root, ORB_APP_AGENTS_CARD), "delivered receipt Back failed");
    orb_kit_arrival_complete(root, false);
    check(!orb_kit_motion_settling(root), "unchanged Delivered result replayed on Back");
    orb_agents_host_destroy(host); lv_obj_delete(root); quiet();
    memset(&s_receipt, 0, sizeof(s_receipt));
}
static lv_obj_t *app_token(lv_obj_t *root, unsigned action)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == action) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = app_token(lv_obj_get_child(root, i), action);
        if (found) return found;
    }
    return NULL;
}
static void apps_advance(orb_shell_t *shell, unsigned duration)
{
    for (unsigned i = 0; i < duration; i += 4) { s_now += 4; orb_shell_poll(shell); lv_timer_handler(); }
}
static void apps_fresh(void)
{
    check(orb_home_init(), "Apps Home fixture failed");
    orb_model_load_demo(); orb_model_set_connection(true, true);
    orb_shell_t *shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    check(shell != NULL, "Apps shell failed");
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70, .usb = true};
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    orb_shell_update(shell, &status); apps_advance(shell, 1200);
    orb_shell_navigate(shell, ORB_SHELL_APPS);
    bool arrived = false;
    for (unsigned i = 0; i < 500; i++) {
        apps_advance(shell, 4);
        orb_shell_open_metrics_t metrics = {0}; orb_shell_open_metrics(shell, &metrics);
        if (metrics.surface == ORB_SHELL_APPS && metrics.transition_end_us && metrics.content_ready && !metrics.retaining_snapshot) {
            arrived = true; break;
        }
    }
    check(arrived, "Apps transition did not finish");
    lv_obj_t *root = orb_shell_root(shell), *token = app_token(root, ORB_SHELL_AGENTS);
    check(token && orb_kit_motion_settling(root), "fresh Face-to-Apps did not admit Pop after its transition");
    int x = lv_obj_get_x(token), y = lv_obj_get_y(token);
    check(lv_obj_get_style_opa(token, 0) == LV_OPA_TRANSP, "Apps snapshot endpoint was not the prepared pose");
    apps_advance(shell, 80);
    check(lv_obj_get_style_opa(token, 0) > LV_OPA_TRANSP && lv_obj_get_style_opa(token, 0) < LV_OPA_COVER,
        "Apps Pop lacked intermediate ink");
    check(lv_obj_get_x(token) == x && lv_obj_get_y(token) == y, "Apps Pop moved the target layout");
    apps_advance(shell, 360);
    check(!orb_kit_motion_settling(root) && !orb_kit_motion_timer_running(), "Apps Pop missed its 450ms deadline");
    unsigned frames = s_frames; apps_advance(shell, 2000);
    check(frames == s_frames, "settled Apps rendered idle frames");
    /* Usage shares the same final-cache ownership: Back must never reuse its
     * prepared minimum, including a programmatic departure during Sweep. */
    orb_usage_snapshot_t usage = {.count = 1, .windows = {{.provider = ORB_AGENT_CLAUDE, .used_percent = 73}}};
    snprintf(usage.windows[0].label, sizeof(usage.windows[0].label), "5 hours");
    check(orb_model_set_usage(&usage), "Usage cache fixture failed");
    orb_shell_navigate(shell, ORB_SHELL_USAGE); apps_advance(shell, 1400);
    static uint8_t expected[SIM_D * SIM_D * 3], actual[sizeof(expected)];
    sim_display_rgb(expected);
    for (unsigned pass = 0; pass < 2; pass++) {
        if (pass) {
            orb_shell_destroy(shell); quiet();
            shell = orb_shell_create(lv_screen_active(), NULL, NULL);
            check(shell != NULL, "fresh Usage shell failed");
            orb_shell_update(shell, &status); apps_advance(shell, 1200);
            orb_shell_navigate(shell, ORB_SHELL_USAGE);
            for (unsigned i = 0; i < 500; i++) {
                apps_advance(shell, 4);
                orb_shell_open_metrics_t metrics = {0}; orb_shell_open_metrics(shell, &metrics);
                if (metrics.surface == ORB_SHELL_USAGE && metrics.transition_end_us && metrics.content_ready && !metrics.retaining_snapshot) break;
            }
            apps_advance(shell, 80);
            float value = orb_kit_ring_get_presented_value(arc(orb_shell_root(shell)));
            check(value > 0 && value < 73, "Usage interrupted-departure fixture missed Sweep");
        }
        orb_shell_navigate(shell, ORB_SHELL_SETTINGS); apps_advance(shell, 1200);
        orb_shell_back(shell); apps_advance(shell, 1400);
        check(orb_shell_surface(shell) == ORB_SHELL_USAGE, "Usage cache Back reached the wrong place");
        sim_display_rgb(actual);
        check(!memcmp(expected, actual, sizeof(expected)), "Back reused a minimum or partial Usage cache plane");
    }
    orb_shell_destroy(shell); quiet();
    /* Contact also suppresses a later snapshot re-arm; Back stays final. */
    orb_apps_view_t *view = orb_apps_view_create(false, NULL, NULL, NULL);
    root = page(); check(view && orb_apps_view_build(view, root, false, true), "Apps admission fixture failed");
    orb_screen_instance_t *screen = orb_apps_view_current(view);
    orb_kit_motion_interrupt(NULL); orb_apps_arrival_prepare(screen, true);
    check(!orb_kit_motion_settling(root), "Apps admission replayed after contact");
    orb_apps_arrival_prepare(screen, false);
    check(!orb_kit_motion_settling(root), "Apps restore replayed Pop");
    orb_apps_view_destroy(view); lv_obj_delete(root); quiet();
}
static void spoken(void)
{
    orb_app_voice_spoken_t *view = orb_app_voice_spoken_create(false, NULL, NULL);
    check(view && orb_app_voice_spoken_present(view, lv_display_get_default(), true, true, NULL, ORB_AGENT_DOT), "spoken wait failed");
    advance(400);
    check(orb_kit_motion_timer_running(), "real Buffering wait did not breathe");
    orb_kit_motion_interrupt(orb_app_voice_spoken_root(view));
    check(orb_kit_motion_timer_running(), "touch interrupted named Buffering wait");
    advance(31000);
    quiet();
    check(orb_app_voice_spoken_present(view, lv_display_get_default(), true, true, NULL, ORB_AGENT_DOT), "repeat spoken projection failed");
    quiet();
    check(!orb_kit_motion_timer_running(), "unchanged Buffering restarted its 30s budget");
    orb_app_voice_spoken_destroy(view); quiet();
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create(); check(display != NULL, "display failed");
    lv_display_add_event_cb(display, rendered, LV_EVENT_RENDER_READY, NULL);
    orb_ui_init(NULL); check(orb_model_init(), "model failed");
    s_model = calloc(1, sizeof(*s_model)); check(s_model != NULL, "fixture failed");
    s_model->online = s_model->synced = s_model->prefs_known = true;
    for (unsigned i = 0; i < 3; i++) s_model->prefs.sources[i] = true;
    external_value(); pop_check(); usage(); dot_wait_restore(); receipt_changed(); spoken(); apps_fresh(); free(s_model);
    puts("alive screens: Usage sweep/follow/unknown semantics and bounded spoken waits passed");
    return EXIT_SUCCESS;
}
