/* HIG §§1–4/7/8: unchanged round instruction list and explicit acceptance.
 * The service request and its workspace-lifetime observation live in the host. */
#include "system_views.h"
#include "orb_fonts.h"
#include "orb_icons.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_input.h"

#include <math.h>
#include <string.h>

typedef struct { lv_obj_t *list; } onboarding_state_t;

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .value.command = {.target = ORB_SHELL_ONBOARDING, .action = ORB_SYSTEM_ONBOARD_DONE}};
    (void)orb_screen_emit(instance, &intent);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    onboarding_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    orb_onboarding_model_t model = {0};
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context, ORB_SHELL_ONBOARDING, &projection) &&
        projection.model && projection.model->data && projection.model->size == sizeof(model))
        memcpy(&model, projection.model->data, sizeof(model));
    lv_obj_t *title = orb_kit_rim_title_create(root, "Welcome to Orb", ORB_KIT_GREY);
    state->list = lv_obj_create(root);
    if (!title || !state->list) return false;
    lv_obj_remove_style_all(state->list);
    lv_obj_set_scrollable(state->list, false);
    lv_obj_set_clickable(state->list, false);
    lv_obj_set_event_bubble(state->list, true);
    lv_obj_set_size(state->list, 466, model.save_state & 2 ? 196 : 240);
    lv_obj_set_pos(state->list, 0, 84);
    lv_obj_set_scrollable(state->list, true); lv_obj_set_clickable(state->list, true);
    lv_obj_set_scroll_dir(state->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_momentum(state->list, true);
    lv_obj_set_style_pad_bottom(state->list, 96, 0);
    /* The edge owns navigation; the centre remains an ordinary scroll list. */
    static const struct { const char *title, *subtitle, *glyph; } rows[] = {
        {"Notifications", "Top edge", ORB_ICON_ARROW_DOWN},
        {"Smart Stack", "Bottom edge", ORB_ICON_ARROW_UP},
        {"Control Center", "Right edge", ORB_ICON_ARROW_LEFT},
        {"Back", "Press BOOT", ORB_ICON_CHEVRON_LEFT},
        {"Talk", "Hold BOOT", ORB_ICON_MIC},
        {"Lock", "Press PWR", ORB_ICON_LOCK},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
            .title = rows[i].title, .subtitle = rows[i].subtitle,
            .glyph = rows[i].glyph, .accent = ORB_KIT_GREY});
        if (!row) return false;
        lv_obj_set_clickable(row, false);
    }
    orb_scroll_edge_follow(state->list, 12, 16);
    orb_kit_list_apply(state->list);
    orb_kit_scroll_arc_attach(state->list);
    if (model.save_state & 2)
        orb_kit_page_label(root, "Not saved", ORB_TEXT_FOOTNOTE2, false, 233, 288, 302, 1, ORB_LABEL);
    lv_obj_t *got = orb_kit_button_primary_create(root, model.save_state & 1 ? "Saving…" :
        model.save_state & 2 ? "Try again" : "Got it", ORB_KIT_CODEX);
    if (!got) return false;
    if (model.save_state & 1) lv_obj_add_state(got, LV_STATE_DISABLED);
    orb_kit_breathe_since(orb_kit_chip_value_label(got), (model.save_state & 1) != 0, model.wait_started);
    orb_kit_place_bottom_arc(got);
    lv_obj_set_user_data(got, (void *)(intptr_t)ORB_SYSTEM_ONBOARD_DONE);
    lv_obj_add_event_cb(got, clicked, LV_EVENT_CLICKED, instance);
    orb_screen_page_bind(instance, state->list, title);
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    /* Save-state changes request the original outer full render, which also
     * preserves scroll. Staging must not fit or rebuild this tree a second time. */
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
    /* Ordinary departures deliberately forget this tutorial's scroll. */
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
    onboarding_state_t *state = orb_screen_state(instance);
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
    onboarding_state_t *state = orb_screen_state(instance);
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
    *value = (orb_screen_chrome_t){.title = "Welcome to Orb"};
}
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SHELL_ONBOARDING, .parent_id = ORB_SHELL_FACE, .ops = &s_ops,
    .state_bytes = sizeof(onboarding_state_t), .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(onboarding_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};
const orb_screen_descriptor_t *orb_onboarding_descriptor(void) { return &s_descriptor; }
