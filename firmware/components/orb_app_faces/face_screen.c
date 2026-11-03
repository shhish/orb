/* Faces keeps its retained renderer/editor. The framework owns the instance;
 * durable preferences and asynchronous results remain in orb_settings. */
#include "orb_app_faces.h"
#include "orb_input.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    orb_faces_t *view;
    uint64_t request_result;
    bool retained;
} faces_state_t;

#define CONTEXT_BYTES (sizeof(orb_screen_context_t) + sizeof(orb_app_faces_context_t))
#define U32(member) {offsetof(orb_app_faces_context_t, member), sizeof(uint32_t), ORB_CONTEXT_U32}
#define BOOL(member) {offsetof(orb_app_faces_context_t, member), sizeof(bool), ORB_CONTEXT_BOOL}
static const orb_context_field_t s_fields[] = {
    U32(prefs[0]), U32(prefs[1]), U32(prefs[2]), U32(prefs[3]),
    U32(prefs[4]), U32(prefs[5]), U32(prefs[6]), U32(prefs[7]),
    U32(prefs[8]), U32(prefs[9]), U32(prefs[10]), U32(prefs[11]),
    U32(prefs[12]), U32(prefs[13]), U32(prefs[14]), U32(prefs[15]),
    U32(mode), U32(slot), U32(face_before_gallery),
    BOOL(browsing), BOOL(dirty), BOOL(save_error), BOOL(failed_select),
};
_Static_assert(sizeof(orb_faces_prefs_t) == 16, "preference intent and context record");

static faces_state_t *state_of(const orb_screen_instance_t *instance)
{
    return orb_screen_alive(instance) ? orb_screen_state(instance) : NULL;
}

static orb_faces_t *view_of(const orb_screen_instance_t *instance)
{
    faces_state_t *state = state_of(instance);
    return state ? state->view : NULL;
}

static void open_target(orb_faces_target_t target, void *context)
{
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = !orb_input_active_is_synthetic(),
        .value.command = {.action = ORB_APP_FACES_OPEN, .value = target}};
    (void)orb_screen_emit(context, &intent);
}

static uint64_t request(const orb_faces_prefs_t *prefs, void *context)
{
    faces_state_t *state = state_of(context);
    if (!state) return 0;
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = !orb_input_active_is_synthetic(), .value.command.action = ORB_APP_FACES_PREFERENCES};
    /* Explicit value fields keep the existing 16-byte durable record intact. */
    const unsigned char *bytes = (const unsigned char *)prefs;
    memcpy(&intent.value.command.target, bytes, 8);
    memcpy(&intent.value.command.value, bytes + 8, 4);
    memcpy(&intent.value.command.extra, bytes + 12, 4);
    state->request_result = 0;
    return orb_screen_emit(context, &intent) ? state->request_result : 0;
}

bool orb_app_faces_preferences_from_intent(const orb_intent_t *intent, orb_faces_prefs_t *prefs)
{
    if (!intent || !prefs || intent->kind != ORB_INTENT_APP_COMMAND ||
        intent->value.command.action != ORB_APP_FACES_PREFERENCES) return false;
    unsigned char *bytes = (unsigned char *)prefs;
    memcpy(bytes, &intent->value.command.target, 8);
    memcpy(bytes + 8, &intent->value.command.value, 4);
    memcpy(bytes + 12, &intent->value.command.extra, 4);
    return orb_settings_faces_valid(prefs);
}

void orb_app_faces_request_result(orb_screen_instance_t *instance, uint64_t ticket)
{
    faces_state_t *state = state_of(instance);
    if (state) state->request_result = ticket;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    faces_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    bool public = orb_screen_descriptor(instance)->id == ORB_APP_FACES_PUBLIC_SCREEN_ID;
    state->view = orb_faces_create(root, &(orb_faces_config_t){.open = open_target, .context = instance,
        .preview = orb_screen_host(instance)->config.preview, .always_on = public, .request = request});
    return state->view != NULL;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    faces_state_t *state = orb_screen_state(instance);
    if (!state->view || !update->model || !update->model->data ||
        update->model->size != sizeof(orb_app_faces_model_t)) return;
    const orb_app_faces_model_t *model = update->model->data;
    if (orb_screen_descriptor(instance)->id == ORB_APP_FACES_PUBLIC_SCREEN_ID) {
        orb_faces_prefs_t prefs;
        orb_settings_faces_current(&prefs);
        orb_faces_set_preferences(state->view, &prefs);
        /* Direct framework users obey the same public-data boundary as the
         * shell host. Never retain a household count or unused location. */
        orb_faces_snapshot_t public = model->snapshot;
        orb_app_faces_public_projection(&public);
        orb_faces_update(state->view, &public);
    } else orb_faces_update(state->view, &model->snapshot);
}

