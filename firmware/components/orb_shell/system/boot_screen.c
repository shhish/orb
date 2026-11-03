/* HIG §§1/3/4/7/8: preserve the existing round setup code, scrollable
 * guidance and explicit Connection recovery. Service precedence stays shell-owned. */
#include "boot_screen.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    orb_system_scaffold_t *scaffold;
    view_t view;
} boot_state_t;

static void grouped_code(char *out, size_t cap, const char *code, size_t digits)
{
    out[0] = '\0';
    if (strlen(code) != digits) return;
    for (size_t i = 0; i < digits; i++) {
        if (code[i] < '0' || code[i] > '9') return;
    }
    size_t half = digits / 2;
    snprintf(out, cap, "%.*s %s", (int)half, code, code + half);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    boot_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (!host->config.project || !host->config.project(host->config.context, ORB_BOOT_SCREEN_ID, &projection) ||
        !projection.model || !projection.model->data || projection.model->size != sizeof(state->scaffold)) return false;
    memcpy(&state->scaffold, projection.model->data, sizeof(state->scaffold));
    if (!state->scaffold) return false;
    /* The shared title belongs to the scaffold, not this instance: page cleanup
     * must not delete a node reused by the next setup/OTA presentation. */
    orb_screen_page_bind(instance, state->scaffold->reading, NULL);
    return true;
}
void orb_boot_screen_content(orb_screen_instance_t *instance, orb_system_scaffold_t *s,
    view_t view, const orb_shell_snapshot_t *m)
{
    if (!s || !m) return;
    if (orb_screen_alive(instance)) {
        boot_state_t *state = orb_screen_state(instance);
        s = state->scaffold;
        state->view = view;
    }
    /* Restore borrowed ink before layout establishes the next lane style.
     * Resuming after layout keeps the original scaffold deadline. */
    orb_kit_breathe(s->lines[LINE_BODY], false);
    char code[24] = "", body[192] = "";
    switch (view) {
    case VIEW_START:
        orb_system_scaffold_line(s, view, LINE_TITLE, "Starting Orb", ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED, 1);
        orb_system_scaffold_line(s, view, LINE_BODY, "Getting things ready", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 2);
        break;
    case VIEW_SETUP:
        grouped_code(code, sizeof(code), m->pop, 8);
        bool connecting = m->net == ORB_SHELL_NET_CONNECTING;
        orb_system_scaffold_line(s, view, LINE_TITLE, connecting ? "Connecting to Wi-Fi" :
             m->net == ORB_SHELL_NET_FAILED ? "Try Wi-Fi again" : "Set up Wi-Fi",
             ORB_TEXT_CAPTION1, ORB_TEXT_EMPHASIZED, 1);
        orb_system_scaffold_line(s, view, LINE_CODE, connecting ? "" : code, ORB_TEXT_TITLE2,
             ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 1);
        orb_system_scaffold_line(s, view, LINE_BODY, connecting ? "Orb is joining your network.\nKeep it near your Mac." :
             !code[0] ? "Preparing setup code" :
             orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "On your Mac, run:" : "In Terminal on your Mac, run:",
             ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 2);
        orb_system_scaffold_line(s, view, LINE_COMMAND, !connecting && code[0] ? "orb-hub setup" : "",
             ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 1);
        orb_system_scaffold_line(s, view, LINE_DETAIL, m->net == ORB_SHELL_NET_FAILED ?
             (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Run setup again." :
              "Wi-Fi could not connect.\nRun setup again to retry.") :
             !connecting && code[0] ? (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ?
              "Enter this code." : "Enter the setup code above.") : "",
             ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 2);
        break;
    case VIEW_PAIR:
        grouped_code(code, sizeof(code), m->pairing_code, 6);
        orb_system_scaffold_line(s, view, LINE_TITLE, "Pair with your Mac", ORB_TEXT_CAPTION1, ORB_TEXT_EMPHASIZED, 1);
        orb_system_scaffold_line(s, view, LINE_CODE, code, ORB_TEXT_TITLE1, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 1);
        lv_obj_set_style_text_letter_space(s->lines[LINE_CODE], 4, 0);
        /* Review #7: U+2011 keeps "orb‑hub" whole when the line wraps. */
        orb_system_scaffold_line(s, view, LINE_BODY, code[0] ? (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ?
             "Use orb\xE2\x80\x91hub on your Mac.\nUse the same Wi-Fi." :
             "Type this code in orb\xE2\x80\x91hub.\nUse the same Wi-Fi.") :
             "Preparing pairing code", ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 3);
        if (m->hub_name[0]) snprintf(body, sizeof(body), "Found %s", m->hub_name);
        orb_system_scaffold_line(s, view, LINE_DETAIL, body, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 2);
        break;
    case VIEW_SERVICE:
        orb_system_scaffold_line(s, view, LINE_TITLE, "Needs attention", ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 1);
        orb_system_scaffold_line(s, view, LINE_BODY, m->service_error, ORB_TEXT_BODY, ORB_TEXT_REGULAR, 3);
        orb_system_scaffold_line(s, view, LINE_DETAIL, "Retry in Connection.", ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 2);
        orb_system_scaffold_primary(s, "Connection", true);
        break;
    case VIEW_IDENTIFY:
        orb_system_scaffold_line(s, view, LINE_TITLE, "This is your Orb", ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 1);
        orb_system_scaffold_line(s, view, LINE_BODY, m->device_name[0] ? m->device_name : "Orb", ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED, 2);
        orb_system_scaffold_line(s, view, LINE_DETAIL, m->device_id, ORB_TEXT_FOOTNOTE2, ORB_TEXT_TABULAR, 1);
        break;
    default: break;
    }
    unsigned kind = view == VIEW_START ? 1 : view == VIEW_SETUP ?
        (m->net == ORB_SHELL_NET_CONNECTING ? 2 : !code[0] ? 3 : 0) :
        view == VIEW_PAIR && !code[0] ? 4 : 0;
    if (kind && kind != s->boot_wait_kind) s->boot_wait_started = lv_tick_get();
    s->boot_wait_kind = kind;
    /* The visible word borrows the shell budget after Connection or rebuild. */

}
static void visible(lv_obj_t *object, bool shown)
{
    lv_obj_set_hidden(object, !shown);
    orb_kit_scroll_arc_refresh(object);
}
void orb_boot_screen_layout(orb_system_scaffold_t *s, view_t view)
{
    bool pair = view == VIEW_PAIR, setup = view == VIEW_SETUP;
    if (!pair && !setup) {
        orb_system_scaffold_layout(s, view);
        orb_kit_breathe_since(s->lines[LINE_BODY], s->boot_wait_kind != 0, s->boot_wait_started);
        return;
    }
    visible(s->cancel, false);
    visible(s->wifi, false);
    visible(s->battery, false);
    visible(s->power, false);
    visible(s->settings, false);
    visible(s->art, true);
    lv_obj_set_pos(s->art, 193, 84);
    orb_kit_disc_set_glyph(s->art, ORB_ICON_DESKTOP);
    /* HIG §§3/7/8 and Design 2.0: identity belongs on the rim. The code
     * remains the hero and complete setup/recovery guidance scrolls below it,
     * rather than letting AX3 or a long Mac name cross the round safe area. */
    visible(s->lines[LINE_TITLE], false);
    int bottom = orb_system_scaffold_design_line(s, LINE_CODE, 174, 398, ORB_LABEL);
    int top = LV_MAX(258, bottom + 12);
    visible(s->reading, true);
    lv_obj_set_pos(s->reading, 100, top);
    lv_obj_set_size(s->reading, 266, LV_MAX(32, 398 - top));
    lv_obj_set_scrollbar_mode(s->reading, LV_SCROLLBAR_MODE_OFF);
    int y = 0;
    const line_t copy[] = {LINE_BODY, LINE_COMMAND, LINE_DETAIL};
    for (unsigned i = 0; i < sizeof(copy) / sizeof(*copy); ++i) {
        lv_obj_t *obj = s->lines[copy[i]];
        if (lv_obj_is_hidden(obj)) continue;
        lv_obj_set_parent(obj, s->reading);
        lv_obj_set_width(obj, 266);
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_color(obj, ORB_KIT_SECONDARY, 0);
        lv_obj_set_style_text_opa(obj, ORB_KIT_SECONDARY_OPA, 0);
        lv_obj_update_layout(obj);
        lv_obj_set_pos(obj, 0, y);
        y += lv_obj_get_height(obj) + 12;
    }
    lv_obj_set_height(s->reading, orb_kit_reader_whole_lines(s->reading, lv_obj_get_height(s->reading)));
    lv_obj_update_layout(s->reading);
    orb_kit_breathe_since(s->lines[LINE_BODY], s->boot_wait_kind != 0, s->boot_wait_started);
}

