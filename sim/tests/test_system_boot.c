/* Exercise the real shared scaffold and setup descriptor without service tasks. */
#include "system/boot_screen.h"
#include "orb_app.h"
#include "orb_faces_host.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_motion.h"
#include "orb_page.h"
#include "orb_memory.h"
#include "orb_screen_lifecycle.h"
#include "orb_system_copy.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_now = 100000;
static orb_system_scaffold_t s_scaffold;
static orb_system_scaffold_t *s_projection = &s_scaffold;
static orb_shell_snapshot_t s_status;
static orb_screen_instance_t *s_input_instance;
static orb_app_host_t *s_destroy_host;
static orb_intent_t s_last;
static unsigned s_commands, s_timer_baseline;
static size_t s_workspace_baseline;
static lv_indev_data_t s_pointer;

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
    unsigned before = s_commands;
    advance(1600);
    assert(used() == s_workspace_baseline && timers() == s_timer_baseline && before == s_commands);
}
static orb_screen_model_t s_model = {&s_projection, sizeof(s_projection)};
static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context; assert(id == ORB_BOOT_SCREEN_ID);
    *update = (orb_screen_update_t){.revisions.device = 1, .model = &s_model}; return true;
}
static bool record(void *context, const orb_intent_t *intent)
{
    (void)context;
    assert(intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_BOOT_SCREEN_ID);
    assert(intent->value.command.action == ORB_BOOT_OPEN_CONNECTION);
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
static void clicked(lv_event_t *event)
{
    (void)event;
    orb_boot_screen_activate(s_input_instance);
}
static void full(orb_screen_instance_t *instance, view_t view)
{
    for (unsigned i = 0; i < LINE_COUNT; i++) {
        orb_system_scaffold_line(&s_scaffold, view, i, "", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 1);
        lv_obj_remove_local_style_prop(s_scaffold.lines[i], LV_STYLE_TEXT_LETTER_SPACE, 0);
    }
    orb_system_scaffold_primary(&s_scaffold, "", false);
    lv_obj_set_hidden(s_scaffold.art, true);
    orb_boot_screen_content(instance, &s_scaffold, view, &s_status);
    orb_system_scaffold_identity(&s_scaffold, true);
    orb_system_scaffold_status(&s_scaffold, &s_status);
    orb_boot_screen_layout(&s_scaffold, view);
    if (view != VIEW_SETUP && view != VIEW_PAIR) orb_system_scaffold_layout_status(&s_scaffold, &s_status);
}
static void exercise(orb_screen_instance_t *instance)
{
    const view_t pages[] = {VIEW_START, VIEW_SETUP, VIEW_PAIR, VIEW_SERVICE, VIEW_IDENTIFY};
    lv_obj_t *reader = s_scaffold.reading, *title = s_scaffold.system_title;
    for (unsigned i = 0; i < sizeof(pages) / sizeof(*pages); i++) {
        full(instance, pages[i]);
        assert(reader == s_scaffold.reading && title == s_scaffold.system_title);
        assert(orb_screen_page_viewport(instance) == reader && !orb_screen_page_title(instance));
        assert(lv_obj_is_hidden(reader) == (pages[i] != VIEW_SETUP && pages[i] != VIEW_PAIR));
        unsigned before = s_commands;
        orb_boot_screen_activate(instance); assert(s_commands == before + 1 && !s_last.physical);
        orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
        orb_boot_screen_activate(instance); assert(s_commands == before + 1);
        orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    }
    full(instance, VIEW_SETUP);
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_CODE]), "1234 5678"));
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_COMMAND]), "orb-hub setup"));
    assert(lv_obj_get_style_text_opa(s_scaffold.lines[LINE_BODY], 0) == ORB_KIT_SECONDARY_OPA);
    full(instance, VIEW_PAIR);
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_CODE]), "123 456"));
    assert(lv_obj_get_style_text_letter_space(s_scaffold.lines[LINE_CODE], 0) == 4);
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_DETAIL]), "Found A Mac with a long visible name"));
    /* Parent and secondary opacity survive changing back to an ordinary page. */
    full(instance, VIEW_SERVICE);
    assert(lv_obj_get_parent(s_scaffold.lines[LINE_BODY]) == s_scaffold.system);
    assert(lv_obj_get_style_text_opa(s_scaffold.lines[LINE_BODY], 0) == ORB_KIT_SECONDARY_OPA);
    assert(!strcmp(orb_kit_button_get_text(s_scaffold.primary), "Connection"));
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    orb_rim_binding_t binding; assert(!orb_boot_descriptor()->ops->rim_binding(instance, &binding));
}
static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    (void)phase;
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    unsigned before = s_commands;
    orb_screen_context_t context = {0};
    assert(orb_screen_save(instance, &context, sizeof(context)) && context.version == 1 && context.size == sizeof(context));
    context.version++; assert(!orb_screen_restore(instance, &context, sizeof(context))); context.version--;
    context.size++; assert(!orb_screen_restore(instance, &context, sizeof(context))); context.size--;
    assert(orb_screen_restore(instance, &context, sizeof(context)) && before == s_commands);
}
static void lifecycle(void)
{
    orb_screen_host_t host; orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project,
        .intent = record, .allow_synthetic = synthetic, .defer_rim = true});
    orb_screen_update_t initial = {.revisions.device = 1, .model = &s_model};
    orb_screen_update_t changed = {.revisions.device = 2, .model = &s_model};
    orb_screen_update_t removed = {.revisions.device = 3}, empty = {.revisions.device = 4};
    orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){.descriptor = orb_boot_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent});
}
static orb_app_host_t *attach(bool preview)
{
    orb_app_host_t *host = orb_boot_host_create(preview, command, NULL); assert(host);
    assert(orb_boot_host_ensure(host, s_scaffold.system, &s_scaffold)); return host;
}
static void host_lifetime(void)
{
    orb_app_host_t *host = attach(false); orb_screen_instance_t *instance = orb_app_host_current(host);
    full(instance, VIEW_PAIR); lv_obj_update_layout(s_scaffold.reading);
    lv_obj_scroll_to_y(s_scaffold.reading, 20, LV_ANIM_OFF);
    int scroll = lv_obj_get_scroll_y(s_scaffold.reading); assert(scroll > 0);
    unsigned before = s_commands;
    orb_app_host_poll(host); orb_app_host_environment(host);
    assert(lv_obj_get_scroll_y(s_scaffold.reading) == scroll && before == s_commands);
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_CODE]), "123 456"));
    lv_obj_t *reader = s_scaffold.reading, *title = s_scaffold.system_title;
    orb_app_host_detach_page(host, s_scaffold.system);
    assert(lv_obj_is_valid(reader) && lv_obj_is_valid(title));
    assert(orb_boot_host_ensure(host, s_scaffold.system, &s_scaffold));
    /* Context contains no scroll: only the shell resets it on a view change. */
    assert(lv_obj_get_scroll_y(reader) == scroll);
    instance = orb_app_host_current(host);
    s_status.net = ORB_SHELL_NET_CONNECTING; full(instance, VIEW_SETUP);
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_CODE]), ""));
    s_status.net = ORB_SHELL_NET_FAILED; full(instance, VIEW_SETUP);
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_DETAIL]), "Run setup again."));
    s_status.pop[2] = 'x'; full(instance, VIEW_SETUP);
    assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_BODY]), "Preparing setup code"));
    strcpy(s_status.pop, "12345678"); s_status.net = ORB_SHELL_NET_PROVISIONING;
    orb_app_host_destroy(host); quiescent();
    host = attach(true); instance = orb_app_host_current(host); full(instance, VIEW_SERVICE);
    before = s_commands; orb_boot_screen_activate(instance);
    assert(before == s_commands && orb_screen_back(instance) == ORB_BACK_CONSUMED);
    orb_app_host_destroy(host); quiescent();
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used; assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_boot_host_create(false, command, NULL)); orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    host = orb_boot_host_create(false, command, NULL); assert(host);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats); reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_boot_host_ensure(host, s_scaffold.system, &s_scaffold) && !orb_app_host_current(host));
    full(NULL, VIEW_SETUP); assert(!strcmp(lv_label_get_text(s_scaffold.lines[LINE_CODE]), "1234 5678"));
    full(NULL, VIEW_SERVICE); assert(!strcmp(orb_kit_button_get_text(s_scaffold.primary), "Connection"));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(!orb_boot_host_ensure(host, s_scaffold.system, NULL));
    assert(orb_boot_host_ensure(host, s_scaffold.system, &s_scaffold)); orb_app_host_destroy(host); quiescent();
    s_destroy_host = attach(false); full(orb_app_host_current(s_destroy_host), VIEW_SERVICE);
    orb_boot_screen_activate(orb_app_host_current(s_destroy_host)); assert(!s_destroy_host); quiescent();
}
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_pointer; }
static void physical(bool synthetic_input)
{
    orb_app_host_t *host = attach(false); s_input_instance = orb_app_host_current(host);
    full(s_input_instance, VIEW_SERVICE);
    lv_obj_update_layout(s_scaffold.system); lv_area_t bounds; lv_obj_get_coords(s_scaffold.primary, &bounds);
    s_pointer.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    lv_indev_t *input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, pointer_read); if (synthetic_input) orb_input_mark_synthetic(input);
    unsigned before = s_commands;
    s_pointer.state = LV_INDEV_STATE_PRESSED; lv_indev_read(input); advance(16);
    s_pointer.state = LV_INDEV_STATE_RELEASED; lv_indev_read(input); advance(16);
    assert(s_commands == before + 1 && s_last.physical == !synthetic_input);
    lv_indev_delete(input); orb_app_host_destroy(host); s_input_instance = NULL; quiescent();
}
static lv_obj_t *find(lv_obj_t *root, const char *value, bool button)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (button ? !strcmp(orb_kit_button_get_text(root), value) :
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), value, button);
        if (found) return found;
    }
    return NULL;
}
static void shell_fallback(void)
{
    /* Leave exactly the measured public privacy-host reservation required by
     * shell creation, so only its later boot-host admission is refused. */
    size_t before = used();
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    orb_app_host_t *privacy = orb_faces_host_public_create(parent); assert(privacy);
    size_t required = used() - before;
    orb_app_host_destroy(privacy); lv_obj_delete(parent); quiescent();
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used - required;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    uint32_t rejections = stats.rejections;
    orb_shell_t *shell = orb_shell_create(NULL, NULL, NULL); assert(shell);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats); assert(stats.rejections > rejections);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_STARTING, .battery_pct = -1,
        .service_error = "Local service unavailable"};
    orb_shell_update(shell, &status); advance(1600);
    lv_obj_t *root = orb_shell_root(shell);
    assert(find(root, status.service_error, false));
    lv_obj_t *connection = find(root, "Connection", true); assert(connection);
    /* Releasing admission capacity does not render or create a boot instance.
     * Its existing button must still take the shell's original menu path. */
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    lv_obj_send_event(connection, LV_EVENT_CLICKED, NULL); advance(1600);
    assert(find(root, "Maintenance", true));
    orb_shell_destroy(shell); quiescent();
}
static void recreate_scaffold(void)
{
    lv_obj_t *system = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(system);
    lv_obj_set_size(system, 466, 466); lv_obj_set_scrollable(system, false);
    orb_system_scaffold_create(&s_scaffold, system, clicked, NULL);
    s_scaffold.cancel = orb_kit_button_secondary_create(system, "Cancel");
}

