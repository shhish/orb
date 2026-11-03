/* HIG §§1/3/7/8: the existing Mac diagnostics and neutral recovery actions.
 * Shell policy still chooses the page and guards physical service requests. */
#include "mac_screen.h"
#include "../orb_system_copy.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { lv_obj_t *list; orb_mac_page_t page; } mac_state_t;

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .value.command = {.target = ORB_MAC_SCREEN_ID,
            .action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event))}};
    /* Acceptance may synchronously recreate the page. Do not use the sender
     * after dispatch; synthetic recovery still reaches the shell's hint guard. */
    (void)orb_screen_emit(instance, &intent);
}

static lv_obj_t *button(orb_screen_instance_t *instance, const char *text, int cy, intptr_t command)
{
    lv_obj_t *button = orb_kit_confirm_action(orb_screen_root(instance), text, cy);
    if (!button) return NULL;
    lv_obj_set_user_data(button, (void *)command);
    lv_obj_set_event_bubble(button, true);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, instance);
    return button;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    mac_state_t *state = orb_screen_state(instance);
    orb_mac_model_t model = {0};
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_MAC_SCREEN_ID, &projection) && projection.model && projection.model->data &&
        projection.model->size == sizeof(model))
        memcpy(&model, projection.model->data, sizeof(model));
    if (model.page > ORB_MAC_UNPAIR) return false;
    state->page = model.page;
    orb_shell_snapshot_t status = {.net = model.net, .link = model.link};
    snprintf(status.ssid, sizeof(status.ssid), "%s", model.ssid);
    snprintf(status.hub_name, sizeof(status.hub_name), "%s", model.hub_name);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    /* Unlike moving workspace pages, this title stays inside its canvas in
     * original child order. Only the reader is exported to shell navigation. */
    const char *title = state->page == ORB_MAC_CONNECTION ? "Mac" : state->page == ORB_MAC_MAINTENANCE ?
                        "Maintenance" : state->page == ORB_MAC_FORGET ? "Forget Wi-Fi?" : "Unpair this Mac?";
    if (!orb_kit_rim_title_create(root, title, ORB_KIT_GREY)) return false;
    state->list = lv_obj_create(root);
    if (!state->list) return false;
    orb_screen_page_bind(instance, state->list, NULL);
    lv_obj_remove_style_all(state->list);
    lv_obj_set_scroll_dir(state->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_chain(state->list, false);
    lv_obj_set_event_bubble(state->list, true);
    if (state->page == ORB_MAC_MAINTENANCE) {
        lv_obj_set_hidden(state->list, true);
        button(instance, "Forget Wi-Fi", 220, 2);
        button(instance, "Unpair Mac", 298, 3);
        return true;
    }
    if (state->page == ORB_MAC_FORGET || state->page == ORB_MAC_UNPAIR) {
        /* Both decisions are neutral: recovery has no default action. Full
         * consequences remain in a scrollable content layer at AX3. */
        orb_kit_confirm_content(state->list, state->page == ORB_MAC_FORGET ?
            "You'll need to set up Wi-Fi again." :
            "Orb will ask for a new pairing code. Wi-Fi stays connected.");
        button(instance, state->page == ORB_MAC_FORGET ? "Forget Wi-Fi" : "Unpair", 286,
                   state->page == ORB_MAC_FORGET ? 4 : 5);
        button(instance, "Cancel", 380, 1);
        return true;
    }
    lv_obj_set_pos(state->list, 0, 90);
    lv_obj_set_size(state->list, 466, 242);
    const orb_shell_snapshot_t *m = &status;
    /* Review #5/#6: every state has its own status line and fix. */
    orb_system_connection_copy_t copy = orb_system_connection_copy(m);
    orb_kit_row_config_t rows[] = {
        {.title = "Wi-Fi", .subtitle = m->ssid, .value = copy.wifi,
         .glyph = m->net == ORB_SHELL_NET_CONNECTED ? ORB_ICON_WIFI_HIGH : ORB_ICON_WIFI_SLASH, .accent = ORB_KIT_GREY},
        {.title = "Mac", .subtitle = m->hub_name, .value = copy.mac, .glyph = ORB_ICON_DESKTOP, .accent = ORB_KIT_GREY},
        {.title = copy.status, .subtitle = copy.fix, .glyph = ORB_ICON_WARNING, .accent = ORB_KIT_GREY},
    };
    unsigned count = copy.status ? 3 : 2;
    int32_t offset = count == 3 ? 0 : (242 - 2 * ORB_KIT_ROW_H - 12) / 2; /* Two rows rest centred. */
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *row = orb_kit_row_create(state->list, &rows[i]);
        lv_obj_set_y(row, offset + (int32_t)i * (ORB_KIT_ROW_H + 12));
        lv_obj_set_clickable(row, false);
    }
    orb_kit_list_apply(state->list);
    lv_obj_scroll_to_y(state->list, model.scroll, LV_ANIM_OFF);
    button(instance, "Maintenance", 380, 1);
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    /* Every shell render rebuilds this presentation. Framework staging and
     * revision delivery must not refit it or restore scroll a second time. */
    (void)instance; (void)value;
}
static void destroy(orb_screen_instance_t *instance) { (void)instance; }
static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    (void)instance;
    return capacity >= sizeof(*context);
}
static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    /* The shell retains only Connection scroll, at its original render entry.
     * Confirmation/maintenance readers still start fresh on every rebuild. */
    (void)instance; (void)context;
    return true;
}
static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    return ORB_BACK_LEAVE_SCREEN;
}
static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
}
static void rim_preview(orb_screen_instance_t *instance, float value)
{
    mac_state_t *state = orb_screen_state(instance);
    lv_obj_scroll_to_y(state->list, (int32_t)lrintf(value), LV_ANIM_OFF);
}
static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)commit; (void)origin; (void)physical;
    rim_preview(instance, value);
}
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    mac_state_t *state = orb_screen_state(instance);
    if (lv_obj_is_hidden(state->list)) return false;
    lv_obj_update_layout(state->list);
    int32_t top = lv_obj_get_scroll_y(state->list);
    int32_t maximum = LV_MAX(0, top + lv_obj_get_scroll_bottom(state->list));
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.property = 1, .kind = ORB_RIM_SCROLL,
        .maximum = maximum, .value = top, .detent = 72, .units_per_turn = 900,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value) { (void)instance; (void)value; }
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    const mac_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->page == ORB_MAC_CONNECTION ? "Mac" :
        state->page == ORB_MAC_MAINTENANCE ? "Maintenance" :
        state->page == ORB_MAC_FORGET ? "Forget Wi-Fi?" : "Unpair this Mac?"};
}
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_MAC_SCREEN_ID, .ops = &s_ops,
    .state_bytes = sizeof(mac_state_t), .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(mac_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};
