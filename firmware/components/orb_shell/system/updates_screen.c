/* HIG §§1/3/4/7: unchanged software status, reader and one explicit action. */
#include "system_views.h"
#include "orb_fonts.h"
#include "orb_icons.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { lv_obj_t *list; } updates_state_t;

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .value.command = {.target = ORB_SHELL_UPDATES, .action = ORB_SYSTEM_CHECK_UPDATE}};
    (void)orb_screen_emit(instance, &intent);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    updates_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    orb_updates_model_t model = {0};
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context, ORB_SHELL_UPDATES, &projection) &&
        projection.model && projection.model->data && projection.model->size == sizeof(model))
        memcpy(&model, projection.model->data, sizeof(model));
    lv_obj_t *title = orb_kit_rim_title_create(root, "Updates", ORB_KIT_CODEX);
    lv_obj_t *disc = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD,
        ORB_KIT_ACCENT_FILL, ORB_KIT_CODEX, ORB_ICON_UPLOAD);
    if (!title || !disc) return false;
    lv_obj_set_pos(disc, 193, 84); lv_obj_set_clickable(disc, false);
    char version[64];
    const char *start = model.firmware_version;
    if (*start == 'v') start++;
    unsigned length = 0;
    while ((start[length] >= '0' && start[length] <= '9') || start[length] == '.') length++;
    if (length && start[length - 1] != '.') snprintf(version, sizeof(version), "Orb %.*s", length, start);
    else snprintf(version, sizeof(version), "Orb development");
    orb_kit_page_label(root, version, ORB_TEXT_TITLE3, true, 233, 184, 340, 1, ORB_LABEL);
    state->list = orb_kit_detail_page(root, &(orb_kit_detail_page_config_t){
        .top = 242, .height = 84, .system = true, .arc = true});
    if (!state->list) return false;
    orb_kit_paragraph(state->list, !model.online ? "Connect your Mac to check for updates." :
        model.pending ? "Checking for updates…" :
        model.result[0] ? model.result : "Check for software from your Mac.",
        ORB_TEXT_FOOTNOTE2, false, 0, ORB_KIT_SECONDARY, true);
    orb_kit_breathe_since(lv_obj_get_child(state->list, -1), model.online && model.pending, model.wait_started);
    lv_obj_set_style_text_opa(state->list, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_t *check = orb_kit_button_primary_create(root, "Check now", ORB_KIT_CODEX);
    if (!check) return false;
    lv_obj_update_layout(check);
    lv_obj_set_pos(check, 233 - lv_obj_get_width(check) / 2, 380 - lv_obj_get_height(check) / 2);
    lv_obj_set_user_data(check, (void *)(intptr_t)ORB_SYSTEM_CHECK_UPDATE);
    lv_obj_set_event_bubble(check, true);
    lv_obj_add_event_cb(check, clicked, LV_EVENT_CLICKED, instance);
    if (!model.online || model.pending) lv_obj_add_state(check, LV_STATE_DISABLED);
    orb_screen_page_bind(instance, state->list, title);
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    /* Status publication still schedules the original outer full render. */
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
    /* The shell's system-place context keeps Updates scroll at its original
     * route/history boundary; do not introduce a second restoration owner. */
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
    updates_state_t *state = orb_screen_state(instance);
    lv_obj_scroll_to_y(state->list, (int32_t)lrintf(value), LV_ANIM_OFF);
    orb_round_list_update(state->list);
}
static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)commit; (void)origin; (void)physical;
    rim_preview(instance, value);
}
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    updates_state_t *state = orb_screen_state(instance);
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
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Updates"};
}
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SHELL_UPDATES, .parent_id = ORB_SHELL_FACE, .ops = &s_ops,
    .state_bytes = sizeof(updates_state_t), .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(updates_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};
const orb_screen_descriptor_t *orb_updates_descriptor(void) { return &s_descriptor; }