static void destroy(orb_screen_instance_t *instance)
{
    /* Parent deletion runs the presenter's own timer/result cleanup while the
     * framework state is still alive. It never replays or cancels a write. */
    faces_state_t *state = orb_screen_state(instance);
    state->view = NULL;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    orb_faces_context_t view;
    if (capacity < CONTEXT_BYTES || !orb_faces_save_context(view_of(instance), &view)) return false;
    orb_app_faces_context_t value = {.mode = view.mode, .slot = view.slot,
        .face_before_gallery = view.face_before_gallery, .browsing = view.browsing,
        .dirty = view.dirty, .save_error = view.save_error, .failed_select = view.failed_select};
    const unsigned char *prefs = (const unsigned char *)&view.prefs;
    for (size_t i = 0; i < sizeof(view.prefs); i++) value.prefs[i] = prefs[i];
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    orb_app_faces_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.mode > UINT8_MAX || value.slot > UINT8_MAX || value.face_before_gallery > UINT8_MAX) return false;
    orb_faces_context_t view = {.mode = value.mode, .slot = value.slot,
        .face_before_gallery = value.face_before_gallery, .browsing = value.browsing,
        .dirty = value.dirty, .save_error = value.save_error, .failed_select = value.failed_select};
    unsigned char *prefs = (unsigned char *)&view.prefs;
    for (size_t i = 0; i < sizeof(view.prefs); i++) {
        if (value.prefs[i] > UINT8_MAX) return false;
        prefs[i] = (uint8_t)value.prefs[i];
    }
    return orb_faces_restore_context(view_of(instance), &view);
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    return orb_faces_back(view_of(instance)) ? ORB_BACK_CONSUMED : ORB_BACK_LEAVE_SCREEN;
}

static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
}

static void rim_preview(orb_screen_instance_t *instance, float value)
{
    orb_app_faces_rotate(instance, (int)lrintf(value));
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)value; (void)commit; (void)origin; (void)physical;
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    int value, maximum;
    if (!orb_app_faces_rotary(instance, &value, &maximum)) return false;
    bool gallery = orb_app_faces_mode(instance) == ORB_FACES_GALLERY;
    *binding = (orb_rim_binding_t){.subpage = orb_app_faces_mode(instance), .property = 1,
        .kind = ORB_RIM_FOCUS, .minimum = gallery ? -32768 : 0,
        .maximum = gallery ? 32767 : maximum, .value = value, .detent = 1,
        .units_per_turn = maximum + 1, .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    orb_faces_set_covered(view_of(instance), value != ORB_VISIBILITY_ACTIVE);
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Face"};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
#define DESCRIPTOR(id_value, privacy_value, aod_value) { \
    .id = id_value, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops, \
    .state_bytes = sizeof(faces_state_t), .context_bytes = CONTEXT_BYTES, .context_version = 1, \
    .context_fields = s_fields, .context_field_count = sizeof(s_fields) / sizeof(s_fields[0]), \
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(faces_state_t), \
        .context_bytes = CONTEXT_BYTES, .declared = true}, \
    .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_HOME | ORB_DOMAIN_TIME | ORB_DOMAIN_SETTINGS, \
    .privacy = privacy_value, .always_on = aod_value, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_FACE, \
}
static const orb_screen_descriptor_t s_descriptor = DESCRIPTOR(ORB_APP_FACES_SCREEN_ID, ORB_PRIVACY_PRIVATE, ORB_AOD_HIDDEN);
static const orb_screen_descriptor_t s_public = DESCRIPTOR(ORB_APP_FACES_PUBLIC_SCREEN_ID, ORB_PRIVACY_PUBLIC, ORB_AOD_PUBLIC_FACE);

const orb_screen_descriptor_t *orb_app_faces_descriptor(void) { return &s_descriptor; }
const orb_screen_descriptor_t *orb_app_faces_public_descriptor(void) { return &s_public; }
lv_obj_t *orb_app_faces_root(orb_screen_instance_t *instance) { return orb_faces_root(view_of(instance)); }
orb_faces_mode_t orb_app_faces_mode(orb_screen_instance_t *instance) { return orb_faces_mode(view_of(instance)); }
void orb_app_faces_hold(orb_screen_instance_t *instance) { orb_faces_hold(view_of(instance)); }
void orb_app_faces_confirm_selection(orb_screen_instance_t *instance) { orb_faces_confirm_selection(view_of(instance)); }
bool orb_app_faces_rotary(orb_screen_instance_t *instance, int *value, int *maximum)
{
    return orb_faces_rotary(view_of(instance), value, maximum);
}
void orb_app_faces_rotate(orb_screen_instance_t *instance, int value)
{
    if (orb_app_faces_mode(instance) == ORB_FACES_GALLERY)
        value = (value % ORB_FACE_COUNT + ORB_FACE_COUNT) % ORB_FACE_COUNT;
    orb_faces_rotate(view_of(instance), value);
}
void orb_app_faces_rotary_finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }

void orb_app_faces_active(orb_screen_instance_t *instance, bool active, bool save_preferences)
{
    if (save_preferences) (void)orb_faces_save(view_of(instance));
    orb_faces_set_active(view_of(instance), active);
}

bool orb_app_faces_retain(orb_screen_instance_t *instance, lv_obj_t *root)
{
    faces_state_t *state = state_of(instance);
    if (!state || state->retained || orb_screen_host(instance)->config.preview ||
        orb_faces_mode(state->view) == ORB_FACES_FACE) return false;
    orb_faces_set_covered(state->view, true);
    lv_obj_set_parent(orb_screen_root(instance), root);
    lv_obj_set_hidden(orb_screen_root(instance), true);
    state->retained = true;
    return true;
}

bool orb_app_faces_resume(orb_screen_instance_t *instance, lv_obj_t *page)
{
    faces_state_t *state = state_of(instance);
    if (!state || !state->retained) return false;
    lv_obj_set_parent(orb_screen_root(instance), page);
    lv_obj_set_hidden(orb_screen_root(instance), false);
    state->retained = false;
    return true;
}

bool orb_app_faces_retained(orb_screen_instance_t *instance)
{
    faces_state_t *state = state_of(instance);
    return state && state->retained;
}