const orb_screen_descriptor_t *orb_mac_descriptor(void) { return &s_descriptor; }
static bool allow_synthetic(const orb_intent_t *intent)
{
    return intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_MAC_SCREEN_ID;
}
static void finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }
orb_app_host_t *orb_mac_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){
        .descriptor = orb_mac_descriptor, .name = "Mac", .forget_on_detach = true,
        .model_bytes = sizeof(orb_mac_model_t), .command = command, .context = context,
        .allow_synthetic = allow_synthetic, .rim_finish = finish});
}
bool orb_mac_host_attach(orb_app_host_t *host, lv_obj_t *parent, orb_mac_page_t page,
    const orb_shell_snapshot_t *status, int16_t scroll)
{
    if (!status) return false;
    orb_mac_model_t model = {.page = page, .net = status->net, .link = status->link, .scroll = scroll};
    snprintf(model.ssid, sizeof(model.ssid), "%s", status->ssid);
    snprintf(model.hub_name, sizeof(model.hub_name), "%s", status->hub_name);
    return orb_app_host_set_model(host, &model, sizeof(model)) && orb_app_host_attach(host, parent);
}
lv_obj_t *orb_mac_host_reader(orb_app_host_t *host)
{
    return orb_screen_page_viewport(orb_app_host_current(host));
}
