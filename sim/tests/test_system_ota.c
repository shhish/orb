/* The OTA descriptor borrows the shell's shared labels. Exercise that lifetime
 * independently of shell precedence and its existing physical service guards. */
#include "system/ota_screen.h"
#include "system/system_views.h"
#include "orb_app.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_motion.h"
#include "orb_page.h"
#include "orb_memory.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_now = 100000;
static orb_ota_scaffold_t s_port;
static orb_ota_wait_t s_wait;
static lv_obj_t *s_reader_arc;
static orb_ota_page_t s_page;
static orb_shell_snapshot_t s_status;
static unsigned s_commands, s_lines, s_timer_baseline;
static size_t s_workspace_baseline;
static orb_intent_t s_last;
static lv_indev_data_t s_pointer;
static orb_app_host_t *s_destroy_host;
static bool s_readable = true;

static uint32_t tick(void) { return s_now; }
static void advance(unsigned duration)
{
    for (unsigned ms = 0; ms < duration; ms += 16) { s_now += 16; lv_timer_handler(); }
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
static void line(void *context, unsigned which, const char *value,
    orb_text_style_t style, orb_text_variant_t variant, unsigned maximum)
{
    (void)context; (void)maximum;
    lv_obj_t *label = s_port.lines[which];
    lv_obj_set_hidden(label, !value[0]); lv_label_set_text(label, value); orb_text_apply(label, style, variant, 0);
    if (which == 2 && (s_page == ORB_OTA_DOWNLOAD || s_page == ORB_OTA_VERIFY))
        orb_kit_value_set(label, value, "100%");
    s_lines++;
}
static void primary(void *context, const char *label, orb_shell_action_t action, bool enabled)
{ (void)context; (void)label; (void)action; (void)enabled; }
static bool read(void *context, orb_ota_scaffold_t *port)
{ (void)context; *port = s_port; return s_readable; }
static orb_ota_source_t s_source = {read, NULL};
static orb_screen_model_t s_model = {&s_source, sizeof(s_source)};
static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context; assert(id == ORB_OTA_SCREEN_ID);
    *update = (orb_screen_update_t){.revisions.device = 1, .model = &s_model}; return true;
}
static bool record(void *context, const orb_intent_t *intent)
{
    (void)context;
    assert(intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_OTA_SCREEN_ID);
    s_last = *intent; s_commands++; return true;
}
static bool command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    (void)context; assert(orb_screen_alive(instance)); record(NULL, intent);
    if (s_destroy_host) { orb_app_host_destroy(s_destroy_host); s_destroy_host = NULL; }
    return true;
}
static bool synthetic(void *context, const orb_intent_t *intent)
{ (void)context; (void)intent; return true; }
static lv_obj_t *button(void)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(s_port.system); i++) {
        lv_obj_t *object = lv_obj_get_child(s_port.system, i);
        const char *caption = orb_kit_button_get_text(object);
        if (lv_obj_is_clickable(object) && (!strcmp(caption, "Check now") ||
            !strcmp(caption, "Update") || !strcmp(caption, "Try again"))) return object;
    }
    return NULL;
}
static void full(orb_screen_instance_t *instance, orb_ota_page_t page)
{
    s_page = page;
    for (unsigned i = 0; i < ORB_OTA_LINE_COUNT; i++) {
        line(NULL, i, "", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 1);
        lv_obj_remove_local_style_prop(s_port.lines[i], LV_STYLE_TEXT_LETTER_SPACE, 0);
    }
    orb_ota_screen_content(instance, page, &s_status, false);
    orb_ota_screen_layout(instance);
}
static void exercise(orb_screen_instance_t *instance)
{
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    lv_obj_t *root = orb_screen_root(instance), *action = button(); assert(action);
    for (unsigned page = 0; page <= ORB_OTA_FAILED; page++) {
        full(instance, page);
        assert(root == orb_screen_root(instance) && action == button());
        orb_screen_chrome_t chrome; orb_screen_chrome(instance, &chrome);
        bool progress = page >= ORB_OTA_DOWNLOAD && page <= ORB_OTA_READY;
        assert(chrome.inset_ring == progress && lv_obj_is_hidden(action) == progress);
        unsigned before = s_commands;
        lv_obj_send_event(action, LV_EVENT_CLICKED, NULL);
        assert(s_commands == before + 1 && !s_last.physical && s_last.origin == ORB_INPUT_TOUCH);
        assert(s_last.value.command.action == (page == ORB_OTA_OFFER || page == ORB_OTA_FAILED ?
            ORB_SHELL_ACTION_UPDATE : ORB_SHELL_ACTION_CHECK_UPDATE));
        orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
        lv_obj_send_event(action, LV_EVENT_CLICKED, NULL); assert(s_commands == before + 1);
        orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    }
    full(instance, ORB_OTA_DOWNLOAD);
    lv_obj_scroll_to_y(s_port.reading, 12, LV_ANIM_OFF);
    int scroll = lv_obj_get_scroll_y(s_port.reading);
    unsigned lines = s_lines;
    assert(orb_kit_ring_get_presented_value(s_port.ring) == s_status.progress);
    orb_ota_screen_progress(instance, 142);
    const char *target;
    assert(orb_kit_ring_semantic_text(s_port.lines[2], &target) && !strcmp(target, "100%"));
    assert(orb_kit_ring_get_presented_value(s_port.ring) == s_status.progress);
    advance(80);
    assert(orb_kit_ring_get_presented_value(s_port.ring) > s_status.progress &&
        orb_kit_ring_get_presented_value(s_port.ring) < 100);
    advance(250);
    assert(!strcmp(lv_label_get_text(s_port.lines[2]), "100%") && lv_arc_get_value(s_port.ring) == lv_arc_get_max_value(s_port.ring));
    orb_ota_screen_progress(instance, -10);
    assert(orb_kit_ring_semantic_text(s_port.lines[2], &target) && !strcmp(target, "0%"));
    assert(lv_arc_get_value(s_port.ring) > 0); /* Follow preserves its initial ink. */
    advance(450);
    assert(!strcmp(lv_label_get_text(s_port.lines[2]), "0%") && !lv_arc_get_value(s_port.ring));
    assert(s_lines == lines && lv_obj_get_scroll_y(s_port.reading) == scroll);
    /* A progress subtitle's secondary alpha intentionally survives becoming
     * the interrupted-update hero; it is baseline history, not a new token. */
    full(instance, ORB_OTA_FAILED);
    assert(lv_obj_get_style_text_opa(s_port.lines[1], 0) == ORB_KIT_SECONDARY_OPA);
    for (unsigned page = 0; page <= ORB_OTA_FAILED; page++) {
        full(instance, page); lv_obj_update_layout(s_port.reading);
        lv_obj_scroll_to_y(s_port.reading, 8 + (int)page, LV_ANIM_OFF);
        int remembered = lv_obj_get_scroll_y(s_port.reading);
        orb_ota_screen_capture(instance, page);
        lv_obj_scroll_to_y(s_port.reading, 0, LV_ANIM_OFF);
        orb_ota_screen_layout(instance);
        assert(lv_obj_get_scroll_y(s_port.reading) == remembered);
    }
}
static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    (void)phase;
    assert(orb_screen_root(instance) && !lv_obj_is_clickable(orb_screen_root(instance)));
    assert(orb_screen_page_viewport(instance) == s_port.reading);
    unsigned before = s_commands;
    unsigned char bytes[64] = {0}; orb_screen_context_t *context = (void *)bytes;
    assert(orb_screen_save(instance, context, sizeof(bytes)));
    assert(context->version == 2 && context->size == 40);
    context->version++; assert(!orb_screen_restore(instance, context, sizeof(bytes))); context->version--;
    context->size++; assert(!orb_screen_restore(instance, context, sizeof(bytes))); context->size--;
    assert(orb_screen_restore(instance, context, sizeof(bytes)) && before == s_commands);
}
static void lifecycle(void)
{
    orb_screen_host_t host; orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project,
        .intent = record, .allow_synthetic = synthetic, .defer_rim = true});
    orb_screen_update_t initial = {.revisions.device = 1, .model = &s_model};
    orb_screen_update_t changed = {.revisions.device = 2, .model = &s_model};
    orb_screen_update_t removed = {.revisions.device = 3}, empty = {.revisions.device = 4};
    orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){.descriptor = orb_ota_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent});
}
static orb_app_host_t *attach(bool preview)
{
    orb_app_host_t *host = orb_ota_host_create(preview, command, NULL); assert(host);
    assert(orb_ota_host_ensure(host, s_port.system, &s_source)); return host;
}
static void host_lifetime(void)
{
    orb_app_host_t *host = attach(false); orb_screen_instance_t *instance = orb_app_host_current(host);
    full(instance, ORB_OTA_OFFER);
    lv_obj_t *action = button(), *title = orb_screen_page_title(instance);
    unsigned lines = s_lines;
    assert(orb_ota_host_ensure(host, s_port.system, &s_source));
    orb_app_host_poll(host); orb_app_host_environment(host);
    assert(action == button() && title == orb_screen_page_title(instance) && lines == s_lines);
    orb_screen_context_t *context = calloc(1, orb_ota_descriptor()->context_bytes); assert(context);
    full(instance, ORB_OTA_FAILED); lv_obj_scroll_to_y(s_port.reading, 20, LV_ANIM_OFF);
    int remembered = lv_obj_get_scroll_y(s_port.reading); assert(remembered > 0);
    assert(lv_obj_is_visible(s_reader_arc));
    lv_obj_set_hidden(s_port.reading, true);
    orb_kit_scroll_arc_refresh(s_port.reading);
    assert(lv_obj_is_hidden(s_reader_arc));
    orb_ota_screen_capture(instance, ORB_OTA_FAILED);
    orb_ota_screen_layout(instance);
    assert(lv_obj_is_visible(s_port.reading) && lv_obj_is_visible(s_reader_arc));
    /* Layout restored the previously captured scroll, as in the shell. */
    lv_obj_scroll_to_y(s_port.reading, remembered, LV_ANIM_OFF);
    orb_ota_screen_capture(instance, ORB_OTA_FAILED);
    assert(orb_screen_save(instance, context, orb_ota_descriptor()->context_bytes));
    lv_obj_t *reader = s_port.reading, *arc = s_reader_arc;
    orb_app_host_detach_page(host, s_port.system); assert(!button());
    assert(lv_obj_is_valid(reader) && lv_obj_is_valid(arc));
    assert(orb_ota_host_ensure(host, s_port.system, &s_source));
    instance = orb_app_host_current(host); full(instance, ORB_OTA_FAILED);
    assert(s_port.reading == reader && s_reader_arc == arc && lv_obj_is_visible(arc));
    assert(lv_obj_get_scroll_y(reader) == remembered);
    free(context); orb_app_host_destroy(host); quiescent();
    host = attach(true); instance = orb_app_host_current(host); full(instance, ORB_OTA_OFFER);
    unsigned before = s_commands; lv_obj_send_event(button(), LV_EVENT_CLICKED, NULL);
    assert(before == s_commands && orb_screen_back(instance) == ORB_BACK_CONSUMED);
    orb_app_host_destroy(host); quiescent();
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used; assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_ota_host_create(false, command, NULL)); orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    host = orb_ota_host_create(false, command, NULL); assert(host);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats); reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_ota_host_ensure(host, s_port.system, &s_source) && !orb_app_host_current(host));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    s_readable = false; assert(!orb_ota_host_ensure(host, s_port.system, &s_source));
    s_readable = true; assert(orb_ota_host_ensure(host, s_port.system, &s_source));
    orb_app_host_destroy(host); quiescent();
    s_destroy_host = attach(false); full(orb_app_host_current(s_destroy_host), ORB_OTA_OFFER);
    lv_obj_send_event(button(), LV_EVENT_CLICKED, NULL); assert(!s_destroy_host && !button()); quiescent();
}
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_pointer; }
static void physical(bool synthetic_input)
{
    orb_app_host_t *host = attach(false); full(orb_app_host_current(host), ORB_OTA_OFFER);
    lv_obj_update_layout(s_port.system); lv_area_t bounds; lv_obj_get_coords(button(), &bounds);
    s_pointer.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    lv_indev_t *input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, pointer_read); if (synthetic_input) orb_input_mark_synthetic(input);
    unsigned before = s_commands;
    s_pointer.state = LV_INDEV_STATE_PRESSED; lv_indev_read(input); advance(16);
    s_pointer.state = LV_INDEV_STATE_RELEASED; lv_indev_read(input); advance(16);
    assert(s_commands == before + 1 && s_last.physical == !synthetic_input);
    lv_indev_delete(input); orb_app_host_destroy(host); quiescent();
}
static void scaffold(void)
{
    s_port.system = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(s_port.system);
    lv_obj_set_size(s_port.system, 466, 466); lv_obj_set_scrollable(s_port.system, false);
    s_port.reading = lv_obj_create(s_port.system); lv_obj_remove_style_all(s_port.reading);
    lv_obj_set_scroll_dir(s_port.reading, LV_DIR_VER); lv_obj_set_scroll_chain(s_port.reading, false);
    s_reader_arc = orb_kit_scroll_arc_attach(s_port.reading); assert(s_reader_arc);
    s_port.ring = orb_kit_ring_create(s_port.system, 218, 10, ORB_KIT_CODEX);
    for (unsigned i = 0; i < ORB_OTA_LINE_COUNT; i++) {
        s_port.lines[i] = lv_label_create(s_port.system);
        lv_obj_set_style_text_align(s_port.lines[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t **hidden[] = {&s_port.art, &s_port.wifi, &s_port.battery, &s_port.power,
        &s_port.settings, &s_port.primary, &s_port.cancel};
    for (unsigned i = 0; i < sizeof(hidden) / sizeof(*hidden); i++) *hidden[i] = lv_obj_create(s_port.system);
    s_port.line = line; s_port.primary_action = primary; s_port.wait = &s_wait;
}
static void waiting_recreation(void)
{
    s_wait = (orb_ota_wait_t){0};
    orb_app_host_t *host = attach(false);
    full(orb_app_host_current(host), ORB_OTA_VERIFY);
    uint32_t started = s_wait.started;
    advance(20000); assert(orb_kit_motion_timer_running());
    orb_app_host_destroy(host); lv_obj_delete(s_port.system);
    scaffold(); host = attach(false);
    full(orb_app_host_current(host), ORB_OTA_VERIFY);
    assert(s_wait.waiting && s_wait.started == started);
    advance(11000); assert(!orb_kit_motion_timer_running());
    orb_app_host_destroy(host);
}

static void invalidation(void)
{
    orb_shell_snapshot_t before = {0}, after = {0};
    assert(!orb_system_views_update(NULL, &after, &before));
    after.net = ORB_SHELL_NET_CONNECTED; assert(!orb_system_views_update(NULL, &after, &before));
    after.update_check_revision++; assert(orb_system_views_update(NULL, &after, &before));
    after = before; strcpy(after.firmware_version, "1.2.3"); assert(orb_system_views_update(NULL, &after, &before));
    after = before; strcpy(after.update_check_result, "No update"); assert(orb_system_views_update(NULL, &after, &before));
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    assert(orb_screen_descriptor_valid(orb_ota_descriptor(), NULL));
    size_t empty_used = used(); unsigned empty_timers = timers();
    scaffold();
    s_status = (orb_shell_snapshot_t){.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .version = "v1.2.3-test", .firmware_version = "v1.1.0-test", .battery_pct = 20, .progress = 42,
        .notes = "A long release note with details about this software and its installation on Orb. Read everything before updating.",
        .error = "The update stopped before completion. Reconnect your Mac."};
    orb_app_host_t *host = attach(false); exercise(orb_app_host_current(host)); orb_app_host_destroy(host);
    advance(1600); s_workspace_baseline = used(); s_timer_baseline = timers();
    lifecycle(); host_lifetime(); physical(false); physical(true); invalidation(); waiting_recreation();
    host = attach(false); full(orb_app_host_current(host), ORB_OTA_FAILED);
    /* Shared scaffold nodes precede the descriptor in parent deletion order. */
    lv_obj_delete(s_port.system); assert(!orb_app_host_current(host)); orb_app_host_destroy(host);
    advance(1600); assert(used() == empty_used && timers() == empty_timers);
    puts("system_ota: shared lifecycle, retained controls/scroll/style, cadence, intents, preview, failure and cleanup passed");
    return 0;
}