static void waiting_recreation(void)
{
    orb_shell_snapshot_t saved = s_status;
    const view_t phases[] = {VIEW_START, VIEW_SETUP, VIEW_PAIR};
    s_status.pop[0] = s_status.pairing_code[0] = '\0';
    for (unsigned i = 0; i < sizeof(phases) / sizeof(*phases); i++) {
        s_scaffold.boot_wait_kind = 0;
        orb_app_host_t *host = attach(false); full(orb_app_host_current(host), phases[i]);
        orb_kit_arrival_complete(s_scaffold.system, false);
        uint32_t started = s_scaffold.boot_wait_started; assert(s_scaffold.boot_wait_kind != 0);
        if (i) assert(lv_obj_get_style_text_opa(s_scaffold.lines[LINE_BODY], 0) == LV_OPA_COVER);
        advance(20000); assert(orb_kit_motion_timer_running());
        orb_app_host_destroy(host); lv_obj_delete(s_scaffold.system);
        recreate_scaffold(); host = attach(false); full(orb_app_host_current(host), phases[i]);
        orb_kit_arrival_complete(s_scaffold.system, false);
        assert(s_scaffold.boot_wait_started == started && orb_kit_motion_timer_running());
        advance(11000); assert(!orb_kit_motion_timer_running());
        (void)sim_display_take_changed(); advance(2000); assert(!sim_display_take_changed());
        orb_app_host_destroy(host);
    }
    s_status = saved;
}

