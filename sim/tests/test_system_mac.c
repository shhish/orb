/* Mac pages rebuild at the shell boundary; the descriptor owns only content.
 * Test lifetime, input and copy boundaries independently of shell policy. */
#include "system/mac_screen.h"
#include "orb_system_copy.h"
#include "orb_app.h"
#include "orb_input.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_memory.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_now = 100000;
static unsigned s_commands, s_timer_baseline;
static size_t s_workspace_baseline;
static orb_mac_model_t s_initial = {.net = ORB_SHELL_NET_FAILED,
    .ssid = "Fixture Wi-Fi", .hub_name = "Fixture Mac"};
static orb_screen_model_t s_model = {&s_initial, sizeof(s_initial)};
static orb_intent_t s_last;
static lv_indev_data_t s_pointer;
static orb_app_host_t *s_destroy_host;
static lv_obj_t *s_destroy_parent;

static uint32_t tick(void) { return s_now; }
static void advance(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 16) {
        s_now += 16; lv_timer_handler();
    }
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
    assert(used() == s_workspace_baseline && timers() == s_timer_baseline && s_commands == before);
}
static lv_obj_t *text(lv_obj_t *root, const char *value)
{
    if (lv_obj_is_hidden(root)) return NULL;
    const char *caption = orb_kit_button_get_text(root);
    if ((caption && !strcmp(caption, value)) ||
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), value); if (found) return found;
    }
    return NULL;
}
static lv_obj_t *action(lv_obj_t *root, unsigned command)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == command) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = action(lv_obj_get_child(root, i), command); if (found) return found;
    }
    return NULL;
}
static bool record(void *context, const orb_intent_t *intent)
{
    (void)context;
    assert(intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_MAC_SCREEN_ID);
    s_last = *intent; s_commands++; return true;
}
static bool command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    (void)context; assert(orb_screen_alive(instance));
    bool accepted = record(NULL, intent);
    if (s_destroy_host) {
        orb_app_host_destroy(s_destroy_host); s_destroy_host = NULL;
        lv_obj_delete(s_destroy_parent); s_destroy_parent = NULL;
    }
    return accepted;
}
static bool synthetic(void *context, const orb_intent_t *intent) { (void)context; (void)intent; return true; }
static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context; assert(id == ORB_MAC_SCREEN_ID);
    *update = (orb_screen_update_t){.revisions.device = 1, .model = &s_model}; return true;
}
static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    (void)phase;
    lv_obj_t *root = orb_screen_root(instance), *reader = orb_screen_page_viewport(instance);
    assert(root && reader && !lv_obj_is_clickable(root));
    /* Revision updates never compose again or replace the initial copy. */
    const char *titles[] = {"Mac", "Maintenance", "Forget Wi-Fi?", "Unpair this Mac?"};
    orb_screen_chrome_t chrome; orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, titles[s_initial.page]));
    if (s_initial.page == ORB_MAC_CONNECTION) assert(text(root, "Fixture Wi-Fi") && text(root, "Wi-Fi failed"));
    if (s_initial.page == ORB_MAC_MAINTENANCE) assert(lv_obj_is_hidden(reader) && action(root, 2) && action(root, 3));
    if (s_initial.page == ORB_MAC_FORGET) assert(text(root, "You'll need to set up Wi-Fi again."));
    if (s_initial.page == ORB_MAC_UNPAIR) assert(text(root, "Orb will ask for a new pairing code. Wi-Fi stays connected."));
}
static void exercise(orb_screen_instance_t *instance)
{
    unsigned before = s_commands;
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    orb_screen_context_t context = {0};
    assert(orb_screen_save(instance, &context, sizeof(context)));
    assert(context.version == 1 && context.size == sizeof(context));
    context.version++; assert(!orb_screen_restore(instance, &context, sizeof(context)));
    context.version--; context.size++; assert(!orb_screen_restore(instance, &context, sizeof(context)));
    lv_obj_t *root = orb_screen_root(instance);
    for (unsigned id = 1; id <= 5; id++) {
        lv_obj_t *button = action(root, id); if (!button) continue;
        lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
        assert(s_commands == ++before && s_last.value.command.action == id);
        assert(s_last.origin == ORB_INPUT_TOUCH && !s_last.physical);
        orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
        lv_obj_send_event(button, LV_EVENT_CLICKED, NULL); assert(s_commands == before);
        orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    }
}
static void lifecycle(void)
{
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project,
        .intent = record, .allow_synthetic = synthetic, .defer_rim = true});
    orb_mac_model_t replacement = {.page = ORB_MAC_UNPAIR};
    orb_screen_model_t changed_model = {&replacement, sizeof(replacement)};
    orb_screen_update_t initial = {.revisions.device = 1, .model = &s_model};
    orb_screen_update_t changed = {.revisions.device = 2, .model = &changed_model};
    orb_screen_update_t removed = {.revisions.device = 3}, empty = {.revisions.device = 4};
    for (unsigned page = 0; page <= ORB_MAC_UNPAIR; page++) {
        s_initial.page = page;
        orb_lifecycle_case_t test = {.descriptor = orb_mac_descriptor(), .initial = &initial,
            .changed = &changed, .removed = &removed, .empty = &empty,
            .exercise = exercise, .verify = verify, .quiescent = quiescent};
        orb_test_screen_lifecycle(&host, &test);
    }
}
static lv_obj_t *page(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 466, 466); lv_obj_set_scrollable(parent, false); return parent;
}
static orb_shell_snapshot_t status(void)
{
    return (orb_shell_snapshot_t){.net = ORB_SHELL_NET_FAILED, .ssid = "Temporary Wi-Fi", .hub_name = "Temporary Mac"};
}
static orb_app_host_t *attach(lv_obj_t *parent, orb_mac_page_t selected, bool preview)
{
    orb_app_host_t *host = orb_mac_host_create(preview, command, NULL); assert(host);
    orb_shell_snapshot_t model = status();
    assert(orb_mac_host_attach(host, parent, selected, &model, 0)); return host;
}
static void projection_and_rebuild(void)
{
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    lv_obj_t *parent = page(); orb_app_host_t *host = orb_mac_host_create(false, command, NULL); assert(host);
    orb_shell_snapshot_t model = status();
    assert(orb_mac_host_attach(host, parent, ORB_MAC_CONNECTION, &model, 0));
    memset(&model, 0, sizeof(model));
    assert(text(parent, "Temporary Wi-Fi") && text(parent, "Temporary Mac"));
    lv_obj_t *reader = orb_mac_host_reader(host); lv_obj_update_layout(reader);
    lv_obj_scroll_to_y(reader, 40, LV_ANIM_OFF);
    int16_t remembered = (int16_t)lv_obj_get_scroll_y(reader); assert(remembered > 0);
    orb_app_host_environment(host); orb_app_host_poll(host);
    assert(reader == orb_mac_host_reader(host) && lv_obj_get_scroll_y(reader) == remembered);
    model = status();
    assert(orb_mac_host_attach(host, parent, ORB_MAC_MAINTENANCE, &model, remembered));
    assert(lv_obj_is_hidden(orb_mac_host_reader(host)) && lv_obj_get_scroll_y(orb_mac_host_reader(host)) == 0);
    assert(orb_mac_host_attach(host, parent, ORB_MAC_FORGET, &model, remembered));
    assert(lv_obj_get_scroll_y(orb_mac_host_reader(host)) == 0);
    assert(orb_mac_host_attach(host, parent, ORB_MAC_CONNECTION, &model, remembered));
    assert(lv_obj_get_scroll_y(orb_mac_host_reader(host)) == remembered);
    lv_obj_delete(parent); assert(!orb_app_host_current(host) && !orb_mac_host_reader(host));
    orb_app_host_destroy(host);
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    quiescent();
}
static void admission_and_preview(void)
{
    for (unsigned selected = 0; selected <= ORB_MAC_UNPAIR; selected++) {
        lv_obj_t *parent = page(); orb_app_host_t *host = attach(parent, selected, true);
        unsigned before = s_commands;
        for (unsigned id = 1; id <= 5; id++) {
            lv_obj_t *button = action(parent, id); if (button) lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
        }
        assert(s_commands == before && orb_screen_back(orb_app_host_current(host)) == ORB_BACK_CONSUMED);
        orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();
    }
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used; assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_mac_host_create(false, command, NULL)); orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    lv_obj_t *parent = page(); orb_app_host_t *host = orb_mac_host_create(false, command, NULL); assert(host);
    orb_shell_snapshot_t model = status(); orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    reserve = stats.budget - stats.used; assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_mac_host_attach(host, parent, ORB_MAC_CONNECTION, &model, 0));
    assert(!orb_app_host_current(host)); orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_mac_host_attach(host, parent, ORB_MAC_CONNECTION, &model, 0));
    orb_app_host_detach_page(host, parent); assert(!orb_mac_host_reader(host));
    orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();
}
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_pointer; }
static void pointer_send(lv_indev_t *input, lv_point_t point, lv_indev_state_t state)
{
    s_pointer.point = point; s_pointer.state = state; lv_indev_read(input); advance(16);
}
static void physical_pointer(bool synthetic_input)
{
    lv_obj_t *parent = page(); orb_app_host_t *host = attach(parent, ORB_MAC_FORGET, false);
    lv_obj_t *button = action(parent, 4); lv_obj_update_layout(parent);
    lv_area_t bounds; lv_obj_get_coords(button, &bounds);
    lv_point_t point = {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    lv_indev_t *input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, pointer_read); if (synthetic_input) orb_input_mark_synthetic(input);
    unsigned before = s_commands;
    pointer_send(input, point, LV_INDEV_STATE_PRESSED); pointer_send(input, point, LV_INDEV_STATE_RELEASED);
    assert(s_commands == before + 1 && s_last.physical == !synthetic_input && s_last.value.command.action == 4);
    lv_indev_delete(input); orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();
}
static void destroy_on_command(void)
{
    s_destroy_parent = page(); s_destroy_host = attach(s_destroy_parent, ORB_MAC_CONNECTION, false);
    unsigned before = s_commands;
    lv_obj_send_event(action(s_destroy_parent, 1), LV_EVENT_CLICKED, NULL);
    assert(s_commands == before + 1 && !s_destroy_host && !s_destroy_parent); quiescent();
}
static void summary(void)
{
    orb_shell_snapshot_t model = status();
    assert(!strcmp(orb_system_connection_summary(&model, true, true), "Wi-Fi offline"));
    model.net = ORB_SHELL_NET_CONNECTED;
    assert(!strcmp(orb_system_connection_summary(&model, true, true), "Mac offline"));
    model.link = ORB_SHELL_LINK_ONLINE;
    assert(!strcmp(orb_system_connection_summary(&model, false, true), "Mac offline"));
    assert(!strcmp(orb_system_connection_summary(&model, true, false), "Syncing"));
    assert(!strcmp(orb_system_connection_summary(&model, true, true), "Connected"));
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    const orb_screen_descriptor_t *descriptor = orb_mac_descriptor(); assert(orb_screen_descriptor_valid(descriptor, NULL));
    const orb_screen_descriptor_t *screens[] = {descriptor};
    orb_app_manifest_t manifest = {.id = descriptor->id, .name = "Mac", .screens = screens,
        .screen_count = 1, .entry = descriptor->id};
    orb_screen_registry_t registry; orb_screen_registry_init(&registry); assert(orb_screen_registry_add(&registry, &manifest, NULL));
    for (unsigned selected = 0; selected <= ORB_MAC_UNPAIR; selected++) {
        lv_obj_t *parent = page(); orb_app_host_t *host = attach(parent, selected, false);
        orb_app_host_destroy(host); lv_obj_delete(parent);
    }
    advance(1600); s_workspace_baseline = used(); s_timer_baseline = timers();
    lifecycle(); projection_and_rebuild(); admission_and_preview(); physical_pointer(false); physical_pointer(true);
    destroy_on_command(); summary();
    puts("system_mac: lifecycle, copied status, recreation/scroll, preview/input, destruction and admission passed");
    return 0;
}