void orb_boot_screen_activate(orb_screen_instance_t *instance)
{
    if (!orb_screen_alive(instance)) return;
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .value.command = {.target = ORB_BOOT_SCREEN_ID, .action = ORB_BOOT_OPEN_CONNECTION}};
    /* Acceptance may synchronously destroy the sender. */
    (void)orb_screen_emit(instance, &intent);
}
static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{ (void)instance; (void)value; }
static void destroy(orb_screen_instance_t *instance)
{ (void)instance; }
static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{ (void)instance; (void)context; return capacity >= sizeof(*context); }
static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{ (void)instance; (void)context; return true; }
static orb_back_result_t back(orb_screen_instance_t *instance)
{ (void)instance; return ORB_BACK_LEAVE_SCREEN; }
static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{ (void)instance; (void)origin; (void)physical; }
static void rim_preview(orb_screen_instance_t *instance, float value)
{
    const boot_state_t *state = orb_screen_state(instance);
    lv_obj_scroll_to_y(state->scaffold->reading, (int32_t)lrintf(value), LV_ANIM_OFF);
}
static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{ (void)commit; (void)origin; (void)physical; rim_preview(instance, value); }
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    const boot_state_t *state = orb_screen_state(instance);
    lv_obj_t *reader = state->scaffold->reading;
    if (lv_obj_is_hidden(reader)) return false;
    lv_obj_update_layout(reader);
    int32_t top = lv_obj_get_scroll_y(reader);
    int32_t maximum = LV_MAX(0, top + lv_obj_get_scroll_bottom(reader));
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.property = 1, .subpage = state->view, .kind = ORB_RIM_SCROLL,
        .maximum = maximum, .value = top, .detent = 72, .units_per_turn = 900,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{ (void)instance; (void)value; }
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    const boot_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = lv_label_get_text(state->scaffold->lines[LINE_TITLE])};
}
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_BOOT_SCREEN_ID, .ops = &s_ops, .state_bytes = sizeof(boot_state_t),
    .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(boot_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};
const orb_screen_descriptor_t *orb_boot_descriptor(void) { return &s_descriptor; }
static bool allow_synthetic(const orb_intent_t *intent)
{ return intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_BOOT_SCREEN_ID; }
static void finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }
orb_app_host_t *orb_boot_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){.descriptor = orb_boot_descriptor,
        .name = "Setup", .model_bytes = sizeof(orb_system_scaffold_t *), .command = command,
        .context = context, .allow_synthetic = allow_synthetic, .rim_finish = finish});
}
bool orb_boot_host_ensure(orb_app_host_t *host, lv_obj_t *parent, orb_system_scaffold_t *scaffold)
{
    if (orb_app_host_current(host)) return true;
    return scaffold && orb_app_host_set_model(host, &scaffold, sizeof(scaffold)) && orb_app_host_attach(host, parent);
}