static void about(void)
{
    orb_shell_snapshot_t a = {.rssi = -56}, b = a;
    assert(!orb_system_about_changed(&a, &b)); b.rssi = -55; assert(orb_system_about_changed(&a, &b));
    b = a; b.rssi = -60; assert(!orb_system_about_changed(&a, &b));
    b = a; strcpy(b.ssid, "network"); assert(orb_system_about_changed(&a, &b));
    b = a; strcpy(b.hub_name, "Mac"); assert(orb_system_about_changed(&a, &b));
    b = a; strcpy(b.firmware_version, "1.2.3"); assert(orb_system_about_changed(&a, &b));
    b = a; b.progress++; assert(!orb_system_about_changed(&a, &b));
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    assert(orb_screen_descriptor_valid(orb_boot_descriptor(), NULL));
    size_t empty_used = used(); unsigned empty_timers = timers();
    lv_obj_t *system = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(system);
    lv_obj_set_size(system, 466, 466); lv_obj_set_scrollable(system, false);
    orb_system_scaffold_create(&s_scaffold, system, clicked, NULL);
    s_scaffold.cancel = orb_kit_button_secondary_create(system, "Cancel");
    s_status = (orb_shell_snapshot_t){.net = ORB_SHELL_NET_PROVISIONING, .battery_pct = -1,
        .pop = "12345678", .pairing_code = "123456", .hub_name = "A Mac with a long visible name",
        .service_error = "A local service could not start.", .device_name = "Desk Orb", .device_id = "orb-test"};
    orb_app_host_t *host = attach(false); exercise(orb_app_host_current(host)); orb_app_host_destroy(host);
    advance(1600); s_workspace_baseline = used(); s_timer_baseline = timers();
    lifecycle(); host_lifetime(); physical(false); physical(true); shell_fallback(); about(); waiting_recreation();
    host = attach(false); full(orb_app_host_current(host), VIEW_SETUP);
    lv_obj_delete(s_scaffold.system); assert(!orb_app_host_current(host)); orb_app_host_destroy(host);
    advance(1600); assert(used() == empty_used && timers() == empty_timers);
    puts("system_boot: shared lifecycle/scaffold, code/layout history, intents, refusal fallback and cleanup passed");
    return 0;
}
