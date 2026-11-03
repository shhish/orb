/* HIG §§1/4/7: retain the existing matte long look and chord-fitted list.
 * Shell route anchors and explicit age-update opportunities stay unchanged. */
#include "orb_app_notifications.h"
#include "orb_input.h"
#include "orb_page.h"

#include <math.h>

typedef struct {
    orb_notifications_page_t page;
    orb_notifications_view_t *view;
} notifications_state_t;

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    notifications_state_t *state = orb_screen_state(instance);
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (state->page.curved && state->page.viewport && lv_obj_get_parent(target) == state->page.viewport) {
        if (lv_obj_get_style_opa(target, 0) == LV_OPA_TRANSP) return;
        lv_indev_t *input = lv_indev_active();
        if (input) {
            lv_point_t point; lv_indev_get_point(input, &point);
            if (!orb_round_list_hit_test(target, &point)) return;
        }
    }
    lv_area_t bounds; lv_obj_get_coords(target, &bounds);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .launch = {.valid = true, .rect = {bounds.x1, bounds.y1,
            bounds.x2 - bounds.x1 + 1, bounds.y2 - bounds.y1 + 1}},
        .value.command = {.target = orb_screen_descriptor(instance)->id,
            .action = (unsigned)(uintptr_t)lv_obj_get_user_data(target)}};
    /* Acceptance may complete Back and destroy this instance synchronously. */
    (void)orb_screen_emit(instance, &intent);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false); lv_obj_set_event_bubble(root, true);
    return true;
}

orb_notifications_page_t orb_app_notifications_compose(orb_screen_instance_t *instance,
    orb_notifications_view_t *view, const orb_notifications_view_config_t *projection)
{
    if (!orb_screen_alive(instance) || !view || !projection) return (orb_notifications_page_t){0};
    notifications_state_t *state = orb_screen_state(instance);
    state->view = view;
    orb_notifications_view_config_t config = *projection;
    config.page = orb_screen_root(instance); config.clicked = clicked; config.clicked_context = instance;
    state->page = orb_screen_descriptor(instance)->id == ORB_APP_NOTIFICATIONS_LIST ?
        orb_notifications_view_list(view, &config) : orb_notifications_view_detail(view, &config);
    orb_screen_page_bind(instance, state->page.viewport, state->page.title);
    return state->page;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    /* Composition and age ticks have distinct shell-owned scheduling. Staging,
     * restore and model revisions must not typeset or advance either of them. */
    (void)instance; (void)value;
}
static void destroy(orb_screen_instance_t *instance)
{ orb_notifications_view_reset(((notifications_state_t *)orb_screen_state(instance))->view); }
static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{ (void)instance; return capacity >= sizeof(*context); }
static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{ (void)instance; (void)context; return true; }
static orb_back_result_t back(orb_screen_instance_t *instance)
{ (void)instance; return ORB_BACK_LEAVE_SCREEN; }
static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{ (void)instance; (void)origin; (void)physical; }
static void rim_preview(orb_screen_instance_t *instance, float value)
{
    notifications_state_t *state = orb_screen_state(instance);
    lv_obj_scroll_to_y(state->page.viewport, (int32_t)lrintf(value), LV_ANIM_OFF);
    if (state->page.curved) orb_round_list_update(state->page.viewport);
}
static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{ (void)commit; (void)origin; (void)physical; rim_preview(instance, value); }
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    notifications_state_t *state = orb_screen_state(instance);
    lv_obj_t *list = state->page.viewport;
    if (!list) return false;
    int maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.property = 1, .kind = ORB_RIM_SCROLL,
        .maximum = maximum, .value = LV_CLAMP(0, lv_obj_get_scroll_y(list), maximum), .detent = 72, .units_per_turn = 900,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{ (void)instance; (void)value; }
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{ (void)instance; *value = (orb_screen_chrome_t){.title = "Notifications"}; }
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
#define NOTIFICATIONS_DESCRIPTOR(screen, parent) { \
    .id = screen, .parent_id = parent, .ops = &s_ops, \
    .state_bytes = sizeof(notifications_state_t), .context_bytes = sizeof(orb_screen_context_t), .context_version = 1, \
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(notifications_state_t), \
        .context_bytes = sizeof(orb_screen_context_t), .declared = true}, \
    .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_TIME | ORB_DOMAIN_DEVICE, \
    .privacy = ORB_PRIVACY_PRIVATE, .always_on = ORB_AOD_HIDDEN, \
    .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN, \
}
static const orb_screen_descriptor_t s_list = NOTIFICATIONS_DESCRIPTOR(ORB_APP_NOTIFICATIONS_LIST, 0);
static const orb_screen_descriptor_t s_detail = NOTIFICATIONS_DESCRIPTOR(ORB_APP_NOTIFICATIONS_DETAIL, ORB_APP_NOTIFICATIONS_LIST);
const orb_screen_descriptor_t *orb_app_notifications_list_descriptor(void) { return &s_list; }
const orb_screen_descriptor_t *orb_app_notifications_detail_descriptor(void) { return &s_detail; }

orb_notifications_page_t orb_app_notifications_page(orb_screen_instance_t *instance)
{
    return orb_screen_alive(instance) ? ((notifications_state_t *)orb_screen_state(instance))->page :
        (orb_notifications_page_t){0};
}

static lv_obj_t *find_action(lv_obj_t *root, unsigned action)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == action) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_action(lv_obj_get_child(root, i), action);
        if (found) return found;
    }
    return NULL;
}
lv_obj_t *orb_app_notifications_action(orb_screen_instance_t *instance, unsigned action)
{
    return orb_screen_alive(instance) ? find_action(orb_screen_root(instance), action) : NULL;
}
